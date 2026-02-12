# FreeDV Monitor

A GTK3 GUI application written in C++17 that cross-compiles to Windows from Linux using mingw-w64.

## Project Structure

```
FreeDVMonitor/
├── CMakeLists.txt                     # Build configuration (Linux + Windows)
├── cmake/
│   └── mingw-w64-x86_64.cmake        # CMake toolchain file for cross-compilation
├── scripts/
│   └── setup-mingw-gtk.sh            # Downloads GTK3 Windows libraries from MSYS2
└── src/
    ├── main.cpp                       # Application entry point
    ├── app_window.h                   # Window struct definition
    └── app_window.cpp                 # Window layout and signal handlers
```

## Prerequisites

### Linux (native build)

```bash
sudo apt install build-essential cmake libgtk-3-dev
```

### Windows cross-compilation (from Linux)

```bash
sudo apt install mingw-w64 cmake curl zstd
```

## Building

### Linux

```bash
cmake -B build-linux
cmake --build build-linux
./build-linux/FreeDVMonitor
```

### Windows (cross-compiled from Linux)

1. Download the GTK3 Windows libraries (one-time, ~25 MB):

   ```bash
   ./scripts/setup-mingw-gtk.sh
   ```

   This fetches pre-built packages from the MSYS2 repository and extracts
   them into `deps/mingw64/`.

2. Configure and build:

   ```bash
   cmake -B build-win --toolchain cmake/mingw-w64-x86_64.cmake
   cmake --build build-win
   ```

   The output is `build-win/FreeDVMonitor.exe` (PE32+ GUI executable).

## Running the Windows Executable

Copy the following to the same directory as `FreeDVMonitor.exe`:

- All `.dll` files from `deps/mingw64/bin/`
- MinGW-w64 runtime DLLs from your system:
  - `libstdc++-6.dll`
  - `libgcc_s_seh-1.dll`
  - `libwinpthread-1.dll`

  These are typically found in `/usr/lib/gcc/x86_64-w64-mingw32/13-win32/`.

You can also test with Wine on Linux:

```bash
export WINEPATH="deps/mingw64/bin;/usr/lib/gcc/x86_64-w64-mingw32/13-win32"
wine build-win/FreeDVMonitor.exe
```

## How It Works

The application uses GTK3's C API called from C++17. This avoids the
cross-compilation complexity of gtkmm (GTK's C++ bindings) while still
allowing C++ features like `std::string`, lambdas, and `static_cast` in the
application code.

### Build system

CMakeLists.txt detects whether it is cross-compiling:

- **Native Linux** &mdash; finds GTK3 via `pkg-config` automatically.
- **Windows cross-compile** &mdash; uses hardcoded include/library paths
  pointing to the downloaded MSYS2 packages in `deps/mingw64/`. The
  toolchain file (`cmake/mingw-w64-x86_64.cmake`) tells CMake to use the
  mingw-w64 compiler and restricts library search paths to the cross-compile
  sysroot.

### GTK3 Windows dependencies

`scripts/setup-mingw-gtk.sh` downloads ~22 MSYS2 packages (GTK3 and its
transitive dependencies: GLib, Cairo, Pango, GDK-Pixbuf, ATK, HarfBuzz,
FreeType, Fontconfig, libepoxy, etc.). Downloaded archives are cached in
`deps/cache/` so re-running the script is fast. To force a fresh download,
delete the `deps/` directory and run the script again.

Package versions are pinned at the top of the script. To update them, check
the latest versions at https://packages.msys2.org/ and edit the `PACKAGES`
array.

## Updating MSYS2 Package Versions

Edit the `PACKAGES` array in `scripts/setup-mingw-gtk.sh`. Each entry has the
format `name=version`. Find current versions at https://packages.msys2.org/.

After updating versions, delete `deps/mingw64/` (or just remove
`deps/mingw64/.setup-done`) and re-run the script.
