#!/usr/bin/env python3
"""A stand-in for the fork's fakeclient binary, for difftest's tests on a
machine without the C++ build. It speaks the same command line and prints the
same event lines: connected, actor, sent, message, done. Movement echoes are
what the legacy server does with a legal update; a move further than 4096
units from the spawn gets a Teleport2 (t=31) back instead, which is the
server's MovementValidation answer. Nothing here talks to a network."""
import json
import sys

MSG_UPDATE_MOVEMENT = 2
MSG_TELEPORT2 = 31
SPAWN = [133857.0, -61130.0, 14662.0]


def emit(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def main(argv):
    args = dict(zip(argv[1::2], argv[2::2]))
    script = args.get("--script")
    profile = int(args.get("--profile-id", "1"))
    emit({"event": "connected"})
    emit({"event": "actor", "idx": profile, "worldOrCell": 60, "pos": SPAWN, "rot": [0, 0, 72]})
    received = 1
    if script:
        with open(script) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                step = json.loads(line)
                if "move" in step:
                    m = step["move"]
                    pos = [SPAWN[0] + m.get("dx", 0), SPAWN[1] + m.get("dy", 0), SPAWN[2] + m.get("dz", 0)]
                    msg = {"t": MSG_UPDATE_MOVEMENT, "idx": profile, "data": {"worldOrCell": 60, "pos": pos, "runMode": m.get("runMode", "Walking")}}
                    emit({"event": "sent", "msg": msg})
                    far = abs(m.get("dx", 0)) > 4096 or abs(m.get("dy", 0)) > 4096 or abs(m.get("dz", 0)) > 4096
                    if far:
                        emit({"event": "message", "msg": {"t": MSG_TELEPORT2, "idx": profile, "pos": SPAWN}})
                    else:
                        emit({"event": "message", "msg": msg})
                    received += 1
                elif "send" in step:
                    emit({"event": "sent", "msg": step["send"]})
    emit({"event": "done", "received": received, "rc": 0})
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
