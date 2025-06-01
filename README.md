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
with the [original IceT library][upstream]. Please see our [publication][layered-icet-paper]. 

Additional tools for testing and benchmarking can be found at
https://github.com/plhempel/layered-icet-tools.


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

In traditional parallel rendering (as in the original IceT), each pixel in an 
image contains a single fragment: a color and, optionally, a depth value 
representing the nearest visible surface or the alpha-composited result along
the viewing ray. This works well when the domains held by the rendering processes
can be globally sorted by depth (i.e., convex decompositions).

Layered compositing extends this idea by allowing each pixel to hold multiple 
fragments (“layers”), each representing a different volume subdomain or other 
partially transparent object along the ray. This is crucial for
applications that require ordered compositing, but cannot guarantee a total
visibility ordering of all processes. One such use case is direct volume
rendering of non-convex domain decompositions.  For more information, see our
[publication][layered-icet-paper].

Layered-IceT supports parallel compositing of layered images produced by an 
external rendering application via the `icetCompositeImageLayered` function:

```c
IceTImage icetCompositeImageLayered(const IceTVoid *color_buffer,
                                    const IceTVoid *depth_buffer,
                                    IceTInt num_layers,
                                    const IceTInt *valid_pixels_viewport,
                                    const IceTDouble *projection_matrix,
                                    const IceTDouble *modelview_matrix,
                                    const IceTFloat *background_color)
```

The `color_buffer` and `depth_buffer` parameters contain the linearized color and
depth layered buffers for the current process. Fragments within a pixel must be 
stored contiguously in the linearized buffers. Non-empty fragments must be ordered
front to back, and placed before any empty fragments. Layered-IceT internally 
compresses the empty fragments within pixels for efficiency.

The individually sorted pixels are then given in the same scanline
order as for regular flat images.  Layered images must always contain depth
information.

The `num_layers` parameter specifies the number of fragments per pixel, which
must be the same for all pixels in the image. If a pixel produces fewer fragments,
the remaining fragments must be filled with empty values (i.e., zero alpha). The 
`num_layers` parameter may differ between processes.

The parameters `valid_pixels_viewport`, `projection_matrix`, `modelview_matrix` and
`background_color` are the same as for the original `icetCompositeImage` function.

The following diagram illustrates correct usage of the `icetCompositeImageLayered`
function with an example and highlights a few pitfalls to avoid:
![Diagram using a example to illustrate correct usage of `iceTCompositeImageLayered`,
highlighting correct ordering of empty and non-empty fragments and expected 
linearization order](API_usage_guide.svg)

To reduce network usage, the number of active fragments per pixel is stored
as an unsigned byte.  If you need more than 255 layers, you can change this
type through the CMake cache variable `ICET_LAYER_COUNT_T`.  Note that this
limit applies to the total number of fragments summed across all processes,
but does not include empty fragments.

Before the call to `icetCompositeImageLayered`, IceT must be configured correctly.
Compositing of layered images is currently only supported with the *sequential*
strategy,

```c
icetStrategy(ICET_STRATEGY_SEQUENTIAL);
```

and with the following single-image strategies: *bswap*, *bswap-folding* and *radix-k*.
For example, you can set the single-image strategy to *radix-k* with:

```c
icetSingleImageStrategy(ICET_SINGLE_IMAGE_STRATEGY_RADIXK);
```

Unlike 
Parallel compositing of layered images does not require a call to the `icetCompositeOrder` 
function. However, it always requires a depth buffer, so a valid depth format must be set:

```c
icetSetDepthFormat(ICET_IMAGE_DEPTH_FLOAT);
```

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
[layered-icet-paper]: https://diglib.eg.org/handle/10.2312/pgv20251151
[upstream]: https://gitlab.kitware.com/icet/icet
