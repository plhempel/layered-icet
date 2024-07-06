## Copyright 2003 Sandia Coporation
## Under the terms of Contract DE-AC04-94AL85000 with Sandia Corporation,
## the U.S. Government retains certain rights in this software.
##
## This source code is released under the New BSD License.
#

SET(ICET_SRCS
  porting.c
  context.c
  state.c
  diagnostics.c
  communication.c
  tiles.c
  timing.c
  matrix.c
  projections.c
  draw.c
  image.c

  ../strategies/common.c
  ../strategies/select.c
  ../strategies/direct.c
  ../strategies/sequential.c
  ../strategies/split.c
  ../strategies/reduce.c
  ../strategies/vtree.c
  ../strategies/bswap.c
  ../strategies/radixk.c
  ../strategies/radixkr.c
  ../strategies/tree.c
  ../strategies/automatic.c

  ../strategies/gpu/bswap.c
  )

SET(ICET_HEADERS
  ../include/IceT.h
  ../include/IceTDevCommunication.h
  ../include/IceTDevContext.h
  ../include/IceTDevDiagnostics.h
  ../include/IceTDevGLImage.h
  ../include/IceTDevImage.h
  ../include/IceTDevMatrix.h
  ../include/IceTDevPorting.h
  ../include/IceTDevProjections.h
  ../include/IceTDevState.h
  ../include/IceTDevStrategySelect.h
  ../include/IceTDevTiming.h
  )

SET(ICET_HEADERS_INTERNAL
  cc_composite_func_body.h
  cc_composite_template_body.h
  compress_func_body.h
  compress_template_body.h
  decompress_func_body.h
  decompress_template_body.h

  ../strategies/common.h
  )

SET(ICET_HEADERS_ALL ${ICET_HEADERS} ${ICET_HEADERS_INTERNAL})

ICET_ADD_LIBRARY(IceTCore ${ICET_SRCS} ${ICET_HEADERS_ALL})

SET_SOURCE_FILES_PROPERTIES(${ICET_HEADERS_ALL}
  PROPERTIES HEADER_FILE_ONLY TRUE
  )

IF (UNIX)
  # Depend on the math library under Unix.
  TARGET_LINK_LIBRARIES(IceTCore m)
ENDIF (UNIX)

IF(NOT ICET_INSTALL_NO_DEVELOPMENT)
  INSTALL(
    FILES ${ICET_HEADERS} ${ICET_BINARY_DIR}/src/include/IceTConfig.h
    DESTINATION ${ICET_INSTALL_INCLUDE_DIR}
    )
ENDIF(NOT ICET_INSTALL_NO_DEVELOPMENT)
