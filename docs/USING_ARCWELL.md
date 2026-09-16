# Using arcwell

The arcwell uAPI, how to drive it, and the two ways to get it wrong.

Audience: someone wiring arcwell into an inference engine. Everything here is
measured on the development hardware; every number names where it came from.
Header: `stub/include/aw_uapi.h`. Device: `/dev/arcwell`.

---

## 1. What arcwell is, in one paragraph

arcwell makes an NVMe controller DMA expert weights straight into Intel Arc VRAM.
No host bounce buffer, no page cache, no per-read filesystem walk. It is a
**throughput** mechanism: measured 2.91 GB/s against a cold host-fed path's
1.58 GB/s, at 16.3x less CPU per GiB. It is **not** a latency mechanism: a single
synchronous expert fetch costs 1.125 ms, while the host CPU can *compute* that
same expert in 198 us. **Used synchronously on the decode path it loses.** Every
design rule below follows from that one fact.

## 2. The shape

It follows cuFile's shape with no CUDA naming or dependency:

| cuFile / GPUDirect | arcwell |
|---|---|
| `cuFileBufRegister` | `AW_IOC_MAP_BUFFER` |
| `cuFileBufDeregister` | `AW_IOC_UNMAP_BUFFER` |
| `cuFileRead` | `AW_IOC_READ_BLOCKS` |
| `cuFileBatchIOSubmit` | `AW_IOC_SUBMIT_BATCH` |
| `cuFileBatchIOGetStatus` | `AW_IOC_BATCH_WAIT` |
| — | `AW_IOC_STATS` (so a host bounce cannot hide) |

The device-pointer analog is a **buffer handle**, not a pointer. There is no file
handle: arcwell takes raw NVMe block ranges, and translating a file to blocks is
the caller's job (§5).

## 3. The lifecycle

```c
int fd = open("/dev/arcwell", O_RDWR);

/* once per buffer, at load */
struct aw_ioc_map_buffer mb = {
    .in_handle = dmabuf_fd,        /* from DRM_IOCTL_PRIME_HANDLE_TO_FD */
    .in_source = AW_BUF_DMABUF,
    .in_length = size,
};
ioctl(fd, AW_IOC_MAP_BUFFER, &mb);
if (!(mb.out_flags & AW_MAP_F_REQUIRE_P2P))
        abort();                   /* see §6 -- this check is not optional */

/* per inference step */
struct aw_ioc_batch_submit sb = { .in_requests = (uintptr_t)reqs,
                                  .in_count = n };
ioctl(fd, AW_IOC_SUBMIT_BATCH, &sb);      /* returns immediately */
... do other work ...
struct aw_ioc_batch_wait bw = { .in_batch_id = sb.out_batch_id,
                                .in_timeout_us = UINT64_MAX };
ioctl(fd, AW_IOC_BATCH_WAIT, &bw);        /* collect */

ioctl(fd, AW_IOC_UNMAP_BUFFER, &handle);  /* or just close(fd) */
```

Buffers are owned by the file descriptor. Closing it releases every registration
and drains every in-flight batch, so a crashing client cannot strand VRAM.

## 4. THE TWO WAYS TO GET THIS WRONG

### 4.1 Calling it synchronously on the critical path

`AW_IOC_READ_BLOCKS` and `AW_IOC_READ_BATCH` block until the transfer finishes.
If the layer that needs an expert is the thing that asks for it, you pay 1.125 ms
per expert on the serial path — against 198 us to compute it on the CPU instead
(`docs/milestone-0.3.0.md` M14) and ~45 us for an overlapped upload (M9).

arcint already learned this once: M9's synchronous slot upload measured 0.4 t/s,
and overlapping it with compute took the same hardware to 9.1 t/s. Re-issuing
arcwell synchronously reproduces that defect with a worse constant.

**Use `AW_IOC_SUBMIT_BATCH` ahead of need, and collect later.** Measured: submit
returns in 18.3% of the batch's wall time, and four batches overlap cleanly.

### 4.2 Trusting `out_flags`, or not checking it

`AW_IOC_MAP_BUFFER` fails rather than silently degrading — but only because it
checks. xe **silently disables peer-to-peer** when `pci_p2pdma_distance() < 0`
(`xe_dma_buf.c:30-32`) and then migrates the buffer to system RAM. A client that
registers with plain `dma_buf_attach()` and does not re-check gets a working-looking
path that quietly runs through host memory.

Always assert `out_flags & AW_MAP_F_REQUIRE_P2P`, and read `AW_IOC_STATS`.

`via_host_bounce` counts mappings the module **detected and refused** because they
could only have been served through host memory. Nonzero does not mean a bounce
happened — no code path bounces; both sites return an error. It means a caller
asked for something whose configuration is wrong.

**Compare it before and after your own work: the counter is module-global, not
per-client.** An absolute reading includes every other client's refusals, and a
refusal you did not cause will otherwise look like your own.

## 5. Getting block ranges: FIEMAP is yours, not arcwell's

arcwell takes absolute NVMe LBAs. Translation is a once-at-open userspace step
(`M4_API.md`), and `stub/tools/aw_fiemap.c` is the reference implementation.

```
absolute LBA = fe_physical / 512 + <partition start sector>
```

Three rules, each learned by a cell going red:

- **Add the partition start.** `fe_physical` is filesystem-relative. Omitting it
  produced an LBA 48 GiB early — inside other partitions — and the read
  *succeeded*, returning plausible garbage. Only a byte comparison caught it.
- **Reject non-plain extents.** `UNWRITTEN`, `DELALLOC`, `INLINE`, `ENCODED`,
  `UNKNOWN` all mean the bytes are not where FIEMAP says yet. Pass
  `FIEMAP_FLAG_SYNC`.
- **Handle spanning.** One expert can cross an extent boundary and become two
  requests at different `in_dest_offset`s in the same buffer. Measured on the real
  UD-Q3_K_XL shard: 24 extents over 40 GB, so a 2.4 MB slice spans a boundary
  ~0.14% of the time — rare enough to miss in testing and certain to happen in
  production.

Layout matters: one file per expert, `fallocate`d, gives exactly **1 extent per
expert**, so an expert is **one request** at one `in_dest_offset`. Ranges inside a
big GGUF work too, but an expert that straddles an extent boundary becomes **two**
requests into the same buffer.

**It does not make an expert one bio, and an earlier revision of this document
wrongly said it did.** A bio holds at most `BIO_MAX_VECS` (256) pages = 1 MiB, so
any expert larger than that is split regardless of layout — measured at 4 bios for
a 2.34 MiB expert (`results/EXPERT_OPTION_B_2026-09-16.txt`). Size batches against
the request count, which the layout controls, not against the segment count, which
it does not.

## 6. Requirements and limits

| | |
|---|---|
| Buffer source | a dma-buf fd exported from an xe VRAM BO |
| BO requirements | `NEEDS_VISIBLE_VRAM`, `CPU_CACHING_WC`, size a multiple of 64 KiB |
| Alignment | `in_dest_offset` and the transfer length must be page-aligned |
| Batch size | 1..`AW_BATCH_MAX` (256) requests |
| Filesystem | one that implements FIEMAP. **ZFS and btrfs do not.** ext4 does |
| Card | the GPU and the NVMe must be reachable for P2P (`pci_p2pdma_distance() >= 0`) |

Module parameters that are not optional in practice:

- `vram_usable=<bytes>` — from xe's `"usable size exclude stolen"` boot line. The
  BAR is **larger than VRAM** (32 GiB BAR over 24 GiB of memory on one card here),
  and carving by BAR length puts DMA-able pages over memory that is not there.
- `carve_all=1` — optional, but if used it must be the **first** carve after boot:
  a p2pdma carve cannot be released, so a prior carve blocks it.

## 7. Costs, measured

| | |
|---|---|
| Bandwidth | 2.91 GB/s (vs 1.58 host cold, 13.99 GB/s fabric ceiling) |
| CPU | 0.0121 CPU-s/GiB (vs 0.1971 host cold) |
| Latency, one expert | 1.125 ms — **do not put this on the critical path** |
| Submit cost | ~9.7 ms per 64-expert batch idle, ~42 ms with four outstanding |
| Host RAM | struct pages for carved VRAM, ~1.56% of the carved range |

The submit cost is why prefetch should use **smaller, more frequent batches**
rather than few deep ones.

## 8. What is not proven yet

- **Prefetch actually hiding the 1.125 ms** against a real routing trace. This is
  the open question; everything above only shows it is *possible* to hide it.
- `B_P_host` measured against arcint's own staging ring rather than an equivalent.
- Anything on the A770: that card's PCIe path goes unresponsive under load and it
  is backlogged. All numbers here are the Arc Pro B60.
