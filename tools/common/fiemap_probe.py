#!/usr/bin/env python3
"""Measure whether THIS filesystem can answer FIEMAP at all.

Reported as an environment attestation, not a library verdict: the arcwell
design leans on "FIEMAP once at open", and that premise does not hold on every
filesystem in the fleet (ZFS has no FIEMAP; btrfs on this host returns
EOPNOTSUPP). Printing the filesystem's own errno keeps the claim honest.
"""
import ctypes
import errno
import os
import struct
import sys
import tempfile

# FS_IOC_FIEMAP = _IOR('f', 11, struct fiemap). Verified against
# <linux/fs.h> on this host (0xc020660b); a wrong number yields a bogus ENOTTY
# and a false "unavailable" verdict, so it is measured, not remembered.
FS_IOC_FIEMAP = 0xC020660B
FIEMAP_HDR = 32


def fiemap_count(fd):
    """Count-only FIEMAP query (fm_extent_count=0). Raises OSError with the
    filesystem's own errno when FIEMAP is not supported."""
    # struct fiemap { u64 start, length; u32 flags, mapped_extents,
    #                 extent_count, reserved }  -> 32 bytes
    buf = ctypes.create_string_buffer(FIEMAP_HDR + 64 * 40)
    struct.pack_into("<QQIIII", buf, 0, 0, 0, 0, 0, 0, 0)
    libc = ctypes.CDLL(None, use_errno=True)
    r = libc.ioctl(fd, ctypes.c_ulong(FS_IOC_FIEMAP), buf)
    if r != 0:
        err = ctypes.get_errno() or errno.EIO
        raise OSError(err, os.strerror(err))
    return struct.unpack_from("<QQIIII", buf, 0)[3]


def main():
    path = tempfile.mktemp(prefix="arcwell_fiemap_", dir="/tmp")
    with open(path, "wb") as fh:
        fh.write(os.urandom(1 << 20))
        fh.flush()
        os.fsync(fh.fileno())
    try:
        fd = os.open(path, os.O_RDONLY)
    except OSError as exc:
        print("open-failed: %s" % exc)
        return 2
    try:
        n = fiemap_count(fd)
        print("fiemap=available extents=%d" % n)
        rc = 0
    except OSError as exc:
        print("fiemap=unavailable errno=%s(%d)" %
              (errno.errorcode.get(exc.errno, "?"), exc.errno))
        rc = 0  # unavailable is a measured environment fact, not a script error
    finally:
        os.close(fd)
        os.unlink(path)
    return rc


if __name__ == "__main__":
    sys.exit(main())
