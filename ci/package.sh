#!/bin/sh
# Pack a built binary for a GitHub Release:
#   ci/package.sh aarch64 0.1.0 OUTDIR    -> OUTDIR/wled-audiosync-0.1.0-aarch64.tar.gz
set -e
ARCH=$1
VERSION=$2
OUT=$3
NAME=wled-audiosync-$VERSION-$ARCH
BIN=build/$ARCH/wled-audiosync
[ -f "$BIN" ] || { echo "$BIN missing: run ci/build.sh $ARCH first" >&2; exit 1; }
rm -rf "build/$NAME"
mkdir -p "build/$NAME" "$OUT"
cp "$BIN" README.md LICENSE "build/$NAME/"
tar czf "$OUT/$NAME.tar.gz" --owner=0 --group=0 -C build "$NAME"
rm -rf "build/$NAME"
echo "$OUT/$NAME.tar.gz"
