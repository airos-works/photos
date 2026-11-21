#!/usr/bin/env python3
"""qmp.py shot FILE.png | key KEY... | type TEXT: drive the VM over QMP."""
import json, os, socket, sys, subprocess
DIR = os.environ.get("VM_DIR") or os.path.join(os.environ.get("PHOTOS_VM_WORK", "/data3/airos/photos-work"), "vm-" + os.environ.get("VM_NAME", "main"))
def qmp(cmds):
    s = socket.socket(socket.AF_UNIX); s.connect(os.path.join(DIR, "qmp.sock"))
    f = s.makefile("rw")
    f.readline(); f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n"); f.flush(); f.readline()
    out = []
    for c in cmds:
        f.write(json.dumps(c) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: out.append(r); break
    return out
cmd = sys.argv[1]
if cmd == "shot":
    ppm = sys.argv[2] + ".ppm"
    qmp([{"execute": "screendump", "arguments": {"filename": ppm}}])
    from PIL import Image; Image.open(ppm).save(sys.argv[2]); os.remove(ppm)
elif cmd == "key":
    for k in sys.argv[2:]:
        keys = [{"type": "qcode", "data": x} for x in k.split("-")]
        qmp([{"execute": "send-key", "arguments": {"keys": keys}}])
elif cmd == "hmp":
    print(qmp([{"execute": "human-monitor-command", "arguments": {"command-line": " ".join(sys.argv[2:])}}]))
elif cmd in ("click", "rclick"):
    # click X Y [W H]: absolute position on a W x H screen (default 1280x800)
    x, y = int(sys.argv[2]), int(sys.argv[3])
    w = int(sys.argv[4]) if len(sys.argv) > 4 else 1280
    h = int(sys.argv[5]) if len(sys.argv) > 5 else 800
    ax, ay = x * 32767 // (w - 1), y * 32767 // (h - 1)
    move = {"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}}]}}
    btn = lambda down: {"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"down": down, "button": "right" if cmd == "rclick" else "left"}}]}}
    qmp([move]); import time; time.sleep(0.15); qmp([btn(True)]); time.sleep(0.08); qmp([btn(False)])
elif cmd == "type":
    import time
    text = " ".join(sys.argv[2:])
    special = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "_": "shift-minus",
               ";": "semicolon", ":": "shift-semicolon", "|": "shift-backslash", ">": "shift-dot",
               "<": "shift-comma", "=": "equal", "'": "apostrophe", '"': "shift-apostrophe",
               "~": "shift-grave_accent", "$": "shift-4", "&": "shift-7", "*": "shift-8",
               ",": "comma", "\n": "ret", "+": "shift-equal", "(": "shift-9", ")": "shift-0"}
    for ch in text:
        if ch in special: k = special[ch]
        elif ch.isupper(): k = "shift-" + ch.lower()
        else: k = ch
        keys = [{"type": "qcode", "data": x} for x in k.split("-")]
        qmp([{"execute": "send-key", "arguments": {"keys": keys}}]); time.sleep(0.02)
elif cmd == "move":
    x, y = int(sys.argv[2]), int(sys.argv[3])
    ax, ay = x * 32767 // 1279, y * 32767 // 799
    qmp([{"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}}]}}])
