#!/usr/bin/env bash
# build.sh: cross-compile the Windows build on Linux into win/build/out/:
#   curtimefix.exe   MinGW-w64 (the launcher)
#   curtimefix.dll   clang-cl + xwin's MSVC CRT/SDK (MSVC ABI: it implements a game interface, ConCommand)
# Needs clang, lld, mingw-w64-gcc, an xwin splat in ~/.local/share/xwin (or XWIN=) and the Source SDK headers in
# third_party/source-sdk-2013 (see README). Same toolchain as CS:TV's Windows build.
# Run it from a path without spaces.
set -euo pipefail
W=$(dirname "$0")
C=$(cd "$W/.." && pwd -L)
W=$C/win
X=${XWIN:-$HOME/.local/share/xwin}
B=$W/build
OUT=$B/out
mkdir -p "$OUT" "$B/obj"

echo "== curtimefix.exe"
x86_64-w64-mingw32-gcc -O2 -Wall -municode -mwindows -o "$OUT/curtimefix.exe" "$W/launcher.c" -static -ladvapi32 -lshell32 -luser32

echo "== curtimefix.dll"
SDK=$C/third_party/source-sdk-2013/src
MSVC="--target=x86_64-pc-windows-msvc -nologo -imsvc $X/crt/include -imsvc $X/sdk/include/ucrt -imsvc $X/sdk/include/um -imsvc $X/sdk/include/shared"
LIBS="-libpath:$X/crt/lib/x86_64 -libpath:$X/sdk/lib/um/x86_64 -libpath:$X/sdk/lib/ucrt/x86_64"
CF="$MSVC -O2 -MT -EHs-c- -GR- /clang:-ffp-contract=off -D_WIN32 -DWIN32 -D_WINDOWS -D_WIN64 -DCOMPILER_MSVC \
    -DCOMPILER_MSVC64 -DCLIENT_DLL -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX"
SDKF="-Wno-everything -I$W/sdk-compat -I$SDK/public -I$SDK/public/tier0 -I$SDK/public/tier1 -I$SDK/common \
    -FI$W/sdk-compat/tier1/bitbuf.h -FI$W/sdk-compat/mathlib/ssemath.h"
for f in fix find plat_win; do clang-cl $CF -W3 -c "$C/src/$f.cpp" -Fo"$B/obj/$f.obj"; done
clang-cl $CF $SDKF -c "$C/src/cmd.cpp" -Fo"$B/obj/cmd.obj"
lld-link -nologo -dll -out:"$OUT/curtimefix.dll" $LIBS "$B"/obj/{fix,find,plat_win,cmd}.obj \
    libcmt.lib libvcruntime.lib libucrt.lib kernel32.lib
ls -la "$OUT"
