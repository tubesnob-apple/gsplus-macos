/* tracer_bridge.c — see tracer_bridge.h for the contract. */

#include "defc.h"
#include "tracer_bridge.h"
#include "symbols.h"

#include <string.h>

/* Addressing-mode tables live in disas.h but that header defines the
 * arrays without `static`, so it can't be included in more than one
 * translation unit without producing duplicate symbols at link. We
 * pull the tables in via extern and mirror only the mode constants we
 * actually use. */
extern const char * const disas_opcodes[256];
extern const word32       disas_types[256];

/* Mirrored from the enum in disas.h — must match 1:1. */
enum {
	DM_ABS = 1, DM_ABSX, DM_ABSY, DM_ABSLONG, DM_ABSIND, DM_ABSXIND,
	DM_IMPLY, DM_ACCUM, DM_IMMED, DM_JUST8,
	DM_DLOC, DM_DLOCX, DM_DLOCY,
	DM_LONG, DM_LONGX,
	DM_DLOCIND, DM_DLOCINDY, DM_DLOCXIND,
	DM_DLOCBRAK, DM_DLOCBRAKY,
	DM_DISP8, DM_DISP8S, DM_DISP8SINDY, DM_DISP16,
	DM_MVPMVN, DM_REPVAL, DM_SEPVAL
};

extern Engine_reg engine;
extern int        g_halt_sim;
extern int        g_stepping;
extern dword64    g_dcycles_end;
extern word32     g_c068_statereg;
extern word32     g_c035_shadow_reg;
extern word32     g_c036_val_speed;

/* ── Side-effect-free memory peek ───────────────────────────────────
 *
 * `get_memory_c` routes through the emulated memory system, which fires
 * soft switches in the $C000-$CFFF I/O page on banks 00/01/E0/E1
 * (speaker click on $C030, keyboard strobe clear on $C010, etc). A
 * debugger that reads arbitrary memory ranges would constantly trigger
 * those, so this peek returns a placeholder for I/O addresses and uses
 * the normal accessor everywhere else (where reads are side-effect
 * free). */
static uint8_t
tracer_peek8(uint32_t addr)
{
	uint32_t bank = (addr >> 16) & 0xff;
	uint32_t off  = addr & 0xffff;
	if((bank == 0x00 || bank == 0x01 || bank == 0xe0 || bank == 0xe1)
	   && off >= 0xc000 && off < 0xd000) {
		return 0xff;  /* hidden — don't touch soft switches */
	}
	return (uint8_t)(get_memory_c(addr & 0xffffff) & 0xff);
}

/* ── Base cycle counts for 65816 opcodes ─────────────────────────────
 *
 * Values are the minimum (no-adjustment) cycle count. Real cycles have
 * +1 for page crosses, +1 when M=8 for some operand widths, +1 for
 * decimal mode, etc. The tracer's cycles column is a guide, not exact. */
static const uint8_t s_cycles_base[256] = {
	/* 00-0F */ 7,6,7,4,5,3,5,6,3,2,2,4,6,4,6,5,
	/* 10-1F */ 2,5,5,7,5,4,6,6,2,4,2,2,6,4,7,5,
	/* 20-2F */ 6,6,8,4,3,3,5,6,4,2,2,5,4,4,6,5,
	/* 30-3F */ 2,5,5,7,4,4,6,6,2,4,2,2,4,4,7,5,
	/* 40-4F */ 6,6,2,4,3,3,5,6,3,2,2,3,3,4,6,5,
	/* 50-5F */ 2,5,5,7,2,4,6,6,2,4,3,2,4,4,7,5,
	/* 60-6F */ 6,6,6,4,3,3,5,6,4,2,2,6,5,4,6,5,
	/* 70-7F */ 2,5,5,7,4,4,6,6,2,4,4,2,6,4,7,5,
	/* 80-8F */ 3,6,3,4,3,3,3,6,2,2,2,3,4,4,4,5,
	/* 90-9F */ 2,6,5,7,4,4,4,6,2,5,2,2,4,5,5,5,
	/* A0-AF */ 2,6,2,4,3,3,3,6,2,2,2,4,4,4,4,5,
	/* B0-BF */ 2,5,5,7,4,4,4,6,2,4,2,2,4,4,4,5,
	/* C0-CF */ 2,6,3,4,3,3,5,6,2,2,2,3,4,4,6,5,
	/* D0-DF */ 2,5,5,7,6,4,6,6,2,4,3,3,6,4,7,5,
	/* E0-EF */ 2,6,3,4,3,3,5,6,2,2,2,3,4,4,6,5,
	/* F0-FF */ 2,5,5,7,5,4,6,6,2,4,4,2,8,4,7,5,
};

/* ── Snapshot ────────────────────────────────────────────────────── */

void
tracer_get_registers(Tracer_regs *out)
{
	if(!out) return;
	memset(out, 0, sizeof(*out));

	out->pc       = engine.kpc & 0xffffff;
	out->a        = (uint16_t)(engine.acc & 0xffff);
	out->x        = (uint16_t)(engine.xreg & 0xffff);
	out->y        = (uint16_t)(engine.yreg & 0xffff);
	out->sp       = (uint16_t)(engine.stack & 0xffff);
	out->dp       = (uint16_t)(engine.direct & 0xffff);
	out->psr      = (uint16_t)(engine.psr & 0x1ff);
	out->dbank    = (uint8_t)(engine.dbank & 0xff);
	out->pbank    = (uint8_t)((engine.kpc >> 16) & 0xff);
	out->m_state  = (uint8_t)(g_c068_statereg & 0xff);
	out->q_state  = (uint8_t)((g_c035_shadow_reg & 0x7f) |
	                          (g_c036_val_speed & 0x80));
	out->shadow   = (uint8_t)(g_c035_shadow_reg & 0xff);
	out->flag_m8  = (engine.psr & 0x20) ? 1 : 0;
	out->flag_x8  = (engine.psr & 0x10) ? 1 : 0;
	out->flag_e   = (engine.psr & 0x100) ? 1 : 0;
	out->halted   = (uint8_t)(g_halt_sim ? 1 : 0);
	out->stepping = (uint8_t)(g_stepping ? 1 : 0);
}

/* ── Disassembly ─────────────────────────────────────────────────── */

int
tracer_disasm(uint32_t start_pc, int m8, int x8,
              Tracer_dasm *out, int max)
{
	int i;
	uint32_t pc = start_pc & 0xffffff;
	int acc_imm_size = m8 ? 1 : 2;
	int x_imm_size   = x8 ? 1 : 2;

	if(!out || max <= 0) return 0;

	for(i = 0; i < max; i++) {
		uint8_t b0 = tracer_peek8(pc);
		uint8_t b1 = tracer_peek8(pc + 1);
		uint8_t b2 = tracer_peek8(pc + 2);
		uint8_t b3 = tracer_peek8(pc + 3);
		/* do_dis() with op_provided=1 reads the operand out of instr
		 * (low 3 bytes, little-endian) instead of calling
		 * get_memory_c — which is what lets us avoid firing soft
		 * switches if the PC walks across the I/O page. */
		word32 instr = ((word32)b0 << 24)
		             | ((word32)b1)
		             | ((word32)b2 << 8)
		             | ((word32)b3 << 16);
		int size = 1;
		char *str = do_dis(pc, acc_imm_size, x_imm_size, 1, instr,
		                   &size);

		if(size < 1) size = 1;
		if(size > 4) size = 4;

		out[i].pc       = pc;
		out[i].nbytes   = (uint8_t)size;
		out[i].bytes[0] = b0;
		out[i].bytes[1] = (size > 1) ? b1 : 0;
		out[i].bytes[2] = (size > 2) ? b2 : 0;
		out[i].bytes[3] = (size > 3) ? b3 : 0;
		out[i].cycles   = s_cycles_base[b0];
		out[i].m8       = (uint8_t)(m8 ? 1 : 0);
		out[i].x8       = (uint8_t)(x8 ? 1 : 0);

		if(str) {
			/* do_dis() returns "BB/AAAA: xx xx xx  MNEM  operand".
			 * Strip the "BB/AAAA: " address prefix (shown in the
			 * Address / Bytes column), then strip the hex byte dump
			 * (also in that column) by skipping past the lowercase
			 * hex run up to the first uppercase mnemonic char. */
			const char *p = str;
			const char *colon = strchr(p, ':');
			if(colon && colon[1] == ' ') p = colon + 2;
			while(*p && !(*p >= 'A' && *p <= 'Z')) p++;
			strncpy(out[i].mnemonic, p, sizeof(out[i].mnemonic) - 1);
			out[i].mnemonic[sizeof(out[i].mnemonic) - 1] = 0;
		} else {
			out[i].mnemonic[0] = 0;
		}

		pc = (pc + size) & 0xffffff;
	}
	return max;
}

/* ── Memory-reference decode ───────────────────────────────────────── */

int
tracer_decode_target(uint32_t pc, int m8, int x8, Tracer_target *out)
{
	uint8_t  opcode;
	word32   dtype;
	int      mode;
	uint32_t dbr, dp, xr, yr;
	const char *mnem;
	int      use_x;
	uint32_t base, eff;
	int      indexed;

	if(!out) return 0;
	memset(out, 0, sizeof(*out));

	pc   = pc & 0xffffff;
	opcode = tracer_peek8(pc);
	dtype  = disas_types[opcode];
	mode   = (int)(dtype & 0xff);
	mnem   = disas_opcodes[opcode];

	dbr = (word32)(engine.dbank & 0xff);
	dp  = (word32)(engine.direct & 0xffff);
	xr  = (word32)(engine.xreg  & (x8 ? 0xff : 0xffff));
	yr  = (word32)(engine.yreg  & (x8 ? 0xff : 0xffff));

	base    = 0;
	eff     = 0;
	indexed = 0;

	switch(mode) {
	case DM_ABS: {
		word32 op = (word32)tracer_peek8(pc + 1)
		         | ((word32)tracer_peek8(pc + 2) << 8);
		base = (dbr << 16) | op;
		eff  = base;
		break;
	}
	case DM_ABSX: {
		word32 op = (word32)tracer_peek8(pc + 1)
		         | ((word32)tracer_peek8(pc + 2) << 8);
		base = (dbr << 16) | op;
		eff  = (base + xr) & 0xffffff;
		indexed = 1;
		break;
	}
	case DM_ABSY: {
		word32 op = (word32)tracer_peek8(pc + 1)
		         | ((word32)tracer_peek8(pc + 2) << 8);
		base = (dbr << 16) | op;
		eff  = (base + yr) & 0xffffff;
		indexed = 1;
		break;
	}
	case DM_ABSLONG:
	case DM_LONG: {
		word32 op = (word32)tracer_peek8(pc + 1)
		         | ((word32)tracer_peek8(pc + 2) << 8)
		         | ((word32)tracer_peek8(pc + 3) << 16);
		base = op & 0xffffff;
		eff  = base;
		break;
	}
	case DM_LONGX: {
		word32 op = (word32)tracer_peek8(pc + 1)
		         | ((word32)tracer_peek8(pc + 2) << 8)
		         | ((word32)tracer_peek8(pc + 3) << 16);
		base = op & 0xffffff;
		eff  = (base + xr) & 0xffffff;
		indexed = 1;
		break;
	}
	case DM_DLOC: {
		word32 op = tracer_peek8(pc + 1);
		base = (dp + op) & 0xffff;
		eff  = base;
		break;
	}
	case DM_DLOCX: {
		word32 op = tracer_peek8(pc + 1);
		base = (dp + op) & 0xffff;
		eff  = (base + xr) & 0xffff;
		indexed = 1;
		break;
	}
	case DM_DLOCY: {
		word32 op = tracer_peek8(pc + 1);
		base = (dp + op) & 0xffff;
		eff  = (base + yr) & 0xffff;
		indexed = 1;
		break;
	}
	/* (zp) — direct-page indirect. Pointer at DP+op is read as a
	 * little-endian word and combined with DBR for the data address. */
	case DM_DLOCIND: {
		word32 op  = tracer_peek8(pc + 1);
		word32 ptr = (dp + op) & 0xffff;
		word32 lo  = tracer_peek8(ptr);
		word32 hi  = tracer_peek8((ptr + 1) & 0xffff);
		base = ptr;                         /* pointer in zero page   */
		eff  = (dbr << 16) | (lo | (hi << 8)); /* dereferenced addr   */
		indexed = 1;
		break;
	}
	/* (zp),Y — direct-page indirect indexed Y (ubiquitous in ORCA). */
	case DM_DLOCINDY: {
		word32 op  = tracer_peek8(pc + 1);
		word32 ptr = (dp + op) & 0xffff;
		word32 lo  = tracer_peek8(ptr);
		word32 hi  = tracer_peek8((ptr + 1) & 0xffff);
		base = ptr;
		eff  = ((dbr << 16) | (lo | (hi << 8))) + yr;
		eff  &= 0xffffff;
		indexed = 1;
		break;
	}
	/* (zp,X) — direct-page indexed indirect. Add X before dereferencing. */
	case DM_DLOCXIND: {
		word32 op  = tracer_peek8(pc + 1);
		word32 ptr = (dp + op + xr) & 0xffff;
		word32 lo  = tracer_peek8(ptr);
		word32 hi  = tracer_peek8((ptr + 1) & 0xffff);
		base = ptr;
		eff  = (dbr << 16) | (lo | (hi << 8));
		indexed = 1;
		break;
	}
	/* [zp] — 24-bit indirect. */
	case DM_DLOCBRAK: {
		word32 op  = tracer_peek8(pc + 1);
		word32 ptr = (dp + op) & 0xffff;
		word32 b0  = tracer_peek8(ptr);
		word32 b1  = tracer_peek8((ptr + 1) & 0xffff);
		word32 b2  = tracer_peek8((ptr + 2) & 0xffff);
		base = ptr;
		eff  = (b0 | (b1 << 8) | (b2 << 16)) & 0xffffff;
		indexed = 1;
		break;
	}
	/* [zp],Y — 24-bit indirect indexed Y. */
	case DM_DLOCBRAKY: {
		word32 op  = tracer_peek8(pc + 1);
		word32 ptr = (dp + op) & 0xffff;
		word32 b0  = tracer_peek8(ptr);
		word32 b1  = tracer_peek8((ptr + 1) & 0xffff);
		word32 b2  = tracer_peek8((ptr + 2) & 0xffff);
		base = ptr;
		eff  = ((b0 | (b1 << 8) | (b2 << 16)) + yr) & 0xffffff;
		indexed = 1;
		break;
	}
	/* nn,S — stack-relative, bank 0. */
	case DM_DISP8S: {
		word32 op = tracer_peek8(pc + 1);
		base = ((word32)(engine.stack & 0xffff) + op) & 0xffff;
		eff  = base;
		break;
	}
	/* (nn,S),Y — stack-relative indirect indexed Y. */
	case DM_DISP8SINDY: {
		word32 op  = tracer_peek8(pc + 1);
		word32 ptr = ((word32)(engine.stack & 0xffff) + op) & 0xffff;
		word32 lo  = tracer_peek8(ptr);
		word32 hi  = tracer_peek8((ptr + 1) & 0xffff);
		base = ptr;
		eff  = ((dbr << 16) | (lo | (hi << 8))) + yr;
		eff  &= 0xffffff;
		indexed = 1;
		break;
	}
	default:
		return 0;   /* immediate, implied, branch, jump, etc. */
	}

	/* Width selection: LDX/LDY/STX/STY/CPX/CPY follow X; everything else
	 * uses M. BIT, TRB, TSB, ROL, LSR etc. follow M.  */
	use_x = 0;
	if(mnem && mnem[0] && mnem[1] && mnem[2]) {
		char c0 = mnem[0], c2 = mnem[2];
		if((c0 == 'L' || c0 == 'S' || c0 == 'C') &&
		   (c2 == 'X' || c2 == 'Y')) {
			use_x = 1;
		}
	}

	out->valid          = 1;
	out->is_indexed     = (uint8_t)indexed;
	out->width          = (uint8_t)(use_x ? (x8 ? 1 : 2)
	                                      : (m8 ? 1 : 2));
	out->base_addr      = base;
	out->effective_addr = eff;
	return 1;
}

/* ── Target symbol for the TSym column ──────────────────────────────
 *
 * Compute the address the instruction at `pc` refers to (data reference
 * for loads/stores, destination for JSR/JMP/branches) and look it up in
 * the symbol index. Returns a string pointer into symbols_describe_pc()'s
 * static buffer on hit, or NULL if the instruction doesn't have a
 * resolvable target (immediate, implied, accumulator, stack, interrupt).
 *
 * Why not reuse tracer_decode_target(): that function is tailored for
 * the Memory View panel (data-memory refs only, uses DBR). For the TSym
 * column we want a broader "where does this instruction point" — which
 * must use PBR for JSR/JMP and must compute branch destinations. */
const char *
tracer_target_symbol(uint32_t pc, int m8, int x8)
{
	uint8_t  opcode;
	word32   dtype;
	int      mode;
	uint32_t pbr, dbr, dp, xr, target;
	int8_t   disp8;
	int16_t  disp16;

	(void)m8;
	pc     = pc & 0xffffff;
	opcode = tracer_peek8(pc);
	dtype  = disas_types[opcode];
	mode   = (int)(dtype & 0xff);

	pbr = (pc >> 16) & 0xff;
	dbr = (word32)(engine.dbank & 0xff);
	dp  = (word32)(engine.direct & 0xffff);
	xr  = (word32)(engine.xreg & (x8 ? 0xff : 0xffff));
	target = 0;

	/* Control flow — these stay in the program bank (PBR) or carry
	 * their own bank in the operand, unlike data refs which use DBR. */
	switch(opcode) {
	case 0x20: /* JSR abs — PBR:operand */
	case 0x4c: /* JMP abs — PBR:operand */
		target = (pbr << 16)
		       | (tracer_peek8(pc + 1))
		       | (tracer_peek8(pc + 2) << 8);
		goto lookup;
	case 0x22: /* JSL long */
	case 0x5c: /* JML long */
		target = (tracer_peek8(pc + 1))
		       | (tracer_peek8(pc + 2) << 8)
		       | (tracer_peek8(pc + 3) << 16);
		goto lookup;
	case 0x6c: /* JMP (abs) — pointer in bank 0, dest in PBR */ {
		word32 ptr = (tracer_peek8(pc + 1))
		          | (tracer_peek8(pc + 2) << 8);
		target = (pbr << 16)
		       | (tracer_peek8(ptr) | (tracer_peek8(ptr + 1) << 8));
		goto lookup;
	}
	case 0x7c: /* JMP (abs,X) — pointer in PBR, dest in PBR */
	case 0xfc: /* JSR (abs,X) — pointer in PBR, dest in PBR */ {
		word32 ptr = ((pbr << 16) | (tracer_peek8(pc + 1)
		          | (tracer_peek8(pc + 2) << 8))) + xr;
		ptr &= 0xffffff;
		target = (pbr << 16)
		       | (tracer_peek8(ptr) | (tracer_peek8(ptr + 1) << 8));
		goto lookup;
	}
	case 0xdc: /* JML [abs] — 24-bit indirect from bank 0 */ {
		word32 ptr = (tracer_peek8(pc + 1))
		          | (tracer_peek8(pc + 2) << 8);
		target = (tracer_peek8(ptr))
		       | (tracer_peek8(ptr + 1) << 8)
		       | (tracer_peek8(ptr + 2) << 16);
		goto lookup;
	}
	default:
		break;
	}

	/* 8-bit relative branch: BPL BMI BVC BVS BRA BCC BCS BNE BEQ.
	 * Target is PC + 2 + signed displacement, staying in PBR. */
	if(mode == DM_DISP8) {
		disp8  = (int8_t)tracer_peek8(pc + 1);
		target = (pbr << 16) | ((pc + 2 + disp8) & 0xffff);
		goto lookup;
	}
	/* 16-bit relative: BRL ($82) — stays in PBR. PER ($62) also encodes
	 * DISP16 but pushes an address rather than branching; still a useful
	 * symbol reference. */
	if(mode == DM_DISP16) {
		disp16 = (int16_t)((word32)tracer_peek8(pc + 1)
		                 | ((word32)tracer_peek8(pc + 2) << 8));
		target = (pbr << 16) | ((pc + 3 + disp16) & 0xffff);
		goto lookup;
	}

	/* Remaining data-reference modes. We recompute here (rather than
	 * routing through tracer_decode_target) because we want the
	 * pre-index / base address — that's usually what maps to a known
	 * symbol (e.g. `LDA tbl,X` should resolve to `tbl`, not `tbl+X`). */
	switch(mode) {
	case DM_ABS:
	case DM_ABSX:
	case DM_ABSY:
		target = (dbr << 16)
		       | (tracer_peek8(pc + 1))
		       | (tracer_peek8(pc + 2) << 8);
		break;
	case DM_ABSLONG:
	case DM_LONG:
	case DM_LONGX:
		target = (tracer_peek8(pc + 1))
		       | (tracer_peek8(pc + 2) << 8)
		       | (tracer_peek8(pc + 3) << 16);
		break;
	case DM_DLOC:
	case DM_DLOCX:
	case DM_DLOCY:
	case DM_DLOCIND:
	case DM_DLOCINDY:
	case DM_DLOCXIND:
	case DM_DLOCBRAK:
	case DM_DLOCBRAKY:
		target = (dp + tracer_peek8(pc + 1)) & 0xffff;
		break;
	default:
		return NULL;
	}

lookup:
	return symbols_describe_pc(target & 0xffffff);
}

/* ── Memory ──────────────────────────────────────────────────────── */

void
tracer_read_memory(uint32_t addr, uint8_t *buf, int len)
{
	int i;
	if(!buf || len <= 0) return;
	for(i = 0; i < len; i++) {
		buf[i] = tracer_peek8((addr + i) & 0xffffff);
	}
}

/* ── Control ─────────────────────────────────────────────────────── */

void
tracer_pause(void)
{
	g_halt_sim    = 1;
	g_dcycles_end = 0;
}

void
tracer_run(void)
{
	g_halt_sim = 0;
	g_stepping = 0;
}

void
tracer_step_into(void)
{
	g_stepping = 1;
	g_halt_sim = 0;
}

void
tracer_step_over(void)
{
	/* Real step-over requires detecting JSR/JSL and setting a one-shot
	 * breakpoint at PC+3/+4. For now behave the same as step-into. */
	tracer_step_into();
}

void
tracer_reset(void)
{
	do_reset();
}

int tracer_is_halted(void)   { return g_halt_sim ? 1 : 0; }
int tracer_is_stepping(void) { return g_stepping ? 1 : 0; }

/* ── Register writers ────────────────────────────────────────────── */

void
tracer_set_pc(uint32_t v)
{
	engine.kpc = v & 0xffffff;
}

void tracer_set_a(uint16_t v)    { engine.acc    = v; }
void tracer_set_x(uint16_t v)    { engine.xreg   = v; }
void tracer_set_y(uint16_t v)    { engine.yreg   = v; }
void tracer_set_sp(uint16_t v)   { engine.stack  = v; }
void tracer_set_dp(uint16_t v)   { engine.direct = v; }
void tracer_set_psr(uint16_t v)  { engine.psr    = v & 0x1ff; }
void tracer_set_dbank(uint8_t v) { engine.dbank  = v; }

void
tracer_set_pbank(uint8_t v)
{
	engine.kpc = (engine.kpc & 0xffff) | ((uint32_t)v << 16);
}
