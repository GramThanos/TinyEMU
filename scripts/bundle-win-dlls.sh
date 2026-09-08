#!/bin/sh
# Copy the MinGW runtime DLLs a cross-built .exe needs into its output directory,
# resolving transitive dependencies from the MinGW sysroot. This makes the .exe
# runnable on a plain Windows host without a MinGW installation.
#
# Usage: bundle-win-dlls.sh <exe> <out_dir>
# Env:   OBJDUMP            (default: x86_64-w64-mingw32-objdump)
#        MINGW_SYSROOT_BIN  (default: /usr/x86_64-w64-mingw32/sys-root/mingw/bin)
set -e

EXE="$1"
OUT="$2"
if [ -z "$EXE" ] || [ -z "$OUT" ]; then
    echo "usage: $0 <exe> <out_dir>" >&2
    exit 2
fi

OBJDUMP="${OBJDUMP:-x86_64-w64-mingw32-objdump}"
SYSROOT_BIN="${MINGW_SYSROOT_BIN:-/usr/x86_64-w64-mingw32/sys-root/mingw/bin}"

resolve() {
    # Print the DLL names $1 directly imports.
    "$OBJDUMP" -p "$1" 2>/dev/null | awk '/DLL Name:/ { print $3 }'
}

# Breadth-first walk of the dependency graph. DLLs present in the MinGW sysroot
# are bundled; anything else (kernel32.dll, ws2_32.dll, ...) is a Windows system
# library and left to the OS.
todo="$EXE"
seen=""
copied=0
while [ -n "$todo" ]; do
    next=""
    for f in $todo; do
        for dll in $(resolve "$f"); do
            case " $seen " in *" $dll "*) continue ;; esac
            seen="$seen $dll"
            if [ -f "$SYSROOT_BIN/$dll" ]; then
                cp -f "$SYSROOT_BIN/$dll" "$OUT/"
                copied=$((copied + 1))
                next="$next $OUT/$dll"
            fi
        done
    done
    todo="$next"
done

echo "bundled $copied MinGW DLL(s) into $OUT"
