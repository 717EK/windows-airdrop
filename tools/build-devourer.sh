#!/usr/bin/env bash
# Build OpenIPC devourer (user-space Realtek Wi-Fi driver) on Windows with
# MinGW-w64, against libusb's official, signature-checked Windows binaries.
# Run from Git Bash. Needs: scoop install cmake ninja mingw pkgconf 7zip; gh; gpg.
#
#   tools/build-devourer.sh [deps-dir]     (default: ../deps next to this repo)
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="${1:-$REPO/../deps}"
DEVOURER_COMMIT=74c6b37ce1c7ae198873d3a720466def691b86fb   # audited 2026-10-13
LIBUSB_TAG=v1.0.30
LIBUSB_FPR=C68187379B23DE9EFC46651E2C80FF56C6830A0E        # Tormod Volden, per libusb's SIGNING_KEYS.txt

mkdir -p "$DEPS"
DEPS="$(cd "$DEPS" && pwd)"

# libusb: download, verify against the release manager's key, unpack.
if [ ! -f "$DEPS/libusb/MinGW64/static/libusb-1.0.a" ]; then
	mkdir -p "$DEPS/libusb-dl" "$DEPS/libusb"
	(cd "$DEPS/libusb-dl" &&
		gh release download "$LIBUSB_TAG" -R libusb/libusb -p "libusb-${LIBUSB_TAG#v}.7z" -p "libusb-${LIBUSB_TAG#v}.7z.asc" --clobber &&
		gh api repos/libusb/libusb/contents/KEYS --jq .content | base64 -d > KEYS &&
		gpg --batch --import KEYS 2>/dev/null &&
		gpg --batch --status-fd 1 --verify "libusb-${LIBUSB_TAG#v}.7z.asc" "libusb-${LIBUSB_TAG#v}.7z" 2>/dev/null |
			grep -q "VALIDSIG .* $LIBUSB_FPR" || { echo "libusb signature check FAILED" >&2; exit 1; })
	(cd "$DEPS/libusb" && 7z x -y "../libusb-dl/libusb-${LIBUSB_TAG#v}.7z" > /dev/null)
fi

# devourer includes both <libusb.h> and <libusb-1.0/libusb.h>; pkgconf wants <prefix>/lib/pkgconfig.
mkdir -p "$DEPS/libusb/include/libusb-1.0" "$DEPS/libusb/lib/pkgconfig"
cp "$DEPS/libusb/include/libusb.h" "$DEPS/libusb/include/libusb-1.0/"
WIN_LIBUSB="$(cygpath -m "$DEPS/libusb")"
cat > "$DEPS/libusb/lib/pkgconfig/libusb-1.0.pc" <<EOF
prefix=$WIN_LIBUSB
includedir=\${prefix}/include
libdir=\${prefix}/MinGW64/static

Name: libusb-1.0
Description: libusb ${LIBUSB_TAG#v} official Windows binaries (MinGW64, static)
Version: ${LIBUSB_TAG#v}
Cflags: -I\${includedir} -I\${includedir}/libusb-1.0
Libs: \${libdir}/libusb-1.0.a
EOF

if [ ! -d "$DEPS/devourer/.git" ]; then
	git -c core.autocrlf=false clone -q https://github.com/OpenIPC/devourer.git "$DEPS/devourer"
fi
git -C "$DEPS/devourer" checkout -q "$DEVOURER_COMMIT"
git -C "$DEPS/devourer" apply --check "$REPO/patches/devourer-01-mingw-txdemo-fork.patch" 2>/dev/null &&
	git -C "$DEPS/devourer" apply "$REPO/patches/devourer-01-mingw-txdemo-fork.patch"

# GNU ld resolves static archives in one pass and CMake puts libusb before
# libdevourer.a, so also append it after everything else.
export PKG_CONFIG_PATH="$WIN_LIBUSB/lib/pkgconfig"
cmake -S "$DEPS/devourer" -B "$DEPS/devourer/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
	-DCMAKE_CXX_STANDARD_LIBRARIES="$WIN_LIBUSB/MinGW64/static/libusb-1.0.a -lkernel32 -luser32 -lgdi32 -lwinspool -lshell32 -lole32 -loleaut32 -luuid -lcomdlg32 -ladvapi32"
cmake --build "$DEPS/devourer/build" -j "$(nproc)"
ctest --test-dir "$DEPS/devourer/build" -j 8

# awdlcap: our capture tool on devourer's API (GPL-2.0, like devourer).
g++ -std=c++20 -O2 -I"$DEPS/devourer/src" -I"$DEPS/devourer/examples/common" 	-I"$DEPS/libusb/include" -I"$DEPS/libusb/include/libusb-1.0" 	"$REPO/radio/devourer/awdlcap.cpp" -o "$DEPS/awdlcap.exe" 	"$DEPS/devourer/build/libdevourer.a" "$DEPS/libusb/MinGW64/static/libusb-1.0.a"
echo "built: $DEPS/devourer/build/rxdemo.exe, txdemo.exe, $DEPS/awdlcap.exe"
