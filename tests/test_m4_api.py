"""M4 API red-first cell: PRIMARY = expert streaming storage->VRAM; direct path
RAW NVMe BLOCK -> VRAM; FIEMAP absent from data path; no aw_set_fiemap ioctl
(FIEMAP standard); ioctl surface = DMA-mapping/registration; layout: both work,
one-file-per-expert recommended, contiguity best-effort; aw_write secondary;
engine BUILT and measured at 0.0.1; gain measured, not claimed from a spec."""
import json, pathlib
R = json.load(open(pathlib.Path(__file__).resolve().parent.parent / "M4_RESULTS.json"))
def test_m4_api_primary_stream():
    m4 = R["m4"]; a=m4["api_decision"]; f=m4["fiemap_resolution"]; i=m4["ioctl_surface"]; l=m4["layout_decision"]
    assert "PRIMARY" in a and "SECONDARY" in a
    assert "RAW NVMe BLOCK -> VRAM" in f and "ABSENT" in f
    assert "NO custom aw_set_fiemap" in i and "FS_IOC_FIEMAP" in i
    assert "one-file-per-expert" in l and "BOTH layouts work" in l
    assert "best-effort" in l, "contiguity must be flagged best-effort"
    assert "BUILT" in m4["kernel_dma_engine"] and "UNBUILT" not in m4["kernel_dma_engine"]
    assert m4["gain_demonstrated"] is False
