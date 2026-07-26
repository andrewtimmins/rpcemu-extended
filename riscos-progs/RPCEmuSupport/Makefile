#
# RPCEmuSupport module - assembled with GNU ARM binutils.
# (build.sh overrides AS/LD/OBJCOPY with the arm-linux-gnueabi- toolchain.)
#
# Formerly the HostCmd gateway module. Only the module's name changed when it
# took on duties beyond the host command channel; the SWI interface, the *
# commands and the host-side protocol are all as they were.
#

AS = armas
LD = armld
OBJCOPY = armobjcopy

MODULE = rpcemusupport
TITLE = RPCEmuSupport

# Version and date come from VersionNum. Either can be overridden on the command
# line, e.g. make MODULE_DATE="$(shell date +'%d %b %Y')" for a true build stamp.
MODULE_VERSION := $(shell awk '$$1 == "Module_MajorVersion" { print $$2 }' VersionNum)
MODULE_DATE := $(shell awk '/^Module_Date/ { $$1 = ""; sub(/^[ \t]+/, ""); print }' VersionNum)

all: $(MODULE),ffa

# The help string (what *Help and *Modules show) and the banner printed by
# *HostCmdStatus, generated so the version lives in exactly one place.
version.inc: VersionNum Makefile
	@printf '@ Generated from VersionNum by the Makefile - do not edit.\n' > $@
	@printf 'help:\n\t.string\t"%s\\t%s (%s)"\n\t.align\n' \
		'$(TITLE)' '$(MODULE_VERSION)' '$(MODULE_DATE)' >> $@
	@printf 'module_banner:\n\t.string\t"%s %s (%s)\\rCopyright (C) 2025-2026 Andy Timmins\\r"\n\t.align\n' \
		'$(TITLE)' '$(MODULE_VERSION)' '$(MODULE_DATE)' >> $@
	@echo "version.inc: $(TITLE) $(MODULE_VERSION) ($(MODULE_DATE))"

%,ffa: %.elf
	$(OBJCOPY) -O binary $< $@

%.elf: %.o
	$(LD) --section-start .text=0 -o $@ $<

$(MODULE).o: $(MODULE).s version.inc
	$(AS) -o $@ $<

%.o: %.s
	$(AS) -o $@ $<

clean:
	rm -f *.o *.elf *,ffa version.inc
