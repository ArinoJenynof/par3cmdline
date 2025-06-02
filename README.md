# Notes
Bare minimum to compile par3cmdline with MSYS2 MinGW-w64 toolchain. According to original notes, code under `windows\` folder uses MSVC-specific code, but when i tried to compile with MinGW-w64 it compiles (and runs, repairing does work) anyway.

# Building
Download and setup [MSYS2](https://github.com/msys2/msys2-installer/releases/tag/nightly-x86_64) first,
```bash
pacman -Syuu --noconfirm
pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,make}
```
and compile this in UCRT64 environment
```bash
cd /path/to/par3cmdline/repo
mingw32-make
```
CMake & VS project is also available from upstream, check src/ folder for the `CMakeLists.txt`s and VS's `.sln`

### Build settings
1. `BLAKE3_USE_ASSEMBLY`: Use BLAKE3 assembly implementation. Use only on x86_64 machine running Windows/Unix and compiling with GCC (MSVC assembly do exists, but I haven't got time to setup MSVC)
2. `LEOPARD_USE_NATIVE_ARCH`: Compile Leopard-RS with GCC's `march=native` option.

**_original ReadMe below_**

# par3cmdline
Official repo for par3cmdline and par3lib

**As of January 29, 2022** The Par3 specification is in near-final form.  This repository will hold a reference implementation. After we have working code and worked out most of the bugs, the specification will be finalized.

The current version of the specification is available at: https://parchive.github.io/doc/Parity_Volume_Set_Specification_v3.0.html
