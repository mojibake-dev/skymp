#!/usr/bin/env python3
"""A stand-in for the fakeclient whose login prints more event lines than a
pipe holds, for difftest's drain test. Rung 5's settings made every login
print about 91 KB of ScriptVariables (MsgType 81) lines, and Linux's pipe
holds 65,536 bytes (pipe(7)); this prints 2,048 lines of about 140 bytes,
292 KB in all.
Profile 1 then waits, up to twenty seconds, for profile 2 to have printed all
of its own: a driver that reads one client's stdout to its end before it
reads the next leaves profile 2 blocked on its full pipe, so profile 1 gives
up and exits 1. The clients meet through files beside their scripts (one
run's scripts share a directory); nothing here talks to a network."""
import json
import os
import sys
import time

LINES = 2048
WAIT_S = 20.0


def emit(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def main(argv):
    args = dict(zip(argv[1::2], argv[2::2]))
    profile = int(args.get("--profile-id", "1"))
    here = os.path.dirname(os.path.abspath(args.get("--script", ".")))
    emit({"event": "connected"})
    emit({"event": "actor", "idx": profile, "worldOrCell": 60, "pos": [0, 0, 0], "rot": [0, 0, 0]})
    for seq in range(LINES):
        emit({"event": "message", "msg": {"t": 81, "seq": seq, "pad": "x" * 80}})
    with open(os.path.join(here, "%d.printed" % profile), "w"):
        pass
    rc = 0
    if profile == 1:
        other = os.path.join(here, "2.printed")
        deadline = time.monotonic() + WAIT_S
        while not os.path.exists(other):
            if time.monotonic() > deadline:
                emit({"event": "error", "error": "profile 2 never printed its login"})
                rc = 1
                break
            time.sleep(0.01)
    emit({"event": "done", "received": LINES, "rc": rc})
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
