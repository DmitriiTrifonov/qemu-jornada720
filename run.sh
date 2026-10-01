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
#   ./run.sh --cf PATH    put a CF card in the slot: a disk image, or a
#                         host directory synced with it; --cf none empties it
#   ./run.sh -- <args>    pass extra arguments to QEMU
#
# The machine is suspended to a file on quit and resumed from it on the
# next start, like the real battery-backed device: CE keeps its registry
# (touch calibration included) and files in RAM. State lives in
# ${XDG_STATE_HOME:-~/.local/state}/qemu-jornada720/j720.state; a state
# QEMU cannot load is moved to j720.state.bad and the machine cold boots.
# CE's Start > Suspend saves and quits the same way; the next start wakes
# CE where it went to sleep (J720_SUSPEND_QUITS=0: stay open, asleep
# until a key or a tap).
#
# With PHP installed, a web server runs for the emulator's lifetime at
# http://10.0.2.2:8720/ (the host as seen from CE; bound to 127.0.0.1
# only): FrogFind, if installed (./frogfind-setup.sh) -- search and
# simplified pages for Pocket IE, which cannot do modern HTTPS -- and
# /files/, the host folder ~/jornada-files (or $J720_FILES) for
# downloading into CE. Its log: $STATE_DIR/frogfind.log.
#
# The CF card stays in the slot across runs, like in the real device: its
# path is kept in $STATE_DIR/cf. CE mounts it as \Storage Card. A
# directory's card is built from it at start and CE's changes are copied
# back when the emulator quits.
# j720-cf.py swaps cards while the emulator runs (through the control
# QMP socket $XDG_RUNTIME_DIR/j720-ctl.sock) and makes card images.
#
# The window is filled with the 640x240 screen scaled "sharp bilinear"
# (nearest by a whole factor, then linear for the rest).
# QEMU_SDL_SCALE=integer keeps whole factors only (crisp, with borders),
# =linear smooths, =nearest is QEMU's old uneven scaling. All but nearest
# draw at the screen's full resolution.
#
# Mouse = stylus (left button = pen down), keyboard goes to CE.
# Ctrl+Alt+Q quits, Ctrl+Alt+F toggles fullscreen.
# -icount is required (see docs/research.md). shift=auto keeps QEMU's
# virtual time, and with it CE's tick (GetTickCount, timers, games), in
# step with the host; it settles ~20 s after a start. With --fast
# (shift=6) CE's tick was measured 16x fast in Solitaire. CE's clock
# on the taskbar comes from the RTC, which follows the host in both.
set -e

start_dir=$PWD
cd "$(dirname "$0")"

QEMU=./build/qemu-system-arm
ROM=roms/jornada720.bin
STATE_DIR=${XDG_STATE_HOME:-$HOME/.local/state}/qemu-jornada720
STATE=$STATE_DIR/j720.state
CF_CARD=$STATE_DIR/cf

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
        --cf)
            [ $# -ge 2 ] || { echo "run.sh: --cf needs a path or none" >&2; exit 1; }
            shift
            mkdir -p "$STATE_DIR"
            case "$1" in
                none) rm -f "$CF_CARD" ;;
                *)  card=$1
                    case "$card" in /*) ;; *) card=$start_dir/$card ;; esac
                    [ -e "$card" ] || { echo "run.sh: no CF card at $card" >&2; exit 1; }
                    echo "$card" > "$CF_CARD" ;;
            esac
            ;;
        --) shift; break ;;
        *) break ;;
    esac
    shift
done

mkdir -p "$STATE_DIR"
QEMU_SDL_SCALE=${QEMU_SDL_SCALE:-sharp}
J720_SUSPEND_QUITS=${J720_SUSPEND_QUITS:-1}
export QEMU_SDL_SCALE J720_SUSPEND_QUITS

# Web server for CE (see the top): FrogFind, if installed, and /files/.
# DuckDuckGo turns away PHP's default (empty) User-Agent as a bot.
FROGFIND_DIR=${XDG_DATA_HOME:-$HOME/.local/share}/qemu-jornada720/frogfind
FROGFIND_UA="Mozilla/5.0 (X11; Linux aarch64; rv:128.0) Gecko/20100101 Firefox/128.0"
J720_FILES=${J720_FILES:-$HOME/jornada-files}
export J720_FILES
php=$(command -v php85 || command -v php || true)
if [ -n "$php" ]; then
    mkdir -p "$J720_FILES"
    docroot=$FROGFIND_DIR
    [ -f "$FROGFIND_DIR/vendor/autoload.php" ] || docroot=$J720_FILES
    PHP_CLI_SERVER_WORKERS=4 "$php" -d "user_agent=\"$FROGFIND_UA\"" \
        -d upload_max_filesize=256M -d post_max_size=256M \
        -S 127.0.0.1:8720 -t "$docroot" j720-router.php \
        > "$STATE_DIR/frogfind.log" 2>&1 &
    frogfind_pid=$!
    trap 'kill $frogfind_pid 2>/dev/null' EXIT
    trap 'exit 1' INT TERM HUP
fi

QMP_SOCK=${XDG_RUNTIME_DIR:-/tmp}/j720-qmp.$$.sock
CTL_SOCK=${XDG_RUNTIME_DIR:-/tmp}/j720-ctl.sock

# The CF slot: a drive "cf", empty or with the card left in it (for a
# directory, its freshly built image). A path goes into -drive with its
# commas doubled.
cf_drive=if=none,id=cf
cf_file=$(python3 ./j720-cf.py prepare) || cf_file=
if [ -n "$cf_file" ]; then
    cf_drive=$cf_drive,format=raw,file=$(printf '%s' "$cf_file" | sed 's/,/,,/g')
fi

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
        -qmp unix:"$CTL_SOCK",server=on,wait=off \
        -drive "$cf_drive" \
        -action shutdown=pause \
        -global migration.store-global-state=off \
        $display "$@" &
    qemu_pid=$!
    save_status=0
    python3 ./j720-save.py $save_opt "$QMP_SOCK" "$STATE" || save_status=$?
    status=0
    wait $qemu_pid || status=$?
    rm -f "$QMP_SOCK" "$CTL_SOCK"
    [ $save_status -eq 3 ] && return 3
    return $status
}

status=0
if [ -f "$STATE" ]; then
    run_qemu --incoming "$@" || status=$?
fi
if [ ! -f "$STATE" ] || [ $status -eq 3 ]; then
    if [ $status -eq 3 ]; then
        echo "run.sh: could not resume from $STATE, moved to $STATE.bad; cold boot" >&2
        mv -f "$STATE" "$STATE.bad"
    fi
    status=0
    run_qemu "$@" || status=$?
fi
# what CE changed on a directory's card goes back into the directory
python3 ./j720-cf.py sync || true
exit $status
