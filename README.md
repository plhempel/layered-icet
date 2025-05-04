> *****************************************************************************
>
> README and installation instructions for Layered-IceT. This is a fork of IceT
> and includes the copyright notice from the original IceT library below.
>
> Author: Kenneth Moreland (kmorel@sandia.gov)
>
> Copyright 2003 Sandia Coporation
>
> Under the terms of Contract DE-AC04-94AL85000 with Sandia Corporation,
> the U.S. Government retains certain rights in this software.
>
> This source code is released under the [New BSD License][bsd].
>
> *****************************************************************************

# Layered-IceT

The Image Composition Engine for Tiles (IceT) is a high-performance sort-last
parallel rendering library. This fork of IceT extends the original library to support 
parallel compositing of layered images, enabling parallel rendering of non-convex
domain decompositions of volumetric data, while ensuring full backward-compatibility 
with the original IceT library. Please see our [publication][layered-icet-paper]. 

The original IceT library is available at https://gitlab.kitware.com/icet/icet.


## Build Instructions

Welcome to the IceT build process.  IceT uses [CMake][cmake] to automatically
tailor itself to your system, so compiling should be relatively painless.
Before building IceT you will need to install CMake on your system.  You
can get CMake from [here][cmake-download].

Once CMake is installed and the IceT source is extracted, run the
interactive CMake configuration tool.  On UNIX, run `ccmake`.  On Win32, run
the CMake program on the desktop or in the start menu.  Note that when
using the interactive configuration tool, you will need to *configure*
several times before you can generate the build files.  This is because as
more information is retrieved, further options are revealed.

After CMake generates build files, compile the applications as applicable
for your system.

Example commands:
```sh
# Create an out-of-source build tree that will not be tracked by git.
cmake -B build
# Interactively configure the build.
ccmake build
# Compile and link.
cmake --build build -j $(($(nproc) * 2))
# Optionally, install IceT on your system.
# The destination can be set via the CMake option CMAKE_INSTALL_PREFIX.
cmake --install build -j $(($(nproc) * 2))
```


## Compositing Layered Images

This fork of IceT has been extended to support layered images with more than
one fragment (i.e. color and depth) per pixel.  Layered images are useful for
applications that require ordered compositing, but cannot guarantee a total
visibility ordering of all processes.  One such use case is direct volume
rendering of non-convex domain decompositions.  For more information, see our
[publication][layered-icet-paper].

Currently, layered compositing is only implemented for pre-rendered images
via the function `icetCompositeImageLayered`.  Compared to
`icetCompositeImage`, it requires an extra argument specifying the number of
fragments per pixel.  That number must be the same for all pixels in the
image, but may differ between processes.  Color and depth values must be
stored contiguously per pixel, ordered front to back.  Empty fragments with
an alpha value of zero must be ordered after any active fragments at that
pixel.  The individually sorted pixels are then given in the same scanline
order as for regular flat images.  Layered images must always contain depth
information.

To reduce network usage, the number of active fragments per pixel is stored
as an unsigned byte.  If you need more than 255 layers, you can change this
type through the CMake cache variable `ICET_LAYER_COUNT_T`.  Note that this
limit applies to the total number of fragments summed across all processes,
but does not include empty fragments.

For now, compositing of layered images is only supported by the *sequential*
strategy with single image strategies *bswap*, *bswap-folding* and *radix-k*.


## Citation
If you use Layered-IceT in your work, please cite our paper:
```
Paul Hempel, Aryaman Gupta, Ivo F. Sbalzarini, and Stefan Gumhold: 
"A Transparent and Efficient Extension of IceT for Parallel Compositing on Non-Convex Volume Domain Decompositions". 
Eurographics Symposium on Parallel Graphics and Visualization (EGPGV), 2025. (in print)
```

## License

IceT is released under the [New BSD License][bsd], see Copyright.txt.
Any contributions to IceT will also be considered to fall under this license,
and it is the responsibility of the authors to secure the necessary
permissions before contributing.

[bsd]: http://opensource.org/licenses/BSD-3-Clause
[cmake]: http://www.cmake.org/
[cmake-download]: http://www.cmake.org/download/
[layered-icet-paper]: tba
