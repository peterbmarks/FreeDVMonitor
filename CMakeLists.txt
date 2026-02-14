cmake_minimum_required(VERSION 3.16)
project(FreeDVMonitor VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Opus (with FARGAN/LPCNet support) ────────────────────────────────────────
include(cmake/BuildOpus.cmake)

# Sources
set(SOURCES
    src/main.cpp
    src/app_window.cpp
    src/rade_decoder.cpp
    src/rade_api.c
    src/rade_rx.c
    src/rade_acq.c
    src/rade_bpf.c
    src/rade_dec.c
    src/rade_dec_data.c
    src/rade_dsp.c
    src/rade_ofdm.c
)

add_executable(${PROJECT_NAME} ${SOURCES})

# RADE C sources need Opus internal headers (config.h, os_support.h, etc.)
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# Link the static opus library (provides FARGAN, LPCNet)
target_link_libraries(${PROJECT_NAME} PRIVATE opus)
add_dependencies(${PROJECT_NAME} opus)

# We're building RADE API statically, not importing from a DLL
target_compile_definitions(${PROJECT_NAME} PRIVATE IS_BUILDING_RADE_API=1)

# Math library on Unix
if(UNIX)
    target_link_libraries(${PROJECT_NAME} PRIVATE m)
endif()

if(CMAKE_CROSSCOMPILING AND WIN32)
    # Cross-compiling for Windows: use GTK3 from deps/mingw64
    set(GTK3_PREFIX "${CMAKE_SOURCE_DIR}/deps/mingw64")

    # Suppress console window on Windows
    set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)

    # Statically link the MinGW C/C++ runtime so we don't need
    # libgcc_s_seh-1.dll, libstdc++-6.dll, or libwinpthread-1.dll
    target_link_options(${PROJECT_NAME} PRIVATE -static-libgcc -static-libstdc++ -Wl,-Bstatic -lpthread -Wl,-Bdynamic)

    target_include_directories(${PROJECT_NAME} PRIVATE
        ${GTK3_PREFIX}/include/gtk-3.0
        ${GTK3_PREFIX}/include/glib-2.0
        ${GTK3_PREFIX}/lib/glib-2.0/include
        ${GTK3_PREFIX}/include/pango-1.0
        ${GTK3_PREFIX}/include/cairo
        ${GTK3_PREFIX}/include/gdk-pixbuf-2.0
        ${GTK3_PREFIX}/include/atk-1.0
        ${GTK3_PREFIX}/include/harfbuzz
        ${GTK3_PREFIX}/include/fribidi
        ${GTK3_PREFIX}/include/freetype2
        ${GTK3_PREFIX}/include/libpng16
        ${GTK3_PREFIX}/include/pixman-1
        ${GTK3_PREFIX}/include
    )

    target_link_directories(${PROJECT_NAME} PRIVATE ${GTK3_PREFIX}/lib)

    target_link_libraries(${PROJECT_NAME} PRIVATE
        gtk-3 gdk-3 glib-2.0 gobject-2.0 gio-2.0
        pango-1.0 pangocairo-1.0 pangoft2-1.0 pangowin32-1.0
        cairo gdk_pixbuf-2.0 atk-1.0
        epoxy harfbuzz fribidi fontconfig freetype
        intl z png16 pixman-1
        portaudio
    )

    # GTK3 DLLs needed at runtime
    set(GTK3_RUNTIME_DLLS
        libatk-1.0-0.dll
        libbrotlicommon.dll
        libbrotlidec.dll
        libbz2-1.dll
        libcairo-2.dll
        libcairo-gobject-2.dll
        libepoxy-0.dll
        libexpat-1.dll
        libffi-8.dll
        libfontconfig-1.dll
        libfreetype-6.dll
        libfribidi-0.dll
        libgdk-3-0.dll
        libgdk_pixbuf-2.0-0.dll
        libgio-2.0-0.dll
        libglib-2.0-0.dll
        libgmodule-2.0-0.dll
        libgobject-2.0-0.dll
        libgraphite2.dll
        libgtk-3-0.dll
        libharfbuzz-0.dll
        libintl-8.dll
        libjpeg-8.dll
        libdatrie-1.dll
        libthai-0.dll
        libpango-1.0-0.dll
        libpangocairo-1.0-0.dll
        libpangoft2-1.0-0.dll
        libpangowin32-1.0-0.dll
        libpcre2-8-0.dll
        libpixman-1-0.dll
        libpng16-16.dll
        libtiff-6.dll
        libdeflate.dll
        libjbig-0.dll
        libLerc.dll
        liblzma-5.dll
        libsharpyuv-0.dll
        libwebp-7.dll
        libzstd.dll
        zlib1.dll
        libiconv-2.dll
        libgcc_s_seh-1.dll
        libstdc++-6.dll
        libwinpthread-1.dll
        libportaudio.dll
    )

    # Copy required DLLs next to the exe after every build
    foreach(DLL ${GTK3_RUNTIME_DLLS})
        set(DLL_SRC "${GTK3_PREFIX}/bin/${DLL}")
        if(EXISTS "${DLL_SRC}")
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${DLL_SRC}" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${DLL}"
                COMMENT "Copying ${DLL}"
            )
        else()
            message(WARNING "DLL not found: ${DLL_SRC}")
        endif()
    endforeach()

    # Install target for packaging
    install(TARGETS ${PROJECT_NAME} DESTINATION bin)
    foreach(DLL ${GTK3_RUNTIME_DLLS})
        set(DLL_SRC "${GTK3_PREFIX}/bin/${DLL}")
        if(EXISTS "${DLL_SRC}")
            install(FILES "${DLL_SRC}" DESTINATION bin)
        endif()
    endforeach()

    if(EXISTS "${GTK3_PREFIX}/share")
        install(DIRECTORY "${GTK3_PREFIX}/share/" DESTINATION share)
    endif()
else()
    # Native build: use system GTK3 via pkg-config
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
    pkg_check_modules(PORTAUDIO REQUIRED portaudio-2.0)

    target_include_directories(${PROJECT_NAME} PRIVATE
        ${GTK3_INCLUDE_DIRS} ${PORTAUDIO_INCLUDE_DIRS})
    target_link_directories(${PROJECT_NAME} PRIVATE
        ${GTK3_LIBRARY_DIRS} ${PORTAUDIO_LIBRARY_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${GTK3_LIBRARIES} ${PORTAUDIO_LIBRARIES})
    target_compile_options(${PROJECT_NAME} PRIVATE
        ${GTK3_CFLAGS_OTHER} ${PORTAUDIO_CFLAGS_OTHER})
endif()
