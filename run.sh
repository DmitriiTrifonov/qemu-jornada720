#!/bin/sh
# Start the HP Jornada 720 emulator in an SDL window.
#
#   ./run.sh              window (SDL)
#   ./run.sh --vnc        no window, VNC on localhost:5900
#   ./run.sh -- <args>    pass extra arguments to QEMU
#
# Mouse = stylus (left button = pen down), keyboard goes to CE.
# Boot takes a few minutes; -icount is required (see docs/research.md).
set -e

cd "$(dirname "$0")"

QEMU=./build/qemu-system-arm
ROM=roms/jornada720.bin

[ -x "$QEMU" ] || { echo "no $QEMU, build it first: ninja -C build qemu-system-arm" >&2; exit 1; }
[ -f "$ROM" ] || { echo "no ROM image at $ROM" >&2; exit 1; }

display="-display sdl"
case "$1" in
    --vnc) display="-display none -vnc 127.0.0.1:0"; shift ;;
esac
[ "$1" = "--" ] && shift

exec "$QEMU" -M jornada720 \
    -drive if=pflash,format=raw,file="$ROM" \
    -icount shift=3,align=off \
    $display "$@"
