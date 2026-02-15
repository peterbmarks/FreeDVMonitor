# FreeDV Monitor NOT WORKING YET!

An application for Windows (and Linux) that can decode FreeDV RADE1 from a chosen audio input.

Receive only, designed for people who want to listen in before installing the full FreeDV-GUI app.

This code contains a C port of the python implementation [here](https://github.com/drowe67/radae)
by David Rowe, VK5DGR.

A GTK3 GUI application written in C++17 that cross-compiles to Windows from Linux using mingw-w64.

![Screenshot](images/FreeDV%20Monitor.PNG)

## Features

- Audio input device selection with persistent settings
- Start/Stop controls for the decoder
- Record button to capture raw audio samples to `recording.raw` (32-bit float at the device's native sample rate)
- Input gain slider (-20 to +20 dB)
- Real-time waterfall spectrum display
- Status bar showing sync status, SNR, and frequency offset

## Project Structure

```
FreeDVMonitor/
├── CMakeLists.txt                     # Build configuration (Linux + Windows)
├── LICENSE                            # BSD 2-Clause license
├── cmake/
│   ├── BuildOpus.cmake                # Downloads and builds Opus with FARGAN
│   └── mingw-w64-x86_64.cmake         # CMake toolchain file for cross-compilation
├── scripts/
│   └── setup-mingw-gtk.sh             # Downloads GTK3 Windows libraries from MSYS2
├── src/
│   ├── main.cpp                       # Application entry point
│   ├── app_window.h                   # Window struct definition
│   ├── app_window.cpp                 # Window layout and signal handlers
│   ├── rade_decoder.h                 # C++ decoder wrapper with PortAudio
│   ├── rade_decoder.cpp
│   ├── rade_api.h                     # C API for the RADE receiver
│   ├── rade_api.c
│   ├── rade_rx.h                      # Receiver state machine
│   ├── rade_rx.c
│   ├── rade_acq.h                     # Signal acquisition
│   ├── rade_acq.c
│   ├── rade_ofdm.h                    # OFDM modem
│   ├── rade_ofdm.c
│   ├── rade_dec.h                     # Neural decoder interface
│   ├── rade_dec.c
│   ├── rade_dec_data.c                # Neural network weights
│   ├── rade_dsp.h                     # DSP utilities (Hilbert, resampler)
│   ├── rade_dsp.c
│   ├── rade_bpf.h                     # Bandpass filter
│   ├── rade_bpf.c
│   ├── rade_constants.h               # Shared constants
│   ├── rade_core.h                    # Core type definitions
│   └── opus-nnet.h.diff               # Patch for Opus RADE integration
└── tests/
    └── test_loopback.c                # Loopback test for the C DSP stack
```

## Prerequisites

### Linux (native build)

```bash
sudo apt install build-essential cmake libgtk-3-dev portaudio19-dev
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

On both platforms, the Opus codec library is automatically downloaded and
built from source with FARGAN neural vocoder support.

## Running the Windows Executable

The build is self-contained: the MinGW C/C++ runtime is statically linked
into the executable, and all required GTK3 and PortAudio DLLs are
automatically copied into `build-win/` alongside the `.exe` as a post-build
step. No manual DLL gathering is needed.

To distribute, copy the entire `build-win/` directory to a Windows machine
(or use `cmake --install build-win --prefix <dest>` for a clean layout).

You can also test with Wine on Linux:

```bash
wine build-win/FreeDVMonitor.exe
```

## How It Works

### Signal processing chain

Audio captured via PortAudio at the device's native sample rate is resampled
to 8 kHz, then converted from a real signal to complex I/Q via a Hilbert FIR
filter. The RADE receiver performs OFDM demodulation, synchronisation, and
feature extraction. A neural decoder produces acoustic features which are fed
to the FARGAN vocoder (from the Opus library) to synthesise 16 kHz speech
output.

### Build system

CMakeLists.txt detects whether it is cross-compiling:

- **Native Linux** &mdash; finds GTK3 and PortAudio via `pkg-config`
  automatically.
- **Windows cross-compile** &mdash; uses hardcoded include/library paths
  pointing to the downloaded MSYS2 packages in `deps/mingw64/`. The
  toolchain file (`cmake/mingw-w64-x86_64.cmake`) tells CMake to use the
  mingw-w64 compiler and restricts library search paths to the cross-compile
  sysroot. The MinGW C/C++ runtime (`libgcc`, `libstdc++`, `libwinpthread`)
  is statically linked so those DLLs are not needed. GTK3 and its
  dependencies remain dynamically linked and the required DLLs are
  automatically copied next to the `.exe` after each build.

In both cases, the Opus library is built from source via `cmake/BuildOpus.cmake`
with `--enable-osce --enable-dred` to enable the FARGAN neural vocoder needed by
the RADE decoder.

### GTK3 Windows dependencies

`scripts/setup-mingw-gtk.sh` downloads ~22 MSYS2 packages (GTK3 and its
transitive dependencies: GLib, Cairo, Pango, GDK-Pixbuf, ATK, HarfBuzz,
FreeType, Fontconfig, libepoxy, etc.). Downloaded archives are cached in
`deps/cache/` so re-running the script is fast. To force a fresh download,
delete the `deps/` directory and run the script again.

Package versions are pinned at the top of the script. To update them, check
the latest versions at https://packages.msys2.org/ and edit the `PACKAGES`
array.

## Testing

A loopback test verifies the C DSP stack independently of the GUI and audio
subsystem:

```bash
cmake -B build-linux
cmake --build build-linux --target test_loopback
./build-linux/test_loopback
```

## Updating MSYS2 Package Versions

Edit the `PACKAGES` array in `scripts/setup-mingw-gtk.sh`. Each entry has the
format `name=version`. Find current versions at https://packages.msys2.org/.

After updating versions, delete `deps/mingw64/` (or just remove
`deps/mingw64/.setup-done`) and re-run the script.

## Source code

Published [here](https://github.com/peterbmarks/FreeDVMonitor)
