#!/bin/sh
# Build and run the host test of the render worker (see worker_test.c).
#
#   test/host/build.sh <big.pdf> <other.pdf> [needle]
#
# Needs ./fetch_mupdf.sh to have run.  MuPDF is built for the host from the
# same patched tree the Amiga build uses, into its own build/ directory
# there.  macOS: the Command Line Tools' clang is called directly (the /usr/bin
# shims may be blocked by the Xcode licence).
set -e
cd "$(dirname "$0")/../.."
M=thirdparty/mupdf-1.28.5-source
OUT=build/host-test
CLT=/Library/Developer/CommandLineTools/usr/bin
if [ -x "$CLT/clang" ]; then
  SDK=$(ls -d /Library/Developer/CommandLineTools/SDKs/MacOSX*.sdk | tail -1)
  CC="$CLT/clang -isysroot $SDK"
  AR="$CLT/ar"
else
  CC=${CC:-cc}
  AR=${AR:-ar}
fi
mkdir -p $OUT
if [ ! -f $M/build/release/libmupdf.a ]; then
  echo "building MuPDF for the host..."
  make -C $M -j8 CC="$CC" AR="$AR" HAVE_X11=no HAVE_GLUT=no HAVE_CURL=no html=no mujs=no tesseract=no \
    barcode=no brotli=no extract=no build=release libs > $OUT/mupdf-build.log 2>&1
fi
$CC -O1 -g -Wall -Wno-unused-parameter -o $OUT/worker_test \
  -Itest/host/shim -Isrc/viewer -Isrc/common -I$M/include -I$M/thirdparty/zlib \
  test/host/worker_test.c src/viewer/worker.c src/common/wpdfmem.c test/host/shim/amigashim.c \
  $M/build/release/libmupdf.a $M/build/release/libmupdf-third.a -lpthread
$OUT/worker_test "$@"
