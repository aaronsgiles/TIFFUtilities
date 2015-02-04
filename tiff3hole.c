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

typedef struct image_worker_data image_worker_data;
struct image_worker_data
{
	image_worker_data *next;
	const char *filename;
	const char *name;
	int			index;
	int			overall_index;
	int			crop_top;
	int			crop_bottom;
	int			crop_left;
	int			crop_right;
	DWORD		threadid;
	bilevel_image *image;
	volatile uint32 done;
	volatile uint32 error;
	bilevel_image *target;
	int			lefthalf;
	char 		status[100];
};

static CRITICAL_SECTION critsect;
static HANDLE event;

static image_worker_data *workerlist = NULL;
static int workercount = 0;

static float target_resolution = 600;
static uint32 targetwidth = 0;
static uint32 targetlength = 0;

static int single_sided = 0;

static uint32 crop_top = ~0;
static uint32 crop_bottom = ~0;
static uint32 crop_left = ~0;
static uint32 crop_right = ~0;
static int load_count;

static int pagecount;
static bilevel_image **finalpage;

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

static bilevel_image *
bilevel_image_alloc(uint32 width, uint32 length, const bilevel_image *clonefrom)
{
	uint16 rowbytes = (width + 7) / 8;
	bilevel_image *image;
	
	/* borrow width/length from clone if not specified */
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

static bilevel_image *
bilevel_image_load(image_worker_data *data)
{
	uint32 top, left, bottom, right;
	uint32 x, y, width, length;
	bilevel_image *image = NULL;
	uint32 *rgba = NULL;
	TIFF *in = NULL;
	uint32 *src;
	
	/* open source image */
	in = TIFFOpen(data->filename, "ru");
	if (in == NULL)
	{
		/* error message is stashed by the TIFF error handler */
		goto error;
	}
	
	/* set the directory */
	if (TIFFSetDirectory(in, data->index) == 0)
	{
		fprintf(stderr, "%s: Unable to select image %d\n", data->index);
		goto error;
	}

	/* read image width */
	width = 0;
	TIFFGetField(in, TIFFTAG_IMAGEWIDTH, &width);
	if (width == 0)
	{
		fprintf(stderr, "%s: Unexpected or missing image width\n", data->filename);
		goto error;
	}

	/* read image length */	
	length = 0;
	TIFFGetField(in, TIFFTAG_IMAGELENGTH, &length);
	if (length == 0)
	{
		fprintf(stderr, "%s: Unexpected or missing image length\n", data->filename);
		goto error;
	}

	/* allocate RGBA buffer */
EnterCriticalSection(&critsect);
	rgba = _TIFFmalloc(width * length * 4);
	if (rgba == NULL)
	{
		fprintf(stderr, "%s: Out of memory allocating RGBA %dx%d\n", data->filename, width, length);
		goto error;
	}

	/* read as RGBA */
	if (TIFFReadRGBAImageOriented(in, width, length, rgba, ORIENTATION_TOPLEFT, 1) == 0)
	{
		fprintf(stderr, "%s: Error reading image\n", data->filename);
		goto error;
	}
	
	/* allocate bilevel_image struct */
	image = bilevel_image_alloc(width, length, NULL);
	if (image == NULL)
	{
		fprintf(stderr, "%s: Out of memory allocating bilevel %dx%d\n", data->filename, width, length);
		goto error;
	}
	image->name = data->filename;
	
	/* fill in the info */
	TIFFGetField(in, TIFFTAG_ORIENTATION, &image->orientation);
	TIFFGetField(in, TIFFTAG_XRESOLUTION, &image->xres);
	TIFFGetField(in, TIFFTAG_YRESOLUTION, &image->yres);
	TIFFGetField(in, TIFFTAG_RESOLUTIONUNIT, &image->resunit);
	
	/* read the image, converting to bilevel along the way */
	src = rgba;
	for (y = 0; y < length; y++)
	{
		uint8 *dst = image->pixels + y * image->rowbytes;
	
		/* downconvert 8bpp samples down to single bit */
		for (x = 0; x < width; x++)
		{
			uint32 pix = *src++;
			int bright = (TIFFGetR(pix) * 4 + TIFFGetG(pix) * 5 + TIFFGetB(pix) * 1);
			if (bright <= 0x40 * 10)
				dst[x / 8] |= 0x80 >> (x % 8);
		}
	}

	/* determine margins and update the globals, skipping first/last pages */
	bilevel_image_compute_margins(image, &data->crop_top, &data->crop_left, &data->crop_right, &data->crop_bottom);
	if ((data->overall_index != 0 || workercount == 1) && (data->overall_index != workercount - 1 || workercount <= 2))
	{
		if (crop_top == ~0 || data->crop_top < crop_top) crop_top = data->crop_top;
		if (crop_left == ~0 || data->crop_left < crop_left) crop_left = data->crop_left;
		if (crop_right == ~0 || data->crop_right < crop_right) crop_right = data->crop_right;
		if (crop_bottom == ~0 || data->crop_bottom < crop_bottom) crop_bottom = data->crop_bottom;
	}
	if (++load_count == workercount)
		SetEvent(event);

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
bilevel_image_save_images(const char *name)
{
	TIFF *out;
	uint32 y;
	int page;
	
	out = TIFFOpen(name, "w");
	if (out == NULL)
		return -1;

	for (page = 0; page < pagecount; page++)
	{
		bilevel_image *image = finalpage[page];

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

static void
bilevel_image_scale_to_target(const bilevel_image *srcimage, bilevel_image *target, uint32 targetleft, uint32 targettop, uint32 targetwidth, uint32 targetlength, int flip)
{
	uint32 scaledwidth, scaledlength;
	long long stepx, stepy;
	long long srcx, srcy;
	uint32 x, y;
	
	/* determine where the original image will go */
	scaledwidth = srcimage->width * targetlength / srcimage->length;
	scaledlength = srcimage->length * targetwidth / srcimage->width;
	if (scaledwidth <= targetwidth)
		scaledlength = targetlength;
	else
		scaledwidth = targetwidth;
	
	/* center */
	targetleft += (targetwidth - scaledwidth) / 2;
	targettop += (targetlength - scaledlength) / 2;
	
	/* compute the x step */
	stepx = ((long long)srcimage->width << 32) / scaledwidth;
	stepy = ((long long)srcimage->length << 32) / scaledlength;
	
	/* iterate over rows */
	srcy = stepy / 2;
	for (y = 0; y < scaledlength; y++)
	{
		uint32 cury = srcy >> 32;
		const uint8 *srcrow;
		uint8 *dstrow = target->pixels + (targettop + y) * target->rowbytes;

		if (flip) cury = srcimage->length - 1 - cury;
		srcrow = srcimage->pixels + cury * srcimage->rowbytes;
		
		/* iterate over columns */
		srcx = stepx / 2;
		for (x = 0; x < scaledwidth; x++)
		{
			uint32 curx = srcx >> 32;
			if (flip) curx = srcimage->width - 1 - curx;
			srcx += stepx;
			if (srcrow[curx >> 3] & (0x80 >> (curx & 7)))
				dstrow[(targetleft + x) >> 3] |= 0x80 >> ((targetleft + x) & 7);
		}
		
		/* advance in Y */
		srcy += stepy;
	}
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
			(*workerlist_tailptr)->overall_index = workercount;
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
scale_image(PVOID param)
{
	image_worker_data *data = param;
	uint32 width = data->target->width;
	uint32 length = data->target->length;
	bilevel_image *original;
	uint32 xoffs, yoffs;
	int left, right, top, bottom;
	
	/* set the thread id */
	data->threadid = GetCurrentThreadId();

	/* load the image */
	data->image = bilevel_image_load(data);
	if (data->image == NULL)
	{
		data->error = TRUE;
		goto done;
	}
	
	/* block until the cropping is settled */
	strcpy(data->status, "Waiting...");
	WaitForSingleObject(event, INFINITE);
	
	/* crop our image */
	strcpy(data->status, "Cropping...");
	original = data->image;
	left = (data->crop_left < crop_left) ? data->crop_left : crop_left;
	right = (data->crop_right < crop_right) ? data->crop_right : crop_right;
	top = (data->crop_top < crop_top) ? data->crop_top : crop_top;
	bottom = (data->crop_bottom < crop_bottom) ? data->crop_bottom : crop_bottom;
	data->image = bilevel_image_crop(original, left, top, data->image->width - right - left, data->image->length - bottom - top);
	bilevel_image_free(original);
	
	/* scale to the target - assume 8.5 x 11 input, put 0.3" inch margins on all sides
	   except for 3-hole side, which gets 0.6" inch */
	strcpy(data->status, "Scaling...");
	xoffs = width * (data->lefthalf ? 30 : 60) / 850;
	yoffs = length * 30 / 1100;
	bilevel_image_scale_to_target(data->image, data->target, xoffs, yoffs, width * 760 / 850, length * 1040 / 1100, 0);
	strcpy(data->status, "Done");

done:
	data->threadid = -1;
	data->done = TRUE;
	return 0;
}

static void
allocate_and_assign_pages(void)
{
	int imagecount = workercount;
	image_worker_data *worker;
	int page, isleft;
	
	/* allocate the page list */
	pagecount = imagecount;
	finalpage = _TIFFmalloc(sizeof(bilevel_image *) * pagecount);
	memset(finalpage, 0, sizeof(bilevel_image *) * pagecount);

	/* allocate the pages */	
	for (page = 0; page < pagecount; page++)
	{
		finalpage[page] = bilevel_image_alloc(targetwidth, targetlength, NULL);
		finalpage[page]->orientation = ORIENTATION_TOPLEFT;
		finalpage[page]->xres = target_resolution;
		finalpage[page]->yres = target_resolution;
		finalpage[page]->resunit = RESUNIT_INCH;
	}
	
	/* assign the pages */
	page = 0;
	isleft = 0;
	for (worker = workerlist; worker != NULL; worker = worker->next)
	{
		worker->target = finalpage[page];
		worker->lefthalf = single_sided ? 0 : (page & 1);
		page++;
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
	event = CreateEvent(NULL, TRUE, FALSE, NULL);

	/* parse arguments */
	while ((c = getopt(argc, argv, "sr:")) != -1)
	{
		switch (c)
		{
			case 'r':
				target_resolution = atof(optarg);
				printf("Target resolution is %f dpi\n", target_resolution);
				break;
			
			case 's':
				single_sided = 1;
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
	
	/* compute target size */
	targetwidth = target_resolution * 8.5;
	targetlength = target_resolution * 11.0;
	
	/* build our list of images as work items */
	if (build_worker_list(&argv[optind], argc - 1 - optind) != 0)
		return -1;

	/* allocate final images and assign pages */
	allocate_and_assign_pages();

	/* rotate each image and compute the inner margins if cropping */
	if (queue_and_wait_for_workers(scale_image, FALSE) != 0)
		return -1;
	
	/* save the result */
	printf("Writing final image\n");
	if (bilevel_image_save_images(argv[argc - 1]))
		return -1;

	return (0);
}


char* stuff[] = {
"usage: tiffbook [options] input.tif [input2.tif [...]] output.tif",
"where options are:",
" -r dpi	output resolution in dpi",
" -s        assume single-sided",
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
