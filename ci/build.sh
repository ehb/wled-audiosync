#!/bin/sh
# Build a statically linked binary with warnings as errors, from the repository root:
#   ci/build.sh aarch64|armhf  -> build/<arch>/wled-audiosync
# Static, so it runs on any Linux of that architecture (LibreELEC has no compiler and a
# C library of its own). The program uses no name lookups, so a static glibc is fine.
set -e
ARCH=$1
case "$ARCH" in
    aarch64) CC=aarch64-linux-gnu-gcc;   PREFIX=aarch64-linux-gnu;   MACHINE=AArch64 ;;
    armhf)   CC=arm-linux-gnueabihf-gcc; PREFIX=arm-linux-gnueabihf; MACHINE=ARM ;;
    *) echo "usage: ci/build.sh aarch64|armhf" >&2; exit 2 ;;
esac

make CC="$CC" BUILD="build/$ARCH" CFLAGS="-O2 -Wall -Wextra -Werror -static"
BIN=build/$ARCH/wled-audiosync
"${PREFIX}-readelf" -h "$BIN" | grep -E "Machine:.*$MACHINE"
if "${PREFIX}-readelf" -l "$BIN" | grep -q INTERP; then
    echo "$BIN is dynamically linked" >&2
    exit 1
fi
"${PREFIX}-strip" "$BIN"
echo "built $BIN"
