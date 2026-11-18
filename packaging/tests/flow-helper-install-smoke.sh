#!/bin/sh
set -eu
unset LD_LIBRARY_PATH

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <configured-build-directory>" >&2
    exit 2
fi

repo=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
build=$(CDPATH= cd -- "$1" && pwd)
work="$repo/.flow-helper-deployment"
stage="$work/stage"
relocated="$work/relocated"

cleanup() {
    rm -rf "$work"
}
trap cleanup EXIT HUP INT TERM
cleanup

cmake --install "$build" --prefix "$stage"
mv "$stage" "$relocated"

helper="$relocated/bin/unravel-flow"
if [ ! -x "$helper" ]; then
    echo "Installed helper is not beside the installed application: $helper" >&2
    exit 1
fi

runpath=$(readelf -d "$helper" |
    sed -n 's/.*Library runpath: \[\(.*\)\]/\1/p')
if [ "$runpath" != '$ORIGIN/../lib' ]; then
    echo "Installed helper has no relocatable library path: $runpath" >&2
    exit 1
fi

dependencies=$(ldd "$helper")
if printf '%s\n' "$dependencies" | grep -Fq 'not found'; then
    echo "Installed helper has unresolved shared libraries:" >&2
    printf '%s\n' "$dependencies" >&2
    exit 1
fi
for library in libclang-cpp libLLVM; do
    if ! printf '%s\n' "$dependencies" |
        grep -E "${library}[^ ]* => ${relocated}/"; then
        echo "Installed helper did not load $library from the relocated prefix." >&2
        printf '%s\n' "$dependencies" >&2
        exit 1
    fi
done

dangling_links=$(find "$relocated/lib" -type l ! -exec test -e {} \; -print)
if [ -n "$dangling_links" ]; then
    echo "Installed runtime has dangling library links:" >&2
    printf '%s\n' "$dangling_links" >&2
    exit 1
fi

python3 "$repo/packaging/tests/flow-helper-smoke.py" "$helper"

if ! find "$relocated/lib/clang" -path '*/include/stddef.h' -print -quit | grep -q .; then
    echo "Installed Clang resource headers were not relocated with the application." >&2
    exit 1
fi
