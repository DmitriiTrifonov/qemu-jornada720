#!/bin/sh
# Start the HP Jornada 720 emulator in an SDL window.
#
#   ./run.sh              window (SDL), fast boot (~25 s to the desktop)
#   ./run.sh --realtime   guest clock runs at real speed, boot ~50 s
#   ./run.sh --vnc        no window, VNC on localhost:5900
#   ./run.sh --fresh      forget the saved state, cold boot (hard reset)
#   ./run.sh -- <args>    pass extra arguments to QEMU
#
# The machine is suspended to a file on quit and resumed from it on the
# next start, like the real battery-backed device: CE keeps its registry
# (touch calibration included) and files in RAM. State lives in
# ${XDG_STATE_HOME:-~/.local/state}/qemu-jornada720/j720.state; a state
# QEMU cannot load is moved to j720.state.bad and the machine cold boots.
#
# Mouse = stylus (left button = pen down), keyboard goes to CE.
# Ctrl+Alt+Q quits, Ctrl+Alt+F toggles fullscreen.
# -icount is required (see docs/research.md). With the default shift=6
# the CE clock runs ahead of real time while CE is busy (boot); on an
# idle desktop it was measured at about real speed. shift=auto keeps it
# in sync at the cost of a slower boot.
set -e

cd "$(dirname "$0")"

QEMU=./build/qemu-system-arm
ROM=roms/jornada720.bin
STATE_DIR=${XDG_STATE_HOME:-$HOME/.local/state}/qemu-jornada720
STATE=$STATE_DIR/j720.state

[ -x "$QEMU" ] || { echo "no $QEMU, build it first: ninja -C build qemu-system-arm" >&2; exit 1; }
[ -f "$ROM" ] || { echo "no ROM image at $ROM" >&2; exit 1; }

icount="shift=6,align=off"
display="-display sdl"
while [ $# -gt 0 ]; do
    case "$1" in
        --realtime) icount="shift=auto,align=off" ;;
        --vnc) display="-display none -vnc 127.0.0.1:0" ;;
        --fresh) rm -f "$STATE" ;;
        --) shift; break ;;
        *) break ;;
    esac
    shift
done

mkdir -p "$STATE_DIR"
QMP_SOCK=${XDG_RUNTIME_DIR:-/tmp}/j720-qmp.$$.sock

# run_qemu [--incoming] <QEMU args>: QEMU pauses on shutdown instead of
# exiting, j720-save.py then saves the machine and quits it. Returns 3
# if the saved state could not be loaded.
run_qemu() {
    save_opt=
    if [ "$1" = "--incoming" ]; then
        save_opt=--incoming
        shift
        set -- -incoming file:"$STATE" "$@"
    fi
    rm -f "$QMP_SOCK"
    "$QEMU" -M jornada720 \
        -drive if=pflash,format=raw,file="$ROM" \
        -icount "$icount" \
        -qmp unix:"$QMP_SOCK",server=on,wait=off \
        -action shutdown=pause \
        -global migration.store-global-state=off \
        $display "$@" &
    qemu_pid=$!
    save_status=0
    python3 ./j720-save.py $save_opt "$QMP_SOCK" "$STATE" || save_status=$?
    status=0
    wait $qemu_pid || status=$?
    rm -f "$QMP_SOCK"
    [ $save_status -eq 3 ] && return 3
    return $status
}

if [ -f "$STATE" ]; then
    status=0
    run_qemu --incoming "$@" || status=$?
    [ $status -eq 3 ] || exit $status
    echo "run.sh: could not resume from $STATE, moved to $STATE.bad; cold boot" >&2
    mv -f "$STATE" "$STATE.bad"
fi
run_qemu "$@"
