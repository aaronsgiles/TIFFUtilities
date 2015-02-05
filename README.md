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

License
=======
Copyright (c) 2015, Aaron Giles
All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
