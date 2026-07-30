#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "rpcemu.h"
#include "arm.h"
#include "mem.h"
extern void *codegen_test_compile(uint32_t opcode);
extern void codegen_test_interp(uint32_t opcode);
#define GBASE 0x20000u
#define RN 12
static uint8_t membuf[4096] __attribute__((aligned(4096)));
static uint8_t saved[4096];
static long checks=0; static int failures=0;
/* Both privilege levels; the maps are per-privilege (see cp15.c). */
static void tlb(void){ intptr_t d=(intptr_t)membuf-(intptr_t)GBASE;
    for (int m = 0; m < 2; m++) { mem_read_map(m)[GBASE>>12]=(uintptr_t)d; mem_write_map(m)[GBASE>>12]=(uintptr_t)d; } }
static void fillmem(void){ for(int i=0;i<4096;i++) membuf[i]=(uint8_t)(i*3+5); }
static void setregs(uint32_t base){ for(int r=0;r<15;r++) arm.reg[r]=0x1000+r*0x111; arm.reg[RN]=base; }
static uint32_t op(int P,int U,int W,int L,uint32_t list){ return (0xeu<<28)|(0x4u<<25)|((uint32_t)P<<24)|((uint32_t)U<<23)|((uint32_t)W<<21)|((uint32_t)L<<20)|((uint32_t)RN<<16)|(list&0xffff); }
static void chk(const char*nm,uint32_t opcode,uint32_t base){
    uint32_t ireg[16],jreg[16]; uint8_t imem[4096]; void(*fn)(void);
    fillmem(); memcpy(saved,membuf,4096); tlb(); setregs(base); arm.event=0;
    codegen_test_interp(opcode);
    for(int r=0;r<16;r++) ireg[r]=arm.reg[r]; memcpy(imem,membuf,4096);
    memcpy(membuf,saved,4096); tlb(); setregs(base); arm.event=0; linecyc=0;
    fn=(void(*)(void))codegen_test_compile(opcode); fn();
    for(int r=0;r<16;r++) jreg[r]=arm.reg[r];
    checks++;
    int bad = memcmp(imem,membuf,4096)!=0;
    for(int r=0;r<15;r++) if(ireg[r]!=jreg[r]) bad=1;
    if(bad){ if(failures<25){ fprintf(stderr,"MISMATCH %s op=%08x base=%08x mem=%s",nm,opcode,base,memcmp(imem,membuf,4096)?"DIFF":"ok");
        for(int r=0;r<15;r++) if(ireg[r]!=jreg[r]) fprintf(stderr," r%d(i=%08x j=%08x)",r,ireg[r],jreg[r]); fprintf(stderr,"\n"); } failures++; }
}
static void sweep(const char*m){
    printf("--- %s ---\n",m);
    uint32_t lists[]={0x0001,0x0003,0x00ff,0x5554,0x1000,0x7fff,0x0f0f,0x00f0};
    uint32_t bases[]={GBASE+0x800, GBASE+0x3f0, GBASE+0x408};
    for(unsigned b=0;b<sizeof(bases)/sizeof(*bases);b++)
      for(unsigned l=0;l<sizeof(lists)/sizeof(*lists);l++)
        for(int P=0;P<2;P++)for(int U=0;U<2;U++)for(int W=0;W<2;W++)for(int L=0;L<2;L++)
          chk(L?"LDM":"STM", op(P,U,W,L,lists[l]), bases[b]);
}
int main(void){
    arm_init(); arm_reset(CPUModel_SA110); initcodeblocks();
    sweep("26-bit mode"); updatemode(0x10|SUPERVISOR); sweep("32-bit mode");
    printf("\n%ld checks, %d failures\n",checks,failures);
    return failures?1:0;
}
