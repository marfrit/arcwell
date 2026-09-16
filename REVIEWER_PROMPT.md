# Reviewer seat — arcwell, first publication review

You are the **Reviewer**. Per `HOUSE_RULES.md`: you classify findings, you do not
edit code, and you review from a fresh archive with the stated diff blob-hash
verified before any results are read. The Fix seat implements what you name.

This gates **first publication to GitHub**, so it carries a disclosure obligation
on top of the correctness one.

*Revision 4. Revisions 1-3 were all reviewed. Rev 1 had a blocker, an
unfalsifiable invariant and a failing hygiene gate. Rev 2 fixed those and
introduced four new problems of its own — a stale header, an unreproducible hash,
a self-referential diff, and three statements the bounce fix left behind. §11
lists every finding and what changed, so you can check the fixes rather than trust
them. Rev 4 is what the Operator's ruling produced: eleven files removed, the
status record corrected against `results/`, the last open behavioural defect
tested and disproved, and two more cell defects found by running the suite rather
than by reading it. §2 and §12 are the new material.*

---

## 1. What you are reviewing

**There is no diff.** History was dropped for publication: `main` is an orphan
branch with a single root commit, because five commits on `dev` carried
operator-local hostnames in their *messages* and no review can fix history. `dev`
is preserved locally and is not published.

So the scope is **the whole tree**, not a range.

```
branch  main
commits 4 (root, the rev-5 concurrency fix, its verification, rev-6 item 24)
files   67
```

The second commit is deliberate. 0.0.1 was published from a single clean root;
the fix that followed is a separate commit rather than an amendment because by
then the repository was public, and rewriting published history to hide that a
double free shipped is not a thing this project should do. The bug and its fix
are both in the log.

Pinned by blob hash rather than commit SHA, so these stay valid if the root commit
is amended:

```
c78a29075b2e  KERNEL_FACTS.md
2811d6470967  RTFM_DISPOSITION.md
0b8175739bf2  docs/USING_ARCWELL.md
ee7fd91256d8  docs/campaigns/miss-tier-direct.md
e600faec6b20  packaging/dkms/arcwell-detect-vram
10ca402b5e8d  packaging/dkms/install.sh
6627f5c26c81  stub/include/aw_uapi.h
99485fe44c62  stub/src/arcwell.c
6fd0b6b50a37  stub/tools/aw_fiemap.c
13e33ce52288  stub/test/aw_wc_test.c
991f75fe9e37  stub/test/aw_expert_test.c
e0f7963e7e70  stub/test/aw_wait_race_test.c
10f2d8ffeadc  HANDOFF.md
4de905948363  BACKLOG.md
```

`stub/src/arcwell.c` and `stub/include/aw_uapi.h` both changed this revision —
the concurrency fix in §13. Rev 4's claim that the module was untouched no longer
holds, and neither does the srcversion equality it rested on.

`BACKLOG.md` is pinned last and edited last, on purpose: it is the file that
records the pinning, so any edit to it after the pin is taken invalidates the pin.
Rev 5 got this wrong once and pushed two stale pins (`install.sh` and `BACKLOG.md`
itself) before catching it.

Verify any file with:

```sh
git ls-tree -r main | grep <path>          # blob hash
git hash-object <path>                     # must match
```

Full inventory and tree hash:

```sh
git ls-tree -r main | grep -v REVIEWER_PROMPT.md | LC_ALL=C sort -k4 | sha256sum
# 0c75171fce40e46e7659456f8a84f69b06f2bc5b265b51fdba5eca3fb7e42e37
```

The exclude is not cosmetic. This prompt is a file in the tree it pins, so a hash
over the whole tree can never match what is printed inside it — rev 2 made exactly
this mistake with the diff range and the review caught it. Excluding the prompt
makes the pin stable across edits to the prompt, which is the only way it can be
verified at all. Everything the pin covers is content you are reviewing; this file
is the instrument, not the subject.

Rev 2 gave a 32-char chain hash with no command and it could not be reproduced;
rev 3 gave a reproducible one over a diff that no longer exists. This is the third
attempt at the same idea and the first one that survives an amendment.

## 2. Scope: the whole tree

Publication publishes everything. Nothing is scoped out.

Before this revision the tree carried eleven files that were either failed
experiments or off-mission. The Operator's instruction was *"do not ship a)
failed experiments b) known defects — remove the former, fix the latter"*, and
they are gone. They are named here so you can see what was removed rather than
having to notice an absence:

| removed file | why |
|---|---|
| `arcwell_nvme_vram.c` | defect D1 — `sg_page(sg) + j` on a page-less VRAM sgt; never worked |
| `m3_nvme.c` | defect D2 — a fabricated `__nvme_submit_sync_cmd` extern, 5 args against the real 7 |
| `arcwell_p2p{,_big,_chunk}.c` | defect D4 — locally-declared `pci_p2pdma_add_resource` with 5 args and size/offset transposed. **This bug *was* "blocker B1"** |
| `m2_phys.c`, `m4_verify.c` | early probes, unreferenced by any result |
| `stub/test/arcwell_road_bench.c` | off-mission — times the host-copy path and probes a retired userspace route |
| `stub/test/arcwell_probe.c` | superseded diagnostic, no result file |
| `stub/run_probe.sh`, `stub/src/run_big_test.sh` | drivers for the above |

`git log` will not show them: the published tree is an orphan root commit (§1).
`BACKLOG.md` keeps the full account of each defect — that is the record, and it
is deliberately *not* scrubbed. **A finding that the BACKLOG discusses a file
that no longer exists is expected, not a defect.**

What `stub/src/` builds now:

| file | status |
|---|---|
| `arcwell.c` | **the module** — ~1130 lines, seven ioctls |
| `aw_dma_map_probe.c` | probe; output in `BACKLOG.md` (Closed, item 3) and `KERNEL_FACTS.md`, **not** in any `results/` file |
| `aw_p2p_provider_probe.c` | probe; output in `KERNEL_FACTS.md` (the B1/D4 disproof), **not** in any `results/` file |
| `aw_m0_gate.c` | the M0 acceptance cell |

Also in scope and NOT yet reviewed by anyone: `tests/*.py`, `M4_API.md`,
`M4_RESULTS.json`, `M4_STUB_REFERENCE.md`, `HANDOFF.md`, `stub/README.md`,
`ENGINEER_PROMPT.md`, `HOUSE_RULES.md`, `packaging/dkms/`, and `README.md`.

`README.md` is new and is addressed to the seat that will integrate arcwell, not
to a browser. **Check it for over-claiming.** It leads with the latency trap
rather than the bandwidth number, cites a `results/` file per claim, and repeats
that `gain_demonstrated` is deliberately false — but it is also the most
persuasive document in the tree, and that is exactly the kind of document that
drifts ahead of its evidence.

## 3. Hygiene gate — pre-cleared, with one thing you cannot fix

I ran it rather than making you be `grep -rE`:

```sh
grep -rlnE 'dirac|noether|hertz|bosch|fritz\.box|192\.168|mfritsche|LXC 130|/rpool|marfrit-openvino' \
     . --exclude-dir=.git --exclude-dir=refs --exclude=REVIEWER_PROMPT.md
```

`--exclude=REVIEWER_PROMPT.md` matters: this file lists the patterns, so it
matches itself. Rev 2 omitted the exclude and then claimed "no hits", which is how
a reviewer's first act became reconciling a contradiction.

**Do not add `\bdata\b` to that pattern.** The target host's bare name is an
ordinary English word; the broad pattern returns 20 files, nearly all of them
"data path", "data structure", "disk data". The hostname *usage* is gone — check
it with a form that distinguishes them:

```sh
grep -rnE '(ssh|scp|rsync|on|host|from|to)[[:space:]]+data\b|`data`|data\.fritz' \
     . --exclude-dir=.git --exclude-dir=refs --exclude=REVIEWER_PROMPT.md \
  | grep -vE 'data path|data structure|data buffer|metadata'
```

The only survivors are two lines in the vendored `stub/src/xe_drm.h`, where Intel
uses `data` as a parameter name.

Five files were scrubbed to get there (`results/E2E`, `results/CL_IMPORT`,
`BACKLOG.md`, `stub/README.md`, `RTFM_DISPOSITION.md`). Re-run it; do not trust
this paragraph.

**RESOLVED — history was dropped.** Rev 2 flagged two commit messages carrying a
hostname and handed the decision to the Operator. The Operator chose to publish at
0.0.1 from a clean root. Checking afterwards, **five** commits were affected, not
two, so a targeted message rewrite would have missed three.

`main` is an orphan branch with one commit. `dev` keeps the full 74-commit history
locally and is not published. Verify:

```sh
git log main --format='%s%n%b' | grep -inE 'fritz\.box|dirac|noether|hertz|bosch|mfritsche'
```

## 4. Read first, in this order

1. `RTFM_DISPOSITION.md`, especially **§0 SETTLED**.
2. `docs/campaigns/miss-tier-direct.md` — the campaign, its gate, its dated log.
3. `KERNEL_FACTS.md` — several entries SUPERSEDE or RETRACT earlier ones. The
   retractions are the interesting part.
4. `docs/USING_ARCWELL.md` — the contract as a consumer sees it.
5. `results/*.txt` — raw output. §5 maps them to cells.

### §0 is fenced, with an escape hatch

"The model does not fit in RAM" is closed and is the premise of the streaming
design. Do not re-derive it.

The citation is `docs/design-qwen-flash-next.md:1084` **in arcint's tree, not this
one** — this repository's `docs/` holds three files and that is not among them.
§0 of `RTFM_DISPOSITION.md` quotes the relevant sentence, so the premise is
readable here even though the source is not.

**But if you believe §0 is wrong, file it as a `record`-class finding against
`RTFM_DISPOSITION.md`.** The fence exists to stop rediscovery, not to make the
premise unquestionable by the one outside look we get.

## 5. Cells to results — it is not one-to-one

15 result files, 12 cells, 3 module-side probes — and the relation is not
one-to-one in either direction. Two probes have no `results/` file at all; their
output lives in `KERNEL_FACTS.md` and `BACKLOG.md`. Rev 2 attributed `B_FABRIC` to
`aw_dma_map_probe.c`; `results/B_FABRIC_2026-09-15.txt:3` names `aw_bfabric.c` and
that is correct.

| cell / probe | result file(s) |
|---|---|
| `stub/test/aw_bfabric.c` | `B_FABRIC` |
| `stub/test/aw_bp.c` | `GATE`, `LATENCY` |
| `stub/test/aw_e2e_test.c` | `E2E`, `B60_PARITY` |
| `stub/test/aw_bo_test.c` | `BO_GATE` |
| `stub/test/aw_cl_import_test.c` | `CL_IMPORT` |
| `stub/test/aw_lifecycle_test.c` | `LIFECYCLE`, `PM_PROTECTION` |
| `stub/test/aw_batch_test.c` | `BATCH` |
| `stub/test/aw_async_test.c` | `ASYNC` |
| `stub/test/aw_gguf_test.c` | `GGUF_OPTION_A` |
| `stub/test/aw_bounce_test.c` | `BOUNCE_DETECTOR` |
| `stub/tools/aw_fiemap.c` | `FIEMAP` |
| `stub/test/aw_wc_test.c` | `WC_CARVE` |
| `stub/test/aw_expert_test.c` | `EXPERT_OPTION_B` |
| `stub/test/aw_wait_race_test.c` | `RACE_FIX` |
| `stub/src/aw_m0_gate.c` | `M0_GATE` |
| `stub/src/aw_dma_map_probe.c` | none — `BACKLOG.md` Closed item 3, `KERNEL_FACTS.md` |
| `stub/src/aw_p2p_provider_probe.c` | none — `KERNEL_FACTS.md`, the B1/D4 disproof |

## 6. Invariants — structural, not counter-reading

**6.1 No host-bounce path exists.** Do not verify this by reading
`via_host_bounce == 0`; revision 1 asked for that and it proved nothing.

Audit it structurally. Every path that could serve a request through system
memory must **return an error**. The sites:

- `stub/src/arcwell.c`, the `!b->attach->peer2peer` branch in `aw_map_buffer()` —
  returns `-EOPNOTSUPP`;
- the `!is_pci_p2pdma_page(p)` branch in the same function — returns `-EFAULT`.

Both now also increment `via_host_bounce`, so the counter records *refused
attempts* rather than being zero by construction. `debug_force_bounce=1` reaches
the first site; `results/BOUNCE_DETECTOR_2026-09-15.txt` has both arms.

**What I want from you:** is there a *third* path? A partially-populated sg, an
error unwind that leaves a buffer usable, a request that succeeds with some pages
system-resident. Name any return site that could reach `submit_bio()` without
having proven every page is a P2PDMA page.

**6.2** `AW_IOC_MAP_BUFFER` must fail, never degrade, when the mapping is not
peer-to-peer.

**6.3** No weight byte passes through the module. It moves descriptors.

**6.4** Every cell must be able to fail, and the red case must be in the result
file. **One cell is known to be missing** — see §8.

## 7. Where I am least confident — start here

1. **Async lifetime — the doubt was correct and the path existed. Find the rest.**
   Rev 3 asked you to "convince yourself there is no path where that memory is
   freed with a bio still referencing it", and listed "two threads calling
   `AW_IOC_BATCH_WAIT` on one id" among the candidates. **There was one, and it
   was that one.** `aw_batch_wait()` did `xa_load` → wait → `xa_erase` → free with
   the erase's return ignored and no lock across the sequence, so two collectors
   both got the pointer, both parked on the same completion, and because the
   completion path used `complete()` — which wakes exactly one — the winner freed
   the allocation while the loser slept inside it. Double `kfree`, double
   `kref_put` per referenced buffer, loser parked forever.

   Fixed by claiming the batch under `cl->lock` before waiting (a second
   collector gets `-EBUSY` and never parks) and by `complete_all()` throughout.
   The two halves compound: either alone leaves a defect, which is worth knowing
   before you evaluate the fix. `-EBUSY` is now in the uAPI contract, not just the
   implementation.

   **So do not treat this section as a list of things I have already cleared.**
   One of six doubts was a live memory-corruption bug. Assume the same rate
   applies to what follows.
2. **`aw_find_get()` vs unmap.** A buffer can be unmapped while a batch holds a
   reference. Check `aw_unmap_buffer()`'s `xa_erase` → `kref_put` ordering against
   a concurrent submit. Structurally the same shape as the bug above — a lookup
   and a lifetime decision that are not one atomic step — so it deserves the same
   suspicion rather than the same reassurance.
3. **Carve clamping.** `vram_usable` is operator-supplied. Unset, the module
   carves by BAR length — over stolen or absent memory. Is a warning enough, or
   should an unset value refuse to load?
4. **Partial batch failure.** `aw_submit_request()` can fail mid-batch with bios
   already in flight. The caller gets `out_err` and an index, but the buffer may
   hold a partial transfer. Is that stated clearly enough that a consumer cannot
   use half-written weights?
5. **`move_notify` poisoning.** `dma_buf_pin()` is held for the buffer's whole
   life, so the exporter must not move it and `move_notify` must not fire. It used
   to be a bare `pr_warn`, which left a buffer whose `b->pages[]` were stale still
   accepting transfers. It now `WARN`s and poisons the buffer so every later lookup
   refuses it. **Bios already in flight cannot be recalled and this does not
   pretend otherwise.** Two questions: is poisoning the right response, or should
   it be fatal to the whole module; and is the pin argument actually sound under
   xe, which is the part I am asserting rather than proving.
6. **DKMS across a kernel upgrade.** The module uses `pci_p2pdma_add_resource`,
   modern block APIs and `dma_buf_dynamic_attach`. AUTOINSTALL rebuilds on the
   next kernel. Does it fail loudly or silently?

`aw_pages_present()` was doubt 5 in rev 3. It is bounded, off the hot path and
priced in the header; the previous reviewer called it the weakest of the six and
was right. Its slot went to `move_notify`.

## 8. Known gaps — confirm they are stated, do not re-find them

- `B_P_host` was measured against my equivalent of arcint's staging ring, not
  `patches/0006` itself.
- Prefetch hiding the 1.125 ms per-expert latency is **unproven**; only its
  possibility is shown.
- The A770 is backlogged, root cause unestablished.
- `carve_all=1` is implemented but untested on a clean BAR.
- **The xarray lookup fix has no scaling cell.** Submit time against live-buffer
  count would catch its regression. I know; file it anyway so it is on the record.

## 9. Pre-registration is NOT independently verifiable. Here is my word for it.

Rev 1 asserted "the gate was stated before the measurements" with no citation.
Rev 2 added a citation — to textual ordering inside a file I wrote. That is the
same class of claim wearing better clothes, and the second reviewer said so.

Stated plainly: **the whole campaign document entered git as a single `A` in
`37ad361`, gate and measurements together.** There is no commit boundary, no
timestamp, and no artefact in this repository that proves the gate and its three
kill conditions existed before the numbers did. The evidence is the ordering of
sections in a document under my control.

Treat it as an unverified claim by the Engineer seat. If pre-registration matters
to the disposition, the finding is "cannot be verified from this repo" and the fix
is procedural: future campaigns commit the gate before the window opens.

## 10. What I want back

Per finding, all of:

- **`PUBLICATION-BLOCKING: yes/no`** — the only decision this review makes
- severity in your own words
- **evidence class**: `code`, `measured`, or `record`
- file and line
- what would have to change

Missing cells are findings. If a section yields nothing, say so explicitly — an
empty section is a signal, not a gap.

## 11. What changed, and from which review

**From the review of rev 1:**

| finding | fix |
|---|---|
| BLOCKER: no diff, scope untracked | committed; §1 has base, head, pathspec, blob hashes, reproducible chain hash |
| MAJOR: invariant 1 unfalsifiable | two increment sites + `debug_force_bounce`; §6.1 recast structurally; `BOUNCE_DETECTOR` added |
| MAJOR: scope narrower than the gate | §2 scopes the whole tree, per-file disposition |
| MAJOR: hygiene already failing | ran it, scrubbed, flagged the two commit messages as operator-blocking |
| MINOR: results mapping wrong | §5 table |
| MINOR: uncited pre-registration | §9 — now says it is **not verifiable**, which is the truth |
| MINOR: severity undefined | §10 binary |
| MINOR: §0 fence too absolute | §4 escape hatch |

**From the review of rev 2** — four of these were caused by rev 2's own fixes:

| finding | fix |
|---|---|
| §9 rested on my word wearing a citation | §9 rewritten to say so explicitly |
| chain hash unreproducible | full sha256 + the exact pipeline, in §1 |
| prompt inside its own diff | pathspec `:!REVIEWER_PROMPT.md`, range pinned to `ac4aad3` |
| §1 header stale, `aw_bounce_test.c` not in the stated range | §1 corrected with a per-commit table |
| bare hostname survived; the grep self-matched | fixed in `BACKLOG.md`; §3 adds the exclude and warns against the broad pattern |
| three stale statements left by the bounce fix | `arcwell.c` header, `docs/USING_ARCWELL.md`, `aw_bo_test.c` message all corrected |
| **six cells hard-failed on an absolute counter** | **decided: delta-based.** `via_host_bounce` is module-global, so an earlier legitimate refusal poisoned every later cell. All six now compare before/after; all rebuilt and passing |
| §2/§5 provenance disagreed | both corrected; the two probes now shown as having no `results/` file |
| `design-qwen-flash-next.md` path read local | §4 says it is arcint's tree |
| `the the` artifact, no redaction notes | fixed; every scrubbed result file now carries a REDACTION block |

**From the Operator, after rev 3:**

| decision | effect |
|---|---|
| publish at 0.0.1 from a clean root | history dropped; the five hostname-bearing commit messages go with it. §1 is now a tree review, not a diff review; §3's operator-blocking finding is closed |
| **"do not ship a) failed experiments b) known defects. Remove the former, fix the latter"** | see §12 |

## 12. What the Operator's ruling changed — review this first

This is the only material that has not been through a review round.

**Eleven files removed.** §2 names each and why. `stub/src/Makefile` was rewritten
to build `arcwell.o` plus the three probes that are evidence for `KERNEL_FACTS.md`
rules. **The check that matters, and the one I want you to repeat:** the module
built from this tree has srcversion `D28A49C0C00071F811D65EF`, byte-identical to
the one loaded when every measurement in `results/` was taken. If the removals had
touched the product, that hash would move. `results/SHIP_REGRESSION_2026-09-15.txt`.

**The stale record was the larger defect, and I did not spot it until I went
looking for defects.** `HANDOFF.md`'s status table was false on *every row* — it
still said "Bytes ever landed in Arc VRAM by a controller DMA: **no**" against
fifteen result files saying otherwise. `stub/README.md` still described the tree as
an unbuilt capability probe with a container build recipe. `M4_API.md`,
`M4_RESULTS.json`, `aw_uapi.h` and two `tests/*.py` assertions all still asserted
`UNBUILT and UNMEASURED`. Publishing any of it would have actively misled a reader
about what the project had done. All corrected against `results/`.

**Please check I did not over-correct in the other direction.** Specifically:
`M4_RESULTS.json` keeps `gain_demonstrated: false` and adds a `gain_scope` field
saying a *transfer* gain is measured while an *inference* gain is not, because the
per-expert latency (1.125 ms) exceeds the CPU compute it would hide behind
(198 µs). `tests/test_m4_boundary.py` now asserts that distinction is present.
If you think the record now reads as a stronger claim than the measurements
support, that is a finding I want.

**Item 20 — the one open behavioural defect — is disproved, not deferred.**
It carried a hypothesis ("a 128 MiB p2pdma carve destroys write-combining for BOs
inside it") that had a `TO CONFIRM` and had never been tested. `stub/test/aw_wc_test.c`
tests it two independent ways: the same BO timed either side of the carve covering
it (2.88 → 2.88 MB/s, factor 1.00×), and a card that has *no* `p2pmem` attribute at
all reading *slower* (1.59 MB/s) than the carved one — the ordering is backwards
from what the hypothesis needs. The real cause is that a CPU read of VRAM through a
WC mmap costs ~1.4 µs per access regardless, and the loop that hung reads 1 MiB
byte-wise, twenty times. What grew was the iteration count.

The original T15 arithmetic "fit" because *any* per-access cost in that range fits.
It was fitted to the conclusion. **That is worth your attention as a process
finding, not just a technical one** — CLAUDE.md §3 names exactly this failure mode
and it got past me at the time. The correction is appended beneath the original
hypothesis in `results/E2E_2026-09-15.txt` rather than replacing it, so the wrong
reasoning stays visible.

**Two cell defects found by running the suite, not by reading it.**
`aw_batch_test` read `max_inflight` as though it described the batch just
submitted; it is a module-global high-water mark that is never reset, so an earlier
larger run left it at 193 and the leg printed `note: max_inflight=193, expected 16`
and passed anyway. **This is the same defect class as the absolute `via_host_bounce`
assertions your rev-2 review caught, in a second place I did not check then.** Now
baseline-aware: `0 -> 16` on a fresh module, and an explicit refusal to conclude on
a dirty one. Separately, `aw_batch_test` and `aw_lifecycle_test` took the DRM node
positionally while every other cell takes `--drm PATH`, so passing `--drm` made
them open nothing and report a false failure. **Worth asking: what else in the
suite reads a module-global counter as if it were local?** I believe the answer is
now nothing, but I believed that after rev 2 as well.

**Unverified gap, stated rather than glossed.** The four OpenCL cells
(`aw_e2e_test`, `aw_cl_import_test`, `aw_bfabric`, `aw_bp`) were NOT re-run against
this tree: the OpenCL runtime lives in a container whose device cgroup permits the
DRM major only, so it cannot open `/dev/arcwell`, and the host that owns the module
has no OpenCL runtime. Their sources are untouched this revision and their results
stand, but the *consumption* leg was not re-executed. The seven transfer cells were,
red-first, all four red legs going red.

## 13. Rev 5 — the concurrency review, and what is NOT verified

A review of rev 4 returned 22 findings. Five were live; the rest had already been
fixed in rev 3 or rev 4 and I have re-checked each against the tree rather than
assuming (the audit is in `BACKLOG.md` T23).

**Two were memory-safety bugs in the shipped module, in code that was public.**

| # | defect | fix |
|---|---|---|
| 1 | `aw_batch_wait()` ignored `xa_erase()`'s return and held no lock across load→wait→erase→free. Two collectors on one id both freed the batch: double `kfree`, double `kref_put` per buffer | claim under `cl->lock` before waiting; second collector gets `-EBUSY` and never parks; erase return checked |
| 2 | every completion used `complete()`, which wakes one waiter. The loser stayed parked inside the allocation the winner freed | `complete_all()` throughout |
| 3 | no cell ran two waiters against one id | `stub/test/aw_wait_race_test.c`, two red legs |
| 4 | `move_notify` was a bare `pr_warn`; a moved buffer kept serving transfers from stale `b->pages[]` | `WARN` + poison the buffer; `importer_priv` now set so it can reach it |

Findings 1 and 2 compound — either fix alone still leaves a defect — which is the
part worth checking in the diff.

**VERIFIED ON HARDWARE — this supersedes the gap this section originally carried.**

`results/RACE_FIX_2026-09-16.txt`. The host came back; everything below was run,
not reasoned about:

- builds clean on the **target** kernel (7.0.14-12-pve, x86_64), no warnings;
- `aw_wait_race_test` in all three legs: `--mutate` goes red on the fixed module,
  `--serial` gives `-EINVAL` on the second collect, the race gives exactly one
  collector and `-EBUSY` (-16) for the other without waiting;
- full suite re-passes, all four red legs red;
- no arcwell `WARN`, `BUG` or oops in dmesg this boot;
- DKMS reinstalled so the installed module is the fixed one, and the version
  purge verified by planting a stale `arcwell/0.0.0`.

**The srcversion moved**: `D28A49C0C00071F811D65EF` → `0CAA5C74C7EFBCD27125AD3`.
Rev 4's "byte-identical to what produced every measurement" pin is therefore
**void**, and that is stated in the new result file rather than quietly dropped.
Every other file in `results/` was measured on the pre-fix module. The change is
confined to batch collection and `move_notify`, the transfer paths are untouched
and the suite re-passes — but the numbers themselves were not re-taken, and you
should treat that as the standing caveat on this revision.

**Two things I did NOT do, both deliberate:**

1. The cell was **not** run against the pre-fix module. That would exercise a
   double `kfree()` in a live kernel on a host running unrelated containers, and
   the likely outcome is a panic. So the red legs show the assertions
   discriminate; they do **not** show this cell would have caught the original bug
   on a running kernel. Different claims, and only the weaker one is made.
2. `results/` was not re-measured on the new module.

**A second packaging defect, and it is the more embarrassing one.** The
version-purge loop added last revision ran *after* the staging step, and since the
purged set includes the version being installed it deleted the source that had
just been staged. `dkms add` failed and the host was left with **no module
installed at all** — worse than the stranding the loop was written to prevent.
Found by running the installer, not by reading it; fixed by purging first;
verified with a planted stale version. That is two consecutive revisions where a
packaging change looked right and was wrong. Weight §7.6 accordingly.

## 14. Rev 6 — item 24, and a documentation defect the new cell caught

The Operator provisioned the dedicated ext4 partition item 24 called for. I
verified it rather than closing the row on the mount line, and the verification
earned its keep.

**Substrate: exactly as specified.** Raw partition, nothing between filesystem and
device, start sector 100665344. All 1700 expert files resolve to exactly one
extent with flags `last,eof`; zero non-plain extents in the set.

**A structural test gap.** `aw_gguf_test` covers option (A) and needs an extent
boundary to straddle, so a single-extent expert file is unreachable by it. The
layout the docs *recommend* therefore had no DMA cell behind it while the fallback
did. `stub/test/aw_expert_test.c` closes that: 32 experts, 78.6 MB, one batch,
every expert byte-verified, `via_host_bounce` unmoved, red leg failing on content
rather than on an error code.

**The defect it caught is in my own documentation.** `docs/USING_ARCWELL.md` §5 and
`README.md` claimed the layout gives "1 extent per expert and one DMA segment".
A segment is a bio and a bio caps at `BIO_MAX_VECS` (256) pages = 1 MiB; these
experts are 2.34 MiB, so three bios is the floor. Measured 4 for one expert,
converging to 3.09 at 32. **Anyone sizing a batch against that sentence was wrong
by a factor of three.** Both documents corrected to say what the layout actually
buys — one *request* per expert, not one bio.

Worth your attention: my first version of the cell asserted `segments == experts`
and went red against a **correct** module. The assertion was wrong, not the code,
and I only found out because I ran it. Please check the corrected assertion is not
now so loose that it cannot fail — it allows up to one extra bio per expert over
the floor, and I would rather you told me that window is too wide than discover it
the same way.

**Standing pattern, fourth instance.** Item 20's fitted arithmetic,
`max_inflight`'s module-global high-water mark, the installer purge tested where
it could not fail, and now a documented claim whose cell could not structurally
reach it. Each was individually defensible; together they say "there is a cell for
this" was being read as "this is measured". If you see a fifth, it is the finding,
not the instance.
