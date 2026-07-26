#
# RPCEmuGfx module - assembled with GNU ARM binutils.
# (build.sh overrides AS/LD/OBJCOPY with the arm-linux-gnueabi- toolchain.)
#
# The display driver for the emulated graphics card. It is carried in that card's
# own ROM, so it must NOT be copied into poduleroms/: that directory is scanned
# into the general-purpose expansion card, and the module would then be presented
# twice. It goes in gfxroms/ instead, which is what src/gfxcard.c reads.
#

AS = armas
LD = armld
OBJCOPY = armobjcopy

MODULE = rpcemugfx

# The module name is a single token, because *RMKill / *RMEnsure and the header
# title need one. The help string uses the readable form, as RISC OS modules
# conventionally do.
TITLE = RPCEmuGfx
DISPLAY_NAME = RPCEmu Graphics

# The name the card's ROM is read from, which is the RISC OS form of the name.
ROM_IMAGE = RPCEmuGfx,ffa

# Version and date come from VersionNum. Either can be overridden on the command
# line, e.g. make MODULE_DATE="$(shell date +'%d %b %Y')" for a true build stamp.
MODULE_VERSION := $(shell awk '$$1 == "Module_MajorVersion" { print $$2 }' VersionNum)
MODULE_DATE := $(shell awk '/^Module_Date/ { $$1 = ""; sub(/^[ \t]+/, ""); print }' VersionNum)

all: $(ROM_IMAGE)

# The help string, which is what *Help and *Modules show. Generated so the
# version and date come from VersionNum alone.
version.inc: VersionNum Makefile
	@printf '@ Generated from VersionNum by the Makefile - do not edit.\n' > $@
	@printf 'help:\n\t.string\t"%s\\t%s (%s)"\n\t.align\n' \
		'$(DISPLAY_NAME)' '$(MODULE_VERSION)' '$(MODULE_DATE)' >> $@
	@echo "version.inc: $(DISPLAY_NAME) $(MODULE_VERSION) ($(MODULE_DATE))"

$(ROM_IMAGE): $(MODULE).elf
	$(OBJCOPY) -O binary $< $@

%.elf: %.o
	$(LD) --section-start .text=0 -o $@ $<

$(MODULE).o: $(MODULE).s version.inc
	$(AS) -o $@ $<

clean:
	rm -f *.o *.elf $(ROM_IMAGE) version.inc
