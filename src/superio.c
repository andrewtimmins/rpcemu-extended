/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2005-2010 Sarah Walker
  Copyright (C) 2025 Andrew Timmins
  
  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
 * SMC FDC37C665GT Super I/O Chip Emulation
 *
 * The chips contain:
 *   - Floppy Disk Controller (82077 compatible) - handled by fdc.c
 *   - IDE Controller interface (665GT only) - handled by ide.c
 *   - Two 16550A-compatible UARTs - handled by serial.c
 *   - IEEE 1284 Parallel Port (SPP/PS2/EPP/ECP modes) - handled by parallel.c
 *   - Configuration registers
 *   - 8042 Keyboard Controller (672 only) - handled by i8042.c
 *   - General Purpose I/O (672 only)
 *
 * Memory-mapped I/O address conversion:
 *   IO port = (memory_address >> 2) & 0x7FF
 */

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "rpcemu.h"
#include "fdc.h"
#include "vidc20.h"
#include "iomd.h"
#include "ide.h"
#include "arm.h"
#include "i8042.h"
#include "parallel.h"
#include "serial.h"

/* ========================================================================
 * Constants and Definitions
 * ======================================================================== */

/* Configuration Mode States */
#define SUPERIO_MODE_NORMAL        0
#define SUPERIO_MODE_INTERMEDIATE  1  /* 665GT: waiting for second 0x55 */
#define SUPERIO_MODE_CONFIGURATION 2

/* SMI interrupt bits */
#define SMI_IRQ2_IRINT  0x04

/* Parallel Port Modes (ECR bits 7:5) */
#define PP_MODE_SPP      0  /* Standard/Compatibility mode */
#define PP_MODE_PS2      1  /* Byte mode (bidirectional) */
#define PP_MODE_FIFO     2  /* Parallel Port FIFO mode */
#define PP_MODE_ECP      3  /* ECP FIFO mode */
#define PP_MODE_EPP      4  /* EPP mode */
#define PP_MODE_RESERVED 5
#define PP_MODE_FIFO_TEST 6  /* FIFO test mode */
#define PP_MODE_CONFIG   7  /* Configuration mode */

/* Parallel Port Status Register bits */
#define PP_STATUS_TIMEOUT  0x01  /* EPP timeout (directly from control bit) */
#define PP_STATUS_RESERVED 0x02  /* Reserved */
#define PP_STATUS_nIRQ     0x04  /* nIRQ (active low, directly from ACK) */
#define PP_STATUS_nERROR   0x08  /* nError (1 = no error) */
#define PP_STATUS_SELECT   0x10  /* Select (1 = printer online) */
#define PP_STATUS_PAPEROUT 0x20  /* Paper Out (0 = paper present) */
#define PP_STATUS_nACK     0x40  /* nAck (1 = idle, 0 = acknowledging) */
#define PP_STATUS_nBUSY    0x80  /* nBusy (1 = ready, 0 = busy) */

/* Parallel Port Control Register bits */
#define PP_CTRL_STROBE     0x01  /* Strobe (directly controls nStrobe) */
#define PP_CTRL_AUTOLF     0x02  /* Auto Linefeed */
#define PP_CTRL_nINIT      0x04  /* nInit (1 = normal, 0 = reset printer) */
#define PP_CTRL_SELECTIN   0x08  /* Select In */
#define PP_CTRL_IRQEN      0x10  /* IRQ enable on ACK */
#define PP_CTRL_BIDI       0x20  /* Bidirectional mode (PS/2 only) */

/* ECR (Extended Control Register) bits */
#define ECR_FIFO_EMPTY     0x01  /* FIFO empty */
#define ECR_FIFO_FULL      0x02  /* FIFO full */
#define ECR_ECP_SERVICE    0x04  /* ECP service interrupt */
#define ECR_DMA_EN         0x08  /* DMA enable */
#define ECR_nERR_INTRP     0x10  /* nError interrupt enable */
#define ECR_MODE_MASK      0xE0  /* Mode bits 7:5 */
#define ECR_MODE_SHIFT     5

/* UART (16550A) Register offsets */
#define UART_RBR  0  /* Receive Buffer Register (read, DLAB=0) */
#define UART_THR  0  /* Transmit Holding Register (write, DLAB=0) */
#define UART_DLL  0  /* Divisor Latch Low (DLAB=1) */
#define UART_IER  1  /* Interrupt Enable Register (DLAB=0) */
#define UART_DLM  1  /* Divisor Latch High (DLAB=1) */
#define UART_IIR  2  /* Interrupt Identification Register (read) */
#define UART_FCR  2  /* FIFO Control Register (write) */
#define UART_LCR  3  /* Line Control Register */
#define UART_MCR  4  /* Modem Control Register */
#define UART_LSR  5  /* Line Status Register */
#define UART_MSR  6  /* Modem Status Register */
#define UART_SCR  7  /* Scratch Register */

/* UART Line Control Register bits */
#define LCR_DLAB   0x80  /* Divisor Latch Access Bit */

/* UART Line Status Register bits */
#define LSR_DR     0x01  /* Data Ready */
#define LSR_OE     0x02  /* Overrun Error */
#define LSR_PE     0x04  /* Parity Error */
#define LSR_FE     0x08  /* Framing Error */
#define LSR_BI     0x10  /* Break Interrupt */
#define LSR_THRE   0x20  /* THR Empty */
#define LSR_TEMT   0x40  /* Transmitter Empty */
#define LSR_FIFO_ERR 0x80  /* Error in FIFO */

/* UART Interrupt Identification Register values */
#define IIR_NO_INT     0x01  /* No interrupt pending */
#define IIR_RLS        0x06  /* Receiver Line Status */
#define IIR_RDA        0x04  /* Received Data Available */
#define IIR_CTI        0x0C  /* Character Timeout */
#define IIR_THRE       0x02  /* THR Empty */
#define IIR_MODEM      0x00  /* Modem Status */
#define IIR_FIFO_EN    0xC0  /* FIFO enabled bits */

/* UART Modem Status Register bits */
#define MSR_DCTS   0x01  /* Delta CTS */
#define MSR_DDSR   0x02  /* Delta DSR */
#define MSR_TERI   0x04  /* Trailing Edge RI */
#define MSR_DDCD   0x08  /* Delta DCD */
#define MSR_CTS    0x10  /* CTS */
#define MSR_DSR    0x20  /* DSR */
#define MSR_RI     0x40  /* RI */
#define MSR_DCD    0x80  /* DCD */

/* ========================================================================
 * Parallel Port State
 * ======================================================================== */

typedef struct {
    uint8_t data;       /* Data register (directly visible) */
    uint8_t status;     /* Status register (directly visible) */
    uint8_t control;    /* Control register (directly visible) */
    
    /* ECP/EPP Extended registers */
    uint8_t ecr;        /* Extended Control Register */
    uint8_t config_a;   /* Configuration Register A (read-only) */
    uint8_t config_b;   /* Configuration Register B */
    
    /* FIFO for ECP mode */
    uint8_t fifo[16];
    int fifo_head;
    int fifo_tail;
    int fifo_count;
    
    /* State tracking */
    int last_strobe;    /* Previous strobe state for edge detection */
    int ack_pending;    /* ACK pulse pending */
    uint16_t base_addr; /* Base I/O address (0x378 or 0x278) */
} ParallelPort;

/* ========================================================================
 * UART State (16550A compatible)
 * ======================================================================== */

typedef struct {
    /* Data registers */
    uint8_t rbr;        /* Receive Buffer Register */
    uint8_t thr;        /* Transmit Holding Register */
    uint8_t dll;        /* Divisor Latch Low */
    uint8_t dlm;        /* Divisor Latch High */
    
    /* Control/Status registers */
    uint8_t ier;        /* Interrupt Enable Register */
    uint8_t iir;        /* Interrupt Identification Register */
    uint8_t fcr;        /* FIFO Control Register */
    uint8_t lcr;        /* Line Control Register */
    uint8_t mcr;        /* Modem Control Register */
    uint8_t lsr;        /* Line Status Register */
    uint8_t msr;        /* Modem Status Register */
    uint8_t scr;        /* Scratch Register */
    
    /* FIFO buffers */
    uint8_t rx_fifo[16];
    uint8_t tx_fifo[16];
    int rx_head, rx_tail, rx_count;
    int tx_head, tx_tail, tx_count;
    
    /* State */
    int fifo_enabled;
    uint16_t base_addr; /* Base I/O address (0x3F8 or 0x2F8) */
} UART;

/* ========================================================================
 * SuperIO Chip State
 * ======================================================================== */

static SuperIOType super_type;

/* Configuration mode */
static int configmode = SUPERIO_MODE_NORMAL;
static uint8_t configregs665[16];
static uint8_t configregs672[256];
static uint8_t configreg;

/* Parallel ports - LPT1 (0x378) and LPT2 (0x278) */
static ParallelPort lpt1;
static ParallelPort lpt2;

/* Serial ports - COM1 (0x3F8) and COM2 (0x2F8) */
static UART com1;
static UART com2;

/* FDC37C672 GP Index Registers */
static int gp_index;
static uint8_t gp_regs[16];

/* ========================================================================
 * Forward declarations
 * ======================================================================== */

static void parallel_reset(ParallelPort *pp, uint16_t base);
static void parallel_write(ParallelPort *pp, uint32_t offset, uint8_t val, ParallelPortID bus_port);
static uint8_t parallel_read(ParallelPort *pp, uint32_t offset, ParallelPortID bus_port);
static void uart_reset(UART *uart, uint16_t base);
static void uart_write(UART *uart, uint32_t offset, uint8_t val, SerialPortID bus_port);
static uint8_t uart_read(UART *uart, uint32_t offset, SerialPortID bus_port);

/* ========================================================================
 * SMI (System Management Interrupt) Handling
 * ======================================================================== */

static void
superio_smi_update(void)
{
    if ((gp_regs[0xe] & gp_regs[0xc]) || (gp_regs[0xf] & gp_regs[0xd])) {
        iomd.irqb.status |= IOMD2_IRQB_SUPERIO_SMI;
    } else {
        iomd.irqb.status &= ~IOMD2_IRQB_SUPERIO_SMI;
    }
    updateirqs();
}

void
superio_smi_setint1(uint8_t i)
{
    gp_regs[0x0e] |= i;
    superio_smi_update();
}

void
superio_smi_setint2(uint8_t i)
{
    gp_regs[0x0f] |= i;
    superio_smi_update();
}

void
superio_smi_clrint1(uint8_t i)
{
    gp_regs[0x0e] &= ~i;
    superio_smi_update();
}

void
superio_smi_clrint2(uint8_t i)
{
    gp_regs[0x0f] &= ~i;
    superio_smi_update();
}

/* ========================================================================
 * Parallel Port Implementation
 * ======================================================================== */

static void
parallel_reset(ParallelPort *pp, uint16_t base)
{
    memset(pp, 0, sizeof(*pp));
    pp->base_addr = base;
    
    /* Initial status: not busy, ACK idle, paper present, online, no error */
    pp->status = PP_STATUS_nBUSY | PP_STATUS_nACK | PP_STATUS_SELECT | PP_STATUS_nERROR;
    
    /* Control: nInit high (not resetting), others low */
    pp->control = PP_CTRL_nINIT;
    
    /* ECR: SPP mode, FIFO empty */
    pp->ecr = (PP_MODE_SPP << ECR_MODE_SHIFT) | ECR_FIFO_EMPTY;
    
    /* Config A: identifies ECP capability, typical value */
    pp->config_a = 0x00;  /* 0x00 = No ECP/EPP capability detected. Forces SPP mode. */
    
    /* Config B: default */
    pp->config_b = 0x00;
}

static void
parallel_fifo_push(ParallelPort *pp, uint8_t data)
{
    if (pp->fifo_count < 16) {
        pp->fifo[pp->fifo_tail] = data;
        pp->fifo_tail = (pp->fifo_tail + 1) & 15;
        pp->fifo_count++;
        
        /* Update ECR flags */
        pp->ecr &= ~ECR_FIFO_EMPTY;
        if (pp->fifo_count >= 16) {
            pp->ecr |= ECR_FIFO_FULL;
        }
    }
}

static uint8_t
parallel_fifo_pop(ParallelPort *pp)
{
    uint8_t data = 0;
    if (pp->fifo_count > 0) {
        data = pp->fifo[pp->fifo_head];
        pp->fifo_head = (pp->fifo_head + 1) & 15;
        pp->fifo_count--;
        
        /* Update ECR flags */
        pp->ecr &= ~ECR_FIFO_FULL;
        if (pp->fifo_count == 0) {
            pp->ecr |= ECR_FIFO_EMPTY;
        }
    }
    return data;
}

static void
parallel_do_handshake(ParallelPort *pp, ParallelPortID bus_port)
{
    /* Send data to the parallel bus */
    parallel_bus_write_data(bus_port, pp->data);
    parallel_bus_strobe(bus_port, 1);
    parallel_bus_strobe(bus_port, 0);
    
    /* Get status from connected device (which may have generated ACK) */
    pp->status = parallel_bus_get_status(bus_port);
}

static void
parallel_write(ParallelPort *pp, uint32_t offset, uint8_t val, ParallelPortID bus_port)
{
    int mode = (pp->ecr >> ECR_MODE_SHIFT) & 7;
    
    switch (offset) {
    case 0:  /* Data register / ECP Address FIFO / EPP Data */
        if (mode == PP_MODE_FIFO || mode == PP_MODE_ECP) {
            /* In FIFO modes, write goes to FIFO */
            parallel_fifo_push(pp, val);
            /* Auto-send in FIFO mode */
            while (pp->fifo_count > 0) {
                uint8_t byte = parallel_fifo_pop(pp);
                pp->data = byte;
                parallel_do_handshake(pp, bus_port);
            }
            rpclog("SuperIO PP: FIFO write 0x%02X '%c'\n", val, 
                   (val >= 32 && val < 127) ? val : '.');
        } else {
            /* Standard mode - just latch the data */
            pp->data = val;
            parallel_bus_write_data(bus_port, val);
            rpclog("SuperIO PP: Data write 0x%02X '%c'\n", val, 
                   (val >= 32 && val < 127) ? val : '.');
        }
        break;
        
    case 1:  /* Status register - mostly read-only in SPP mode */
        /* In EPP mode, this might be the EPP address register */
        if (mode == PP_MODE_EPP) {
            /* EPP address strobe - not commonly used */
            rpclog("SuperIO PP: EPP address write 0x%02X\n", val);
        }
        /* Status bits 0-2 can be written in some modes */
        break;
        
    case 2:  /* Control register */
        rpclog("SuperIO PP: Control write 0x%02X (strobe=%d autolf=%d init=%d sel=%d irq=%d bidi=%d)\n",
               val, val & 1, (val >> 1) & 1, (val >> 2) & 1, 
               (val >> 3) & 1, (val >> 4) & 1, (val >> 5) & 1);
        
        /* Detect strobe edge: data is sent on falling edge of strobe */
        if ((pp->control & PP_CTRL_STROBE) && !(val & PP_CTRL_STROBE)) {
            /* Strobe falling edge - send the latched data */
            parallel_do_handshake(pp, bus_port);
        }
        
        /* Update parallel bus control signals */
        parallel_bus_set_ctrl(bus_port, val);
        
        pp->control = val & 0x3F;  /* Only bits 0-5 are writable */
        break;
        
    case 3:  /* EPP Address (in EPP mode) or undefined */
        if (mode == PP_MODE_EPP) {
            rpclog("SuperIO PP: EPP address port write 0x%02X\n", val);
        }
        break;
        
    case 4:  /* EPP Data 0-3 (in EPP mode) */
    case 5:
    case 6:
    case 7:
        if (mode == PP_MODE_EPP) {
            pp->data = val;
            parallel_do_handshake(pp, bus_port);
            rpclog("SuperIO PP: EPP data write 0x%02X\n", val);
        }
        break;
    }
}

static uint8_t
parallel_read(ParallelPort *pp, uint32_t offset, ParallelPortID bus_port)
{
    int mode = (pp->ecr >> ECR_MODE_SHIFT) & 7;
    uint8_t val = 0;
    
    switch (offset) {
    case 0:  /* Data register */
        if ((pp->control & PP_CTRL_BIDI) || mode == PP_MODE_PS2 || 
            mode == PP_MODE_ECP || mode == PP_MODE_FIFO) {
            /* Bidirectional - return input from peripheral */
            val = parallel_bus_read_data(bus_port);
        } else {
            /* Standard mode - return last written value */
            val = pp->data;
        }
        break;
        
    case 1:  /* Status register */
        val = parallel_bus_get_status(bus_port);
        pp->status = val;
        rpclog("SuperIO PP: Status read 0x%02X (busy=%d ack=%d pe=%d sel=%d err=%d)\n",
               val, !(val & PP_STATUS_nBUSY), !(val & PP_STATUS_nACK),
               (val & PP_STATUS_PAPEROUT) >> 5, (val & PP_STATUS_SELECT) >> 4,
               !(val & PP_STATUS_nERROR));
        break;
        
    case 2:  /* Control register */
        val = pp->control;
        break;
        
    case 3:  /* EPP Address (in EPP mode) */
    case 4:  /* EPP Data 0-3 */
    case 5:
    case 6:
    case 7:
        /* EPP read operations - return dummy data */
        val = 0xFF;
        break;
    }
    
    return val;
}

/* ECP extended register access (+0x400 from base) */
static void
parallel_write_ecp(ParallelPort *pp, uint32_t offset, uint8_t val, ParallelPortID bus_port)
{
    switch (offset) {
    case 0:  /* ECP Data FIFO (in ECP/FIFO mode) or Test FIFO */
        rpclog("SuperIO PP: ECP FIFO write 0x%02X\n", val);
        parallel_fifo_push(pp, val);
        /* Auto-send from FIFO */
        while (pp->fifo_count > 0) {
            uint8_t byte = parallel_fifo_pop(pp);
            pp->data = byte;
            parallel_do_handshake(pp, bus_port);
        }
        break;
        
    case 1:  /* Config Register A - read only */
        break;
        
    case 2:  /* ECR - Extended Control Register */
        rpclog("SuperIO PP: ECR write 0x%02X (mode=%d dma=%d)\n", 
               val, (val >> 5) & 7, (val >> 3) & 1);
        /* Preserve FIFO status bits (0-1), allow writing others */
        /* Force Mode (bits 7:5) to 0 (SPP) to disable FIFO/ECP mode.
           This forces RISC OS to use the standard SPP driver which works using interrupts. */
        pp->ecr = (val & 0x1C) | (pp->ecr & 0x03);
        break;
        
    case 3:  /* Config Register B */
        pp->config_b = val;
        break;
        
    default:
        break;
    }
}

static uint8_t
parallel_read_ecp(ParallelPort *pp, uint32_t offset)
{
    uint8_t val = 0;
    
    switch (offset) {
    case 0:  /* ECP Data FIFO - return from FIFO */
        val = parallel_fifo_pop(pp);
        break;
        
    case 1:  /* Config Register A */
        val = pp->config_a;
        break;
        
    case 2:  /* ECR */
        val = pp->ecr;
        rpclog("SuperIO PP: ECR read 0x%02X (mode=%d empty=%d full=%d)\n",
               val, (val >> 5) & 7, val & 1, (val >> 1) & 1);
        break;
        
    case 3:  /* Config Register B */
        val = pp->config_b;
        break;
        
    default:
        break;
    }
    
    return val;
}

/* ========================================================================
 * UART (16550A) Implementation
 * ======================================================================== */

static void
uart_reset(UART *uart, uint16_t base)
{
    memset(uart, 0, sizeof(*uart));
    uart->base_addr = base;
    
    /* Initial state: transmitter empty and ready */
    uart->lsr = LSR_THRE | LSR_TEMT;
    
    /* No interrupt pending */
    uart->iir = IIR_NO_INT;
    
    /* Modem status: all signals inactive (inverted logic on some bits) */
    uart->msr = 0;
    
    /* Default divisor for 9600 baud at 1.8432 MHz crystal */
    uart->dll = 12;
    uart->dlm = 0;
}

static void
uart_update_iir(UART *uart)
{
    /* Check interrupt sources in priority order */
    if ((uart->ier & 0x04) && (uart->lsr & (LSR_OE | LSR_PE | LSR_FE | LSR_BI))) {
        /* Receiver Line Status */
        uart->iir = IIR_RLS;
    } else if ((uart->ier & 0x01) && (uart->lsr & LSR_DR)) {
        /* Received Data Available */
        uart->iir = IIR_RDA;
    } else if ((uart->ier & 0x02) && (uart->lsr & LSR_THRE)) {
        /* Transmitter Holding Register Empty */
        uart->iir = IIR_THRE;
    } else if ((uart->ier & 0x08) && (uart->msr & 0x0F)) {
        /* Modem Status */
        uart->iir = IIR_MODEM;
    } else {
        uart->iir = IIR_NO_INT;
    }
    
    /* Add FIFO enabled bits if FIFO is on */
    if (uart->fifo_enabled) {
        uart->iir |= IIR_FIFO_EN;
    }
}

static void
uart_write(UART *uart, uint32_t offset, uint8_t val, SerialPortID bus_port)
{
    switch (offset) {
    case UART_THR:  /* THR or DLL */
        if (uart->lcr & LCR_DLAB) {
            uart->dll = val;
        } else {
            /* Transmit data to attached device via serial bus */
            serial_bus_write_data(bus_port, val);
            uart->lsr &= ~LSR_THRE;
            uart->lsr &= ~LSR_TEMT;
            /* Immediately "transmit" */
            uart->lsr |= LSR_THRE | LSR_TEMT;
            uart_update_iir(uart);
        }
        break;
        
    case UART_IER:  /* IER or DLM */
        if (uart->lcr & LCR_DLAB) {
            uart->dlm = val;
        } else {
            uart->ier = val & 0x0F;
            uart_update_iir(uart);
        }
        break;
        
    case UART_FCR:  /* FCR (write-only) */
        uart->fcr = val;
        uart->fifo_enabled = (val & 0x01) != 0;
        if (val & 0x02) {
            /* Clear receive FIFO */
            uart->rx_head = uart->rx_tail = uart->rx_count = 0;
        }
        if (val & 0x04) {
            /* Clear transmit FIFO */
            uart->tx_head = uart->tx_tail = uart->tx_count = 0;
        }
        uart_update_iir(uart);
        break;
        
    case UART_LCR:
        uart->lcr = val;
        break;
        
    case UART_MCR:
        uart->mcr = val & 0x1F;
        /* Notify serial bus of control signal changes (DTR, RTS, etc.) */
        serial_bus_set_ctrl(bus_port, val & 0x0F);
        /* Loopback mode affects MSR */
        if (val & 0x10) {
            /* Loopback: MCR outputs connect to MSR inputs */
            uart->msr = ((val & 0x01) << 4) |  /* DTR -> DSR */
                        ((val & 0x02) << 3) |  /* RTS -> CTS */
                        ((val & 0x04) << 4) |  /* OUT1 -> RI */
                        ((val & 0x08) << 4);   /* OUT2 -> DCD */
        }
        break;
        
    case UART_SCR:
        uart->scr = val;
        break;
        
    default:
        break;
    }
}

static uint8_t
uart_read(UART *uart, uint32_t offset, SerialPortID bus_port)
{
    uint8_t val = 0;
    
    switch (offset) {
    case UART_RBR:  /* RBR or DLL */
        if (uart->lcr & LCR_DLAB) {
            val = uart->dll;
        } else {
            /* Read receive buffer */
            val = uart->rbr;
            uart->lsr &= ~LSR_DR;
            uart_update_iir(uart);
        }
        break;
        
    case UART_IER:  /* IER or DLM */
        if (uart->lcr & LCR_DLAB) {
            val = uart->dlm;
        } else {
            val = uart->ier;
        }
        break;
        
    case UART_IIR:  /* IIR (read-only) */
        val = uart->iir;
        /* Reading IIR clears THRE interrupt */
        if ((uart->iir & 0x0E) == IIR_THRE) {
            uart->iir = IIR_NO_INT;
            if (uart->fifo_enabled) {
                uart->iir |= IIR_FIFO_EN;
            }
        }
        break;
        
    case UART_LCR:
        val = uart->lcr;
        break;
        
    case UART_MCR:
        val = uart->mcr;
        break;
        
    case UART_LSR:
        val = uart->lsr;
        /* Reading LSR clears error bits */
        uart->lsr &= ~(LSR_OE | LSR_PE | LSR_FE | LSR_BI);
        uart_update_iir(uart);
        break;
        
    case UART_MSR:
        if (uart->mcr & 0x10) {
            /* Loopback mode - use internal looped-back signals */
            val = uart->msr;
        } else {
            /* Get status from attached device via serial bus */
            val = serial_bus_get_status(bus_port);
        }
        /* Reading MSR clears delta bits */
        uart->msr &= 0xF0;
        uart_update_iir(uart);
        break;
        
    case UART_SCR:
        val = uart->scr;
        break;
    }
    
    return val;
}

/* ========================================================================
 * Serial Port External Interface (called by serial bus)
 * ======================================================================== */

/**
 * Inject a received byte into a UART's receive buffer.
 * Called by serial bus when device sends data to host.
 */
void
superio_serial_rx(SerialPortID port, uint8_t data)
{
    UART *uart;
    
    switch (port) {
    case SERIAL_PORT_COM1:
        uart = &com1;
        break;
    case SERIAL_PORT_COM2:
        uart = &com2;
        break;
    default:
        return;
    }
    
    /* Put data in receive buffer register */
    uart->rbr = data;
    
    /* Set Data Ready bit in LSR */
    uart->lsr |= LSR_DR;
    
    /* Update interrupt identification */
    uart_update_iir(uart);
    
    /* Trigger FIQ for COM1 if RX interrupt is enabled */
    if (port == SERIAL_PORT_COM1 && (uart->ier & 0x01)) {
        iomd.fiq.status |= IOMD_FIQ_SERIAL;
        updateirqs();
    }
}

/**
 * Update a UART's modem status register.
 * Called by serial bus when device status lines change.
 */
void
superio_serial_update_msr(SerialPortID port, uint8_t status)
{
    UART *uart;
    uint8_t old_status;
    uint8_t delta;
    
    switch (port) {
    case SERIAL_PORT_COM1:
        uart = &com1;
        break;
    case SERIAL_PORT_COM2:
        uart = &com2;
        break;
    default:
        return;
    }
    
    /* Calculate delta bits (changes since last update) */
    old_status = uart->msr & 0xF0;
    delta = (old_status ^ status) >> 4;
    
    /* Update MSR: delta bits in low nibble, status in high nibble */
    uart->msr = (status & 0xF0) | (uart->msr & 0x0F) | delta;
    
    /* Update interrupt identification */
    uart_update_iir(uart);
}

/* ========================================================================
 * Configuration Registers
 * ======================================================================== */

static void
superio_config_reg_write(uint8_t reg, uint8_t val)
{
    if (super_type == SuperIOType_FDC37C665GT) {
        switch (reg) {
        case 0:
            /* CR0: IDE enable, FDC enable, etc. */
            configregs665[reg] = val;
            break;
            
        case 1:
            /* CR1: Parallel port configuration
               Bits 1:0 = Address (00=3BC, 01=378, 10=278, 11=disable)
               Bits 3:2 = Mode (00=compatible, 01=PS/2, 10=EPP, 11=ECP) */
            configregs665[reg] = val;
            /* Note: We don't dynamically relocate ports yet */
            if ((val & 0x3) != 0x3) {
                rpclog("SuperIO: Parallel port address config = %d\n", val & 0x3);
            }
            break;
            
        case 2:
            /* CR2: Serial port configuration */
            configregs665[reg] = val;
            break;
            
        case 3:
            /* CR3: Power and misc */
            configregs665[reg] = val;
            break;
            
        case 4:
            /* CR4: Reserved */
            configregs665[reg] = val;
            break;
            
        case 5:
            /* CR5: FDC/IDE configuration */
            configregs665[reg] = val;
            break;
            
        case 6:
            /* CR6: Floppy drive type for drives 0-3 */
            configregs665[reg] = val;
            break;
            
        default:
            configregs665[reg & 0xF] = val;
            break;
        }
    } else if (super_type == SuperIOType_FDC37C672) {
        /* 672 has additional registers for GP, power management */
        switch (reg) {
        case 0xb4:
            gp_regs[0x0c] = val;
            break;
        case 0xb5:
            gp_regs[0x0d] = val;
            break;
        default:
            configregs672[reg] = val;
            break;
        }
    }
}

/* ========================================================================
 * Snapshot for Inspector
 * ======================================================================== */

void
superio_get_snapshot(SuperIOStateSnapshot *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->super_type = (int) super_type;
    snapshot->config_mode = (uint8_t) configmode;
    snapshot->config_register = configreg;
    snapshot->scratch = com1.scr;
    snapshot->line_ctrl = com1.lcr;
    snapshot->gp_index = (uint8_t) gp_index;
    memcpy(snapshot->gp_regs, gp_regs, sizeof(gp_regs));
    snapshot->print_status = lpt2.status;
    memcpy(snapshot->config_regs_665, configregs665, sizeof(configregs665));
    memcpy(snapshot->config_regs_672, configregs672, sizeof(configregs672));
}

/* ========================================================================
 * Reset
 * ======================================================================== */

void
superio_reset(SuperIOType chosen_super_type)
{
    assert(chosen_super_type == SuperIOType_FDC37C665GT || 
           chosen_super_type == SuperIOType_FDC37C672);

    super_type = chosen_super_type;
    configmode = SUPERIO_MODE_NORMAL;
    configreg = 0;

    /* Reset parallel ports */
    parallel_reset(&lpt1, 0x378);
    parallel_reset(&lpt2, 0x278);

    /* Reset serial ports */
    uart_reset(&com1, 0x3F8);
    uart_reset(&com2, 0x2F8);

    /* Reset GP registers */
    gp_index = 0;
    memset(gp_regs, 0, sizeof(gp_regs));

    /* Initial configuration register default values from datasheet */
    configregs665[0x0] = 0x3b;
    configregs665[0x1] = 0x9e;  /* Parallel at 278, EPP mode */
    configregs665[0x2] = 0xdc;
    configregs665[0x3] = 0x78;
    configregs665[0x4] = 0x00;
    configregs665[0x5] = 0x00;
    configregs665[0x6] = 0xff;  /* Floppy drive types */
    configregs665[0x7] = 0x00;
    configregs665[0x8] = 0x00;
    configregs665[0x9] = 0x00;
    configregs665[0xa] = 0x00;
    configregs665[0xb] = 0x00;
    configregs665[0xc] = 0x00;
    configregs665[0xd] = 0x65;  /* Chip ID */
    configregs665[0xe] = 0x02;  /* Chip revision */
    configregs665[0xf] = 0x00;

    configregs672[0x03] = 0x03;
    configregs672[0x20] = 0x40;
    configregs672[0x24] = 0x04;
    configregs672[0x26] = 0xf0;
    configregs672[0x27] = 0x03;

    /* Reset FDC */
    fdc_reset();
}

/* ========================================================================
 * Main I/O Write Handler
 * ======================================================================== */

void
superio_write(uint32_t addr, uint32_t val)
{
    /* Convert memory-mapped address to IO port */
    uint32_t port = (addr >> 2) & 0x7FF;
    uint8_t byte = (uint8_t) val;

    /* -------------------- Configuration Mode Entry -------------------- */
    if (configmode != SUPERIO_MODE_CONFIGURATION) {
        if ((port == 0x3f0) && (byte == 0x55)) {
            if (super_type == SuperIOType_FDC37C665GT) {
                if (configmode == SUPERIO_MODE_NORMAL) {
                    configmode = SUPERIO_MODE_INTERMEDIATE;
                } else if (configmode == SUPERIO_MODE_INTERMEDIATE) {
                    configmode = SUPERIO_MODE_CONFIGURATION;
                }
            } else if (super_type == SuperIOType_FDC37C672) {
                configmode = SUPERIO_MODE_CONFIGURATION;
            }
            return;
        } else {
            configmode = SUPERIO_MODE_NORMAL;
        }
    }

    /* -------------------- Configuration Mode Access -------------------- */
    if (configmode == SUPERIO_MODE_CONFIGURATION) {
        if (port == 0x3f0) {
            if (byte == 0xaa) {
                configmode = SUPERIO_MODE_NORMAL;
            } else {
                configreg = (super_type == SuperIOType_FDC37C665GT) ? 
                            (byte & 0xF) : byte;
            }
        } else if (port == 0x3f1) {
            superio_config_reg_write(configreg, byte);
        }
        return;
    }

    /* -------------------- 8042 Keyboard Controller (672 only) -------------------- */
    if (super_type == SuperIOType_FDC37C672) {
        if (port == 0x60) {
            i8042_data_write(byte);
            return;
        } else if (port == 0x64) {
            i8042_command_write(byte);
            return;
        } else if (port == 0xea) {
            gp_index = byte & 0xF;
            return;
        } else if (port == 0xeb) {
            gp_regs[gp_index] = byte;
            superio_smi_update();
            return;
        }
    }

    /* -------------------- IDE (665GT only) -------------------- */
    if ((port >= 0x1f0 && port <= 0x1f7) || port == 0x3f6) {
        if (super_type == SuperIOType_FDC37C665GT) {
            writeide(port, byte);
        }
        return;
    }

    /* -------------------- Parallel Port LPT2 (0x278-0x27F) -------------------- */
    if (port >= 0x278 && port <= 0x27f) {
        parallel_write(&lpt2, port - 0x278, byte, PARALLEL_PORT_LPT2);
        return;
    }

    /* -------------------- Parallel Port LPT1 (0x378-0x37F) -------------------- */
    if (port >= 0x378 && port <= 0x37f) {
        parallel_write(&lpt1, port - 0x378, byte, PARALLEL_PORT_LPT1);
        return;
    }

    /* -------------------- LPT2 ECP Registers (0x678-0x67F) -------------------- */
    if (port >= 0x678 && port <= 0x67f) {
        parallel_write_ecp(&lpt2, port - 0x678, byte, PARALLEL_PORT_LPT2);
        return;
    }

    /* -------------------- LPT1 ECP Registers (0x778-0x77F) -------------------- */
    if (port >= 0x778 && port <= 0x77f) {
        parallel_write_ecp(&lpt1, port - 0x778, byte, PARALLEL_PORT_LPT1);
        return;
    }

    /* -------------------- FDC (0x3F0-0x3F7) -------------------- */
    if (port >= 0x3f0 && port <= 0x3f7) {
        fdc_write(port, byte);
        return;
    }

    /* -------------------- Serial Port COM1 (0x3F8-0x3FF) -------------------- */
    if (port >= 0x3f8 && port <= 0x3ff) {
        uart_write(&com1, port - 0x3f8, byte, SERIAL_PORT_COM1);
        /* Trigger FIQ on transmit ready (for RISC OS compatibility) */
        if (port == 0x3f9 && (byte & 2)) {
            iomd.fiq.status |= IOMD_FIQ_SERIAL;
            updateirqs();
        }
        return;
    }

    /* -------------------- Serial Port COM2 (0x2F8-0x2FF) -------------------- */
    if (port >= 0x2f8 && port <= 0x2ff) {
        uart_write(&com2, port - 0x2f8, byte, SERIAL_PORT_COM2);
        return;
    }

    /* -------------------- Unhandled -------------------- */
    rpclog("SuperIO: Unhandled write port=0x%03X val=0x%02X\n", port, byte);
}

/* ========================================================================
 * Main I/O Read Handler
 * ======================================================================== */

uint8_t
superio_read(uint32_t addr)
{
    /* Convert memory-mapped address to IO port */
    uint32_t port = (addr >> 2) & 0x7FF;

    /* -------------------- Configuration Mode Access -------------------- */
    if (configmode == SUPERIO_MODE_CONFIGURATION && port == 0x3f1) {
        if (super_type == SuperIOType_FDC37C672) {
            switch (configreg) {
            case 0xb4: return gp_regs[0x0c];
            case 0xb5: return gp_regs[0x0d];
            case 0xb6: return gp_regs[0x0e];
            case 0xb7: return gp_regs[0x0f];
            }
        }
        if (super_type == SuperIOType_FDC37C665GT) {
            return configregs665[configreg];
        } else {
            return configregs672[configreg];
        }
    }

    /* -------------------- 8042 Keyboard Controller (672 only) -------------------- */
    if (super_type == SuperIOType_FDC37C672) {
        if (port == 0x60) {
            return i8042_data_read();
        } else if (port == 0x64) {
            return i8042_status_read();
        } else if (port == 0xea) {
            return gp_index;
        } else if (port == 0xeb) {
            return gp_regs[gp_index];
        }
    }

    /* -------------------- IDE (665GT only) -------------------- */
    if ((port >= 0x1f0 && port <= 0x1f7) || port == 0x3f6) {
        if (super_type == SuperIOType_FDC37C665GT) {
            return readide(port);
        }
        return 0xFF;
    }

    /* -------------------- Parallel Port LPT2 (0x278-0x27F) -------------------- */
    if (port >= 0x278 && port <= 0x27f) {
        return parallel_read(&lpt2, port - 0x278, PARALLEL_PORT_LPT2);
    }

    /* -------------------- Parallel Port LPT1 (0x378-0x37F) -------------------- */
    if (port >= 0x378 && port <= 0x37f) {
        return parallel_read(&lpt1, port - 0x378, PARALLEL_PORT_LPT1);
    }

    /* -------------------- LPT2 ECP Registers (0x678-0x67F) -------------------- */
    if (port >= 0x678 && port <= 0x67f) {
        return parallel_read_ecp(&lpt2, port - 0x678);
    }

    /* -------------------- LPT1 ECP Registers (0x778-0x77F) -------------------- */
    if (port >= 0x778 && port <= 0x77f) {
        return parallel_read_ecp(&lpt1, port - 0x778);
    }

    /* -------------------- FDC (0x3F0-0x3F7) -------------------- */
    if (port >= 0x3f0 && port <= 0x3f7) {
        return fdc_read(port);
    }

    /* -------------------- Serial Port COM1 (0x3F8-0x3FF) -------------------- */
    if (port >= 0x3f8 && port <= 0x3ff) {
        if (port == 0x3fa) {
            /* Reading IIR clears serial FIQ */
            iomd.fiq.status &= ~IOMD_FIQ_SERIAL;
            updateirqs();
        }
        return uart_read(&com1, port - 0x3f8, SERIAL_PORT_COM1);
    }

    /* -------------------- Serial Port COM2 (0x2F8-0x2FF) -------------------- */
    if (port >= 0x2f8 && port <= 0x2ff) {
        return uart_read(&com2, port - 0x2f8, SERIAL_PORT_COM2);
    }

    /* -------------------- Unhandled -------------------- */
    rpclog("SuperIO: Unhandled read port=0x%03X\n", port);
    return 0xFF;
}
