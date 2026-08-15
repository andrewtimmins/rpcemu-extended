#
# MultiFS and MultiFSFiler - assembled with GNU ARM binutils.
# (build.sh overrides AS/LD/OBJCOPY with the arm-linux-gnueabi- toolchain.)
#
# MultiFS reads FAT media, which is what a USB stick almost always is and what
# FileCore will not mount. MultiFSFiler puts it on the icon bar. See docs/usb.md.
#
AS = armas
LD = armld
OBJCOPY = armobjcopy


TITLE_multifs = MultiFS
TITLE_multifsfiler = MultiFSFiler

MODULE_VERSION := $(shell awk '$$1 == "Module_MajorVersion" { print $$2 }' VersionNum)
MODULE_DATE := $(shell awk '/^Module_Date/ { $$1 = ""; sub(/^[ \t]+/, ""); print }' VersionNum)

all: multifs,ffa multifsfiler,ffa

# The help string, which is what *Help and *Modules show. Generated so the
# version and date come from VersionNum alone.
multifs-version.inc: VersionNum Makefile
	@printf '@ Generated from VersionNum by the Makefile - do not edit.\n' > $@
	@printf 'help:\n\t.string\t"%s\\t%s (%s) RPCEmu"\n\t.align\n' \
		'$(TITLE_multifs)' '$(MODULE_VERSION)' '$(MODULE_DATE)' >> $@
	@echo "multifs-version.inc: $(TITLE_multifs) $(MODULE_VERSION) ($(MODULE_DATE))"

%,ffa: %.elf
	$(OBJCOPY) -O binary $< $@

%.elf: %.o
	$(LD) --section-start .text=0 -o $@ $<

multifs-version.inc multifsfiler-version.inc: VersionNum Makefile

multifsfiler-version.inc:
	@printf '@ Generated from VersionNum by the Makefile - do not edit.\n' > $@
	@printf 'help:\n\t.string\t"%s\\t%s (%s) RPCEmu"\n\t.align\n' \
		'$(TITLE_multifsfiler)' '$(MODULE_VERSION)' '$(MODULE_DATE)' >> $@
	@echo "multifsfiler-version.inc: $(TITLE_multifsfiler) $(MODULE_VERSION) ($(MODULE_DATE))"

multifs.o: multifs.s multifs-version.inc
	$(AS) -o $@ $<

multifsfiler.o: multifsfiler.s multifsfiler-version.inc
	$(AS) -o $@ $<

clean:
	rm -f *.o *.elf *,ffa *-version.inc
