#!/usr/bin/env bash
# Exercise the public Make entrypoints without downloading compiler collections.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
mkdir -p "$root/build"
work=$(mktemp -d "$root/build/test-static-build.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
repo="$work/source with spaces"
mkdir -p "$repo/scripts"
cp "$root/Makefile" "$repo/"
cp "$root/scripts/"{build_cmdf_static,discover_target_tools}.sh "$repo/scripts/"
export TEST_BUILD_ROOT="$repo" TEST_BUILD_LOG="$work/steps"
cat > "$repo/scripts/cpkt-toolchains.sh" <<'SCRIPT'
#!/usr/bin/env bash
set -euo pipefail
printf '%s %s\n' "$1" "$2" >> "$TEST_BUILD_LOG"
case "$1" in
  ensure)
    [[ ${TEST_ENSURE_FAIL:-0} == 0 ]] || exit 7
    touch "$TEST_BUILD_ROOT/ready"
    ;;
  discover)
    test -f "$TEST_BUILD_ROOT/ready"
    printf 'status=ready\nreadelf=%s/scripts/readelf\n' "$TEST_BUILD_ROOT"
    ;;
  *) exit 2;;
esac
SCRIPT
cat > "$repo/scripts/build.sh" <<'SCRIPT'
#!/usr/bin/env bash
set -euo pipefail
printf 'build %s\n' "$*" >> "$TEST_BUILD_LOG"
test -f "$TEST_BUILD_ROOT/ready"
mkdir -p "$TEST_BUILD_ROOT/build/x86_64-linux-musl-release"
printf 'static-cli-fixture\n' > "$TEST_BUILD_ROOT/build/x86_64-linux-musl-release/cmdf"
chmod +x "$TEST_BUILD_ROOT/build/x86_64-linux-musl-release/cmdf"
SCRIPT
cat > "$repo/scripts/readelf" <<'SCRIPT'
#!/usr/bin/env bash
set -euo pipefail
printf 'readelf %s\n' "$1" >> "$TEST_BUILD_LOG"
if [[ ${TEST_DYNAMIC:-0} == 1 ]]; then printf ' (NEEDED) Shared library: [libc.so]\n'; fi
SCRIPT
cat > "$repo/scripts/cpkt-aflpp.sh" <<'SCRIPT'
#!/usr/bin/env bash
set -euo pipefail
printf 'aflpp %s\n' "$1" >> "$TEST_BUILD_LOG"
SCRIPT
chmod +x "$repo/scripts/"*
cat > "$work/build-expected" <<'EXPECTED'
ensure x86_64-linux-musl
build x86_64-linux-musl-release --target cmdf
discover x86_64-linux-musl
readelf -l
readelf -d
EXPECTED
for state in cold warm; do
  : > "$TEST_BUILD_LOG"
  make --no-print-directory -C "$repo" build > "$work/$state.log" 2>&1 || {
    cat "$work/$state.log" >&2; exit 1;
  }
  cmp "$work/build-expected" "$TEST_BUILD_LOG"
  test -x "$repo/build/cmdf-static/cmdf"
  cmp "$repo/build/x86_64-linux-musl-release/cmdf" "$repo/build/cmdf-static/cmdf"
done
rm -rf -- "$repo/build"
rm "$repo/ready"
: > "$TEST_BUILD_LOG"
if TEST_ENSURE_FAIL=1 make --no-print-directory -C "$repo" build > "$work/failure.log" 2>&1; then
  printf 'make build accepted toolchain preparation failure\n' >&2; exit 1;
fi
printf 'ensure x86_64-linux-musl\n' > "$work/failure-expected"
cmp "$work/failure-expected" "$TEST_BUILD_LOG"
test ! -e "$repo/build"
: > "$TEST_BUILD_LOG"
if TEST_DYNAMIC=1 make --no-print-directory -C "$repo" build > "$work/dynamic.log" 2>&1; then
  printf 'make build accepted a dynamic CLI\n' >&2; exit 1;
fi
grep -q 'cmdf install binary must be static' "$work/dynamic.log"
test ! -e "$repo/build/cmdf-static/cmdf"
: > "$TEST_BUILD_LOG"
make --no-print-directory -C "$repo" tools-native > "$work/tools.log" 2>&1
printf 'ensure x86_64-linux-gnu\nensure x86_64-linux-musl\naflpp ensure\n' > "$work/tools-expected"
cmp "$work/tools-expected" "$TEST_BUILD_LOG"
