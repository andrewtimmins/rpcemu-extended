#
# ScrollWheel module.
#
# Assembled with clang rather than GNU as, because the source is written in the
# mnemonic order clang and the ARM ARM use (strbeq, not streqb) which GNU as
# will not take. Everything else here matches the other guest modules.
#
# There is a link step, and it is not optional. objcopy'ing the object file
# straight to a binary, which is what this used to do, never resolves a
# relocation: clang leaves every BL to be fixed up at link time, so a BL to a
# label in this same file assembles to &EBFFFFFE - branch to itself - and the
# module hangs the machine the moment initialisation reaches it. Nothing warned.
# It went unnoticed because the source had no BL in it until now and the built
# module has been committed, untouched, since the first commit.
#

CC = clang
AS = $(CC)
ASFLAGS = --target=arm-unknown-none-eabi -Wall
LD = arm-none-eabi-ld
OBJCOPY = arm-none-eabi-objcopy

MODULE = scrollwheel

all: $(MODULE),ffa

$(MODULE),ffa: $(MODULE).elf
	$(OBJCOPY) -O binary $< $@

$(MODULE).elf: $(MODULE).o
	$(LD) --section-start .text=0 -o $@ $<

$(MODULE).o: $(MODULE).s
	$(AS) $(ASFLAGS) -c -o $@ $<

clean:
	rm -f $(MODULE),ffa $(MODULE).o $(MODULE).elf
