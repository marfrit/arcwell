#!/bin/bash
# Install arcwell as a DKMS module. Run as root from the repository root.
set -eu
VER=$(awk -F'"' '/^PACKAGE_VERSION=/{print $2}' packaging/dkms/dkms.conf)
SRC=/usr/src/arcwell-$VER

# PURGE FIRST. This loop removes /usr/src/arcwell-<ver> for every registered
# version, and $VER is normally one of them -- so running it AFTER staging
# deletes the source that was just staged and `dkms add` then fails with the
# module half-removed and nothing installed. Measured: it uninstalled a working
# module and left the host with none.
echo "== purging previously registered versions =="
for old in $(dkms status 2>/dev/null | sed -n 's|^arcwell/\([^,:]*\).*|\1|p' | sort -u); do
	echo "  removing arcwell/$old"
	dkms remove -m arcwell -v "$old" --all 2>/dev/null || true
	rm -rf "/usr/src/arcwell-$old"
done

echo "== staging $SRC =="
rm -rf "$SRC"
install -d "$SRC/include"
install -m644 stub/src/arcwell.c        "$SRC/arcwell.c"
install -m644 stub/include/aw_uapi.h    "$SRC/include/aw_uapi.h"
install -m644 packaging/dkms/dkms.conf  "$SRC/dkms.conf"
install -m644 packaging/dkms/Makefile   "$SRC/Makefile"

echo "== udev =="
install -m644 packaging/dkms/99-arcwell.rules /etc/udev/rules.d/99-arcwell.rules
udevadm control --reload 2>/dev/null || true
getent group render >/dev/null || echo "  WARNING: no 'render' group; the rule will not change ownership"

echo "== detector =="
install -m755 packaging/dkms/arcwell-detect-vram /usr/local/sbin/arcwell-detect-vram

echo "== dkms add/build/install =="
dkms add    -m arcwell -v "$VER"
dkms build  -m arcwell -v "$VER"
dkms install -m arcwell -v "$VER"

# Verify the node permissions rather than assuming. udev is asynchronous: a stat
# immediately after modprobe races it and shows root:root, which looks like a
# broken rule and is not one.
if modprobe arcwell 2>/dev/null; then
	udevadm settle
	echo "== /dev/arcwell: $(stat -c '%A %U:%G' /dev/arcwell 2>/dev/null || echo 'absent') =="
	rmmod arcwell 2>/dev/null || true
fi

cat <<MSG

arcwell $VER installed via DKMS.

NEXT, and it is not optional -- the module refuses to carve safely without it:

    arcwell-detect-vram <gpu-bdf> [nvme-bdf] [bdev] > /tmp/arcwell.conf \
        && install -m644 /tmp/arcwell.conf /etc/modprobe.d/arcwell.conf
    modprobe arcwell

Write to a temp file and install only on success. A naive
"detect ... | tee /etc/modprobe.d/arcwell.conf" TRUNCATES the config before the
detector runs, so a failed detection leaves an empty or garbage file and modprobe
then fails with "Invalid argument". Measured, on the first run of this installer.

vram_usable comes from xe's own boot line; the PCI BAR is larger than VRAM and
carving by BAR length registers DMA pages over memory that is not there.
MSG
