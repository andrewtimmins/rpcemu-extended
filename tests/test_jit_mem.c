/* Differential test for the arm64 recompiler's LDR/STR/LDRB/STRB codegen.
   Points a TLB page directly at a host buffer so both the interpreter handler
   and the recompiled block hit it, then compares loaded Rd, base writeback and
   memory contents across addressing modes. Runs under qemu-aarch64. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "rpcemu.h"
#include "arm.h"
#include "mem.h"

extern void *codegen_test_compile(uint32_t opcode);
extern void codegen_test_interp(uint32_t opcode);

#define GBASE 0x20000u        /* guest page base for the test buffer */
#define RN_NUM 4
#define RD_NUM 0

static uint8_t membuf[4096] __attribute__((aligned(4096)));
static uint8_t saved[4096];
static long checks = 0; static int failures = 0;

static void tlb_setup(void)
{
    intptr_t disp = (intptr_t)membuf - (intptr_t)GBASE;  /* host = guest + disp */
    /* Both privilege levels: the maps are per-privilege (see cp15.c), and
       LDRT/STRT switch to the User set part way through a case. */
    for (int m = 0; m < 2; m++) {
        mem_read_map(m)[GBASE >> 12] = (uintptr_t)disp;  /* bit0 clear => readable */
        mem_write_map(m)[GBASE >> 12] = (uintptr_t)disp; /* bits0-1 clear => writable */
    }
}

static void fill_membuf(void)
{
    for (int i = 0; i < 4096; i++) membuf[i] = (uint8_t)(i * 7 + 1);
}

/* opcode builders: cond=AL, class 01. */
static uint32_t ldrstr_imm(int load, int byte, int pre, int up, int wb, int rn, int rd, uint32_t off12)
{
    return (0xeu<<28)|(1u<<26)|((uint32_t)pre<<24)|((uint32_t)up<<23)|((uint32_t)byte<<22)
         |((uint32_t)wb<<21)|((uint32_t)load<<20)|((uint32_t)rn<<16)|((uint32_t)rd<<12)|(off12&0xfff);
}
static uint32_t ldrstr_reg(int load, int byte, int pre, int up, int wb, int rn, int rd, int rm)
{
    return (0xeu<<28)|(1u<<26)|(1u<<25)|((uint32_t)pre<<24)|((uint32_t)up<<23)|((uint32_t)byte<<22)
         |((uint32_t)wb<<21)|((uint32_t)load<<20)|((uint32_t)rn<<16)|((uint32_t)rd<<12)|(uint32_t)rm;
}

static void run_case(const char *name, uint32_t opcode, uint32_t rn_val, uint32_t rd_val, uint32_t rm_val)
{
    const int rn = (opcode>>16)&0xf, rd = (opcode>>12)&0xf;
    uint32_t iref_rd, iref_rn, jref_rd, jref_rn;
    void (*fn)(void);

    /* --- interpreter reference --- */
    fill_membuf(); memcpy(saved, membuf, 4096); tlb_setup();
    arm.reg[RN_NUM] = rn_val; arm.reg[RD_NUM] = rd_val; arm.reg[2] = rm_val;
    arm.event = 0;
    codegen_test_interp(opcode);
    iref_rd = arm.reg[rd]; iref_rn = arm.reg[rn];
    uint8_t imem[4096]; memcpy(imem, membuf, 4096);

    /* --- recompiled --- */
    memcpy(membuf, saved, 4096); tlb_setup();
    arm.reg[RN_NUM] = rn_val; arm.reg[RD_NUM] = rd_val; arm.reg[2] = rm_val;
    arm.event = 0; linecyc = 0;
    fn = (void(*)(void))codegen_test_compile(opcode);
    fn();
    jref_rd = arm.reg[rd]; jref_rn = arm.reg[rn];

    checks++;
    int bad = (iref_rd != jref_rd) || (iref_rn != jref_rn) || memcmp(imem, membuf, 4096) != 0;
    if (bad && failures < 30) {
        fprintf(stderr, "MISMATCH %-6s op=%08x rn=%08x rd=%08x rm=%08x | "
            "Rd i=%08x j=%08x  Rn i=%08x j=%08x  mem %s\n",
            name, opcode, rn_val, rd_val, rm_val, iref_rd, jref_rd, iref_rn, jref_rn,
            memcmp(imem, membuf, 4096) ? "DIFF" : "ok");
    }
    if (bad) failures++;
}

static void sweep(const char *mode)
{
    printf("--- %s ---\n", mode);
    /* Keep base +/- offset within the single mapped page [GBASE, GBASE+4096). */
    uint32_t bases[] = { GBASE+0x800, GBASE+0x804, GBASE+0x808, GBASE+0x840,
                         GBASE+0x801, GBASE+0x802, GBASE+0x880 };
    uint32_t offs[]  = { 0, 4, 8, 0x10, 1, 2, 3, 0x40 };
    for (unsigned b=0;b<sizeof(bases)/sizeof(*bases);b++) {
      for (unsigned o=0;o<sizeof(offs)/sizeof(*offs);o++) {
        uint32_t base=bases[b], off=offs[o];
        for (int load=0; load<2; load++) for (int byte=0; byte<2; byte++)
          for (int pre=0; pre<2; pre++) for (int up=0; up<2; up++) for (int wb=0; wb<2; wb++) {
            /* imm offset */
            run_case(load?(byte?"LDRB":"LDR"):(byte?"STRB":"STR"),
                     ldrstr_imm(load,byte,pre,up,wb,RN_NUM,RD_NUM,off), base, 0x12345678u, 0);
            /* reg offset (Rm=r2), no shift */
            run_case(load?(byte?"LDRBr":"LDRr"):(byte?"STRBr":"STRr"),
                     ldrstr_reg(load,byte,pre,up,wb,RN_NUM,RD_NUM,2), base, 0x9abcdef0u, off);
          }
      }
    }
}

int main(void)
{
    arm_init(); arm_reset(CPUModel_SA110); initcodeblocks();
    sweep("26-bit mode");
    updatemode(0x10 | SUPERVISOR);
    sweep("32-bit mode");
    printf("\n%ld checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
