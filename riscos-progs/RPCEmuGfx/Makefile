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

# The desktop utility is a separate application, assembled and linked to run at
# &8000 (an absolute file), then carried inside the module's ROM image.
APP_IMAGE = gfxapp

all: $(ROM_IMAGE)

# The version as packed BCD, which is the form GraphicsV_ReadInfo reports: the
# digits of "x.yz" as hex nibbles, so 0.02 becomes &002. Derived here rather
# than written out again, so VersionNum stays the only place the version lives.
MODULE_VERSION_BCD := $(shell echo '$(MODULE_VERSION)' | tr -d '.')

# The help string, which is what *Help and *Modules show. Generated so the
# version and date come from VersionNum alone.
version.inc: VersionNum Makefile
	@printf '@ Generated from VersionNum by the Makefile - do not edit.\n' > $@
	@printf 'help:\n\t.string\t"%s\\t%s (%s)"\n\t.align\n' \
		'$(DISPLAY_NAME)' '$(MODULE_VERSION)' '$(MODULE_DATE)' >> $@
	@printf '\n\t@ Version as BCD, for GraphicsV_ReadInfo.\n' >> $@
	@printf '\t.set\tVERSION_BCD, 0x%s\n' '$(MODULE_VERSION_BCD)' >> $@
	@echo "version.inc: $(DISPLAY_NAME) $(MODULE_VERSION) ($(MODULE_DATE))"

$(ROM_IMAGE): $(MODULE).elf
	$(OBJCOPY) -O binary $< $@

%.elf: %.o
	$(LD) --section-start .text=0 -o $@ $<

$(MODULE).o: $(MODULE).s version.inc resfs.inc $(APP_IMAGE)
	$(AS) -o $@ $<

# The files carried in the card's ROM. Regenerated whenever a file with a RISC OS
# filetype suffix is added, removed or changed here, so icons, templates and
# message files only have to be dropped into this directory.
RESOURCES = $(wildcard *,[0-9a-f][0-9a-f][0-9a-f]) \
	    $(wildcard !Boot !Help !Run !Sprites !Sprites22 Messages Templates)

resfs.inc: mkresfs.py $(APP_IMAGE) $(RESOURCES)
	python3 mkresfs.py . $@

$(APP_IMAGE): app.s
	$(AS) -o app.o app.s
	$(LD) --section-start .text=0x8000 -o app.elf app.o
	$(OBJCOPY) -O binary app.elf $@

clean:
	rm -f *.o *.elf $(ROM_IMAGE) $(APP_IMAGE) version.inc resfs.inc
