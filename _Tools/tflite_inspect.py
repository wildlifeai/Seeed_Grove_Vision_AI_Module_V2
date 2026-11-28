"""Show what is inside a TFLite model: its layers and its input and output tensors.

    python _Tools/tflite_inspect.py <model.tflite | model.TFL | model_data.cc> [--all]

Reads a model file, or the C array that a Himax scenario app compiles in (a .cc, .c or .h file
of 0x.. bytes). Prints the operator types, the layers in order (the first and last few; --all
for every one), and each input and output tensor with its name, shape, type, scale and zero
point. It ends with a verdict on the output: probabilities (the model ends in softmax) or
logits.

The rule for the verdict: TFLite's int8 quantization spec fixes a softmax (or logistic) output at
scale 1/256 and zero point -128, so int8 values -128..127 mean probabilities 0..255/256. A model
compiled by Vela shows only one 'ethos-u' layer, which hides the softmax, so for those models the
output quantization is the only evidence.

Standard library only: it walks the flatbuffer itself (schema: tensorflow/lite/schema/schema.fbs).
Background: _Documentation/development reports/2026-11-05_NN_threshold/CLAUDE_person_model_softmax.md
"""
import argparse, re, struct, sys

TYPES = {0: "f32", 1: "f16", 2: "i32", 3: "u8", 4: "i64", 6: "bool", 7: "i16", 9: "i8"}
BUILTINS = {0: "ADD", 1: "AVERAGE_POOL_2D", 2: "CONCATENATION", 3: "CONV_2D", 4: "DEPTHWISE_CONV_2D",
            6: "DEQUANTIZE", 9: "FULLY_CONNECTED", 14: "LOGISTIC", 17: "MAX_POOL_2D", 18: "MUL",
            19: "RELU", 21: "RELU6", 22: "RESHAPE", 25: "SOFTMAX", 32: "CUSTOM", 34: "PAD",
            40: "MEAN", 114: "QUANTIZE"}


def load(path):
    """Return the model bytes from a .tflite/.TFL file or a C array of 0x.. bytes."""
    if path.lower().endswith((".cc", ".c", ".h", ".cpp")):
        text = open(path, errors="ignore").read()
        text = text[text.find("{"):]
        return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{1,2})\b", text))
    return open(path, "rb").read()


class Flat:
    """Just enough flatbuffer reading for the TFLite schema."""

    def __init__(self, b):
        self.b = b

    def u32(self, o): return struct.unpack_from("<I", self.b, o)[0]
    def i32(self, o): return struct.unpack_from("<i", self.b, o)[0]

    def field(self, table, n):
        """Absolute offset of field n of the table at 'table', or None if absent."""
        vt = table - self.i32(table)
        vlen = struct.unpack_from("<H", self.b, vt)[0]
        if 4 + 2 * n >= vlen:
            return None
        off = struct.unpack_from("<H", self.b, vt + 4 + 2 * n)[0]
        return table + off if off else None

    def ref(self, o): return o + self.u32(o)

    def vector(self, o):
        """(start, length) of the vector referenced at o."""
        v = self.ref(o)
        return v + 4, self.u32(v)

    def tables(self, o):
        if o is None:
            return []
        s, n = self.vector(o)
        return [self.ref(s + 4 * k) for k in range(n)]

    def ints(self, o, size=4, fmt="<i"):
        if o is None:
            return []
        s, n = self.vector(o)
        return [struct.unpack_from(fmt, self.b, s + size * k)[0] for k in range(n)]

    def string(self, o):
        if o is None:
            return ""
        s, n = self.vector(o)
        return self.b[s:s + n].decode(errors="replace")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("model")
    ap.add_argument("--all", action="store_true", help="list every layer")
    a = ap.parse_args()

    b = load(a.model)
    if b[4:8] != b"TFL3":
        sys.exit(f"{a.model}: not a TFLite model (no TFL3 identifier)")
    f = Flat(b)
    model = f.u32(0)
    print(f"{a.model}: {len(b)} bytes, schema version {f.u32(f.field(model, 0))}")

    # Operator codes: builtin_code (field 3) supersedes deprecated_builtin_code (field 0)
    codes = []
    for t in f.tables(f.field(model, 1)):
        dep = b[f.field(t, 0)] if f.field(t, 0) else 0
        new = f.i32(f.field(t, 3)) if f.field(t, 3) else 0
        custom = f.string(f.field(t, 1))
        code = max(dep, new)
        codes.append(custom if custom else BUILTINS.get(code, f"builtin {code}"))
    print("Operator types:", ", ".join(codes))

    graph = f.tables(f.field(model, 2))[0]
    tensors = f.tables(f.field(graph, 0))

    def tensor(i):
        t = tensors[i]
        q = f.field(t, 4)
        scale = zp = []
        if q:
            qt = f.ref(q)
            scale = f.ints(f.field(qt, 2), 4, "<f")
            zp = f.ints(f.field(qt, 3), 8, "<q")
        return {"name": f.string(f.field(t, 3)), "shape": f.ints(f.field(t, 0)),
                "type": TYPES.get(b[f.field(t, 1)] if f.field(t, 1) else 0, "?"),
                "scale": scale, "zp": zp}

    ops = f.tables(f.field(graph, 3))
    rows = []
    for o in ops:
        idx = f.u32(f.field(o, 0)) if f.field(o, 0) else 0
        outs = f.ints(f.field(o, 2))
        rows.append(f"{codes[idx]} -> {tensor(outs[0])['name'] if outs else '?'}")
    print(f"Layers ({len(rows)}):")
    shown = rows if a.all or len(rows) <= 8 else rows[:3] + [f"... {len(rows) - 6} more ..."] + rows[-3:]
    for r in shown:
        print("  " + r)

    def show(kind, i):
        t = tensor(i)
        q = f"scale {t['scale'][0]:.8g}, zero point {t['zp'][0]}" if t["scale"] else "not quantized"
        print(f"{kind}: '{t['name']}' {t['type']} {t['shape']}, {q}")
        return t

    for i in f.ints(f.field(graph, 1)):
        show("Input ", i)
    outputs = [show("Output", i) for i in f.ints(f.field(graph, 2))]

    out = outputs[0]
    last_softmax = rows and rows[-1].startswith(("SOFTMAX", "LOGISTIC"))
    softmax_quant = (out["type"] == "i8" and out["scale"] and abs(out["scale"][0] - 1 / 256) < 1e-9
                     and out["zp"] and out["zp"][0] == -128)
    if last_softmax or softmax_quant:
        why = "the last layer is softmax" if last_softmax else "softmax quantization: scale 1/256, zero point -128"
        print(f"Verdict: the output is PROBABILITIES ({why}). Probability = (value + 128) / 256.")
    else:
        print("Verdict: the output is LOGITS (no softmax at the end). Apply softmax for probabilities.")


if __name__ == "__main__":
    main()
