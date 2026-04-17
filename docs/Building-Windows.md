# Building n2n for Windows

This document describes how to build n2n for Windows using CMake and MinGW.

## Prerequisites

### Option 1: MSYS2 (Recommended)

1. Download and install [MSYS2](https://www.msys2.org/)

2. Open MSYS2 UCRT64 or MINGW64 terminal and install dependencies:
```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-make
pacman -S mingw-w64-ucrt-x86_64-openssl mingw-w64-ucrt-x86_64-zlib mingw-w64-ucrt-x86_64-zstd
```

3. For wintun support, download wintun.dll from [WireGuard/wintun releases](https://github.com/WireGuard/wintun/releases)
   and place it in the `thirdparty/` directory as `wintun.dll`.

### Option 2: Cross-Compilation from Linux

1. Install MinGW-w64 toolchain:
```bash
# Debian/Ubuntu
sudo apt install mingw-w64 cmake

# Fedora
sudo dnf install mingw64-gcc-c++ mingw64-cmake mingw64-zlib mingw64-openssl mingw64-zstd
```

2. Download wintun.dll to the `thirdparty/` directory.

## Building with CMake

### Using MSYS2 Terminal

```bash
cd n2n
mkdir build && cd build
cmake -G "MinGW Makefiles" ..
cmake --build . --parallel
```

### Cross-Compilation from Linux

```bash
cd n2n
mkdir build && cd build
cmake -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake ..
cmake --build . --parallel
```

The executables will be created in `build/src/`:
- `edge.exe` - n2n edge client
- `supernode.exe` - n2n supernode server

## Installation

### Install to Custom Directory
```bash
cmake --install . --prefix /path/to/install
```

### Install to Default Location (requires Administrator)
```bash
cmake --install . --prefix "C:/Program Files/n2n"
```

## Wintun Driver Support

n2n supports WireGuard's wintun driver for improved performance on Windows.

### Enabling Wintun

Wintun support is enabled by default when building. To explicitly enable/disable:

```bash
cmake -G "MinGW Makefiles" -DN2N_ENABLE_WINTUN=ON ..
```

### Using Wintun at Runtime

Use the `-w` or `--use-wintun` option to prefer wintun over TAP-Win32:

```bash
edge.exe -d n2n_tun -w -c <community> -k <key> -l <supernode:port>
```

### Installing Wintun Driver

If wintun.dll is not present, n2n will attempt to use TAP-Win32 as fallback.
For best experience, install wintun driver from: https://www.wintun.net/

## Traditional Build (Autotools)

If you prefer the traditional autotools build:

```bash
./autogen.sh
./configure --host=x86_64-w64-mingw32
make
```

## Troubleshooting

### "wintun.dll not found" warning
- Ensure wintun.dll is in the same directory as edge.exe or in PATH
- n2n will fall back to TAP-Win32 automatically

### "wintun_open failed" error
- Check that you have administrator privileges
- Try running edge.exe as Administrator
- Ensure no other VPN is using the same network adapter

### CMake cannot find OpenSSL
```bash
pacman -S mingw-w64-ucrt-x86_64-openssl
```

### Build fails with linking errors
- Ensure all dependencies are installed for the correct MinGW environment
- Try cleaning the build: `rm -rf build && mkdir build`
