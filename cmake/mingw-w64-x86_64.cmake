set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MINGW_PREFIX x86_64-w64-mingw32)

set(CMAKE_C_COMPILER   ${MINGW_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${MINGW_PREFIX}-g++)
set(CMAKE_RC_COMPILER  ${MINGW_PREFIX}-windres)

# Search paths: prefer our downloaded deps, then the system mingw sysroot
set(GTK3_PREFIX "${CMAKE_SOURCE_DIR}/deps/mingw64")
set(CMAKE_FIND_ROOT_PATH ${GTK3_PREFIX} /usr/${MINGW_PREFIX})

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Ensure pkg-config looks in the right place if available
set(ENV{PKG_CONFIG_PATH} "${GTK3_PREFIX}/lib/pkgconfig")
set(ENV{PKG_CONFIG_LIBDIR} "${GTK3_PREFIX}/lib/pkgconfig")
