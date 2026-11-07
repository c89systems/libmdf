# Development and verification

Run `make help` for the authoritative local command list.

For a new Linux development checkout, run `make tools-native` before the first
Debug build to prepare its pinned toolchains and AFL++.

Run `make build-debug` before using clangd, and again after `make clean`. This
normal build prepares generated headers, the installed-SDK Lua facade and
`build/debug/compile_commands.json`, which `.clangd` discovers automatically.
Each file uses its owning CMake target's language and compile flags: C89 for
the library and C99 for the Lua binding. Public headers have their own C89
compile commands. The native `editor_clangd` check verifies normal discovery
for implementations and public headers; it includes the Lua implementation
when `LIBMDF_BUILD_LUA_TESTS` is enabled.

Keep API declarations and hover comments synchronized in
`include/libmdf/mdf.h` and `include/libmdf/mdf.h.in`. CMake generates the
installed header from the template with the build's version. The
`public_header_documentation_sync` test checks that all three agree except
for version macros.

## Toolchains and caches

`make build` prepares the pinned x86_64 musl toolchain before configuring its
static CLI, reusing the collection when already available. `make tools-native`
explicitly prepares both native GNU and musl collections plus AFL++; use
`make tools-cross` for the complete declared target matrix.
Toolchain files and runtime discovery never provision tools. Direct CMake and
other build entrypoints require their selected tools to be prepared first.
Linux uses the pinned complete
Bootlin collections; Darwin cross builds use the prepared osxcross SDK and pinned
host MIG helper. `CPKT_TOOLCHAIN_CACHE` and `CPKT_DEPENDENCY_CACHE` overrides use
this checkout's owned `build/` or `.cache/` paths, or the declared global cpkt cache
(default `~/.cache/cpkt`, with separate `toolchains/` and `deps/` directories).
Linux compiler collections resolve under `~/.cache/cpkt/toolchains/roots/` by
default; dependency archives use `~/.cache/cpkt/deps/`. Clang/LLVM and the
developer-prepared osxcross/Apple SDK remain workstation prerequisites.
The declared global cache and installed workstation prerequisites survive
`make clean`. Repo-local cache overrides are disposable. Cleanup removes the entire
local `build/`, `dist/` and `.cache/`, including hidden entries and local tools.
Each checkout owns its generated state; another checkout's sources, tools, build state
or caches cannot serve as shared prerequisites, even through overrides or symlinks.

## Checks

Run commands from the source root. `make help` is the authoritative command list.
`CMAKE_BUILD_PARALLEL_LEVEL` defaults to 2; Make and CTest serialize operations.

| Command | Coverage |
| --- | --- |
| `make build` | Static native musl CLI |
| `make test` | Native API/CLI, streaming contracts, pager PTYs, Lua, goldens and lifecycle checks |
| `make valgrind` | Memory ownership, failures, word storage, pager and Lua checks |
| `make fuzz-smoke` | Bounded AFL++ smoke and expected-crash/startup-failure gates |
| `make fuzz` / `make fuzz-long` | Fresh longer fuzz jobs |
| `make parity-quick` | Bounded Go parity smoke |
| `make parity` | Full ANSI, HTML and fragmented-stream parity matrices |
| `make test-hardening` | Native, memory, fuzz smoke and complete parity |
| `make lua-test` | Installed-SDK Lua facade |
| `make example-smoke-local` | Basic embedding example |

Native checks exercise layout, fragmentation and capacity boundaries. Memory
checks use compact fixtures for the same ownership and failure transitions.
Pager checks include tiny cache seams, full-size content allowances, repeated
search cycles, resized selections, and documents that outgrow the cache after
narrowing. Sink/trace tests assert actual writes, not only final rendered text;
see the [streaming contract](streaming.md).

AFL++ uses the pinned Bootlin loader/runtime directly. A saved crash fails the
gate even if AFL++ reaches its time limit normally. Runs and reproducers remain
under `build/fuzz/runs/`. Quick parity is an explicit smoke and does not replace
full parity. Streaming parity is chunk-bound and uses no timing sleeps.

## Reusing proof

`make test` forces the native suite. `make check-native`, `make valgrind`,
`make fuzz-smoke` and the parity targets reuse suite success only when their native
CMake prerequisites are unchanged. Records live under `build/*/checks/`; failed
runs remove their record and preserve CTest diagnostics. CMake does not cache
individual CTest cases. Keep the checkout, configured tools and execution modes
stable during a check. After restoring timestamps, changing unrepresented
execution environment options, or replacing prepared tools, run `make check-clean`.
If a compiler collection or its cache location changes, first reconfigure each
affected build directory with `cmake --fresh --preset <preset>`. CMake's automatic
compiler-change reset can discard preset options; a fresh configure reapplies them.
Keep unrelated build directories and shared tool caches intact.
The test fixtures own isolated build directories and do not reconfigure active
Debug, Release or fuzz producers.

## Documentation

Keep the README focused on getting started and link detailed usage from `docs/`.
The `documentation_links` check validates local file links without network access.
SDK, CLI and Lua source packages ship this same README and complete guide set;
artifact verification rejects missing, stale or unexpected guide files. Native
packaging checks also prove that guide-only changes refresh archives without
rebuilding the library or CLI.

## Goldens and local Lua CLI

`make golden-test` and `make cmdf-golden-test` compare the library/CLI outputs
against the checked-in snapshots. Regenerate with `make golden-update` or
`make cmdf-golden-update` only for an intentional behavior change. Parity
exceptions in `testdata/goldens/libmdf/PARITY_EXCLUSIONS.txt` require matching
native output and sink/trace proof.

`make build-debug` also creates `build/debug/cmdf.sh`, a development wrapper
for the generated Lua CLI, local module tree and pinned interpreter. It can run
from another working directory. `make lua-env` prints the interpreter and module
paths.

## Benchmarks

`make benchmark` measures C and Lua API ANSI/HTML paths; `make benchmark-cmdf`
measures C/Lua CLI ANSI, HTML and deck output. Pass options through `BENCH_ARGS`.
`make bench-check` compares against `testdata/benchmarks/libmdf-baseline.json`;
use `BENCH_CHECK_ARGS` for its options. The baseline records its reference and
measurement settings. Compare reference and candidate on the same host and
toolchain before attributing a timing difference to the code.

## ABI and releases

The shared-library ABI is independent of the release version.
`LIBMDF_ABI_VERSION` in `CMakeLists.txt` sets the SONAME; Lua-only changes do
not themselves require a C ABI bump. Follow the [release guide](release.md)
for preparation and the final signed/tagged gate.
