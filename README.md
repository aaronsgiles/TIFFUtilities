TIFFUtilities
=============
Set of command line utilities for processing scanned TIFF files

This repository contains several useful command-line utilities for processing scanned TIFF images. They are all written for Windows and utilize thread pools to run optimally on multi-core systems.

Build Tools
===========
To build these tools, make sure you have gcc (aka [mingw](http://mingw-w64.sourceforge.net/)) in your PATH and run m.cmd to build. There is no formal makefile since these are so simple.

tiffalign
=========
```
tiffalign [options] input.tif [input2.tif [input3.tif [...]]]
```
This one is my primary workhorse. It takes a list of TIF files,
converts them down to 1-bit black & white, and attempts to rotate
them so they are straight. It can also optionally attempt to remove 
specks from the TIFF file.

The auto rotation algorithm is optimized for music files and looks
optimizes for long scanlines of black pixels (corresponding to the
staff lines). It tends to work for other uses as well, but 
occasionally will do something wonky. For music, though, it is very
solid.

tiffrotate
==========
```
tiffrotate [options] input.tif [input2.tif [...]] output.tif
```
This is a superset of tiffalign that can also perform a crop. I
originally used this all-in-one program, but these days I use
tiffalign to straighten individual files and then run tiffrotate
with the -r option to crop the final results and assemble them
into an output file.

tiff3hole
=========
```
tiff3hole [options] input.tif [input2.tif [...]] output.tif
```
This utility reads in one or more TIFF files and treats them as a
series of pages to be printed back-to-back on 8.5" x 11" paper. 
It scales the pages up or down to fit a standard letter size, and
then shifts alternating pages to allow room for a 3-hole punch.

tiffbook
========
```
tiffbook [options] input.tif [input2.tif [...]] output.tif
```
This utility is for taking a series of pages and spreading them
out onto larger pages 2x as wide in galley order so that they
can be printed and assembled with a saddle stitch. I don't use this
one much.

====================================================================
