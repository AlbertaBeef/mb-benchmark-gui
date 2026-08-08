#!/usr/bin/env python3
"""Count multiply-accumulates per frame in an ONNX graph.

Produces the GOP/frame figures in CLAUDE.md's "Work per frame" section, which
turn a measured frame rate into achieved TOPS. That section says to reproduce
these rather than trust them if a model changes -- this is the reproduction, so
it lives in the repo instead of being retyped each time.

Counts Conv / ConvTranspose / Gemm / MatMul only. Everything else (activations,
adds, pooling, resize, the DFL/NMS tail) is well under 1% of a CNN's arithmetic
and is not what any vendor's TOPS rating measures either.

**GOP = 2 x MAC.** A multiply-accumulate is two operations, and that factor is
the single most common way these numbers get quoted wrong by 2x. Ultralytics'
published "GFLOPs" for YOLOv8s/m (28.6 / 78.9) already include it, which is what
validates this counter -- see --check.

Shapes come from onnx.shape_inference with the batch pinned to 1. A dynamic
spatial dim must be pinned too (SCRFD ships dynamic H/W): use --shape.

    tools/model-ops.py model.onnx [more.onnx ...]
    tools/model-ops.py --shape 640x640 scrfd_10g_bnkps.onnx
    tools/model-ops.py --check          # validate against known-good values
"""
import argparse
import os
import sys

import onnx
from onnx import shape_inference, numpy_helper


def _dims(vi):
    return [d.dim_value if d.HasField("dim_value") else None
            for d in vi.type.tensor_type.shape.dim]


def pin_inputs(model, hw=None):
    """Batch -> 1, and optionally spatial dims -> hw, so inference can run."""
    for vi in model.graph.input:
        sh = vi.type.tensor_type.shape
        if not sh.dim:
            continue
        d0 = sh.dim[0]
        d0.ClearField("dim_param")
        d0.dim_value = 1
        if hw and len(sh.dim) == 4:
            for i, v in zip((2, 3), hw):
                if not sh.dim[i].HasField("dim_value") or sh.dim[i].dim_value == 0:
                    sh.dim[i].ClearField("dim_param")
                    sh.dim[i].dim_value = v
    return model


def count(path, hw=None):
    model = pin_inputs(onnx.load(path), hw)
    try:
        model = shape_inference.infer_shapes(model, strict_mode=False)
    except Exception as e:                                  # noqa: BLE001
        print(f"  ! shape inference failed: {e}", file=sys.stderr)

    shapes = {}
    for coll in (model.graph.input, model.graph.output, model.graph.value_info):
        for vi in coll:
            shapes[vi.name] = _dims(vi)
    inits = {t.name: list(t.dims) for t in model.graph.initializer}
    shapes.update({k: v for k, v in inits.items() if k not in shapes})

    params = sum(int(numpy_helper.to_array(t).size) for t in model.graph.initializer)

    macs = 0
    unknown = []
    for n in model.graph.node:
        if n.op_type in ("Conv", "ConvTranspose"):
            out = shapes.get(n.output[0])
            w = shapes.get(n.input[1])
            if not out or not w or any(d is None for d in out):
                unknown.append(f"{n.op_type}:{n.name or n.output[0]}")
                continue
            # out = [N, Cout, ...spatial]; w = [Cout, Cin/g, kh, kw]
            spatial = 1
            for d in out[2:]:
                spatial *= d
            kernel = 1
            for d in w[2:]:
                kernel *= d
            # ConvTranspose weights are [Cin, Cout/g, kh, kw]; either way the
            # per-output-element cost is Cin/g * prod(kernel).
            cin_over_g = w[1] if n.op_type == "Conv" else w[0]
            cout = out[1]
            macs += spatial * cout * cin_over_g * kernel

        elif n.op_type == "Gemm":
            w = shapes.get(n.input[1])
            out = shapes.get(n.output[0])
            if not w or not out or any(d is None for d in out):
                unknown.append(f"Gemm:{n.name or n.output[0]}")
                continue
            macs += out[-1] * (w[0] if w[1] == out[-1] else w[1])

        elif n.op_type == "MatMul":
            a = shapes.get(n.input[0])
            b = shapes.get(n.input[1])
            out = shapes.get(n.output[0])
            if not a or not b or not out or any(d is None for d in out) \
               or any(d is None for d in b):
                unknown.append(f"MatMul:{n.name or n.output[0]}")
                continue
            k = b[-2] if len(b) >= 2 else 1
            n_out = 1
            for d in out:
                n_out *= d
            macs += n_out * k

    return macs, params, unknown


KNOWN = {  # name -> (path, hw, expected GOP or None)
    "ArcFace MobileFaceNet": ("mbf.onnx", None, 0.88),
    "SCRFD-500M":  ("scrfd_500m_bnkps.onnx", (640, 640), 1.47),
    "OSNet x1.0":  ("osnet_x1_0_market.onnx", None, 1.96),
    "SCRFD-2.5G":  ("scrfd_2.5g_bnkps.onnx", (640, 640), 6.86),
    "SCRFD-10G":   ("scrfd_10g_bnkps.onnx", (640, 640), 26.68),
    "YOLOv8s":     ("yolov8s.onnx", None, 28.60),
    "YOLOv8m":     ("yolov8m.onnx", None, 78.94),
}
REF = "/home/ubuntu/envic_ai_python/ai_reference/models/"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("models", nargs="*")
    ap.add_argument("--shape", help="pin spatial dims, e.g. 640x640")
    ap.add_argument("--check", action="store_true",
                    help="run the reference set and compare with CLAUDE.md")
    a = ap.parse_args()

    hw = None
    if a.shape:
        hw = tuple(int(x) for x in a.shape.lower().split("x"))

    items = []
    if a.check:
        items = [(k, REF + v[0], v[1], v[2]) for k, v in KNOWN.items()]
    for p in a.models:
        items.append((os.path.basename(p), p, hw, None))

    print(f"{'model':28s} {'GMAC':>9s} {'GOP':>9s} {'params':>10s}  note")
    worst = 0.0
    for name, path, shp, expect in items:
        if not os.path.exists(path):
            print(f"{name:28s} {'-':>9s} {'-':>9s} {'-':>10s}  MISSING {path}")
            continue
        macs, params, unknown = count(path, shp)
        gmac, gop = macs / 1e9, 2 * macs / 1e9
        note = ""
        if unknown:
            note = f"{len(unknown)} node(s) unresolved: {unknown[:2]}"
        if expect is not None:
            d = abs(gop - expect) / expect * 100.0
            worst = max(worst, d)
            note = (note + "  " if note else "") + f"expect {expect} ({d:.2f}% off)"
        print(f"{name:28s} {gmac:9.3f} {gop:9.2f} {params/1e6:9.2f}M  {note}")
    if a.check:
        print(f"\nworst deviation from CLAUDE.md: {worst:.2f}%")


if __name__ == "__main__":
    main()
