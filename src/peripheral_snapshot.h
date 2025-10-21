#ifndef PERIPHERAL_SNAPSHOT_H
#define PERIPHERAL_SNAPSHOT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct {
    uint32_t palette[256];
    int palindex;
    uint32_t border_colour;
    uint32_t cursor_palette[3];
    uint32_t hdsr;
    uint32_t hcsr;
    uint32_t hder;
    uint32_t vdsr;
    uint32_t vcsr;
    uint32_t vcer;
    uint32_t vder;
    uint32_t b0;
    uint32_t b1;
    uint32_t bit8;
    uint32_t screen_width;
    uint32_t screen_height;
    uint32_t cursor_x;
    uint32_t cursor_y;
    uint32_t cursor_height;
} VIDCStateSnapshot;

typedef struct {
    int super_type;
    uint8_t config_mode;
    uint8_t config_register;
    uint8_t scratch;
    uint8_t line_ctrl;
    uint8_t gp_index;
    uint8_t gp_regs[16];
    uint8_t print_status;
    uint8_t config_regs_665[16];
    uint8_t config_regs_672[256];
} SuperIOStateSnapshot;

typedef struct {
    uint8_t atastat;
    uint8_t error;
    uint8_t command;
    uint8_t fdisk;
    uint8_t asc;
    uint8_t packet_status;
    uint8_t disc_changed;
    uint8_t reset_in_progress;
    uint8_t drive;
    uint8_t drive_present[2];
    uint8_t drive_is_cdrom[2];
    uint8_t drive_skip512[2];
    uint8_t drive_lba[2];
    int secount;
    int sector;
    int cylinder;
    int head;
    int cylprecomp;
    int pos;
    int packlen;
    int cdpos;
    int cdlen;
    int spt[2];
    int hpc[2];
} IDEStateSnapshot;

typedef struct {
    uint8_t slot_used[8];
    uint8_t irq[8];
    uint8_t fiq[8];
} PodulesStateSnapshot;

#ifdef __cplusplus
} /* extern "C" */
#endif /* __cplusplus */

#endif /* PERIPHERAL_SNAPSHOT_H */
