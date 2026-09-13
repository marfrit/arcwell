#!/usr/bin/env python3
"""arcwell M0 experiment 2: consumption path on one card, one fresh process.

Bytes are landed in VRAM through OpenVINO's remote-tensor API (OpenCL USM
device buffer), then a GPU kernel (Convert u8->i32, ReduceSum over rows of
4096) consumes every byte; the per-row partial sums are compared exactly
against numpy. Three ways of landing the bytes are timed: pageable host
memory -> device, USM host (pinned) -> device, and the kernel reading the
USM host buffer directly over PCIe. A single-byte mutation must move exactly
one partial sum, so the cell can fail.
"""
import hashlib
import sys
import threading
import time

import numpy as np
import openvino as ov
import openvino.opset13 as ops

dev = sys.argv[1]
mib = int(sys.argv[2]) if len(sys.argv) > 2 else 256
ROW = 4096
rows = (mib << 20) // ROW
nbytes = rows * ROW

core = ov.Core()
print("openvino", ov.__version__, "device", dev, core.get_property(dev, "FULL_DEVICE_NAME"))
print("device total mem", core.get_property(dev, "GPU_DEVICE_TOTAL_MEM_SIZE"))

p = ops.parameter([rows, ROW], ov.Type.u8, name="x")
s = ops.reduce_sum(ops.convert(p, ov.Type.i32), ops.constant(np.array([1], np.int64)), keep_dims=False)
model = ov.Model([s], [p], "bytesum")
t0 = time.perf_counter()
cm = core.compile_model(model, dev)
print(f"compile {time.perf_counter() - t0:.3f} s")
req = cm.create_infer_request()

rng = np.random.default_rng(20260913)
data = rng.integers(0, 256, size=(rows, ROW), dtype=np.uint8)
expected = data.astype(np.int32).sum(axis=1)
print(f"payload {nbytes} bytes ({mib} MiB) sha256={hashlib.sha256(data.tobytes()).hexdigest()[:16]}")

ctx = core.get_default_context(dev)
dev_t = ctx.create_tensor(ov.Type.u8, ov.Shape([rows, ROW]), {"SHARED_MEM_TYPE": "USM_DEVICE_BUFFER"})
usm_host = ctx.create_tensor(ov.Type.u8, ov.Shape([rows, ROW]), {"SHARED_MEM_TYPE": "USM_HOST_BUFFER"})
pageable = ov.Tensor(data, shared_memory=True)  # a copy here made the mutation cell blind (run 1)
print("device tensor params", {k: str(v) for k, v in dev_t.get_params().items()})

BDF = {"B60": "0000:0f:00.0", "A770": "0000:06:00.0"}
bdf = next((v for k, v in BDF.items() if k in core.get_property(dev, "FULL_DEVICE_NAME")), None)
link_seen = set()
_stop = False


def _sample_link():
    while not _stop:
        try:
            spd = open(f"/sys/bus/pci/devices/{bdf}/current_link_speed").read().strip()
            wid = open(f"/sys/bus/pci/devices/{bdf}/current_link_width").read().strip()
            link_seen.add(f"{spd} x{wid}")
        except OSError:
            link_seen.add("unreadable")
            return
        time.sleep(0.001)


sampler = threading.Thread(target=_sample_link, daemon=True)
sampler.start()


def infer(t):
    req.set_tensor("x", t)
    t0 = time.perf_counter()
    req.infer()
    dt = time.perf_counter() - t0
    return req.get_output_tensor().data.copy(), dt


def gbs(sec):
    return nbytes / sec / 1e9


# warm-up: first inference compiles the static-shape kernels
dev_t.copy_from(pageable)
out, dt = infer(dev_t)
print(f"warm-up infer on device tensor {dt * 1e3:.1f} ms, exact={np.array_equal(out, expected)}")

for label, fn in [
    ("pageable host -> device copy_from", lambda: dev_t.copy_from(pageable)),
    ("USM host (pinned) fill from pageable", lambda: usm_host.copy_from(pageable)),
    ("USM host (pinned) -> device copy_from", lambda: dev_t.copy_from(usm_host)),
]:
    best = None
    for i in range(3):
        t0 = time.perf_counter()
        fn()
        t1 = time.perf_counter()
        out, dt = infer(dev_t)  # fences the copy; the kernel reads what landed
        t2 = time.perf_counter()
        best = min(best, (t1 - t0, t2 - t0, dt)) if best else (t1 - t0, t2 - t0, dt)
        assert np.array_equal(out, expected), f"{label}: kernel read wrong bytes on iteration {i}"
    c, tot, inf = best
    print(f"{label:42s} copy {c * 1e3:8.2f} ms ({gbs(c):6.2f} GB/s)  copy+infer {tot * 1e3:8.2f} ms  infer {inf * 1e3:7.2f} ms  exact=True")

best = None
for i in range(3):
    out, dt = infer(usm_host)
    best = dt if best is None else min(best, dt)
    assert np.array_equal(out, expected), f"kernel over USM host: wrong bytes on iteration {i}"
print(f"{'kernel reads USM host directly (no copy)':42s} {'':31s} infer {best * 1e3:7.2f} ms ({gbs(best):6.2f} GB/s effective)  exact=True")

# mutation: one byte, one row
r, c = rows // 2, 7
data[r, c] ^= 0xFF
mutated_expected = data.astype(np.int32).sum(axis=1)
dev_t.copy_from(pageable)
out, _ = infer(dev_t)
diff = np.flatnonzero(out != expected)
print(f"mutation: flipped byte at row {r} col {c}; rows whose partial moved: {diff.tolist()}; "
      f"matches mutated expectation: {np.array_equal(out, mutated_expected)}")
assert diff.tolist() == [r], "mutation did not move exactly one partial"

back = ov.Tensor(ov.Type.u8, ov.Shape([rows, ROW]))
dev_t.copy_to(back)
print(f"device -> host copy_to byte-exact with mutated payload: {np.array_equal(back.data, data)}")
_stop = True
sampler.join(timeout=1)
print(f"PCIe link states seen on {bdf} during the run: {sorted(link_seen)}")
print("RESULT consumption_verified=True", dev)
