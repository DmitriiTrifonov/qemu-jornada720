# qemu-jornada720

[**English**](README.md) · [Русский](README.ru.md)

An **HP Jornada 720** (Handheld PC, StrongARM SA-1110) machine for **QEMU**:
the real **Windows CE (H/PC 2000)** ROM boots to the desktop, with working
display, keyboard and touchscreen.

![CE desktop in the emulator](docs/img/desktop-clean.png)

A local experiment, not meant for QEMU upstream (see `src/hw/arm/jornada720.c`).

## What works

- Booting the real ROM to the desktop: ~50 s (~25 s with `--fast`).
- CE time (tick, timers, clock) runs in step with the host.
- 640x240 display: frame buffer and 2D BitBLT engine of the Epson S1D13806 controller.
- Keyboard and touchscreen via the MCU on the SSP. The stock CE calibration works.
- Batteries read as "full" to CE, no warnings.
- Networking: an NE2000-compatible Ethernet card sits in the PC Card slot,
  attached to QEMU's built-in network (NAT, DHCP, DNS; no root needed).
  CE finds the card by itself and gets the address 10.0.2.15. HTTP sites open
  in Pocket IE; HTTPS does not: modern servers reject the old TLS from CE.
- Web via FrogFind: `./frogfind-setup.sh` installs a local copy of
  [FrogFind](https://github.com/ActionRetro/FrogFind) (search through
  DuckDuckGo and a "reader" that simplifies any page, HTTPS included), and
  `run.sh` starts it together with the emulator. In Pocket IE:
  `http://10.0.2.2:8720/`. Requires composer and PHP with dom, xml, gd, intl.
- Russian input: switch the layout on the phone (in sxmo/sway, Caps) and
  type; the emulator enters the characters into CE as Alt + character code
  (the CE keyboard driver supports this). Cyrillic is rendered with the ROM
  fonts; CE menus stay in English.
- Files between the phone and CE: everything in `~/jornada-files` can be
  downloaded in Pocket IE from `http://10.0.2.2:8720/files/`, and the form at
  the bottom of the same page ("Browse…" → "Upload") puts a file from CE
  into that folder (only PHP is needed).
- State persists between runs: on exit the machine is suspended to a file,
  and on start it is restored in ~1 s. The CE registry, calibration and files
  live in RAM (battery-backed on the real device), so without this every
  start would be a hard reset.

Not implemented: sound, IrDA, the CF slot (empty), USB. The emulation needs
`-icount`, so it is not fast. Details and open items are in `docs/plan.md`.

## Running

```
./run.sh              # SDL window, CE time tracks the host; cold boot ~50 s
./run.sh --fast       # cold boot ~25 s, but the CE tick runs fast (up to ~16x)
./run.sh --vnc        # no window, VNC on localhost:5900
./run.sh --fresh      # discard the saved state, cold boot
./run.sh -- <args>    # extra QEMU arguments, e.g. -full-screen
```

- The picture is stretched to the whole window with "sharp bilinear":
  integer upscaling without smoothing, the remainder smoothly.
  `QEMU_SDL_SCALE=integer ./run.sh` uses integer factors only (perfectly
  crisp, with borders), `=linear` is soft.
- Mouse or finger = stylus. The keyboard goes to CE.
- **Ctrl+Alt+Q**, closing the window, or **Start → Suspend** in CE quits
  with a save; the next start resumes CE where it left off
  (`J720_SUSPEND_QUITS=0` makes Suspend just put CE to sleep until a key
  press or touch).
  **Ctrl+Alt+F** toggles full screen.
- Saved state: `~/.local/state/qemu-jornada720/j720.state` (~38 MB). If QEMU
  cannot load it, the file is renamed to `j720.state.bad` and the machine
  cold-boots.
- To stop the emulator from outside with a save, send SIGTERM to the
  `j720-save.py` process. SIGTERM to QEMU itself closes it **without** saving.
- `J720_TOUCH_DEBUG=1` logs touches to stderr: window coordinates, screen
  pixels, ADC values.

A Jornada 720 ROM dump is required at `roms/jornada720.bin` (32 MiB, not
included in git).

## Building

QEMU is built from source with our patches:

```
git clone https://gitlab.com/qemu-project/qemu.git qemu-src
ln -s ../../../src/hw/arm/jornada720.c qemu-src/hw/arm/jornada720.c
cd qemu-src && for p in ../patches/0*.patch; do git apply "$p"; done && cd ..
mkdir build && cd build && ../qemu-src/configure --target-list=arm-softmmu
ninja qemu-system-arm
```

The window needs SDL2 and networking needs libslirp (dev packages, before
`configure`). What each patch does is described in `patches/README.md`.

## Layout

- `src/hw/arm/jornada720.c`: the board model: memory, Epson, MCU, stubs.
- `patches/`: QEMU changes: StrongARM core, SA-1110, SDL frontend.
- `run.sh`, `j720-save.py`: launching, plus save and restore over QMP.
- `docs/plan.md`: plan and work status; `docs/research.md`: hardware
  analysis and how each problem was tracked down.

## License

GPL-2.0-or-later, the same as QEMU, which this project extends and patches. See `LICENSE`.
The Jornada 720 ROM is not included and remains the property of its owners.
