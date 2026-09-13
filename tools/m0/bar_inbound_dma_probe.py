#!/usr/bin/env python3
"""arcwell M0 experiment 1 probe.

O_DIRECT read from the NVMe block device straight into an mmap of a GPU VRAM
BAR (the PCI sysfs resource2 file). Records what the kernel and the hardware
did. Read-only on the NVMe (LBA 0, the GPT header). The only VRAM that could
be touched is one 4 KiB block at a free offset, and only if the kernel accepts
a BAR mapping as a DMA target at all.
"""
import errno
import hashlib
import mmap
import os
import time

NVME = "/dev/nvme0n1"
BLK = 4096
CARDS = [
    ("Arc Pro B60", "0000:0f:00.0", 16 << 30),
    ("Arc A770", "0000:06:00.0", 8 << 30),
]


def sha(b):
    return hashlib.sha256(b).hexdigest()[:16]


def main():
    print("kernel", os.uname().release, "utc", time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
    nfd = os.open(NVME, os.O_RDONLY | os.O_DIRECT)
    ctl = mmap.mmap(-1, BLK)
    n = os.preadv(nfd, [ctl], 0)
    print(f"control: O_DIRECT preadv({NVME} LBA0 -> anonymous host page) = {n} bytes sha256={sha(ctl[:BLK])}")
    for name, bdf, off in CARDS:
        res = f"/sys/bus/pci/devices/{bdf}/resource2"
        size = os.path.getsize(res)
        print(f"[{name}] {res}: BAR size {size >> 30} GiB, target offset {off >> 30} GiB")
        try:
            fd = os.open(res, os.O_RDWR | os.O_SYNC)
            m = mmap.mmap(fd, BLK, flags=mmap.MAP_SHARED,
                          prot=mmap.PROT_READ | mmap.PROT_WRITE, offset=off)
        except OSError as e:
            print(f"  mmap of BAR: OSError errno={e.errno} ({errno.errorcode.get(e.errno)}): {e.strerror}")
            continue
        before = bytes(m[:BLK])
        print(f"  CPU read through BAR mapping before: sha256={sha(before)} first16={before[:16].hex()}")
        try:
            n = os.preadv(nfd, [m], 0)
            print(f"  O_DIRECT preadv(NVMe LBA0 -> BAR mapping) = {n} bytes  ** kernel accepted a BAR mapping as DMA target **")
        except OSError as e:
            print(f"  O_DIRECT preadv(NVMe LBA0 -> BAR mapping): OSError errno={e.errno} ({errno.errorcode.get(e.errno)}): {e.strerror}")
        after = bytes(m[:BLK])
        state = "CHANGED" if after != before else "unchanged"
        print(f"  CPU read through BAR mapping after : sha256={sha(after)} {state}; equals NVMe block: {after == bytes(ctl[:BLK])}")
        m.close()
        os.close(fd)


if __name__ == "__main__":
    main()
