#include "tif_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#include <windows.h>

#include "tiffio.h"

typedef struct bilevel_image bilevel_image;
struct bilevel_image
{
	const char *name;
	uint32	width;
	uint32	length;
	uint16	orientation;
	float	xres;
	float	yres;
	uint16	resunit;
	uint16	rowbytes;
	uint8	pixels[1];
};

typedef struct rotate_worker_data rotate_worker_data;
struct rotate_worker_data
{
	const bilevel_image *image;
	double		angle;
	HANDLE		event;
	long long 	score;
};

typedef struct image_worker_data image_worker_data;
struct image_worker_data
{
	image_worker_data *next;
	const char *filename;
	const char *name;
	int			index;
	DWORD		threadid;
	bilevel_image *image;
	volatile uint32 done;
	volatile uint32 error;
	uint32		left;
	uint32		right;
	uint32		top;
	uint32		bottom;
	char 		status[100];
};

static CRITICAL_SECTION critsect;

static image_worker_data *workerlist = NULL;
static int workercount = 0;

static uint32 median_width, median_length;
static uint32 cropwidth = 0;
static uint32 croplength = 0;
static int cleanit = 0;
static int norotate = 0;

static const uint8 popcount[256] =
{
	0,1,1,2,1,2,2,3, 1,2,2,3,2,3,3,4,
	1,2,2,3,2,3,3,4, 2,3,3,4,3,4,4,5,
	1,2,2,3,2,3,3,4, 2,3,3,4,3,4,4,5,
	2,3,3,4,3,4,4,5, 3,4,4,5,4,5,5,6,
	1,2,2,3,2,3,3,4, 2,3,3,4,3,4,4,5,
	2,3,3,4,3,4,4,5, 3,4,4,5,4,5,5,6,
	2,3,3,4,3,4,4,5, 3,4,4,5,4,5,5,6,
	3,4,4,5,4,5,5,6, 4,5,5,6,5,6,6,7,

	1,2,2,3,2,3,3,4, 2,3,3,4,3,4,4,5,
	2,3,3,4,3,4,4,5, 3,4,4,5,4,5,5,6,
	2,3,3,4,3,4,4,5, 3,4,4,5,4,5,5,6,
	3,4,4,5,4,5,5,6, 4,5,5,6,5,6,6,7,
	2,3,3,4,3,4,4,5, 3,4,4,5,4,5,5,6,
	3,4,4,5,4,5,5,6, 4,5,5,6,5,6,6,7,
	3,4,4,5,4,5,5,6, 4,5,5,6,5,6,6,7,
	4,5,5,6,5,6,6,7, 5,6,6,7,6,7,7,8
};
	
static	void usage(void);

#define FACTOR(val, size)  ((val < size / 6 || val > 5 * size / 6) ? 1 : 3)

static void
warning_handler(const char *module, const char *format, va_list args)
{
	/* just suppress output here; real handling happens in warning_handler_ext */
}

static void
error_handler(const char *module, const char *format, va_list args)
{
	/* just suppress output here; real handling happens in error_handler_ext */
}

static void
warning_handler_ext(thandle_t handle, const char *module, const char *format, va_list args)
{
	DWORD threadid = GetCurrentThreadId();
	const char *name = "Unknown";
	image_worker_data *worker;
	
	/* find our worker item */
	for (worker = workerlist; worker != NULL; worker = worker->next)
		if (threadid == worker->threadid)
		{
			name = worker->name;
			break;
		}

	/* if we found one, output a warning */
//	if (worker != NULL)
//		printf(format, args);
//		vsnprintf(worker->status, sizeof(worker->status), format, args);
}

static void
error_handler_ext(thandle_t handle, const char *module, const char *format, va_list args)
{
	DWORD threadid = GetCurrentThreadId();
	const char *name = "Unknown";
	image_worker_data *worker;
	
	/* find our worker item */
	for (worker = workerlist; worker != NULL; worker = worker->next)
		if (threadid == worker->threadid)
		{
			name = worker->name;
			break;
		}

	/* if we found one, output an error */
	if (worker != NULL)
//		printf(format, args);
		vsnprintf(worker->status, sizeof(worker->status), format, args);
}

inline int
get_pixel(const bilevel_image *image, int y, int x)
{
	if (y < 0 || x < 0 || y >= image->length || x >= image->width)
		return 0;
	return (image->pixels[y * image->rowbytes + x / 8] & (0x80 >> (x % 8))) != 0;
}

inline void
set_pixel(bilevel_image *image, int y, int x)
{
	if (y < 0 || x < 0 || y >= image->length || x >= image->width)
		return;
	image->pixels[y * image->rowbytes + x / 8] |= 0x80 >> (x % 8);
}

inline void
clear_pixel(bilevel_image *image, int y, int x)
{
	if (y < 0 || x < 0 || y >= image->length || x >= image->width)
		return;
	image->pixels[y * image->rowbytes + x / 8] &= ~(0x80 >> (x % 8));
}

static bilevel_image *
bilevel_image_alloc(uint32 width, uint32 length, const bilevel_image *clonefrom)
{
	uint16 rowbytes = (width + 7) / 8;
	bilevel_image *image;
	
	/* borrow width/height from clone if not specified */
	if (clonefrom != NULL)
	{
		if (width == 0) width = clonefrom->width;
		if (length == 0) length = clonefrom->length;
	}
	
	/* allocate memory for the image */
	rowbytes = (width + 7) / 8;
	image = _TIFFmalloc(sizeof(*image) + length * rowbytes);
	if (image == NULL)
		return NULL;
	
	/* clear to 0 and fill in the basics */
	memset(image, 0, sizeof(*image) + length * rowbytes);
	image->width = width;
	image->length = length;
	image->rowbytes = rowbytes;
	
	/* clone remaining fields */
	if (clonefrom != NULL)
	{
		image->name = clonefrom->name;
		image->orientation = clonefrom->orientation;
		image->xres = clonefrom->xres;
		image->yres = clonefrom->yres;
		image->resunit = clonefrom->resunit;
	}
	return image;
}

static void
bilevel_image_free(bilevel_image *image)
{
	_TIFFfree(image);
}

static bilevel_image *
bilevel_image_load(const char *name, int index)
{
	uint32 x, y, width, length, minb, maxb, threshb;
	bilevel_image *image = NULL;
	uint32 *rgba = NULL;
	TIFF *in = NULL;
	uint32 *src;
	
	/* open source image */
	in = TIFFOpen(name, "ru");
	if (in == NULL)
	{
		/* error message is stashed by the TIFF error handler */
		goto error;
	}
	
	/* set the directory */
	if (TIFFSetDirectory(in, index) == 0)
	{
		fprintf(stderr, "%s: Unable to select image %d\n", index);
		goto error;
	}

	/* read image width */
	width = 0;
	TIFFGetField(in, TIFFTAG_IMAGEWIDTH, &width);
	if (width == 0)
	{
		fprintf(stderr, "%s: Unexpected or missing image width\n", name);
		goto error;
	}

	/* read image length */	
	length = 0;
	TIFFGetField(in, TIFFTAG_IMAGELENGTH, &length);
	if (length == 0)
	{
		fprintf(stderr, "%s: Unexpected or missing image length\n", name);
		goto error;
	}

	/* allocate RGBA buffer */
EnterCriticalSection(&critsect);
	rgba = _TIFFmalloc(width * length * 4);
	if (rgba == NULL)
	{
		fprintf(stderr, "%s: Out of memory allocating RGBA %dx%d\n", name, width, length);
		goto error;
	}

	/* read as RGBA */
	if (TIFFReadRGBAImageOriented(in, width, length, rgba, ORIENTATION_TOPLEFT, 1) == 0)
	{
		fprintf(stderr, "%s: Error reading image\n", name);
		goto error;
	}
	
	/* allocate bilevel_image struct */
	image = bilevel_image_alloc(width, length, NULL);
	if (image == NULL)
	{
		fprintf(stderr, "%s: Out of memory allocating bilevel %dx%d\n", name, width, length);
		goto error;
	}
	image->name = name;
	
	/* fill in the info */
	TIFFGetField(in, TIFFTAG_ORIENTATION, &image->orientation);
	TIFFGetField(in, TIFFTAG_XRESOLUTION, &image->xres);
	TIFFGetField(in, TIFFTAG_YRESOLUTION, &image->yres);
	TIFFGetField(in, TIFFTAG_RESOLUTIONUNIT, &image->resunit);
	
	/* determine the min/max brightness */
	src = rgba;
	minb = 0xff * 10;
	maxb = 0 * 10;
	for (y = 0; y < length; y++)
		for (x = 0; x < width; x++)
		{
			uint32 pix = *src++;
			int bright = (TIFFGetR(pix) * 4 + TIFFGetG(pix) * 5 + TIFFGetB(pix) * 1);
			if (bright < minb) minb = bright;
			if (bright > maxb) maxb = bright;
		}
	
	/* read the image, converting to bilevel along the way */
	src = rgba;
	threshb = minb + ((maxb - minb) * 75 / 100);
	for (y = 0; y < length; y++)
		for (x = 0; x < width; x++)
		{
			uint32 pix = *src++;
			int bright = (TIFFGetR(pix) * 4 + TIFFGetG(pix) * 5 + TIFFGetB(pix) * 1);
			if (bright <= threshb)
				set_pixel(image, y, x);
		}

	/* free memory */
	_TIFFfree(rgba);
LeaveCriticalSection(&critsect);
	TIFFClose(in);
	return image;

error:
	if (image != NULL)
		bilevel_image_free(image);
	if (rgba != NULL)
		_TIFFfree(rgba);
	if (in != NULL)
		TIFFClose(in);
	return NULL;
}

static int
bilevel_image_save_images(const char *name, const image_worker_data *worklist)
{
	TIFF *out;
	uint32 y;
	
	out = TIFFOpen(name, "w");
	if (out == NULL)
		return -1;

	for ( ; worklist != NULL; worklist = worklist->next)
	{
		bilevel_image *image = worklist->image;

		TIFFSetField(out, TIFFTAG_IMAGEWIDTH, image->width);
		TIFFSetField(out, TIFFTAG_IMAGELENGTH, image->length);
		TIFFSetField(out, TIFFTAG_BITSPERSAMPLE, 1);
		TIFFSetField(out, TIFFTAG_SAMPLESPERPIXEL, 1);
		TIFFSetField(out, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
		TIFFSetField(out, TIFFTAG_COMPRESSION, COMPRESSION_CCITTFAX4);
		TIFFSetField(out, TIFFTAG_FILLORDER, FILLORDER_MSB2LSB);
		TIFFSetField(out, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISWHITE);
		TIFFSetField(out, TIFFTAG_ROWSPERSTRIP, image->length);
		if (image->orientation != 0) TIFFSetField(out, TIFFTAG_ORIENTATION, image->orientation);
		if (image->xres != 0) TIFFSetField(out, TIFFTAG_XRESOLUTION, image->xres);
		if (image->yres != 0) TIFFSetField(out, TIFFTAG_YRESOLUTION, image->yres);
		if (image->resunit != 0) TIFFSetField(out, TIFFTAG_RESOLUTIONUNIT, image->resunit);

		for (y = 0; y < image->length; y++)
			if (TIFFWriteScanline(out, (tdata_t)(image->pixels + y * image->rowbytes), y, 0) < 0)
				goto error;

		if (TIFFWriteDirectory(out) == 0)
			goto error;
	}

	TIFFClose(out);
	return 0;

error:
	TIFFClose(out);
	remove(name);
	return -1;
}

static long long
bilevel_image_rotate_score(const bilevel_image *image, double angle)
{
	long long dxdx, dydx, dxdy, dydy;
	long long srcstartx, srcstarty;
	long long summary = 0;
	double sinval, cosval;
	long dstx, dsty;
	uint32 *vrun;
	
	/* convert angle to rotation matrix */
	sinval = sin(angle * M_PI / 180.0);
	cosval = cos(angle * M_PI / 180.0);
	dxdx = (long long)(cosval * (double)(1ll << 32));
	dydx = (long long)(-sinval * (double)(1ll << 32));
	dxdy = -dydx;
	dydy = dxdx;

	/* pick starting source x,y such that we remain centered */
	srcstartx = ((long long)(image->width / 2) * (double)(1ll << 32)) - dxdx * (image->width / 2) - dxdy * (image->length / 2);
	srcstarty = ((long long)(image->length / 2) * (double)(1ll << 32)) - dydx * (image->width / 2) - dydy * (image->length / 2);

	/* allocate memory to track vertical runs */
	vrun = _TIFFmalloc(image->width * sizeof(uint32));
	if (vrun == NULL)
	{
		fprintf(stderr, "bilevel_image_rotate_score: Out of memory allocating vrun for rotation\n");
		return 0;
	}
	memset(vrun, 0, image->width * sizeof(uint32));

	/* iterate over the destination */
	for (dsty = 0; dsty < image->length; dsty++)
	{
		long long srcx = srcstartx + dsty * dxdy;
		long long srcy = srcstarty + dsty * dydy;
		long run = 0;
		
		/* iterate over destination rows */
		for (dstx = 0; dstx < image->width; dstx++)
		{
			uint32 sx = srcx >> 32;
			uint32 sy = srcy >> 32;
			
			/* if we're in range, count black pixel runs */
			if (get_pixel(image, sy, sx))
			{
				run++;
				vrun[dstx]++;
			}
			
			/* otherwise, end the current run and update the vertical runs as well for this column */
			else
			{
				summary += FACTOR(dsty, image->length) * run * run + 
							FACTOR(dstx, image->width) * vrun[dstx] * vrun[dstx];
				run = 0;
				vrun[dstx] = 0;
			}
			
			/* advance source in both X and Y */
			srcx += dxdx;
			srcy += dydx;
		}
		
		/* account for any runs off the end */
		summary += FACTOR(dsty, image->length) * run * run;
	}

	/* account for any runs off the bottom */	
	for (dstx = 0; dstx < image->width; dstx++)
		summary += FACTOR(dstx, image->width) * vrun[dstx] * vrun[dstx];

	/* free memory and return the result */
	_TIFFfree(vrun);
	return summary;
}

typedef struct
{
	uint8 *visited;
	int max;
	int rowbytes;
	int failed;
	int xorigin, yorigin;
} object_bounds;

static void
get_object_max_dim_r(bilevel_image *image, int dy, int dx, object_bounds *bounds)
{
	int x, y;
	if (bounds->failed || dy < -bounds->max || dx < -bounds->max || dy > bounds->max || dx > bounds->max)
	{
		bounds->failed = 1;
		return;
	}
	if (bounds->visited[dy * bounds->rowbytes + dx])
		return;
	bounds->visited[dy * bounds->rowbytes + dx] = 1;
	x = bounds->xorigin + dx;
	y = bounds->yorigin + dy;
	if (get_pixel(image, y-1, x)) get_object_max_dim_r(image, dy-1, dx, bounds);
	if (get_pixel(image, y+1, x)) get_object_max_dim_r(image, dy+1, dx, bounds);
	if (get_pixel(image, y, x-1)) get_object_max_dim_r(image, dy, dx-1, bounds);
	if (get_pixel(image, y, x+1)) get_object_max_dim_r(image, dy, dx+1, bounds);
}

static int
get_object_max_dim(bilevel_image *image, int y, int x, int max1, int max2)
{
	object_bounds bounds;
	
	bounds.max = max1;
	bounds.rowbytes = 2 * (max1 + 1) + 1;
	bounds.failed = 0;
	bounds.xorigin = x;
	bounds.yorigin = y;
	bounds.visited = malloc(bounds.rowbytes * bounds.rowbytes);
	memset(bounds.visited, 0, bounds.rowbytes * bounds.rowbytes);

	bounds.visited += max1 * bounds.rowbytes + max1;
	get_object_max_dim_r(image, 0, 0, &bounds);
	bounds.visited -= max1 * bounds.rowbytes + max1;
	
	if (!bounds.failed)
	{
		int miny, maxy, minx, maxx, x, y;
		miny = minx = bounds.rowbytes;
		maxy = maxx = -1;
		for (y = 0; y < bounds.rowbytes; y++)
			for (x = 0; x < bounds.rowbytes; x++)
				if (bounds.visited[y * bounds.rowbytes + x])
				{
					if (y < miny) miny = y;
					if (y > maxy) maxy = y;
					if (x < minx) minx = x;
					if (x > maxx) maxx = x;
				}
		
		if (maxy - miny > maxx - minx)
			bounds.failed = (maxy - miny > max1 || maxx - minx > max2);
		else
			bounds.failed = (maxx - minx > max1 || maxy - miny > max2);
	}
	
	free(bounds.visited);
	
	return bounds.failed;
}

static void
erase_object(bilevel_image *image, int y, int x)
{
	clear_pixel(image, y, x);
	if (get_pixel(image, y-1, x)) erase_object(image, y-1, x);
	if (get_pixel(image, y+1, x)) erase_object(image, y+1, x);
	if (get_pixel(image, y, x-1)) erase_object(image, y, x-1);
	if (get_pixel(image, y, x+1)) erase_object(image, y, x+1);
}

static void
bilevel_image_clean(bilevel_image *image, char *status)
{
	int x, y;
	
	/* first despeckle */
	for (y = 0; y < image->length; y++)
	{
		sprintf(status, "Despeckle scanning (%d)...", y);
		for (x = 0; x < image->width; x++)
			if (get_pixel(image, y, x))
			{
				/* find extent of object; skip if any pixels to the left or above because
				   we already did them */
				if (get_pixel(image, y, x-1) || get_pixel(image, y-1, x))
					continue;
		sprintf(status, "Checking (%d,%d)...", y, x);
				if (!get_object_max_dim(image, y, x, 8, 3))
				{
		sprintf(status, "Erasing (%d,%d)...", y, x);
					erase_object(image, y, x);
				}
			}
	}
}

static bilevel_image *
bilevel_image_rotate(const bilevel_image *image, double angle)
{
	long long dxdx, dydx, dxdy, dydy;
	long long srcstartx, srcstarty;
	double sinval, cosval;
	bilevel_image *result;
	long dstx, dsty;
	
	/* convert angle to rotation matrix */
	sinval = sin(angle * M_PI / 180.0);
	cosval = cos(angle * M_PI / 180.0);
	dxdx = (long long)(cosval * (double)(1ll << 32));
	dydx = (long long)(-sinval * (double)(1ll << 32));
	dxdy = -dydx;
	dydy = dxdx;

	/* pick starting source x,y such that we remain centered */
	srcstartx = ((long long)(image->width / 2) * (double)(1ll << 32)) - dxdx * (image->width / 2) - dxdy * (image->length / 2);
	srcstarty = ((long long)(image->length / 2) * (double)(1ll << 32)) - dydx * (image->width / 2) - dydy * (image->length / 2);

	/* allocate memory for the destination image */
	result = bilevel_image_alloc(0, 0, image);
	if (result == NULL)
	{
		fprintf(stderr, "bilevel_image_rotate: Out of memory allocating bilevel %dx%d\n", image->width, image->length);
		return NULL;
	}
	
	/* iterate over the destination */
	for (dsty = 0; dsty < result->length; dsty++)
	{
		long long srcx = srcstartx + dsty * dxdy;
		long long srcy = srcstarty + dsty * dydy;
		
		/* iterate over destination rows */
		for (dstx = 0; dstx < result->width; dstx++)
		{
			/* if we're in range, count black pixel runs */
			if (get_pixel(image, srcy >> 32, srcx >> 32))
				set_pixel(result, dsty, dstx);
			
			/* advance source in both X and Y */
			srcx += dxdx;
			srcy += dydx;
		}
	}
	return result;
}

static DWORD WINAPI
bilevel_image_auto_rotate_worker(LPVOID param)
{
	rotate_worker_data *data = param;
	data->score = bilevel_image_rotate_score(data->image, data->angle);
	SetEvent(data->event);
	return 0;
}

static bilevel_image *
bilevel_image_auto_rotate(const bilevel_image *image, char *status)
{
	rotate_worker_data left;
	rotate_worker_data right;
	rotate_worker_data middle;
	rotate_worker_data leftmid;
	rotate_worker_data rightmid;
	HANDLE eventlist[5];
	int pass = 0;
	
	/* set up the workers */
	left.image = right.image = middle.image = leftmid.image = rightmid.image = image;
	eventlist[0] = left.event = CreateEvent(NULL, TRUE, FALSE, NULL);
	eventlist[1] = right.event = CreateEvent(NULL, TRUE, FALSE, NULL);
	eventlist[2] = middle.event = CreateEvent(NULL, TRUE, FALSE, NULL);
	eventlist[3] = leftmid.event = CreateEvent(NULL, TRUE, FALSE, NULL);
	eventlist[4] = rightmid.event = CreateEvent(NULL, TRUE, FALSE, NULL);

	/* scan from -10 .. 10 */	
	left.angle = -10.0;
	right.angle = 10.0;
	
	/* iterate until we've found the best match within 1/1000th of a degree */
	while (fabs(right.angle - left.angle) > 0.001)
	{
		int count = 0;
		
		pass++;
		sprintf(status, "Scanning for best angle.... %7.3f |%.*s%.*s|", middle.angle, pass, "================", 16-pass, "                ");

		/* on the first pass, we have to compute all 5 */
		if (pass == 1)
		{
			left.angle = -10.0;
			ResetEvent(left.event);
			QueueUserWorkItem(bilevel_image_auto_rotate_worker, &left, WT_EXECUTEDEFAULT);
	
			right.angle = 10.0;
			ResetEvent(right.event);
			QueueUserWorkItem(bilevel_image_auto_rotate_worker, &right, WT_EXECUTEDEFAULT);
	
			middle.angle = 10.0;
			ResetEvent(middle.event);
			QueueUserWorkItem(bilevel_image_auto_rotate_worker, &middle, WT_EXECUTEDEFAULT);
		}
	
		/* on all passes we compute the remaining 2 */
		leftmid.angle = (left.angle + middle.angle) / 2.0;
		ResetEvent(leftmid.event);
		QueueUserWorkItem(bilevel_image_auto_rotate_worker, &leftmid, WT_EXECUTEDEFAULT);

		rightmid.angle = (right.angle + middle.angle) / 2.0;
		ResetEvent(rightmid.event);
		QueueUserWorkItem(bilevel_image_auto_rotate_worker, &rightmid, WT_EXECUTEDEFAULT);
		
		/* wait for everyone to be done */
		WaitForMultipleObjects(5, eventlist, TRUE, INFINITE);

		/* if leftmid is our best candidate, make it the new middle */
		if (leftmid.score >= left.score && leftmid.score >= middle.score)
		{
			right.angle = middle.angle, right.score = middle.score;
			middle.angle = leftmid.angle, middle.score = leftmid.score;
		}

		/* if middle is our best candidate, it remains the middle */
		else if (middle.score >= leftmid.score && middle.score >= rightmid.score)
		{
			left.angle = leftmid.angle, left.score = leftmid.score;
			right.angle = rightmid.angle, right.score = rightmid.score;
		}

		/* if rightmid is our best candidate, make it the new middle */
		else if (rightmid.score >= right.score && rightmid.score >= middle.score)
		{
			left.angle = middle.angle, left.score = middle.score;
			middle.angle = rightmid.angle, middle.score = rightmid.score;
		}

		/* warn about non-convergence */
		else
		{
			sprintf(status, "Results not converging as expected: %lld - %lld - %lld - %lld - %lld", left.score, leftmid.score, middle.score, rightmid.score, right.score);
			left.angle = leftmid.angle, left.score = leftmid.score;
			right.angle = rightmid.angle, right.score = rightmid.score;
		}
	}
	
	/* free the events */
	CloseHandle(left.event);
	CloseHandle(right.event);
	CloseHandle(middle.event);
	CloseHandle(leftmid.event);
	CloseHandle(rightmid.event);

	return bilevel_image_rotate(image, middle.angle);
}

static bilevel_image *
bilevel_image_crop(const bilevel_image *image, int left, int top, uint32 width, uint32 length)
{
	bilevel_image *result;
	int shift = left & 7;
	uint32 x, y;

	/* allocate memory for the destination image */
	result = bilevel_image_alloc(width, length, image);
	if (result == NULL)
	{
		fprintf(stderr, "bilevel_image_crop: Out of memory allocating bilevel %dx%d\n", width, length);
		return NULL;
	}
	
	/* iterate over rows */
	for (y = 0; y < length; y++)
	{
		uint32 srcy = top + y;
		
		/* skip any rows that aren't within the source image bounds */
		if (srcy < image->length)
		{
			const uint8 *srcrow = image->pixels + srcy * image->rowbytes;
			uint8 *dstrow = result->pixels + y * result->rowbytes;
			uint8 lastpix = 0;
			uint32 srcx;
			
			/* grab the first partial pixels if we can */
			srcx = left >> 3;
			if (srcx < image->rowbytes)
				lastpix = srcrow[srcx];

			/* iterate over groups of 8 pixels */
			for (x = 0; x < width; x += 8)
			{
				uint8 newpix = 0;

				/* if we can fetch within the rowbytes, fetch a new batch of pixels */
				if (++srcx < image->rowbytes)
					newpix = srcrow[srcx];
				
				/* store the shifted values and shift */
				*dstrow++ = (lastpix << shift) | (newpix >> (8 - shift));
				lastpix = newpix;
			}
		}
	}
	
	return result;
}

static void
bilevel_image_compute_margins(const bilevel_image *image, uint32 *top, uint32 *left, uint32 *right, uint32 *bottom)
{
	uint32 x, y;
	
	/* trim the top */
	if (top != NULL)
		for (*top = 0; *top < image->length; *top += 1)
		{
			const uint8 *src = image->pixels + *top * image->rowbytes;
			uint32 pop = 0;

			for (x = 0; x < image->width; x += 8)
				pop += popcount[*src++];
			if (pop * 1000 > image->width)
				break;
		}

	/* trim the bottom */
	if (bottom != NULL)
		for (*bottom = 0; *bottom < image->length; *bottom += 1)
		{
			const uint8 *src = image->pixels + (image->length - 1 - *bottom) * image->rowbytes;
			uint32 pop = 0;

			for (x = 0; x < image->width; x += 8)
				pop += popcount[*src++];
			if (pop * 1000 > image->width)
				break;
		}
	
	/* trim the left */
	if (left != NULL)
		for (*left = 0; *left < image->width; *left += 1)
		{
			const uint8 *src = image->pixels + *left / 8;
			int shift = 7 - (*left % 8);
			uint32 pop = 0;
			
			for (y = 0; y < image->length; y++)
			{
				pop += (*src >> shift) & 1;
				src += image->rowbytes;
			}
			if (pop * 1000 > image->length)
				break;
		}

	/* trim the right */
	if (right != NULL)
		for (*right = 0; *right < image->width; *right += 1)
		{
			const uint8 *src = image->pixels + (image->width - 1 - *right) / 8;
			int shift = 7 - ((image->width - 1 - *right) % 8);
			uint32 pop = 0;
			
			for (y = 0; y < image->length; y++)
			{
				pop += (*src >> shift) & 1;
				src += image->rowbytes;
			}
			if (pop * 1000 > image->length)
				break;
		}
}

static int
build_worker_list(char *files[], int count)
{
	image_worker_data **workerlist_tailptr;
	int filenum;

	/* create workers for each image and queue them */
	workerlist = NULL;
	workerlist_tailptr = &workerlist;
	workercount = 0;
	for (filenum = 0; filenum < count; filenum++)
	{
		const char *name = files[filenum];
		int numimages;
		int index;
		TIFF *in;
	
		/* open source file */
		in = TIFFOpen(name, "ru");
		if (in == NULL)
		{
			fprintf(stderr, "%s: Unable to open file\n", name);
			return -1;
		}

		/* count images, then close the file */
		for (numimages = 1; TIFFReadDirectory(in) != 0; numimages++) ;
		TIFFClose(in);
		
		/* create workers for all images */
		for (index = 0; index < numimages; index++)
		{
			/* compute the full name */
			char *fullname = malloc(strlen(name) + 20);
			if (numimages > 1)
				sprintf(fullname, "%s(%d)", name, index);
			else
				strcpy(fullname, name);
			
			/* allocate a new item */
			*workerlist_tailptr = _TIFFmalloc(sizeof(*workerlist));
			memset(*workerlist_tailptr, 0, sizeof(*workerlist));

			/* set the filename and image index */
			(*workerlist_tailptr)->filename = name;
			(*workerlist_tailptr)->name = fullname;
			(*workerlist_tailptr)->index = index;
			strcpy((*workerlist_tailptr)->status, "Loading...");

			/* add to the list */
			workerlist_tailptr = &(*workerlist_tailptr)->next;
			workercount++;
		}
	}
	return 0;
}

static int
queue_and_wait_for_workers(LPTHREAD_START_ROUTINE callback, int move_cursor_back)
{
	CONSOLE_SCREEN_BUFFER_INFO bufferinfo;
	image_worker_data *worker;
	int alldone;

	/* queue all the items with the given callback */	
	for (worker = workerlist; worker != NULL; worker = worker->next)
	{
		worker->done = worker->error = FALSE;
		QueueUserWorkItem(callback, worker, WT_EXECUTEDEFAULT);
	}

	/* update the status periodically */
	GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &bufferinfo);
	if (move_cursor_back)
		bufferinfo.dwCursorPosition.Y -= workercount;

	/* loop until done */	
	alldone = FALSE;
	while (!alldone)
	{
		image_worker_data *worker;
		int errorcount = 0;
		
		/* every half second or so */
		Sleep(500);
		
		/* update status for all workers, and see if we're done or hit an error */
		alldone = TRUE;
		SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), bufferinfo.dwCursorPosition);
		for (worker = workerlist; worker != NULL; worker = worker->next)
		{
			char namebuf[30];
			if (!worker->done)
				alldone = FALSE;
			if (worker->error)
				errorcount++;
			if (strlen(worker->name) > 20)
			{
				memcpy(namebuf, worker->name, 8);
				memcpy(namebuf + 8, "...", 3);
				memcpy(namebuf + 11, worker->name + strlen(worker->name) - 9, 9);
				namebuf[20] = 0;
			}
			else
				strcpy(namebuf, worker->name);
			printf("%20.20s: %-59.59s\n", namebuf, worker->status);
		}
		
		/* on error, just bail */
		if (errorcount != 0)
			return -1;

		/* move back to our previous cursor position */
		GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &bufferinfo);
		bufferinfo.dwCursorPosition.Y -= workercount;
	}
	return 0;
}

static DWORD WINAPI
rotate_and_compute_margins(PVOID param)
{
	image_worker_data *data = param;
	bilevel_image *tempimage;
	
	/* set the thread id */
	data->threadid = GetCurrentThreadId();

	/* load the image */
	data->image = bilevel_image_load(data->filename, data->index);
	if (data->image == NULL)
	{
		data->error = TRUE;
		goto done;
	}

	/* clean the image */
	if (cleanit)
		bilevel_image_clean(data->image, data->status);
	
	/* rotate the image */
	if (!norotate)
	{
		tempimage = bilevel_image_auto_rotate(data->image, data->status);
		bilevel_image_free(data->image);
		data->image = tempimage;
	}
	
	/* compute the margins if cropping */
	if (cropwidth != 0 && croplength != 0)
	{
		strcpy(data->status, "Computing Margins...");
		bilevel_image_compute_margins(data->image, &data->top, &data->left, &data->right, &data->bottom);
	}
	strcpy(data->status, "Waiting...");

done:
	data->threadid = -1;
	data->done = TRUE;
	return 0;
}

static int __cdecl
uint32_compare(const void *item1, const void *item2)
{
	uint32 a = *(uint32 *)item1;
	uint32 b = *(uint32 *)item2;
	return (a < b) ? -1 : (a > b) ? 1 : 0;
}

static void
compute_median_size(void)
{
	uint32 *wlist = _TIFFmalloc(workercount * sizeof(uint32));
	uint32 *hlist = _TIFFmalloc(workercount * sizeof(uint32));
	image_worker_data *worker;
	int index;
	
	/* sort all images and find the median height and width */
	index = 0;
	for (worker = workerlist; worker != NULL; worker = worker->next)
	{
		hlist[index] = worker->image->length - worker->top - worker->bottom;
		wlist[index] = worker->image->width - worker->left - worker->right;
		index++;
	}
	qsort(hlist, workercount, sizeof(*hlist), uint32_compare);
	qsort(wlist, workercount, sizeof(*wlist), uint32_compare);
	median_length = hlist[workercount / 2];
	median_width = wlist[workercount / 2];
	
	/* free memory */
	_TIFFfree(wlist);
	_TIFFfree(hlist);
}

static DWORD WINAPI
crop_image(PVOID param)
{
	image_worker_data *data = param;
	uint32 trimlength, trimwidth;
	bilevel_image *tempimage;
	int top, left;
	
	/* set the thread id */
	data->threadid = GetCurrentThreadId();

	/* determine how much to trim from the image */
	trimlength = data->image->length - data->top - data->bottom;
	trimwidth = data->image->width - data->left - data->right;
	
	/* if we're smaller than the median, use the median values */
	if (trimlength < median_length)
		trimlength = median_length;
	if (trimwidth < median_width)
		trimwidth = median_width;

	/* crop it */
	strcpy(data->status, "Cropping...");
	top = data->top - (int)(croplength - trimlength) / 2;
	left = data->left - (int)(cropwidth - trimwidth) / 2;
	
	tempimage = bilevel_image_crop(data->image, left, top, cropwidth, croplength);
	bilevel_image_free(data->image);
	data->image = tempimage;
	strcpy(data->status, "Done");

done:
	data->threadid = -1;
	data->done = TRUE;
	return 0;
}

static void
normalize_resolutions(void)
{
	image_worker_data *worker;
	float xres = 0;
	float yres = 0;

	/* find the maximum resolution */
	for (worker = workerlist; worker != NULL; worker = worker->next)
	{
		if (worker->image->xres >= xres)
			xres = worker->image->xres;
		if (worker->image->yres >= yres)
			yres = worker->image->yres;
	}
	
	/* then set that value for everyone */
	for (worker = workerlist; worker != NULL; worker = worker->next)
	{
		worker->image->xres = xres;
		worker->image->yres = yres;
	}
}

int
main(int argc, char* argv[])
{
	extern int optind;
	extern char *optarg;
	
	int c, argnum;
	char *xptr;

	InitializeCriticalSection(&critsect);

	/* parse arguments */
	while ((c = getopt(argc, argv, "lrc:")) != -1)
	{
		switch (c)
		{
			case 'c':
				xptr = strchr(optarg, 'x');
				if (xptr == NULL) usage();
				*xptr = 0;
				croplength = atoi(optarg);
				cropwidth = atoi(xptr + 1);
				printf("Cropping to %dx%d\n", croplength, cropwidth);
				break;
			
			case 'l':
				cleanit = 1;
				break;

			case 'r':
				norotate = 1;
				break;

			case '?':
				usage();
				break;
		}
	}
	
	/* make sure we have at least 2 left */
	if (argc - optind < 2)
		usage();
	
	/* set our handlers */
	TIFFSetErrorHandler(error_handler);
	TIFFSetErrorHandlerExt(error_handler_ext);
	TIFFSetWarningHandler(warning_handler);
	TIFFSetWarningHandlerExt(warning_handler_ext);
	
	/* build our list of images as work items */
	if (build_worker_list(&argv[optind], argc - 1 - optind) != 0)
		return -1;

	/* rotate each image and compute the inner margins if cropping */
	if (queue_and_wait_for_workers(rotate_and_compute_margins, FALSE) != 0)
		return -1;
	
	/* perform the final crop */
	if (cropwidth != 0 && croplength != 0)
	{
		compute_median_size();
		if (queue_and_wait_for_workers(crop_image, TRUE))
			return -1;
	}
	
	/* normalize the resolution of all images */
	normalize_resolutions();

	/* save the result */
	printf("Writing final image\n");
	if (bilevel_image_save_images(argv[argc - 1], workerlist))
		return -1;

	return (0);
}


char* stuff[] = {
"usage: tiffrotate [options] input.tif [input2.tif [...]] output.tif",
"where options are:",
" -c heightxwidth   auto-crop to the given size",
" -l                clean the TIFF",
" -r                do not attempt to rotate",
NULL
};

static void
usage(void)
{
	char buf[BUFSIZ];
	int i;

	setbuf(stderr, buf);
        fprintf(stderr, "%s\n\n", TIFFGetVersion());
	for (i = 0; stuff[i] != NULL; i++)
		fprintf(stderr, "%s\n", stuff[i]);
	exit(-1);
}
