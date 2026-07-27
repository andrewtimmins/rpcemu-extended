#
# SharedClipboard module - assembled with GNU ARM binutils.
# (build.sh overrides AS/LD/OBJCOPY with the arm-linux-gnueabi- toolchain.)
#
# The guest half of the shared clipboard. It goes in poduleroms/, which is
# scanned into the general-purpose expansion card, so it loads at boot with
# nothing to install.
#
# The interface it speaks is RiscOS Cloverleaf's; see the top of the source.
#

AS = armas
LD = armld
OBJCOPY = armobjcopy

MODULE = sharedclipboard

# A single token where names are parsed (*RMKill, *RMEnsure, the module header),
# and the readable form where one is displayed.
TITLE = SharedClipboard
DISPLAY_NAME = Shared Clipboard Helper

ROM_IMAGE = sharedclipboard,ffa

MODULE_VERSION := $(shell awk '$$1 == "Module_MajorVersion" { print $$2 }' VersionNum)
MODULE_DATE := $(shell awk '/^Module_Date/ { $$1 = ""; sub(/^[ \t]+/, ""); print }' VersionNum)

all: $(ROM_IMAGE)

# The help string, which is what *Help and *Modules show.
version.inc: VersionNum Makefile
	@printf '@ Generated from VersionNum by the Makefile - do not edit.\n' > $@
	@printf 'help:\n\t.string\t"%s\\t%s (%s)"\n\t.align\n' \
		'$(DISPLAY_NAME)' '$(MODULE_VERSION)' '$(MODULE_DATE)' >> $@
	@echo "version.inc: $(DISPLAY_NAME) $(MODULE_VERSION) ($(MODULE_DATE))"

$(ROM_IMAGE): $(MODULE).elf
	$(OBJCOPY) -O binary $< $@

%.elf: %.o
	$(LD) --section-start .text=0 -o $@ $<

$(MODULE).o: $(MODULE).s version.inc ucstable.inc
	$(AS) -o $@ $<

clean:
	rm -f *.o *.elf $(ROM_IMAGE) version.inc
