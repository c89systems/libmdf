# Installation and linking

Archives are split by purpose.

Source archives are named:

```text
libmdf-<version>.tar.gz
```

They contain the release source tree for rebuilding the C library, CLI, tests,
Lua bindings, and release artifacts.

Library SDK archives are named:

```text
libmdf-<version>-<target>.tar.gz
```

They contain:

```text
include/libmdf.h
include/libmdf/mdf.h
lib/libmdf.a
lib/libmdf.so* or lib/libmdf.dylib*
lib/cmake/libmdf/
lib/pkgconfig/libmdf.pc
share/libmdf/package-metadata.txt
share/doc/libmdf/LICENSE
share/doc/libmdf/README.md
share/doc/libmdf/OFL.txt
share/doc/libmdf/docs/*.md
```

CLI archives are named:

```text
cmdf-<version>-<target>.tar.gz
```

They contain:

```text
bin/cmdf
bin/cmdf.lua
share/doc/cmdf/LICENSE
share/doc/cmdf/README.md
share/doc/cmdf/OFL.txt
share/doc/cmdf/docs/*.md
```

Both CLI archives and library SDK archives include JetBrains Mono and its
`OFL.txt` attribution. The font is embedded by libmdf for self-contained HTML
and deck output.

Lua release artifacts are named:

```text
libmdf-lua-<version>.tar.gz
libmdf-<version>-1.rockspec
libmdf-<version>-1.src.rock
```

They provide the Lua 5.5 facade and native binding package source. The source
rock is built against an extracted SDK archive during verification.

Prebuilt targets:

```text
x86_64-linux-gnu
x86_64-linux-musl
aarch64-linux-gnu
aarch64-linux-musl
armhf-linux-gnu
armhf-linux-musl
arm64-apple-darwin
```

Linux `cmdf` binaries are static. The library SDK archives ship both static
and shared libraries.

## CMake

After extracting an SDK archive, point CMake at the archive prefix. On Linux,
the consuming project must use its pinned Bootlin CMake toolchain. The SDK does
not export a compiler, ELF interpreter, RPATH, or cache path. Apply the
package's development-runtime helper to every local executable; it selects the
same direct Bootlin runtime policy as libmdf's own examples and tests.

```cmake
find_package(libmdf CONFIG REQUIRED)

add_executable(app app.c)
target_link_libraries(app PRIVATE libmdf::mdf_static)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  libmdf_configure_development_runtime(app)
endif()
```

For example, a native Linux consumer configures with its own lifecycle-owned
Bootlin toolchain and the extracted SDK prefix:

```sh
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/your-project/cmake/toolchains/x86_64-linux-gnu.cmake \
  -Dlibmdf_DIR=/path/to/libmdf-sdk/lib/cmake/libmdf
cmake --build build
```

Set `libmdf_DIR` directly: the Bootlin toolchain intentionally restricts
package lookup to its sysroot, so `CMAKE_PREFIX_PATH` alone cannot find an SDK
prefix outside that sysroot.

The consuming toolchain must set `LIBMDF_BOOTLIN_LOADER` and
`LIBMDF_BOOTLIN_RUNTIME_RPATH`; `libmdf_configure_development_runtime` rejects
a Linux consumer that has not done so. Do not copy a path from libmdf's pinned
collection into a consumer or release artifact.

The package exports `libmdf::mdf_static` and `libmdf::mdf_shared` when both
library variants are present.

When building from source, the main CMake options are:

```text
LIBMDF_BUILD_STATIC=ON|OFF
LIBMDF_BUILD_SHARED=ON|OFF
LIBMDF_BUILD_BINARY=ON|OFF
LIBMDF_BUILD_EXAMPLES=ON|OFF
LIBMDF_BUILD_TESTS=ON|OFF
LIBMDF_BUILD_FUZZERS=ON|OFF
LIBMDF_INSTALL=ON|OFF
LIBMDF_INSTALL_BINARY=ON|OFF
LIBMDF_CMDF_STATIC_RUNTIME=ON|OFF
```

The project presets and `Makefile` targets are the preferred local entry points
for development and release builds.

On Linux, `cmake --install` excludes the development `cmdf`, whose interpreter
and RPATH refer to the local Bootlin collection. To install the CLI, use
`make install` or configure `LIBMDF_CMDF_STATIC_RUNTIME=ON` together with
`LIBMDF_INSTALL_BINARY=ON`. That executable is fully static and relocatable.

`make install` accepts `PREFIX` (default `/usr/local`), `BINDIR` (default
`$(PREFIX)/bin`), and `DESTDIR` for staging. Set them in the environment or on
the Make command line. It copies the already-built CLI to
`$(DESTDIR)$(BINDIR)/cmdf` without rebuilding it.

```sh
PREFIX="$HOME/.local" make install
BINDIR="$HOME/bin" make install
make install PREFIX=/usr DESTDIR="$PWD/stage"
```

## pkg-config

The SDK also ships relocatable pkg-config metadata. On Linux, use it with the
same consuming-project Bootlin compiler and direct ELF runtime policy shown
above; the `.pc` file deliberately contains no local toolchain paths or runtime
link flags. This development-only example assumes the consuming project vendors
the lifecycle resolver and uses an SDK with the usual `lib` directory.

```sh
SDK_PREFIX=/path/to/libmdf-sdk
toolchain_env=$(./scripts/cpkt-toolchains.sh env x86_64-linux-gnu) || exit 1
eval "$toolchain_env"
loader=$(find "$CPKT_TOOLCHAIN_SYSROOT/lib" -maxdepth 1 -type f \
  \( -name 'ld-linux*.so*' -o -name 'ld-musl-*.so*' \) -print | sort | head -n 1)
libgcc_dir=$(dirname -- "$("$CC" -print-file-name=libgcc_s.so.1)")
export PKG_CONFIG_PATH="$SDK_PREFIX/lib/pkgconfig"
"$CC" app.c \
  $(pkg-config --cflags --libs libmdf) \
  "-Wl,--dynamic-linker,$loader" \
  "-Wl,--disable-new-dtags,-rpath,$SDK_PREFIX/lib:$CPKT_TOOLCHAIN_SYSROOT/lib:$CPKT_TOOLCHAIN_SYSROOT/usr/lib:$libgcc_dir"
```
