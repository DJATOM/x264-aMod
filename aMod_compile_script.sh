#!/bin/sh

cd "$(dirname "$0")" >/dev/null && [ -f x264.h ] || exit 1

api="$(grep '#define X264_BUILD' < x264.h | sed 's/^.* \([1-9][0-9]*\).*$/\1/')"
ver="x"
version=""

if [ -d .git ] && command -v git >/dev/null 2>&1 ; then
    localver="$(($(git rev-list HEAD | wc -l)))"
    if [ "$localver" -gt 1 ] ; then
        ver_diff="$(($(git rev-list origin/master..HEAD | wc -l)))"
        ver="$((localver-ver_diff))"
        if [ "$ver_diff" -ne 0 ] ; then
            ver="$ver+$ver_diff"
        fi
        if git status | grep -q "modified:" ; then
            ver="${ver}M"
        fi
        version="r$ver"
    fi
fi



function compile_x264 {
    if [ -n "$1" ]; then
        opt=" -march=${1}"
        name="-opt-${1}"
    else
        opt=""
        name="-generic"
    fi
 
    ./configure --enable-pic --prefix=/usr --includedir=/usr/include --enable-static --extra-cflags="-I/usr/include${opt}" --extra-ldflags=-L/usr/lib --host=x86_64-pc-mingw64
    make clean
    make -j24
    strip x264.exe
    upx -9 x264.exe
    mv x264.exe "x264-aMod-x64-core${api}-${version}${name}.exe"
}

compile_x264 "znver2"
compile_x264 "znver3"
compile_x264 "znver1"
compile_x264 "sandybridge"
compile_x264 "haswell"
compile_x264 "skylake"
compile_x264 "nehalem"
compile_x264
