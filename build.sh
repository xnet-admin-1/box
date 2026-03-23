#!/bin/bash
# Box — build all native components for Android aarch64
# Run inside xnet-dev container:
#   docker exec -it xnet-dev bash /workspace/box/build.sh
#
# Prerequisites: Android NDK at /opt/android-sdk/ndk/26.1.10909125

set -euo pipefail

TALLOC_VERSION="2.4.4"
LIBARCHIVE_VERSION="3.8.6"
PROOT_REPO="https://github.com/termux/proot.git"
PROOT_BRANCH="master"
ANDROID_API=26

NDK="/opt/android-sdk/ndk/26.1.10909125"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"

export CC="$TOOLCHAIN/bin/aarch64-linux-android${ANDROID_API}-clang"
export AR="$TOOLCHAIN/bin/llvm-ar"
export STRIP="$TOOLCHAIN/bin/llvm-strip"
export RANLIB="$TOOLCHAIN/bin/llvm-ranlib"
export OBJCOPY="$TOOLCHAIN/bin/llvm-objcopy"
export OBJDUMP="$TOOLCHAIN/bin/llvm-objdump"

BUILD="/tmp/box-build"
OUT="$BUILD/out/arm64-v8a"
mkdir -p "$BUILD" "$OUT"

if ! command -v gawk &>/dev/null; then
    echo "Installing gawk..."
    apt-get update -qq && apt-get install -y -qq gawk
fi
if ! command -v cmake &>/dev/null; then
    echo "Installing cmake + ninja..."
    apt-get update -qq && apt-get install -y -qq cmake ninja-build
fi

echo "=== Box — native build for Android aarch64 ==="
echo "NDK: $NDK"
echo ""

# ── 1. talloc ──────────────────────────────────────────────────────
echo "[1/4] talloc $TALLOC_VERSION..."
cd "$BUILD"
if [ ! -d "talloc-$TALLOC_VERSION" ]; then
    wget -q "https://www.samba.org/ftp/talloc/talloc-$TALLOC_VERSION.tar.gz"
    tar xzf "talloc-$TALLOC_VERSION.tar.gz"
    rm -f "talloc-$TALLOC_VERSION.tar.gz"
fi
cd "talloc-$TALLOC_VERSION"

cat > cross-answers.txt <<'ANSWERS'
Checking uname sysname type: "Linux"
Checking uname machine type: "dontcare"
Checking uname release type: "dontcare"
Checking uname version type: "dontcare"
Checking simple C program: OK
rpath library support: OK
-Wl,--version-script support: FAIL
Checking getconf LFS_CFLAGS: OK
Checking for large file support without additional flags: OK
Checking for -D_FILE_OFFSET_BITS=64: OK
Checking for -D_LARGE_FILES: OK
Checking correct behavior of strtoll: OK
Checking for working strptime: OK
Checking for C99 vsnprintf: OK
Checking for HAVE_SHARED_MMAP: OK
Checking for HAVE_MREMAP: OK
Checking for HAVE_INCOHERENT_MMAP: OK
Checking for HAVE_SECURE_MKSTEMP: OK
Checking getconf large file support flags work: OK
Checking for HAVE_IFACE_IFCONF: FAIL
ANSWERS

mkdir -p /tmp/mock-bin
ln -sf "$CC" /tmp/mock-bin/cc
export PATH="/tmp/mock-bin:$PATH"

make distclean 2>/dev/null || true
./configure --prefix="$BUILD/talloc-install" \
    --disable-rpath --disable-python \
    --cross-compile --cross-answers=cross-answers.txt

make -j$(nproc)

mkdir -p "$BUILD/talloc-out/include" "$BUILD/talloc-out/lib"
"$AR" rcs "$BUILD/talloc-out/lib/libtalloc.a" \
    bin/default/talloc.c.5.o \
    bin/default/lib/replace/replace.c.1.o \
    bin/default/lib/replace/closefrom.c.1.o
cp talloc.h "$BUILD/talloc-out/include/"
cp bin/default/libtalloc.so "$BUILD/talloc-out/lib/libtalloc.so"
"$STRIP" "$BUILD/talloc-out/lib/libtalloc.so"
echo "  talloc OK"

# ── 2. libarchive ─────────────────────────────────────────────────
echo "[2/4] libarchive $LIBARCHIVE_VERSION..."
cd "$BUILD"
if [ ! -d "libarchive-$LIBARCHIVE_VERSION" ]; then
    wget -q "https://github.com/libarchive/libarchive/releases/download/v$LIBARCHIVE_VERSION/libarchive-$LIBARCHIVE_VERSION.tar.xz"
    tar xJf "libarchive-$LIBARCHIVE_VERSION.tar.xz"
    rm -f "libarchive-$LIBARCHIVE_VERSION.tar.xz"
fi

cmake -S "libarchive-$LIBARCHIVE_VERSION" -B "$BUILD/libarchive-build" \
    -DCMAKE_SYSTEM_NAME=Android \
    -DCMAKE_ANDROID_NDK="$NDK" \
    -DCMAKE_ANDROID_ARCH_ABI=arm64-v8a \
    -DCMAKE_ANDROID_STL_TYPE=c++_static \
    -DCMAKE_SYSTEM_VERSION=$ANDROID_API \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$BUILD/libarchive-install" \
    -DENABLE_TEST=OFF \
    -DENABLE_TAR=ON \
    -DENABLE_CPIO=OFF \
    -DENABLE_CAT=OFF \
    -DENABLE_UNZIP=OFF \
    -DENABLE_XATTR=OFF \
    -DENABLE_ACL=OFF \
    -DENABLE_ICONV=OFF \
    -DENABLE_EXPAT=OFF \
    -DENABLE_LIBXML2=OFF \
    -DENABLE_OPENSSL=OFF \
    -DENABLE_LIBB2=OFF \
    -DENABLE_LZ4=OFF \
    -DENABLE_ZSTD=OFF \
    -DENABLE_LZMA=OFF \
    -DENABLE_BZip2=OFF

cmake --build "$BUILD/libarchive-build" --target bsdtar
"$STRIP" "$BUILD/libarchive-build/bin/bsdtar"
echo "  libarchive OK"

# ── 3. proot ───────────────────────────────────────────────────────
echo "[3/4] proot (Termux-patched)..."
cd "$BUILD"
if [ ! -d "proot" ]; then
    git clone --depth=1 --branch="$PROOT_BRANCH" "$PROOT_REPO" proot
fi
cd proot/src

export CFLAGS="-I$BUILD/talloc-out/include"
export LDFLAGS="-L$BUILD/talloc-out/lib"
export PROOT_UNBUNDLE_LOADER="../libexec/proot"

make distclean 2>/dev/null || true

ASHMEM_FILE="extension/ashmem_memfd/ashmem_memfd.c"
if ! head -1 "$ASHMEM_FILE" | grep -q 'string.h'; then
    sed -i '1i #include <string.h>' "$ASHMEM_FILE"
fi

make V=1 -j$(nproc) PREFIX="$BUILD/proot-install" install

"$STRIP" "$BUILD/proot-install/bin/proot"
find "$BUILD/proot/libexec" -type f -exec "$STRIP" {} \; 2>/dev/null || true
echo "  proot OK"

# ── 4. package ─────────────────────────────────────────────────────
echo "[4/4] Packaging..."

cp "$BUILD/proot-install/bin/proot" "$OUT/libproot-xed.so"
[ -f "$BUILD/proot/libexec/proot/loader" ] && cp "$BUILD/proot/libexec/proot/loader" "$OUT/libproot.so"
[ -f "$BUILD/proot/libexec/proot/loader32" ] && cp "$BUILD/proot/libexec/proot/loader32" "$OUT/libproot32.so"
cp "$BUILD/talloc-out/lib/libtalloc.so" "$OUT/libtalloc.so"
cp "$BUILD/libarchive-build/bin/bsdtar" "$OUT/libbsdtar.so"

echo ""
echo "=== Done ==="
ls -lh "$OUT/"
echo ""
echo "Artifacts in: $OUT/"
