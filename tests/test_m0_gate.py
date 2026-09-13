"""arcwell M0 feasibility gate (red-first acceptance).

The cells read M0_RESULTS.json at the repository root. Each cell names what
it holds; a missing file, a missing row or a row that was never measured is a
failure, not a skip. Run with `python3 -m pytest tests/test_m0_gate.py -v`.
"""
import json
import pathlib

import pytest

ROOT = pathlib.Path(__file__).resolve().parent.parent
RESULTS = ROOT / "M0_RESULTS.json"

REQUIRED_CARDS = ("Arc Pro B60", "Arc A770")
PROBE_OUTCOMES = ("accepted", "rejected_by_hardware", "refused_by_kernel")
PROBE_SIGNATURE = "O_DIRECT preadv(NVMe LBA0 -> BAR mapping)"
CONSUMPTION_SIGNATURE = "RESULT consumption_verified=True"


@pytest.fixture(scope="module")
def results():
    if not RESULTS.exists():
        pytest.fail(f"M0 not recorded: {RESULTS.name} is absent from the tree")
    return json.loads(RESULTS.read_text())


def test_e1_bar_aperture_covers_vram(results):
    """Experiment 1, row 'aperture': both cards expose a resizable BAR at
    least as large as their VRAM (a 'large VRAM aperture')."""
    cards = results["experiment1"]["cards"]
    for name in REQUIRED_CARDS:
        assert name in cards, f"experiment1.cards lacks {name}"
        c = cards[name]
        for key in ("vram_bytes", "bar2_bytes", "rebar_max_bytes", "cpu_accessible_vram_bytes"):
            assert isinstance(c.get(key), int) and c[key] > 0, f"{name}: {key} not recorded"
        assert c["bar2_bytes"] >= c["vram_bytes"], f"{name}: BAR2 smaller than VRAM"
        assert c["cpu_accessible_vram_bytes"] <= c["vram_bytes"], f"{name}: accessible > physical"
        assert c["bar2_bytes"] <= c["rebar_max_bytes"], f"{name}: BAR2 above the ReBAR maximum"


def test_e1_inbound_dma_probe_run_and_recorded(results):
    """Experiment 1, row 'inbound DMA': the probe was RUN on both cards and
    its outcome recorded as one stated fact with the raw log attached."""
    probe = results["experiment1"]["inbound_dma_probe"]
    assert probe.get("status") == "run", f"inbound DMA probe status is {probe.get('status')!r}, not 'run'"
    assert probe.get("outcome") in PROBE_OUTCOMES, f"outcome {probe.get('outcome')!r} is not a stated fact"
    log = probe.get("raw_log", "")
    for name in REQUIRED_CARDS:
        assert f"[{name}]" in log, f"raw log has no section for {name}"
    assert log.count(PROBE_SIGNATURE) >= len(REQUIRED_CARDS), "raw log lacks the probe's result line per card"
    assert isinstance(probe.get("rung3_physically_possible"), bool), "rung-3 decision not recorded"


def test_e1_v1_rides_rung2(results):
    """Experiment 1, row 'decision': v1 is rung 2 whatever the probe said."""
    assert results["experiment1"].get("v1_rung") == 2


def test_e2_consumption_verified_on_both_cards(results):
    """Experiment 2: the OpenCL/OpenVINO consumption path was verified on both
    cards, byte-exact, with the mutation moving exactly one partial."""
    cards = results["experiment2"]["cards"]
    for name in REQUIRED_CARDS:
        assert name in cards, f"experiment2.cards lacks {name}"
        c = cards[name]
        assert c.get("consumption_verified") is True, f"{name}: consumption not verified"
        log = c.get("raw_log", "")
        assert CONSUMPTION_SIGNATURE in log, f"{name}: raw log lacks the verified marker"
        assert "rows whose partial moved: [" in log and "matches mutated expectation: True" in log, \
            f"{name}: mutation evidence missing from raw log"
        for key in ("h2d_pageable_gbs", "h2d_pinned_gbs", "kernel_over_usm_host_gbs"):
            assert isinstance(c.get(key), (int, float)) and c[key] > 0, f"{name}: {key} not recorded"
