#!/bin/sh
# Download MuPDF into thirdparty/ and apply the vbcc patches.
#
# thirdparty/ is git-ignored: MuPDF is ~70 MB of tarball and 200 MB unpacked,
# and nothing in it is ours except patches/.  Re-running replaces the tree,
# so a hand edit made in there is lost - put it in the patch instead.
set -e

VER=1.28.5
SHA256=98a5c10cda20c3992cdf76ff6b2a1149c32bd79cc796d3f703230b1185b7e934
URL=https://mupdf.com/downloads/archive/mupdf-$VER-source.tar.gz

cd "$(dirname "$0")"
mkdir -p thirdparty
TGZ=thirdparty/mupdf-$VER-source.tar.gz
DIR=thirdparty/mupdf-$VER-source

if [ ! -f "$TGZ" ]; then
  echo "downloading $URL"
  curl -fL -o "$TGZ.part" "$URL"
  mv "$TGZ.part" "$TGZ"
fi
echo "$SHA256  $TGZ" | shasum -a 256 -c -

rm -rf "$DIR"
tar -xzf "$TGZ" -C thirdparty
for p in patches/mupdf-$VER-*.patch; do
  echo "applying $p"
  patch -d "$DIR" -p1 --forward --silent < "$p"
done
echo "MuPDF $VER ready in $DIR"
