# CoProTest: a RISC OS program that exercises RPCEmuCoPro's SWIs.
#
# A relocatable module, not an application: HostCmd drives the command line and
# an application leaving through OS_Exit never returns control to it, so the test
# is a * command instead. Linked at zero, as a module is.
#
# Deliberately not in poduleroms: it is a test and has no business loading on
# every machine. Load it with *RMLoad.

AS = arm-linux-gnueabi-as
LD = arm-linux-gnueabi-ld
OBJCOPY = arm-linux-gnueabi-objcopy

TARGET = coprotest,ffa

all: $(TARGET)

coprotest.o: coprotest.s
	$(AS) -o $@ $<

coprotest.elf: coprotest.o
	$(LD) --section-start .text=0 -o $@ $<

$(TARGET): coprotest.elf
	$(OBJCOPY) -O binary $< $@
	rm -f $<

clean:
	rm -f coprotest.o coprotest.elf $(TARGET)
