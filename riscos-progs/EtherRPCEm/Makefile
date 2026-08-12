#
# EtherRPCEm - the guest network driver, assembled with GNU ARM binutils.
# (build.sh overrides AS/LD/OBJCOPY with the arm-linux-gnueabi- toolchain.)
#
# This replaces the Acorn DDE build that had to run inside a guest: the module
# is now assembler, like the other guest modules, so it builds on the host and
# in CI. The C sources it was transcribed from (c.Module, s.intveneer,
# cmhg.ModHdr, s.errors, the h. headers and the MkEther,feb recipe that built
# them) are in git history at 9e44930, not in this directory - see README.md.
#
# The result belongs in netroms/, not poduleroms/: the emulator builds the
# network card's ROM around it at run time (src/network.c), while poduleroms/ is
# scanned into the general-purpose expansion card.
#

AS = armas
LD = armld
OBJCOPY = armobjcopy

MODULE = etherrpcem

# The emulator loads this file by name when it builds the network card's ROM
# (src/network.c), so the case matters here in a way it does not for poduleroms/,
# which is scanned as a directory.
ROM_IMAGE = EtherRPCEm,ffa

# The name *Help and *Modules show, and who wrote what. The original authors'
# credit was in the cmhg help string and has to stay somewhere the user can see
# it, so it stays here - in the order the work happened, with the author of the
# assembler port last. This is the only guest module whose help string carries a
# copyright, and that is why: the others are ours outright.
TITLE = EtherRPCEm
COPYRIGHT = \xa9 John Ballance/Castle, Alex Waugh, Andy Timmins

# Version and date come from VersionNum. Either can be overridden on the command
# line, e.g. make MODULE_DATE="$(shell date +'%d %b %Y')" for a true build stamp.
MODULE_VERSION := $(shell awk '$$1 == "Module_MajorVersion" { print $$2 }' VersionNum)
MODULE_DATE := $(shell awk '/^Module_Date/ { $$1 = ""; sub(/^[ \t]+/, ""); print }' VersionNum)

all: $(ROM_IMAGE)

# The help string, generated so the version and date come from VersionNum alone.
version.inc: VersionNum Makefile
	@printf '@ Generated from VersionNum by the Makefile - do not edit.\n' > $@
	@printf 'help:\n\t.string\t"%s\\t%s (%s) %s"\n\t.align\n' \
		'$(TITLE)' '$(MODULE_VERSION)' '$(MODULE_DATE)' '$(COPYRIGHT)' >> $@
	@echo "version.inc: $(TITLE) $(MODULE_VERSION) ($(MODULE_DATE))"

$(ROM_IMAGE): $(MODULE).elf
	$(OBJCOPY) -O binary $< $@

%.elf: %.o
	$(LD) --section-start .text=0 -o $@ $<

# AutoSense/EtherRPCEm,ffb is .incbin'd, so a change to it has to rebuild the
# module just as a change to the source does.
$(MODULE).o: $(MODULE).s version.inc AutoSense/EtherRPCEm,ffb
	$(AS) -o $@ $<

clean:
	rm -f *.o *.elf "$(ROM_IMAGE)" version.inc
