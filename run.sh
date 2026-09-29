#!/bin/sh
# Start the HP Jornada 720 emulator in an SDL window.
#
#   ./run.sh              window (SDL), CE time runs at real speed;
#                         cold boot ~50 s, resume ~1 s
#   ./run.sh --fast       CE time runs ~16x fast while CE is busy; cold
#                         boot ~25 s (--realtime: the default, kept for
#                         old callers)
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
# If FrogFind is installed (./frogfind-setup.sh), it is served for the
# emulator's lifetime at http://10.0.2.2:8720/ (the host as seen from CE;
# bound to 127.0.0.1 only): search and simplified pages for Pocket IE,
# which cannot do modern HTTPS. Its log: $STATE_DIR/frogfind.log.
#
# Mouse = stylus (left button = pen down), keyboard goes to CE.
# Ctrl+Alt+Q quits, Ctrl+Alt+F toggles fullscreen.
# -icount is required (see docs/research.md). shift=auto keeps QEMU's
# virtual time, and with it CE's tick (GetTickCount, timers, games), in
# step with the host; it settles ~20 s after a start. With --fast
# (shift=6) CE's tick was measured 16x fast in Solitaire. CE's clock
# on the taskbar comes from the RTC, which follows the host in both.
set -e

cd "$(dirname "$0")"

QEMU=./build/qemu-system-arm
ROM=roms/jornada720.bin
STATE_DIR=${XDG_STATE_HOME:-$HOME/.local/state}/qemu-jornada720
STATE=$STATE_DIR/j720.state

[ -x "$QEMU" ] || { echo "no $QEMU, build it first: ninja -C build qemu-system-arm" >&2; exit 1; }
[ -f "$ROM" ] || { echo "no ROM image at $ROM" >&2; exit 1; }

icount="shift=auto,align=off"
display="-display sdl"
while [ $# -gt 0 ]; do
    case "$1" in
        --realtime) icount="shift=auto,align=off" ;;
        --fast) icount="shift=6,align=off" ;;
        --vnc) display="-display none -vnc 127.0.0.1:0" ;;
        --fresh) rm -f "$STATE" ;;
        --) shift; break ;;
        *) break ;;
    esac
    shift
done

mkdir -p "$STATE_DIR"

# DuckDuckGo turns away PHP's default (empty) User-Agent as a bot
FROGFIND_DIR=${XDG_DATA_HOME:-$HOME/.local/share}/qemu-jornada720/frogfind
FROGFIND_UA="Mozilla/5.0 (X11; Linux aarch64; rv:128.0) Gecko/20100101 Firefox/128.0"
php=$(command -v php85 || command -v php || true)
if [ -n "$php" ] && [ -f "$FROGFIND_DIR/vendor/autoload.php" ]; then
    PHP_CLI_SERVER_WORKERS=4 "$php" -d "user_agent=\"$FROGFIND_UA\"" \
        -S 127.0.0.1:8720 -t "$FROGFIND_DIR" > "$STATE_DIR/frogfind.log" 2>&1 &
    frogfind_pid=$!
    trap 'kill $frogfind_pid 2>/dev/null' EXIT
    trap 'exit 1' INT TERM HUP
fi

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
