#!/bin/sh
# Start the HP Jornada 720 emulator in an SDL window.
#
#   ./run.sh              window (SDL), fast boot (~25 s to the desktop)
#   ./run.sh --realtime   guest clock runs at real speed, boot ~50 s
#   ./run.sh --vnc        no window, VNC on localhost:5900
#   ./run.sh -- <args>    pass extra arguments to QEMU
# (--realtime and --vnc can be combined, in that order)
#
# Mouse = stylus (left button = pen down), keyboard goes to CE.
# -icount is required (see docs/research.md). With the default shift=6
# the CE clock runs roughly 12x faster than real time, because CE keeps
# the CPU busy even when it looks idle; shift=auto keeps it in sync.
set -e

cd "$(dirname "$0")"

QEMU=./build/qemu-system-arm
ROM=roms/jornada720.bin

[ -x "$QEMU" ] || { echo "no $QEMU, build it first: ninja -C build qemu-system-arm" >&2; exit 1; }
[ -f "$ROM" ] || { echo "no ROM image at $ROM" >&2; exit 1; }

icount="shift=6,align=off"
if [ "$1" = "--realtime" ]; then
    icount="shift=auto,align=off"
    shift
fi

display="-display sdl"
case "$1" in
    --vnc) display="-display none -vnc 127.0.0.1:0"; shift ;;
esac
[ "$1" = "--" ] && shift

exec "$QEMU" -M jornada720 \
    -drive if=pflash,format=raw,file="$ROM" \
    -icount "$icount" \
    $display "$@"
