#!/usr/bin/env python3
"""Save the Jornada 720 VM to a file when it is asked to shut down.

run.sh starts QEMU with -action shutdown=pause and a QMP socket, then runs
this. Quitting from the window (Ctrl+Alt+Q, closing it) makes QEMU emit
SHUTDOWN and pause instead of exiting; SIGTERM to this script does the
same (a signal to QEMU itself always forces it to exit unsaved). We then
migrate the whole machine into STATE (via STATE.tmp, renamed only once
complete) and tell QEMU to quit.

    j720-save.py [--incoming] QMP_SOCKET STATE

With --incoming, QEMU was started with -incoming file:STATE; exit status
3 means it went away before the state finished loading.
"""
import json
import os
import signal
import socket
import sys
import time


class SaveRequest(Exception):
    pass


def on_sigterm(signum, frame):
    raise SaveRequest()


class QMP:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(path)
        self.buf = b""
        self.events = []
        self.read()                     # greeting
        self.cmd("qmp_capabilities")

    def read(self):
        while b"\n" not in self.buf:
            data = self.sock.recv(65536)
            if not data:
                raise EOFError("QEMU closed the QMP connection")
            self.buf += data
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def cmd(self, name, **args):
        msg = {"execute": name}
        if args:
            msg["arguments"] = args
        self.sock.sendall(json.dumps(msg).encode() + b"\n")
        while True:
            r = self.read()
            if "event" in r:
                self.events.append(r)
            elif "error" in r:
                raise RuntimeError("%s: %s" % (name, r["error"]["desc"]))
            else:
                return r["return"]

    def wait_event(self, name):
        while True:
            while self.events:
                if self.events.pop(0)["event"] == name:
                    return
            r = self.read()
            if "event" in r:
                self.events.append(r)


def connect(path):
    for _ in range(100):
        try:
            return QMP(path)
        except (FileNotFoundError, ConnectionRefusedError):
            time.sleep(0.1)
    return QMP(path)


def main():
    args = sys.argv[1:]
    incoming = args[:1] == ["--incoming"]
    if incoming:
        args = args[1:]
    path, state = args
    tmp = state + ".tmp"
    try:
        qmp = connect(path)
    except (EOFError, OSError) as e:
        print("j720-save: %s" % e, file=sys.stderr)
        return 3 if incoming else 1
    if incoming:
        try:
            while qmp.cmd("query-migrate").get("status") != "completed":
                time.sleep(0.1)
        except (EOFError, OSError, RuntimeError) as e:
            print("j720-save: resume failed: %s" % e, file=sys.stderr)
            return 3
    signal.signal(signal.SIGTERM, on_sigterm)
    try:
        qmp.wait_event("SHUTDOWN")
    except SaveRequest:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        qmp.cmd("stop")
    except (EOFError, OSError) as e:
        print("j720-save: %s, nothing saved" % e, file=sys.stderr)
        return 1
    signal.signal(signal.SIGTERM, signal.SIG_IGN)

    print("j720-save: saving to %s" % state, file=sys.stderr)
    ok = False
    try:
        qmp.cmd("migrate", uri="file:" + tmp)
        while True:
            status = qmp.cmd("query-migrate").get("status")
            if status == "completed":
                ok = True
                break
            if status in ("failed", "cancelled"):
                print("j720-save: migration %s" % status, file=sys.stderr)
                break
            time.sleep(0.1)
    except RuntimeError as e:
        print("j720-save: %s" % e, file=sys.stderr)
    if ok:
        os.replace(tmp, state)
    elif os.path.exists(tmp):
        os.unlink(tmp)
    try:
        qmp.cmd("quit")
    except (EOFError, OSError):
        pass
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
