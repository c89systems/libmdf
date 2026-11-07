# Build, package and release

The [pkt-systems-cmake-lifecycle skill](https://github.com/c89systems/cpkt/tree/trunk/skills/pkt-systems-cmake-lifecycle)
governs this checkout. Make exposes commands, Bash sequences phases, CMake
owns producers and CTest owns verification. See the [development guide](development.md)
for tool preparation and local checks. All commands below run from the source root.

`make prerelease` performs incremental binary and Lua preparation: format checking,
native tests, Valgrind, AFL++ smoke, complete parity, binary packaging and final-byte
checks for those artifacts. `make package-source` archives committed source inputs.
Complete artifact verification extracts that archive and checks its version,
file inventory, contents and executable modes against the release inputs.
Ordinary build and test targets never clean.

Keep implementation, testing and review on one development branch. Once a release
is authorized, preserve that branch and squash its verified change onto the local
default branch. Present that squash commit for amendment and signing there; signing,
the lightweight release tag and the final gate all use the default-branch commit.
Create no intermediate release branch. The default branch is resolved from local
`origin/HEAD`, with the project's local `trunk` as the fallback when that ref is
unavailable. An unresolved default or detached HEAD cannot pass the final gate.

Owned Git builds use one exact lightweight `vX.Y.Z` tag on HEAD, an explicit
`LIBMDF_RELEASE_VERSION=X.Y.Z` candidate override, or `0.0.0`. Multiple exact
lightweight release tags are an error. Annotated and symbolic tags do not supply
release identity. Extracted sources use their own injected `VERSION` and
`RELEASE_MANIFEST`, including when extracted beneath another Git checkout.

`make release` is a local final proof gate. Before cleaning or starting a producer,
it requires HEAD attached to the resolved default branch, a clean committed
checkout, a verified signature on the final amended commit, one exact lightweight
tag on HEAD, no candidate override and no partial target selection. Its preflight is also available
as `make lifecycle-version-contract`; it never creates or removes tags. Final
artifacts and staging use the owned `dist/` and `build/package/` paths; redirected
`LIBMDF_DIST_DIR` or `LIBMDF_PACKAGE_WORK_DIR` values are rejected before cleaning.
The final gate runs full `make clean` once, then runs the required checks and builds
every target through its normal Release preset. The glibc x86_64 producer lives at
`build/x86_64-linux-gnu-release`, alongside the other target directories; SDK and CLI
archives share those producers. `make test-release` runs the native Release tests
there before final packaging. Source archive verification adds no second build.
A failed phase stops the run;
there is no retagging, automatic repair or publication inside the gate.
Prepare required release tools and archives in the declared global cache so they
survive that clean. Keep remote default-branch and release-tag refs unchanged until
the complete signed and tagged local release gate passes; never push to trigger CI.
Updates limited to documentation or skill instructions use normal verification,
commit/signing and explicitly authorized push policy, without release tags,
artifacts or a release build.

CMake install components `sdk` and `cli` stage each product. CMake archive outputs
reuse unchanged producer inputs. Tarballs and source rocks have deterministic
ordering, timestamps and metadata; intentional executable modes are preserved.
GNU tar is required; on macOS select it with `LIBMDF_TAR=gtar`.

`LIBMDF_TARGETS` selects a binary rehearsal without changing unrelated artifacts.
Its checksum inventory lives at `build/package/binary-CHECKSUMS`; it cannot stand in
for the final release manifest. `make package-verify` verifies that scope through
CTest. `make release-lua-artifacts` prepares the Lua archive, public rockspec and
source rock; `make verify-lua-artifacts` proves the source rock against the extracted
native SDK. The existing pinned interpreter is shared across these checks.
Lua artifact generation also uses CMake output dependencies: unchanged preparation
preserves artifact timestamps and matching verification results; changed sources
or missing outputs regenerate the declared set.

`make package-checksums` requires the complete declared target matrix, source archive
and all three Lua artifacts. `make verify-release-archives` verifies that complete
inventory through CTest: SHA-256, layout, architecture, versions, public exports,
licenses, relocatable metadata, installed static/shared/pkg-config consumers and
privacy of actual delivered bytes. Native Linux consumers and CLIs execute before
and after SDK relocation. Cross consumers are linked with their selected target
compiler; execution is deferred because this project does not select cross runners.
Extra or stale release-looking artifacts cause the complete privacy gate to fail.
Publication must use the verified checksum inventory and its manifest, with explicit
engineer authorization; these targets never push or publish.

Darwin cross builds select the newest complete prepared `arm64-apple-darwin25.x`
collection and its SDK. Override `OSXCROSS_ROOT` or `CPKT_OSXCROSS_HOST` to select a
prepared collection. Links use the real osxcross linker through `--ld-path`.
Target inspection uses explicit selected tools, the selected CMake cache, matching
compiler siblings or the pinned resolver. There is no host-binutils fallback.
Darwin artifacts are inspected as Mach-O bytes and keep their ABI-correct `@rpath`
dylib identity. Packaging does not mutate their signatures with stripping or
`install_name_tool`.
