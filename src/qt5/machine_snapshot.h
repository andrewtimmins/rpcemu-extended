#ifndef MACHINE_SNAPSHOT_H
#define MACHINE_SNAPSHOT_H

#include <stdint.h>

#include <QtCore/QMetaType>

#include "rpcemu.h"
#include "../peripheral_snapshot.h"

/**
 * Lightweight view of the emulator state captured on the emulator thread
 * and marshalled to the GUI. All fields are POD types to simplify copying
 * between threads.
 */
typedef struct MachineSnapshot {
    char model_name[64];
    char cpu_name[64];
    int dynarec;                /**< Non-zero when dynarec core is active */

    uint32_t regs[16];          /**< General-purpose registers R0-R15 */
    uint32_t cpsr;              /**< Current program status register */
    uint32_t mode;              /**< Current CPU mode identifier */
    uint32_t pc;                /**< Program counter (virtual address) */

    uint32_t pipeline_addr[8];  /**< Virtual addresses of upcoming instructions */
    uint32_t pipeline_data[8];  /**< Raw instruction words for pipeline addresses */

    int mmu_enabled;            /**< Non-zero if the MMU is active */
    int privileged_mode;        /**< Non-zero if CPU is in privileged mode */

    uint32_t iomd_irqa_status;
    uint32_t iomd_irqa_mask;
    uint32_t iomd_irqb_status;
    uint32_t iomd_irqb_mask;
    uint32_t iomd_fiq_status;
    uint32_t iomd_fiq_mask;
    uint32_t iomd_dma_status;
    uint32_t iomd_dma_mask;

    uint32_t iomd_timer0_counter;
    uint32_t iomd_timer0_in_latch;
    uint32_t iomd_timer0_out_latch;
    uint32_t iomd_timer1_counter;
    uint32_t iomd_timer1_in_latch;
    uint32_t iomd_timer1_out_latch;
    uint8_t iomd_sound_status;

    int floppy_motor_on;        /**< 1 if floppy motor engaged */

    float perf_mips;
    float perf_mhz;
    float perf_tlb_sec;
    float perf_flush_sec;

    uint32_t config_mem_size;   /**< RAM size in MB */
    uint32_t config_vram_size;  /**< VRAM size in MB */
    NetworkType network_type;
    int cpu_idle_enabled;       /**< 1 when CPU idle throttling is enabled */

    int debug_paused;
    int debug_pause_requested;
    DebugPauseReason debug_pause_reason;
    uint32_t debug_halt_pc;
    uint32_t debug_halt_opcode;
    uint32_t debug_last_pc;
    uint32_t debug_last_opcode;
    uint32_t debug_hit_address;
    uint32_t debug_hit_value;
    uint8_t debug_hit_size;
    uint8_t debug_hit_is_write;
    uint8_t debug_step_active;
    uint8_t debug_reserved0;
    uint32_t debug_breakpoint_count;
    uint32_t debug_breakpoints[DEBUGGER_MAX_BREAKPOINTS];
    uint32_t debug_watchpoint_count;
    DebugWatchpointInfo debug_watchpoints[DEBUGGER_MAX_WATCHPOINTS];

    VIDCStateSnapshot vidc;
    SuperIOStateSnapshot superio;
    IDEStateSnapshot ide;
    PodulesStateSnapshot podules;
    uint8_t vidc_double_x;
    uint8_t vidc_double_y;
    uint8_t reserved_peripherals[2];
} MachineSnapshot;

Q_DECLARE_METATYPE(MachineSnapshot)

#endif // MACHINE_SNAPSHOT_H
