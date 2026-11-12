#!/bin/sh
# Builds the Debian package from the AppDir linuxdeploy fills for the
# AppImage. Qt, its ICU, libclang with the LLVM it links and clang's own
# headers come along, in /opt/unravel: distributions ship a Qt too old
# for it, and libclang in many versions under different names. Every
# other library comes from the system and becomes a dependency, which
# dpkg-shlibdeps works out, so this has to run on the oldest system the
# package is for.
#
# Usage: build-deb.sh <AppDir> <version> <output.deb>
set -eu

if [ $# -ne 3 ]; then
    echo "Usage: $0 <AppDir> <version> <output.deb>" >&2
    exit 2
fi
appdir=$1
version=$2
output=$3
logo=$(cd "$(dirname "$0")/../../logo" && pwd)

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# Under debian/ dpkg-shlibdeps takes the libraries in it as the package's own
root=$work/debian/unravel
prefix=$root/opt/unravel

mkdir -p "$prefix/lib" "$root/usr/bin" "$root/DEBIAN"
for dir in bin plugins qml translations; do
    if [ -d "$appdir/usr/$dir" ]; then
        cp -a "$appdir/usr/$dir" "$prefix/"
    fi
done
cp -a "$appdir"/usr/lib/libQt6* "$prefix/lib/"
for lib in libicu libclang libLLVM; do
    cp -a "$appdir"/usr/lib/$lib* "$prefix/lib/" 2>/dev/null || true
done
# clang's headers, found beside the program as ../lib/clang/<version>
cp -a "$appdir/usr/lib/clang" "$prefix/lib/"
ln -s /opt/unravel/bin/unravel "$root/usr/bin/unravel"
ln -s /opt/unravel/bin/unravel-cli "$root/usr/bin/unravel-cli"
install -Dm644 "$logo/unravel.desktop" "$root/usr/share/applications/unravel.desktop"
install -Dm644 "$logo/unravel.svg" "$root/usr/share/icons/hicolor/scalable/apps/unravel.svg"
chmod -R u+rwX,go+rX,go-w "$root"

printf 'Source: unravel\n\nPackage: unravel\nArchitecture: amd64\n' > "$work/debian/control"
libs=$(cd "$work" && find debian/unravel/opt -type f \( -name '*.so*' -o -path '*/bin/unravel*' \) \
    -exec dpkg-shlibdeps -O -l"$prefix/lib" {} + | sed -n 's/^shlibs:Depends=//p')

cat > "$root/DEBIAN/control" <<CONTROL
Package: unravel
Version: $version
Architecture: amd64
Maintainer: Henning Gruendl <henning.gruendl@qt.io>
Installed-Size: $(du -sk "$root" | cut -f1)
Depends: $libs
Recommends: g++ | clang, python3, git
Section: devel
Priority: optional
Homepage: https://iam-peter.github.io/unravel/
Description: See the architecture, flows and connections of C, C++ and Python code
 Indexes a project with clang (and Python's ast) and shows what a class
 is made of and what it touches, how control flows, which modules lean
 on which, where the cycles, duplicates, dead code and complexity are,
 and what chosen git commits changed. Built for reviewing and
 understanding code you didn't write yourself.
CONTROL

dpkg-deb --root-owner-group -Zxz -b "$root" "$output"
