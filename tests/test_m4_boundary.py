"""M4 red-first cell: forces the boundary-fall decision to be recorded and the
kernel path state to be recorded. At 0.0.1 that state is BUILT and MEASURED."""
import json, pathlib
R = json.load(open(pathlib.Path(__file__).resolve().parent.parent / "M4_RESULTS.json"))
def test_m4_boundary_fallen_recorded():
    m4 = R["m4"]
    assert m4["boundary_fallen"] is True, "boundary-fall decision not recorded"
    assert "host bounce" in m4["reason"], "why the userspace path is closed must be stated"
    assert m4["kernel_dma_path"].startswith("BUILT and MEASURED")
    assert m4["gain_demonstrated"] is False, "no end-to-end inference gain claim allowed"
    assert "INFERENCE gain" in m4["gain_scope"], (
        "a measured transfer gain must not be allowed to read as an inference gain")
