#!/usr/bin/env python3
"""Writes the light-plugins test's plugins (thuum docs/verbs/light-plugins.md):
four tiny plugins this repo owns, no game content. Record and group layout:
UESP, "Skyrim Mod:Mod File Format" (a record is type, data size, flags, form
id, revision, version, unknown, then fields of type, u16 size, data; a group is
"GRUP", total size, label, group type, stamp; TES4 lists its masters as MAST
and DATA). Run from this directory; the outputs are committed next to it.

  Base.esm   full master; MISC 0x000800 BaseItem
  Light.esp  light by its TES4 flag (1 << 9), master Base.esm; MISC 0x000800
             LightItem, FLST 0x000801 naming BaseItem and LightItem
  After.esp  full, masters Base.esm and Light.esp; MISC 0x000800 AfterItem,
             FLST 0x000801 naming LightItem, BaseItem and AfterItem
  Small.esl  light by its extension alone (no flag), master Base.esm; MISC
             0x000800 SmallItem
"""
import struct

def field(kind, data):
    return kind.encode() + struct.pack("<H", len(data)) + data

def record(kind, flags, form_id, fields):
    body = b"".join(fields)
    return kind.encode() + struct.pack("<IIIIHH", len(body), flags, form_id, 0, 44, 0) + body

def group(label, records):
    body = b"".join(records)
    return b"GRUP" + struct.pack("<I", 24 + len(body)) + label.encode() + struct.pack("<iHHHH", 0, 0, 0, 0, 0) + body

def tes4(flags, masters, num_records):
    fields = [field("HEDR", struct.pack("<fiI", 1.71, num_records, 0x801))]
    for m in masters:
        fields += [field("MAST", m.encode() + b"\0"), field("DATA", struct.pack("<Q", 0))]
    return record("TES4", flags, 0, fields)

def misc(form_id, edid):
    return record("MISC", 0, form_id, [field("EDID", edid.encode() + b"\0")])

def flst(form_id, edid, ids):
    return record("FLST", 0, form_id, [field("EDID", edid.encode() + b"\0")] + [field("LNAM", struct.pack("<I", i)) for i in ids])

LIGHT = 1 << 9
MASTER = 1
files = {
    "Base.esm": tes4(MASTER, [], 1) + group("MISC", [misc(0x00000800, "BaseItem")]),
    "Light.esp": tes4(LIGHT, ["Base.esm"], 2)
        + group("MISC", [misc(0x01000800, "LightItem")])
        + group("FLST", [flst(0x01000801, "LightList", [0x00000800, 0x01000800])]),
    "After.esp": tes4(0, ["Base.esm", "Light.esp"], 2)
        + group("MISC", [misc(0x02000800, "AfterItem")])
        + group("FLST", [flst(0x02000801, "AfterList", [0x01000800, 0x00000800, 0x02000800])]),
    "Small.esl": tes4(0, ["Base.esm"], 1) + group("MISC", [misc(0x01000800, "SmallItem")]),
}
for name, data in files.items():
    with open(name, "wb") as f:
        f.write(data)
    print(name, len(data))
