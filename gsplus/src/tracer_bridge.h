/* tracer_bridge.h — C API feeding the native "Debugger Tracer" window.
 *
 * Provides structured snapshots of CPU state (registers + pseudo-registers),
 * forward-walking disassembly with per-instruction metadata (bytes, base
 * cycles, M/X flags), raw memory reads, and pause/run/step/reset controls.
 *
 * All writes go through the same globals the text debugger uses
 * (engine struct, g_halt_sim, g_stepping), so the tracer and the F8
 * console stay in sync.
 */

#ifndef GSPLUS_TRACER_BRIDGE_H
#define GSPLUS_TRACER_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Register snapshot ───────────────────────────────────────────────
 *
 * A one-shot copy of the CPU and I/O state that drives the Registers
 * panel. Returned by value, so Swift can marshal it cheaply. Hex values
 * are held in their natural width. 24-bit PC is bank (high byte) + addr. */
typedef struct {
	uint32_t pc;        /* 24-bit: (kbank << 16) | kaddr */
	uint16_t a;
	uint16_t x;
	uint16_t y;
	uint16_t sp;
	uint16_t dp;
	uint16_t psr;       /* full processor status word, 9 bits */
	uint8_t  dbank;
	uint8_t  pbank;
	uint8_t  m_state;   /* $C068 Machine State pseudo-register */
	uint8_t  q_state;   /* lower 7 bits of $C035 | high bit of $C036 */
	uint8_t  flag_m8;   /* 1 = 8-bit acc/mem, 0 = 16-bit */
	uint8_t  flag_x8;   /* 1 = 8-bit index, 0 = 16-bit */
	uint8_t  flag_e;    /* emulation-mode bit */
	uint8_t  halted;    /* g_halt_sim */
	uint8_t  stepping;  /* g_stepping */
} Tracer_regs;

void tracer_get_registers(Tracer_regs *out);

/* ── Disassembly ─────────────────────────────────────────────────────
 *
 * Walk forward from start_pc, decoding one instruction per row. M/X
 * arguments are the flag values used for decoding; pass the current
 * engine values (or a user-selected override) to keep the immediate-mode
 * byte counts correct for LDA/LDX etc. Returns the count of rows filled
 * (<= max). */
typedef struct {
	uint32_t pc;          /* 24-bit address of this line */
	uint8_t  nbytes;      /* 1..4 */
	uint8_t  bytes[4];
	uint8_t  cycles;      /* base cycle count (no page-cross adj) */
	uint8_t  m8;
	uint8_t  x8;
	uint8_t  _pad[1];
	char     mnemonic[48];
} Tracer_dasm;

int tracer_disasm(uint32_t start_pc, int m8, int x8,
                  Tracer_dasm *out, int max);

/* ── Memory-reference decode for the current instruction ────────────
 *
 * Used by the tracer's Memory View panel to auto-follow whatever
 * address the next instruction is about to read/write. Populates `out`
 * and returns 1 if the instruction has a memory target we know how to
 * decode; returns 0 for instructions that don't reference data memory
 * (implied, immediate, branches, jumps, control flow). */
typedef struct {
	uint8_t  valid;
	uint8_t  is_indexed;    /* 1 if an index register was added */
	uint8_t  width;         /* bytes accessed: 1 or 2 */
	uint8_t  _pad;
	uint32_t base_addr;     /* pre-index 24-bit address */
	uint32_t effective_addr;/* post-index 24-bit address */
} Tracer_target;

int tracer_decode_target(uint32_t pc, int m8, int x8, Tracer_target *out);

/* ── Memory ──────────────────────────────────────────────────────── */

/* Reads len bytes starting at addr (24-bit). Uses get_memory_c, which
 * applies bank/slot mapping, so this is the same view the emulator sees. */
void tracer_read_memory(uint32_t addr, uint8_t *buf, int len);

/* ── Control ─────────────────────────────────────────────────────── */

void tracer_pause(void);
void tracer_run(void);
void tracer_step_into(void);
void tracer_step_over(void);    /* treated as step_into for now */
void tracer_reset(void);

int  tracer_is_halted(void);
int  tracer_is_stepping(void);

/* ── Register writers (editable fields in the Registers panel) ─── */

void tracer_set_pc(uint32_t v);    /* 24-bit; high byte is K */
void tracer_set_a(uint16_t v);
void tracer_set_x(uint16_t v);
void tracer_set_y(uint16_t v);
void tracer_set_sp(uint16_t v);
void tracer_set_dp(uint16_t v);
void tracer_set_psr(uint16_t v);
void tracer_set_dbank(uint8_t v);
void tracer_set_pbank(uint8_t v);

#ifdef __cplusplus
}
#endif

#endif
