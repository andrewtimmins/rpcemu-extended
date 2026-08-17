#
# MultiFS and MultiFSFiler - assembled with GNU ARM binutils.
# (build.sh overrides AS/LD/OBJCOPY with the arm-linux-gnueabi- toolchain.)
#
# MultiFS reads FAT media, which is what a USB stick almost always is and what
# FileCore will not mount. MultiFSFiler puts it on the icon bar. See docs/usb.md.
#
# The images are named with a number in front because that is how they are named
# in the card's ROM, where files load in name order: MultiFS needs SCSIFS, which
# is 60, and the filer needs MultiFS. Building them under the names they are
# actually installed under keeps one convention across every guest module and
# lets tests/check-guest-modules.sh compare like with like.
#
AS = armas
LD = armld
OBJCOPY = armobjcopy


TITLE_multifs = MultiFS
TITLE_multifsfiler = MultiFSFiler

MODULE_VERSION := $(shell awk '$$1 == "Module_MajorVersion" { print $$2 }' VersionNum)
MODULE_DATE := $(shell awk '/^Module_Date/ { $$1 = ""; sub(/^[ \t]+/, ""); print }' VersionNum)

all: 70-multifs,ffa 80-multifsfiler,ffa

# The help string, which is what *Help and *Modules show. Generated so the
# version and date come from VersionNum alone.
multifs-version.inc: VersionNum Makefile
	@printf '@ Generated from VersionNum by the Makefile - do not edit.\n' > $@
	@printf 'help:\n\t.string\t"%s\\t%s (%s) RPCEmu"\n\t.align\n' \
		'$(TITLE_multifs)' '$(MODULE_VERSION)' '$(MODULE_DATE)' >> $@
	@echo "multifs-version.inc: $(TITLE_multifs) $(MODULE_VERSION) ($(MODULE_DATE))"

multifsfiler-version.inc: VersionNum Makefile
	@printf '@ Generated from VersionNum by the Makefile - do not edit.\n' > $@
	@printf 'help:\n\t.string\t"%s\\t%s (%s) RPCEmu"\n\t.align\n' \
		'$(TITLE_multifsfiler)' '$(MODULE_VERSION)' '$(MODULE_DATE)' >> $@
	@echo "multifsfiler-version.inc: $(TITLE_multifsfiler) $(MODULE_VERSION) ($(MODULE_DATE))"

70-multifs,ffa: multifs.elf
	$(OBJCOPY) -O binary $< $@

80-multifsfiler,ffa: multifsfiler.elf
	$(OBJCOPY) -O binary $< $@

%.elf: %.o
	$(LD) --section-start .text=0 -o $@ $<

# multifs.s .includes the per-format files, so they are dependencies of the
# object even though they are never assembled on their own.
multifs.o: multifs.s multifs-fat.s multifs-exfat.s multifs-ntfs.s multifs-version.inc
	$(AS) -o $@ $<

RESOURCES = $(wildcard *,[0-9a-f][0-9a-f][0-9a-f])

# The sprites and window template MultiFSFiler puts into Resources:$, built
# into the module image itself. Drop a file in here and it appears.
resfs.inc: mkresfs.py $(RESOURCES)
	python3 mkresfs.py . $@

multifsfiler.o: multifsfiler.s multifsfiler-version.inc resfs.inc
	$(AS) -o $@ $<

clean:
	rm -f *.o *.elf *,ffa *-version.inc
