#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PREFIX=${PREFIX:-/usr/local}
BINDIR=${BINDIR:-"$PREFIX/bin"}
DESTDIR=${DESTDIR:-}
SRC="$ROOT/build/cmdf-static/cmdf"
DST="$DESTDIR$BINDIR/cmdf"

if [ ! -x "$SRC" ]; then
  printf '%s\n' "cmdf has not been built for install; run make build first" >&2
  exit 1
fi

install -d "$(dirname "$DST")"
install -m 0755 "$SRC" "$DST"
printf '%s\n' "$DST"
