#!/bin/bash
# Builds the app and the command line tools on Haiku and creates the .hpkg.
# Needs haiku_devel, zlib_devel and libslirp_devel. Bump the revision in PackageInfo
# (1.2-2, -3, ...) for every package meant to replace an installed one.
#
#   package/build-package.sh
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
NAME=bebox
VERSION=$(grep '^version' package/PackageInfo | awk '{print $2}')
ARCH=$(getarch)

make -j4
TOOLS=$ROOT/package/tools
mkdir -p "$TOOLS"
g++ -O2 -std=c++17 -ffp-contract=off -Isrc tools/bebox-img/main.cpp src/core/storage/*.cpp \
	src/core/hobbit/HobbitVolume.cpp \
	src/core/Log.cpp -lz -o "$TOOLS/bebox-img"
g++ -O2 -std=c++17 -ffp-contract=off -DHAVE_SLIRP -Isrc src/headless/main.cpp src/core/*.cpp \
	src/core/*/*.cpp -lslirp -lz -lnetwork -o "$TOOLS/bebox-headless"

PKG=$ROOT/package/pkg
rm -rf "$PKG"
mkdir -p "$PKG/apps" "$PKG/bin" "$PKG/data/deskbar/menu/Applications" \
	"$PKG/documentation/packages/$NAME"
cp objects.*-release/BeBox "$PKG/apps/BeBox"
cp "$TOOLS/bebox-img" "$TOOLS/bebox-headless" "$PKG/bin/"
ln -s ../../../../apps/BeBox "$PKG/data/deskbar/menu/Applications/BeBox"
cp README.md LICENSE "$PKG/documentation/packages/$NAME/"
sed "s/^architecture.*/architecture\t$ARCH/" package/PackageInfo \
	> "$PKG/.PackageInfo"

OUT=$ROOT/package/$NAME-$VERSION-$ARCH.hpkg
rm -f "$OUT"
(cd "$PKG" && package create -q "$OUT")
echo "Created $OUT"
echo "Install with: pkgman install $OUT"
