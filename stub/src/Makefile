# arcwell — the module, plus the probes that produced the recorded evidence.
#
# Failed experiments are not shipped. The p2pdma probes that carried fabricated
# externs (defect D4), the never-working dma-buf candidate (D1) and the quarantined
# raw-PRP probe (D2) were removed at 0.0.1; what they established is in
# KERNEL_FACTS.md, which is the part worth keeping.
ccflags-y += -I$(src)/../include

obj-m += arcwell.o              # the product

# Diagnostics, kept because results/ cites them and they still run:
obj-m += aw_dma_map_probe.o     # dma_map_resource() on the GPU BAR for the NVMe device
obj-m += aw_p2p_provider_probe.o # kernel-7.0 p2pdma provider path; disproved blocker B1
obj-m += aw_m0_gate.o           # the M0 acceptance cell

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
