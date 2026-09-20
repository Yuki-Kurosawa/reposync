#!/usr/bin/env bash
# Build reposync-gui.exe (SDL3 + libssh2) with MSYS2 and stage the runtime DLLs.
#
# Usage (from MSYS2 bash, or Windows with D:\msys64\usr\bin\bash.exe -lc):
#   bash win-build.sh
#
# Prereqs:  pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-sdl3 \
#                     mingw-w64-x86_64-sdl3-ttf mingw-w64-x86_64-libssh2
set -uo pipefail
export PATH=/mingw64/bin:$PATH
cd "$(dirname "$0")"

echo "=== build (SDL3 + libssh2) ==="
make -f Makefile.win clean > /dev/null 2>&1
make -f Makefile.win LIBSSH2_ROOT=/mingw64 CC=gcc
rc=$?
if [ $rc -ne 0 ]; then
    echo "build failed rc=$rc"
    exit $rc
fi

echo "=== staging runtime DLLs ==="
for d in SDL3 SDL3_ttf libssh2-1 libfreetype-6 libharfbuzz-0 libgraphite2 \
         libglib-2.0-0 libiconv-2 libintl-8 libbrotlidec libbrotlicommon \
         libbz2-1 libpng16-16 libcrypto-3-x64 zlib1 libstdc++-6 \
         libwinpthread-1 libgcc_s_seh-1 libsqlite3-0; do
    if [ -f /mingw64/bin/$d.dll ]; then
        cp -f /mingw64/bin/$d.dll . && echo "  staged $d.dll"
    else
        echo "  (skip) $d.dll not in /mingw64/bin"
    fi
done
echo "done: reposync-gui.exe + DLLs in $(pwd)"
