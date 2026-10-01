#!/usr/bin/env python3
"""Put a CompactFlash card into the Jornada 720's CF slot, or take it out.

    j720-cf.py insert IMAGE|DIR   card from a disk image, or a host
                                  directory synced with the card
    j720-cf.py eject              empty the slot
    j720-cf.py status             what is in the slot
    j720-cf.py new IMAGE [DIR]    make a FAT16 card image (~500 MB, sparse),
                                  with the files of DIR on it if given

CE mounts the card as \\Storage Card. With the emulator running (run.sh),
the card goes in or out right away, through run.sh's control QMP socket;
the slot's content is also remembered for the next ./run.sh, like a card
left in the device. Swapping cards takes the old one out first and waits
a little, so CE sees the slot empty in between.

A directory becomes a card image of its own (in the state directory),
built from it when the card goes in and at every start of the emulator.
What CE changes on the card (new, changed, deleted files and folders) is
copied back into the directory when the card comes out and when the
emulator quits. A file changed on both sides is kept as it is in the
directory, and CE's version is put next to it as "NAME (Jornada).EXT".
Run by run.sh:

    j720-cf.py prepare            image file for the slot (prints it)
    j720-cf.py sync               copy CE's changes back into the directory

To get files out of an image card: mtools (mcopy -i IMAGE@@32256 ::FILE .)
or a loop mount, with the card out of the slot or the emulator stopped.
"""
import hashlib
import importlib.util
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
STATE_DIR = os.path.join(os.environ.get("XDG_STATE_HOME") or
                         os.path.expanduser("~/.local/state"),
                         "qemu-jornada720")
CARD_FILE = os.path.join(STATE_DIR, "cf")
SYNC_IMAGE = os.path.join(STATE_DIR, "cf-sync.img")
SYNC_INFO = os.path.join(STATE_DIR, "cf-sync.json")
CTL_SOCK = os.path.join(os.environ.get("XDG_RUNTIME_DIR") or "/tmp",
                        "j720-ctl.sock")
SWAP_PAUSE = 3      # seconds with the slot empty when swapping cards

sys.dont_write_bytecode = True      # no __pycache__ next to the scripts
spec = importlib.util.spec_from_file_location(
    "j720_save", os.path.join(HERE, "j720-save.py"))
j720_save = importlib.util.module_from_spec(spec)
spec.loader.exec_module(j720_save)


class FAT16:
    """Read-only view of the first FAT16 partition of a card image."""

    def __init__(self, path):
        with open(path, "rb") as f:
            self.img = f.read()
        img = self.img
        base = 0
        if img[0x1fe:0x200] == b"\x55\xaa" and img[0x1c2] in (4, 6, 0xe):
            base = struct.unpack("<I", img[0x1c6:0x1ca])[0] * 512
        (bps, self.spc, rsv, nfat, nroot, _, _, spf) = \
            struct.unpack("<HBHBHHBH", img[base + 11:base + 24])
        if not spf:
            raise ValueError("%s: not FAT12/16" % path)
        self.csize = bps * self.spc
        self.fat = base + rsv * bps
        self.root = self.fat + nfat * spf * bps
        self.data = self.root + nroot * 32

    def chain(self, c):
        while 2 <= c < 0xfff8:
            yield c
            c = struct.unpack("<H", self.img[self.fat + 2 * c:
                                             self.fat + 2 * c + 2])[0]

    def read(self, c, size=None):
        out = b"".join(self.img[self.data + (x - 2) * self.csize:
                                self.data + (x - 1) * self.csize]
                       for x in self.chain(c))
        return out if size is None else out[:size]

    def entries(self, raw):
        """(name, is_dir, first cluster, size, mtime) of a directory."""
        lfn = {}
        for i in range(0, len(raw), 32):
            e = raw[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xe5:
                lfn = {}
                continue
            if e[11] == 0x0f:
                part = (e[1:11] + e[14:26] + e[28:32]).decode("utf-16-le")
                lfn[e[0] & 0x1f] = part.split("\0")[0]
                continue
            if e[11] & 0x08:                # volume label
                lfn = {}
                continue
            if lfn:
                name = "".join(lfn[k] for k in sorted(lfn))
            else:
                base = e[0:8].decode("cp1252").rstrip()
                ext = e[8:11].decode("cp1252").rstrip()
                base = base.lower() if e[12] & 0x08 else base
                ext = ext.lower() if e[12] & 0x10 else ext
                name = base + ("." + ext if ext else "")
            lfn = {}
            if name in (".", ".."):
                continue
            t, d = struct.unpack("<HH", e[22:26])
            try:
                mtime = time.mktime((1980 + (d >> 9), (d >> 5) & 15, d & 31,
                                     t >> 11, (t >> 5) & 63, (t & 31) * 2,
                                     0, 0, -1))
            except (OverflowError, ValueError):
                mtime = None
            yield (name, bool(e[11] & 0x10), struct.unpack("<H", e[26:28])[0],
                   struct.unpack("<I", e[28:32])[0], mtime)

    def walk(self):
        """{relative path: (data or None for a directory, mtime)}"""
        out = {}

        def visit(raw, prefix):
            for name, is_dir, c, size, mtime in self.entries(raw):
                path = prefix + name
                if is_dir:
                    out[path] = (None, mtime)
                    visit(self.read(c), path + "/")
                else:
                    out[path] = (self.read(c, size), mtime)
        visit(self.img[self.root:self.data], "")
        return out


def digest(data):
    return "dir" if data is None else hashlib.sha1(data).hexdigest()


def host_digest(path):
    if os.path.isdir(path):
        return "dir"
    try:
        with open(path, "rb") as f:
            return hashlib.sha1(f.read()).hexdigest()
    except OSError:
        return None


def card():
    try:
        with open(CARD_FILE) as f:
            return f.read().strip() or None
    except FileNotFoundError:
        return None


def build(directory):
    """Card image of a directory, and what was on it then."""
    if os.path.exists(SYNC_IMAGE):
        os.unlink(SYNC_IMAGE)
    new(SYNC_IMAGE, directory, quiet=True)
    tree = FAT16(SYNC_IMAGE).walk()
    with open(SYNC_INFO, "w") as f:
        json.dump({"dir": directory,
                   "files": {p: digest(d) for p, (d, _) in tree.items()}}, f)


def sync():
    """Copy what CE changed on a directory's card back into it."""
    try:
        with open(SYNC_INFO) as f:
            info = json.load(f)
    except FileNotFoundError:
        return
    directory, before = info["dir"], info["files"]
    if not os.path.exists(SYNC_IMAGE) or not os.path.isdir(directory):
        return
    now = FAT16(SYNC_IMAGE).walk()
    done = []
    for path in sorted(now):                    # parents before children
        data, mtime = now[path]
        if before.get(path) == digest(data):
            continue
        dest = os.path.join(directory, path)
        if data is None:
            os.makedirs(dest, exist_ok=True)
            continue
        on_host = host_digest(dest)
        if on_host == digest(data):
            continue
        if on_host is not None and on_host != before.get(path):
            stem, ext = os.path.splitext(dest)  # changed on both sides
            dest = "%s (Jornada)%s" % (stem, ext)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as f:
            f.write(data)
        if mtime:
            os.utime(dest, (mtime, mtime))
        done.append("+" + os.path.relpath(dest, directory))
    for path in sorted(set(before) - set(now), reverse=True):
        dest = os.path.join(directory, path)
        if before[path] == "dir":
            try:
                os.rmdir(dest)                  # only if left empty
            except OSError:
                continue
        elif host_digest(dest) == before[path]:
            os.unlink(dest)                     # unless changed on the host
        else:
            continue
        done.append("-" + path)
    info["files"] = {p: digest(d) for p, (d, _) in now.items()}
    with open(SYNC_INFO, "w") as f:
        json.dump(info, f)
    if done:
        print("j720-cf: synced into %s: %s" % (directory, " ".join(done)),
              file=sys.stderr)


def slot_file(path):
    """The image that goes into the slot for a card."""
    if os.path.isdir(path):
        build(path)
        return SYNC_IMAGE
    return path


def connect():
    """The running emulator's QMP, or None if it is not running."""
    try:
        return j720_save.QMP(CTL_SOCK)
    except OSError:
        return None


def medium(qmp):
    for dev in qmp.cmd("query-block"):
        if dev["device"] == "cf":
            return dev.get("inserted", {}).get("file")
    return None


def take_out(qmp):
    """Empty the slot and sync a directory's card back; True if a card
    was in the running emulator."""
    was_in = bool(qmp and medium(qmp))
    if was_in:
        qmp.cmd("eject", device="cf")   # QEMU lets go of the image
    sync()
    return was_in


def insert(path):
    path = os.path.abspath(path)
    if not os.path.exists(path):
        sys.exit("j720-cf: no such file or directory: %s" % path)
    qmp = connect()
    if take_out(qmp):
        time.sleep(SWAP_PAUSE)
    os.makedirs(STATE_DIR, exist_ok=True)
    with open(CARD_FILE, "w") as f:
        f.write(path + "\n")
    if qmp:
        qmp.cmd("blockdev-change-medium", device="cf",
                filename=slot_file(path), format="raw")
    print("j720-cf: %s %s" % ("inserted" if qmp else "will insert at the "
                              "next start:", path))


def eject():
    take_out(connect())
    if os.path.exists(CARD_FILE):
        os.unlink(CARD_FILE)
    print("j720-cf: slot empty")


def status():
    qmp = connect()
    path = card()
    if qmp:
        print("running, in the slot: %s" %
              ((path or "?") if medium(qmp) else "nothing"))
    print("for the next start: %s" % (path or "nothing"))


def prepare():
    """Before a start: the card's image, synced and rebuilt if needed."""
    sync()                      # a run that ended without syncing
    path = card()
    if not path:
        return
    if not os.path.exists(path):
        print("j720-cf: CF card %s is gone, the slot is empty" % path,
              file=sys.stderr)
        return
    print(slot_file(path))


def new(image, src=None, quiet=False):
    if os.path.exists(image):
        sys.exit("j720-cf: %s already exists" % image)
    qemu_img = os.path.join(HERE, "build", "qemu-img")
    if not os.access(qemu_img, os.X_OK):
        qemu_img = shutil.which("qemu-img")
    if not qemu_img:
        sys.exit("j720-cf: needs qemu-img (build/qemu-img or in PATH)")
    with tempfile.TemporaryDirectory() as empty:
        subprocess.run([qemu_img, "convert", "-f", "vvfat", "-O", "raw",
                        "fat:16:" + os.path.abspath(src or empty), image],
                       check=True)
    if not quiet:
        print("j720-cf: made %s" % image)


def main():
    args = sys.argv[1:]
    try:
        if args[:1] == ["insert"] and len(args) == 2:
            insert(args[1])
        elif args == ["eject"]:
            eject()
        elif args == ["status"]:
            status()
        elif args[:1] == ["new"] and len(args) in (2, 3):
            new(*args[1:])
        elif args == ["prepare"]:
            prepare()
        elif args == ["sync"]:
            sync()
        else:
            sys.exit(__doc__)
    except (RuntimeError, ValueError, subprocess.CalledProcessError) as e:
        sys.exit("j720-cf: %s" % e)


if __name__ == "__main__":
    main()
