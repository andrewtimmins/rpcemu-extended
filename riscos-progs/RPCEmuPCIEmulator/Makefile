#
# RPCEmuPCIEmulator module - assembled with GNU ARM binutils.
# (build.sh overrides AS/LD/OBJCOPY with the arm-linux-gnueabi- toolchain.)
#
# Supplies the handful of PCI SWIs that RISC OS drivers use to get at
# DMA-capable memory, on a machine that has no PCI bus. See docs/usb.md.
#
AS = armas
LD = armld
OBJCOPY = armobjcopy

MODULE = rpcemupciemulator

TITLE = RPCEmuPCIEmulator

# Version and date come from VersionNum. Either can be overridden on the command
# line, e.g. make MODULE_DATE="$(shell date +'%d %b %Y')" for a true build stamp.
MODULE_VERSION := $(shell awk '$$1 == "Module_MajorVersion" { print $$2 }' VersionNum)
MODULE_DATE := $(shell awk '/^Module_Date/ { $$1 = ""; sub(/^[ \t]+/, ""); print }' VersionNum)

all: $(MODULE),ffa

# The help string, which is what *Help and *Modules show. Generated so the
# version and date come from VersionNum alone.
version.inc: VersionNum Makefile
	@printf '@ Generated from VersionNum by the Makefile - do not edit.\n' > $@
	@printf 'help:\n\t.string\t"%s\\t%s (%s)"\n\t.align\n' \
		'$(TITLE)' '$(MODULE_VERSION)' '$(MODULE_DATE)' >> $@
	@echo "version.inc: $(TITLE) $(MODULE_VERSION) ($(MODULE_DATE))"

%,ffa: %.elf
	$(OBJCOPY) -O binary $< $@

%.elf: %.o
	$(LD) --section-start .text=0 -o $@ $<

$(MODULE).o: $(MODULE).s version.inc
	$(AS) -o $@ $<

clean:
	rm -f *.o *.elf *,ffa version.inc
