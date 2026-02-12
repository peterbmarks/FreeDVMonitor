cmake_minimum_required(VERSION 3.16)
project(FreeDVMonitor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Sources
set(SOURCES
    src/main.cpp
    src/app_window.cpp
)

add_executable(${PROJECT_NAME} ${SOURCES})

if(CMAKE_CROSSCOMPILING AND WIN32)
    # Cross-compiling for Windows: use GTK3 from deps/mingw64
    set(GTK3_PREFIX "${CMAKE_SOURCE_DIR}/deps/mingw64")

    # Suppress console window on Windows
    set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)

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
    )

    # Bundle DLLs on install
    install(TARGETS ${PROJECT_NAME} DESTINATION bin)
    file(GLOB GTK3_DLLS "${GTK3_PREFIX}/bin/*.dll")
    install(FILES ${GTK3_DLLS} DESTINATION bin)

    # Copy share directory (icons, schemas) for GTK runtime
    if(EXISTS "${GTK3_PREFIX}/share")
        install(DIRECTORY "${GTK3_PREFIX}/share/" DESTINATION share)
    endif()
else()
    # Native build: use system GTK3 via pkg-config
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 REQUIRED gtk+-3.0)

    target_include_directories(${PROJECT_NAME} PRIVATE ${GTK3_INCLUDE_DIRS})
    target_link_directories(${PROJECT_NAME} PRIVATE ${GTK3_LIBRARY_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${GTK3_LIBRARIES})
    target_compile_options(${PROJECT_NAME} PRIVATE ${GTK3_CFLAGS_OTHER})
endif()
