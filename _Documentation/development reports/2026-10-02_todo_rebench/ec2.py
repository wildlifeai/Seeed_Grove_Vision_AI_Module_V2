"""Send one Engineer Console command, pre-type a second, and send it DELAY s after the first.
  ec2.py "<first>" "<second>" <delay s>"""
import sys, time
import ec


def type_only(text):
    root = ec.dump()
    edit = ec.find(root, lambda n: n.get("class") == "android.widget.EditText")
    ec.tap(*ec.centre(edit)); time.sleep(0.3)
    ec.adb("shell", "input", "keyevent", "KEYCODE_MOVE_END")
    ec.adb("shell", "input", "keyevent", *(["KEYCODE_DEL"] * 40))
    ec.adb("shell", "input", "text", text.replace(" ", "%s"))
    time.sleep(1.0)
    root = ec.dump()
    edit = ec.find(root, lambda n: n.get("class") == "android.widget.EditText")
    if edit is None or edit.get("text") != text:
        sys.exit(f"input reads {edit.get('text') if edit is not None else None!r}, not {text!r}")
    ex, ey = ec.centre(edit)
    btn = [n for n in ec.nodes(root) if n is not edit and n.get("class") == "android.view.ViewGroup"
           and abs(ec.centre(n)[1] - ey) < 80 and ec.centre(n)[0] > ex][-1]
    return ec.centre(btn)


first, second, delay = sys.argv[1], sys.argv[2], float(sys.argv[3])
send1 = type_only(first)
ec.tap(*send1); t0 = time.time()
print(f"{time.strftime('%H:%M:%S')} sent {first!r}", flush=True)
time.sleep(0.8)
send2 = type_only(second)
while time.time() - t0 < delay:
    time.sleep(0.05)
ec.tap(*send2)
print(f"{time.strftime('%H:%M:%S')} sent {second!r}, {time.time() - t0:.1f} s after the first", flush=True)
