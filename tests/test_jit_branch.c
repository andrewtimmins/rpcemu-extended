#include <stdint.h>
#include <stdio.h>
#include "rpcemu.h"
#include "arm.h"
extern void *codegen_test_compile(uint32_t opcode);
extern void codegen_test_interp(uint32_t opcode);
extern uint32_t *pcpsr;              /* &arm.reg[15] in 26-bit mode, &arm.reg[16] in 32-bit */
extern uint8_t flaglookup[16][16];   /* flaglookup[cond][flag nibble] */
static long checks=0; static int failures=0;
static uint32_t bop(int link,uint32_t off24){return (0xeu<<28)|(0x5u<<25)|((uint32_t)link<<24)|(off24&0xffffff);}
static void chk(int link,uint32_t off24,uint32_t r15){
    uint32_t op=bop(link,off24), ir15,ir14,jr15,jr14; void(*fn)(void);
    uint32_t base=arm.reg[cpsr]&0x0fffffff;
    arm.reg[15]=r15; arm.reg[14]=0xdeadbeef; arm.event=0;
    codegen_test_interp(op);
    arm.reg[15] += 4; /* pipeline advance the real interp loop applies per instruction */
    ir15=arm.reg[15]; ir14=arm.reg[14];
    arm.reg[15]=r15; arm.reg[14]=0xdeadbeef; arm.event=0; linecyc=0;
    fn=(void(*)(void))codegen_test_compile(op); fn(); jr15=arm.reg[15]; jr14=arm.reg[14];
    checks++;
    if((ir15!=jr15)||(link&&ir14!=jr14)){
        if(failures<30) fprintf(stderr,"MISMATCH %s off=%06x r15=%08x | R15 i=%08x j=%08x  R14 i=%08x j=%08x\n",
            link?"BL":"B ",off24,r15,ir15,jr15,ir14,jr14);
        failures++;
    }
    (void)base;
}
/* Conditional B/BL. The suite above only ever built condition 0xe (AL), so no
   conditional branch had ever been compared against the interpreter on any
   backend - which is how a broken conditional branch can coexist with a passing
   test suite and tens of millions of clean fuzz cases.

   codegen_test_interp() calls the opcode handler directly and does not evaluate
   the condition, so the reference has to model it: a condition that fails means
   the PC simply advances one instruction, which is also what the recompiled
   block's endblock() PC update produces. */
static void chk_cond(int link,uint32_t off24,uint32_t r15,int cond,unsigned flags){
    uint32_t op=(((uint32_t)cond)<<28)|(0x5u<<25)|((uint32_t)link<<24)|(off24&0xffffff);
    uint32_t ir15,ir14,jr15,jr14; void(*fn)(void);
    const int mode26 = (pcpsr == &arm.reg[15]);
    const int taken = flaglookup[cond][flags];
    uint32_t r15v = mode26 ? ((r15 & 0x03fffffcu) | (flags<<28)) : r15;

    /* Reference */
    if (mode26) { arm.reg[15]=r15v; }
    else { arm.reg[15]=r15v; arm.reg[16]=(arm.reg[16]&0x0fffffffu)|(flags<<28); }
    arm.reg[14]=0xdeadbeef; arm.event=0;
    if (taken) { codegen_test_interp(op); arm.reg[15]+=4; }
    else { arm.reg[15]=r15v+4; }
    ir15=arm.reg[15]; ir14=arm.reg[14];

    /* Recompiled */
    if (mode26) { arm.reg[15]=r15v; }
    else { arm.reg[15]=r15v; arm.reg[16]=(arm.reg[16]&0x0fffffffu)|(flags<<28); }
    arm.reg[14]=0xdeadbeef; arm.event=0; linecyc=0;
    fn=(void(*)(void))codegen_test_compile(op); fn(); jr15=arm.reg[15]; jr14=arm.reg[14];

    checks++;
    if((ir15!=jr15)||(taken&&link&&ir14!=jr14)){
        if(failures<30) fprintf(stderr,
            "MISMATCH %s cond=%x flags=%x %s off=%06x r15=%08x | R15 i=%08x j=%08x  R14 i=%08x j=%08x\n",
            link?"BL":"B ",cond,flags,taken?"taken":"not-taken",off24,r15v,ir15,jr15,ir14,jr14);
        failures++;
    }
}

static void sweep_cond(const char*m){
    printf("--- %s, every condition code against every flag state ---\n",m);
    uint32_t offs[]={0x000010,0x7fffff,0xffffff};
    uint32_t r15s[]={0x00010000,0x00040000};
    for(int cond=0;cond<15;cond++)          /* 15 = NV, never executed */
      for(unsigned flags=0;flags<16;flags++)
        for(int link=0;link<2;link++)
          for(unsigned o=0;o<sizeof(offs)/sizeof(*offs);o++)
            for(unsigned r=0;r<sizeof(r15s)/sizeof(*r15s);r++)
              chk_cond(link,offs[o],r15s[r],cond,flags);
}

static void sweep(const char*m){
    printf("--- %s ---\n",m);
    uint32_t offs[]={0,1,2,0x100,0x7fffff,0x800000,0xffffff,0x400000,0x000010};
    uint32_t r15s[]={0x00008000,0x00010000,0x00040000,0x01000000,0x0000000c};
    for(int link=0;link<2;link++)
      for(unsigned o=0;o<sizeof(offs)/sizeof(*offs);o++)
        for(unsigned r=0;r<sizeof(r15s)/sizeof(*r15s);r++)
          chk(link,offs[o],r15s[r]);
}
int main(void){
    arm_init(); arm_reset(CPUModel_SA110); initcodeblocks();
    sweep("26-bit mode");
    sweep_cond("26-bit mode");
    updatemode(0x10|SUPERVISOR);
    sweep("32-bit mode");
    sweep_cond("32-bit mode");
    printf("\n%ld checks, %d failures\n",checks,failures);
    return failures?1:0;
}
