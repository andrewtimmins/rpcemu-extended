#
# RPCEmuMonitor module - assembled with GNU ARM binutils.
# (build.sh overrides AS/LD/OBJCOPY with the arm-linux-gnueabi- toolchain.)
#
# Carries the monitor definition the RISC OS versions without EDID support need,
# and loads it. This one DOES belong in poduleroms/, unlike the graphics card's
# driver: it is a general-purpose module rather than one card's own firmware.
#

AS = armas
LD = armld
OBJCOPY = armobjcopy

MODULE = rpcemumonitor

TITLE = RPCEmuMonitor
DISPLAY_NAME = RPCEmu Monitor

ROM_IMAGE = RPCEmuMonitor,ffa

MODULE_VERSION := $(shell awk '$$1 == "Module_MajorVersion" { print $$2 }' VersionNum)
MODULE_DATE := $(shell awk '/^Module_Date/ { $$1 = ""; sub(/^[ \t]+/, ""); print }' VersionNum)

# The mode ladder comes from the emulator's own table, so the definition this
# module carries and the modes the emulator offers cannot drift apart.
DISPLAY_MODE_C = ../../src/display_mode.c

all: $(ROM_IMAGE)

mdf.inc: mkmdf.py $(DISPLAY_MODE_C)
	@python3 mkmdf.py $(DISPLAY_MODE_C) $@

version.inc: VersionNum Makefile
	@printf '@ Generated from VersionNum by the Makefile - do not edit.\n' > $@
	@printf 'help:\n\t.string\t"%s\\t%s (%s)"\n\t.balign\t4\n' \
		'$(DISPLAY_NAME)' '$(MODULE_VERSION)' '$(MODULE_DATE)' >> $@
	@echo "version.inc: $(DISPLAY_NAME) $(MODULE_VERSION) ($(MODULE_DATE))"

$(MODULE).o: $(MODULE).s version.inc mdf.inc
	$(AS) -o $@ $(MODULE).s

$(MODULE).elf: $(MODULE).o
	$(LD) --section-start .text=0 -o $@ $<

$(ROM_IMAGE): $(MODULE).elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -f *.o *.elf $(ROM_IMAGE) version.inc mdf.inc

.PHONY: all clean
