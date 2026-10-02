"""Drive the app over adb. Usage:
  ec.py dump                      list clickable/text nodes with their bounds
  ec.py tap <x> <y>
  ec.py tapdesc <content-desc>    tap the first node whose content-desc or text matches exactly
  ec.py send <command text>       type into the Engineer Console input and press send
"""
import os, re, subprocess, sys, time
import xml.etree.ElementTree as ET

ENV = dict(os.environ, MSYS_NO_PATHCONV="1")


def adb(*a, out=False):
    r = subprocess.run(["adb", *a], capture_output=True, env=ENV)
    return r.stdout.decode(errors="replace") if out else r.returncode


def dump():
    adb("shell", "uiautomator", "dump", "/sdcard/ui.xml")
    xml = adb("exec-out", "cat", "/sdcard/ui.xml", out=True)
    return ET.fromstring(xml[xml.find("<?xml"):] if "<?xml" in xml else xml)


def centre(n):
    x1, y1, x2, y2 = map(int, re.findall(r"\d+", n.get("bounds")))
    return (x1 + x2) // 2, (y1 + y2) // 2


def nodes(root):
    return list(root.iter("node"))


def tap(x, y):
    adb("shell", "input", "tap", str(x), str(y))


def find(root, pred):
    for n in nodes(root):
        if pred(n):
            return n
    return None


def send(text):
    root = dump()
    edit = find(root, lambda n: n.get("class") == "android.widget.EditText")
    if edit is None:
        sys.exit("no EditText on screen (is the Engineer Console open?)")
    tap(*centre(edit)); time.sleep(0.4)
    # clear whatever is there
    adb("shell", "input", "keyevent", "KEYCODE_MOVE_END")
    adb("shell", "input", "keyevent", *(["KEYCODE_DEL"] * 40))
    adb("shell", "input", "text", text.replace(" ", "%s"))
    time.sleep(1.5)
    root = dump()
    edit = find(root, lambda n: n.get("class") == "android.widget.EditText")
    if edit is None or edit.get("text") != text:
        sys.exit(f"input reads {edit.get('text') if edit is not None else None!r}, not {text!r}; not sent")
    ex, ey = centre(edit)
    # the send button: the clickable node right of the input on the same row
    btn = None
    for n in nodes(root):
        if n is not edit and n.get("class") == "android.view.ViewGroup":
            cx, cy = centre(n)
            if abs(cy - ey) < 80 and cx > ex:
                btn = n
    if btn is None:
        sys.exit("no send button found beside the input")
    tap(*centre(btn))
    print(f"{time.strftime('%H:%M:%S')} sent {text!r}")


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "dump":
        for n in nodes(dump()):
            t, d = n.get("text"), n.get("content-desc")
            if t or d or n.get("clickable") == "true":
                print(n.get("bounds"), n.get("class").split(".")[-1], repr(t)[:60], repr(d)[:60],
                      "C" if n.get("clickable") == "true" else "")
    elif cmd == "tap":
        tap(int(sys.argv[2]), int(sys.argv[3]))
    elif cmd == "tapdesc":
        want = " ".join(sys.argv[2:])
        n = find(dump(), lambda n: n.get("content-desc") == want or n.get("text") == want)
        if n is None:
            sys.exit(f"nothing called {want!r}")
        tap(*centre(n)); print("tapped", want, centre(n))
    elif cmd == "send":
        send(" ".join(sys.argv[2:]))
