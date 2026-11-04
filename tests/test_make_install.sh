#!/usr/bin/env bash
# Exercise the actual Make install entrypoint entirely inside owned staging.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
unset prefix exec_prefix bindir PREFIX DESTDIR MAKEFLAGS MFLAGS MAKEOVERRIDES
mkdir -p "$root/build"
work=$(mktemp -d "$root/build/test-make-install.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
repo="$work/source with spaces"
mkdir -p "$repo/scripts" "$repo/build/cmdf-static"
cp "$root/Makefile" "$repo/"
cp "$root/scripts/install_cmdf.sh" "$repo/scripts/"
source_binary="$repo/build/cmdf-static/cmdf"
printf '#!/bin/sh\nprintf "installed fixture\\n"\n' > "$source_binary"
chmod 0700 "$source_binary"
case_number=0
check_install() {
  local expected=$1 stage target
  shift
  case_number=$((case_number + 1))
  stage="$work/stage $case_number"
  target="$stage$expected/cmdf"
  # No build helper is present: install must use the existing producer.
  (umask 077; make --no-print-directory -C "$repo" install DESTDIR="$stage" "$@") \
    > "$work/install-$case_number.log" 2>&1 || {
      cat "$work/install-$case_number.log" >&2; exit 1;
    }
  cmp "$source_binary" "$target"
  test "$(stat -c '%a' "$target")" = 755
  test "$(cat "$work/install-$case_number.log")" = "$target"
  test "$("$target")" = 'installed fixture'
}
check_install /usr/local/bin
check_install /usr/bin prefix=/usr
check_install /opt/cmdf/bin prefix=/usr exec_prefix=/opt/cmdf
check_install /custom/commands prefix=/usr exec_prefix=/opt/ignored bindir=/custom/commands
check_install "/path with 'quotes'/commands" "bindir=/path with 'quotes'/commands"
prefix=/environment check_install /environment/bin
bindir=/environment/commands check_install /environment/commands
bindir=/environment/commands check_install /command-line/commands bindir=/command-line/commands
check_install /bin prefix=
check_install /bin prefix=/usr exec_prefix=
check_install '' bindir=
rm "$source_binary"
if make --no-print-directory -C "$repo" install DESTDIR="$work/missing" bindir=/bin \
    > "$work/missing.log" 2>&1; then
  printf 'make install accepted a missing build\n' >&2; exit 1;
fi
grep -Fq 'run make build first' "$work/missing.log"
test ! -e "$work/missing"
