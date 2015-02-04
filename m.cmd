gcc tiff3hole.c -g -fno-omit-frame-pointer -O3 -Ilibtiff -Wl,--large-address-aware libtiff3.dll -o tiff3hole.exe
gcc tiffalign.c -g -fno-omit-frame-pointer -O3 -Ilibtiff -Wl,--large-address-aware libtiff3.dll -o tiffalign.exe
gcc tiffrotate.c -g -fno-omit-frame-pointer -O3 -Ilibtiff -Wl,--large-address-aware libtiff3.dll -o tiffrotate.exe
gcc tiffbook.c -g -fno-omit-frame-pointer -O3 -Ilibtiff -Wl,--large-address-aware libtiff3.dll -o tiffbook.exe
