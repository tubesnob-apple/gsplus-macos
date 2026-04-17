/*
 * debug_server.c — GSplus Apple IIgs emulator debug endpoint
 *
 * Runs a Unix domain socket server in a background pthread. External
 * processes connect and exchange newline-delimited JSON to inspect and
 * control the emulated Apple IIgs. One command per connection.
 *
 * Socket: /tmp/gsplus_debug.sock  (mode 0600, owner only)
 *
 * Supported commands and their required JSON fields:
 *   get_registers
 *   read_memory        addr, len
 *   search_memory      start, end, pattern  ("4c 00 c0" hex string)
 *   halt
 *   continue
 *   step
 *   get_break_info
 *   debugger_command   text                 (queued to main thread)
 *   list_volumes
 *   list_files         slot, path
 *   read_file          slot, path
 *   read_volume        slot
 *   send_keys          keys                 (inject into keyboard paste buffer)
 *
 * Thread model:
 *   - get_registers, read_memory, search_memory, get_break_info,
 *     list_volumes, list_files, read_file, read_volume:
 *       handled in server thread (fast, no halt needed)
 *   - halt, continue, step:
 *       atomic flag writes, safe from any thread
 *   - debugger_command:
 *       queued to main thread via mutex/condvar, processed in
 *       debug_server_poll() which is called from the 16ms loop
 */

#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <errno.h>

#include "defc.h"       /* includes iwm.h (Disk, Iwm structs) and all typedefs */
#include "debug_server.h"
#include "symbols.h"

/* ── Externs from the emulator core ─────────────────────────────────────── */
extern Engine_reg  engine;
extern int         g_halt_sim;
extern int         g_stepping;
extern dword64     g_dcycles_end;
extern byte       *g_memory_ptr;
extern byte       *g_slow_memory_ptr;
extern word32      g_c068_statereg;
extern int         g_irq_pending;
extern Iwm         g_iwm;
extern int         g_num_breakpoints;
extern Break_point g_break_pts[];

/* From debugger.c */
void do_debug_cmd(const char *in_str);


/* From sim65816.c — warm reset the emulator (same as pressing 'r' in debug window) */
void do_reset(void);

/* Memory read (engine_c.c) */
word32 get_memory_c(word32 addr);

/* ── dbg_printf capture (declared here, referenced in debugger.c) ────────── */
/* debugger.c's dbg_vprintf checks g_dbg_capture and appends to it when set */
char *g_dbg_capture     = NULL;
int   g_dbg_capture_len = 0;
int   g_dbg_capture_cap = 0;

/* ── Persistent log ring buffer (always-on capture of dbg_printf output) ─── */
#define LOG_BUF_SIZE  (128 * 1024)
static char            g_log_buf[LOG_BUF_SIZE];
static int             g_log_pos     = 0;   /* next write position */
static int             g_log_wrapped = 0;   /* 1 once the buffer has wrapped */
static pthread_mutex_t g_log_mutex   = PTHREAD_MUTEX_INITIALIZER;

void debug_server_log(const char *text, int len) {
	if(len <= 0) return;
	pthread_mutex_lock(&g_log_mutex);
	for(int i = 0; i < len; i++) {
		g_log_buf[g_log_pos] = text[i];
		if(++g_log_pos >= LOG_BUF_SIZE) {
			g_log_pos    = 0;
			g_log_wrapped = 1;
		}
	}
	pthread_mutex_unlock(&g_log_mutex);
}

/* ── Restart flag (set by server thread, consumed by main thread) ────────── */
static int g_restart_pending = 0;

/* ── Constants ───────────────────────────────────────────────────────────── */
#define SOCK_PATH            "/tmp/gsplus_debug.sock"
#define MAX_REQ_SIZE         (64 * 1024)
#define PRODOS_BLOCK_SIZE    512
#define MAX_FILE_READ_BYTES  (1 * 1024 * 1024)   /* 1 MB cap per file */

/* ── Break state (written from main thread, read from server thread) ─────── */
typedef struct {
	int    valid;
	word32 pc, acc, xreg, yreg, stack, direct, dbank, psr;
	int    reason;
} BreakState;

static BreakState      g_break_state;
static pthread_mutex_t g_break_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── Main-thread command queue (debugger_command only) ───────────────────── */
typedef struct {
	char  cmd[512];
	char *resp;         /* malloc'd; caller frees */
	int   done;
} MainCmd;

static MainCmd         g_main_cmd;
static int             g_main_cmd_pending = 0;
static pthread_mutex_t g_main_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_main_ready = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  g_main_done  = PTHREAD_COND_INITIALIZER;

/* ═══════════════════════════════════════════════════════════════════════════
 * JSON builder
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef struct {
	char *buf;
	int   len;
	int   cap;
} Jb;

static void jb_init(Jb *j) {
	j->cap = 8192;
	j->buf = (char *)malloc(j->cap);
	j->len = 0;
	if(j->buf) j->buf[0] = 0;
}

static void jb_grow(Jb *j, int need) {
	while(j->len + need >= j->cap) {
		j->cap *= 2;
		j->buf = (char *)realloc(j->buf, j->cap);
	}
}

static void jb_cat(Jb *j, const char *s) {
	int n = (int)strlen(s);
	jb_grow(j, n + 1);
	memcpy(j->buf + j->len, s, n);
	j->len += n;
	j->buf[j->len] = 0;
}

static void jb_printf(Jb *j, const char *fmt, ...) {
	char tmp[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	jb_cat(j, tmp);
}

static void jb_str(Jb *j, const char *s) {
	/* JSON-escaped string including surrounding quotes */
	if(!s) s = "";
	jb_grow(j, (int)strlen(s) * 6 + 4);
	j->buf[j->len++] = '"';
	for(; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if(c == '"')       { j->buf[j->len++] = '\\'; j->buf[j->len++] = '"'; }
		else if(c == '\\') { j->buf[j->len++] = '\\'; j->buf[j->len++] = '\\'; }
		else if(c == '\n') { j->buf[j->len++] = '\\'; j->buf[j->len++] = 'n'; }
		else if(c == '\r') { j->buf[j->len++] = '\\'; j->buf[j->len++] = 'r'; }
		else if(c == '\t') { j->buf[j->len++] = '\\'; j->buf[j->len++] = 't'; }
		else if(c < 0x20)  { j->len += sprintf(j->buf + j->len, "\\u%04x", c); }
		else if(c >= 0x80) { j->len += sprintf(j->buf + j->len, "\\u%04x", c); }
		else               { j->buf[j->len++] = (char)c; }
	}
	j->buf[j->len++] = '"';
	j->buf[j->len]   = 0;
}

static const char B64[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void jb_b64(Jb *j, const byte *data, int len) {
	/* Base64-encode data and append as a JSON string */
	jb_grow(j, ((len + 2) / 3) * 4 + 4);
	j->buf[j->len++] = '"';
	for(int i = 0; i < len; i += 3) {
		unsigned v = (unsigned)data[i] << 16;
		if(i+1 < len) v |= (unsigned)data[i+1] << 8;
		if(i+2 < len) v |= (unsigned)data[i+2];
		j->buf[j->len++] = B64[(v >> 18) & 63];
		j->buf[j->len++] = B64[(v >> 12) & 63];
		j->buf[j->len++] = (i+1 < len) ? B64[(v >>  6) & 63] : '=';
		j->buf[j->len++] = (i+2 < len) ? B64[ v        & 63] : '=';
	}
	j->buf[j->len++] = '"';
	j->buf[j->len]   = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Minimal JSON parser
 * ═══════════════════════════════════════════════════════════════════════════ */
static const char *jparse_find(const char *json, const char *key) {
	char keybuf[80];
	snprintf(keybuf, sizeof(keybuf), "\"%s\"", key);
	const char *p = strstr(json, keybuf);
	if(!p) return NULL;
	p += strlen(keybuf);
	while(*p == ' ' || *p == '\t') p++;
	if(*p != ':') return NULL;
	p++;
	while(*p == ' ' || *p == '\t') p++;
	return p;
}

static long jparse_int(const char *json, const char *key, long defval) {
	const char *v = jparse_find(json, key);
	if(!v) return defval;
	if(*v == '"') {
		v++;
		/* Accept "BB/OOOO" bank/offset format or plain hex */
		const char *slash = strchr(v, '/');
		if(slash && slash - v <= 2) {
			return (strtol(v, NULL, 16) << 16) | strtol(slash + 1, NULL, 16);
		}
		return strtol(v, NULL, 0);
	}
	return strtol(v, NULL, 0);
}

static void jparse_str(const char *json, const char *key, char *out, int sz) {
	const char *v = jparse_find(json, key);
	if(!v || *v != '"') { out[0] = 0; return; }
	v++;
	int i = 0;
	while(*v && *v != '"' && i < sz - 1) {
		if(*v == '\\') {
			v++;
			if(*v == 'n') out[i++] = '\n';
			else if(*v == 't') out[i++] = '\t';
			else out[i++] = *v;
		} else {
			out[i++] = *v;
		}
		v++;
	}
	out[i] = 0;
}

static int jparse_hexbytes(const char *json, const char *key,
                            byte *out, int out_sz) {
	/* Parse a hex string value like "4c 00 c0" into bytes */
	const char *v = jparse_find(json, key);
	if(!v || *v != '"') return 0;
	v++;
	int count = 0;
	while(*v && *v != '"' && count < out_sz) {
		while(*v == ' ') v++;
		if(*v == '"') break;
		if(!v[0] || !v[1]) break;
		char hex[3] = { v[0], v[1], 0 };
		out[count++] = (byte)strtol(hex, NULL, 16);
		v += 2;
	}
	return count;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ProDOS filesystem reader
 * ═══════════════════════════════════════════════════════════════════════════ */

static int prodos_read_block(Disk *disk, word32 blk, byte *buf) {
	/* Prefer in-memory raw_data (mmap'd or preloaded) */
	if(disk->raw_data) {
		word32 offset = blk * PRODOS_BLOCK_SIZE;
		dword64 img_size = disk->dimage_size ? disk->dimage_size : disk->raw_dsize;
		if((dword64)offset + PRODOS_BLOCK_SIZE > img_size) return -1;
		memcpy(buf, disk->raw_data + (size_t)disk->dimage_start + offset,
		       PRODOS_BLOCK_SIZE);
		return 0;
	}
	/* Fall back to file I/O */
	if(!disk->name_ptr || !disk->name_ptr[0]) return -1;
	int fd = open(disk->name_ptr, O_RDONLY | O_BINARY);
	if(fd < 0) return -1;
	off_t off = (off_t)disk->dimage_start + (off_t)blk * PRODOS_BLOCK_SIZE;
	lseek(fd, off, SEEK_SET);
	int n = (int)read(fd, buf, PRODOS_BLOCK_SIZE);
	close(fd);
	return (n == PRODOS_BLOCK_SIZE) ? 0 : -1;
}

/* Read file data for seedling/sapling/tree storage types.
 * Returns malloc'd buffer (caller frees) or NULL.
 * *out_len = -1 means file exceeds MAX_FILE_READ_BYTES. */
static byte *prodos_read_file_data(Disk *disk, word32 key_blk,
                                    int storage_type, word32 eof,
                                    int *out_len) {
	*out_len = 0;
	if(eof == 0) return NULL;
	if(eof > (word32)MAX_FILE_READ_BYTES) { *out_len = -1; return NULL; }

	byte *data = (byte *)calloc(1, eof);
	if(!data) return NULL;

	byte ibuf[PRODOS_BLOCK_SIZE];
	byte mbuf[PRODOS_BLOCK_SIZE];
	byte dbuf[PRODOS_BLOCK_SIZE];
	int stype = (storage_type >> 4) & 0xF;
	int written = 0;

	if(stype == 1) {
		/* Seedling: key_blk is the sole data block */
		if(prodos_read_block(disk, key_blk, dbuf) == 0) {
			int copy = (eof < PRODOS_BLOCK_SIZE) ? (int)eof : PRODOS_BLOCK_SIZE;
			memcpy(data, dbuf, copy);
		}
		written = eof;

	} else if(stype == 2) {
		/* Sapling: key_blk is index block, 256 data-block pointers */
		if(prodos_read_block(disk, key_blk, ibuf) < 0) { free(data); return NULL; }
		for(int i = 0; i < 256 && written < (int)eof; i++) {
			word32 dblk = ibuf[i] | ((word32)ibuf[i + 256] << 8);
			if(dblk == 0) { written += PRODOS_BLOCK_SIZE; continue; }
			if(prodos_read_block(disk, dblk, dbuf) < 0) break;
			int copy = (int)eof - written;
			if(copy > PRODOS_BLOCK_SIZE) copy = PRODOS_BLOCK_SIZE;
			memcpy(data + written, dbuf, copy);
			written += copy;
		}

	} else if(stype == 3) {
		/* Tree: key_blk is master index; each entry → a sapling index */
		if(prodos_read_block(disk, key_blk, mbuf) < 0) { free(data); return NULL; }
		for(int m = 0; m < 128 && written < (int)eof; m++) {
			word32 iblk = mbuf[m] | ((word32)mbuf[m + 256] << 8);
			if(iblk == 0) { written += 256 * PRODOS_BLOCK_SIZE; continue; }
			if(prodos_read_block(disk, iblk, ibuf) < 0) break;
			for(int i = 0; i < 256 && written < (int)eof; i++) {
				word32 dblk = ibuf[i] | ((word32)ibuf[i + 256] << 8);
				if(dblk == 0) { written += PRODOS_BLOCK_SIZE; continue; }
				if(prodos_read_block(disk, dblk, dbuf) < 0) break;
				int copy = (int)eof - written;
				if(copy > PRODOS_BLOCK_SIZE) copy = PRODOS_BLOCK_SIZE;
				memcpy(data + written, dbuf, copy);
				written += copy;
			}
		}
	} else {
		free(data); return NULL;
	}
	*out_len = (int)eof;
	return data;
}

static const char *prodos_type_name(byte ftype) {
	switch(ftype) {
		case 0x00: return "UNK"; case 0x01: return "BAD";
		case 0x04: return "TXT"; case 0x06: return "BIN";
		case 0x0F: return "DIR"; case 0x19: return "ADB";
		case 0x1A: return "AWP"; case 0x1B: return "ASP";
		case 0xB0: return "SRC"; case 0xB3: return "S16";
		case 0xB5: return "MDI"; case 0xCA: return "ICN";
		case 0xD7: return "BAS"; case 0xE0: return "PAS";
		case 0xFF: return "SYS"; default:    return "BIN";
	}
}

/* Walk a ProDOS directory linked list starting at dir_blk.
 * read_contents: if non-zero, include file data as base64
 * recursive: if non-zero, descend into subdirectories */
static void prodos_walk_dir(Jb *jb, Disk *disk, word32 dir_blk,
                             int read_contents, int recursive, int *first_out) {
	byte blk[PRODOS_BLOCK_SIZE];
	word32 cur = dir_blk;
	int is_vol_blk = 1;   /* first block has volume/dir header as entry 0 */

	while(cur) {
		if(prodos_read_block(disk, cur, blk) < 0) break;
		word32 next = blk[2] | ((word32)blk[3] << 8);
		int ei_start = is_vol_blk ? 1 : 0;   /* skip header entry */
		is_vol_blk = 0;

		for(int ei = ei_start; ei < 13; ei++) {
			byte *ep = blk + 4 + ei * 0x27;
			byte stype_nlen = ep[0];
			int stype = (stype_nlen >> 4) & 0xF;
			int nlen  = stype_nlen & 0xF;
			if(stype == 0) continue;   /* inactive / deleted entry */

			char name[16] = {0};
			memcpy(name, ep + 1, nlen);

			byte   ftype    = ep[16];
			word32 key_blk  = ep[17] | ((word32)ep[18] << 8);
			word32 blks     = ep[19] | ((word32)ep[20] << 8);
			word32 eof      = ep[21] | ((word32)ep[22] << 8) | ((word32)ep[23] << 16);
			word32 aux_type = ep[31] | ((word32)ep[32] << 8);

			if(!(*first_out)) jb_cat(jb, ",");
			*first_out = 0;

			jb_cat(jb, "{\"name\":");
			jb_str(jb, name);
			jb_printf(jb, ",\"type\":\"%s\",\"ftype\":\"$%02X\"",
			          prodos_type_name(ftype), ftype);
			jb_printf(jb, ",\"eof\":%u,\"blocks\":%u,\"aux\":\"$%04X\"",
			          eof, blks, aux_type);

			if(stype == 0xD) {
				/* Subdirectory */
				if(recursive) {
					jb_cat(jb, ",\"files\":[");
					int fe = 1;
					prodos_walk_dir(jb, disk, key_blk, read_contents, recursive, &fe);
					jb_cat(jb, "]");
				}
			} else if(read_contents) {
				int dlen = 0;
				byte *fdata = prodos_read_file_data(disk, key_blk,
				                                    stype_nlen, eof, &dlen);
				if(dlen == -1) {
					jb_printf(jb, ",\"data_note\":\"file exceeds 1MB (%u bytes)\"", eof);
				} else if(fdata && dlen > 0) {
					jb_cat(jb, ",\"data_b64\":");
					jb_b64(jb, fdata, dlen);
					free(fdata);
				} else {
					jb_cat(jb, ",\"data_b64\":\"\"");
				}
			}
			jb_cat(jb, "}");
		}
		cur = next;
	}
}

/* Navigate a ProDOS path and return the directory block, or 0 on error. */
static word32 prodos_find_dir(Disk *disk, const char *path) {
	word32 dir_blk = 2;   /* root volume directory */
	char pathbuf[512];
	strncpy(pathbuf, path ? path : "/", sizeof(pathbuf) - 1);
	pathbuf[sizeof(pathbuf)-1] = 0;

	char *tok = strtok(pathbuf, "/");
	while(tok && tok[0]) {
		byte blk[PRODOS_BLOCK_SIZE];
		word32 cur = dir_blk;
		int found = 0;
		int first_blk = 1;
		while(cur && !found) {
			if(prodos_read_block(disk, cur, blk) < 0) return 0;
			word32 next = blk[2] | ((word32)blk[3] << 8);
			for(int ei = first_blk ? 1 : 0; ei < 13; ei++) {
				byte *ep = blk + 4 + ei * 0x27;
				if(((ep[0] >> 4) & 0xF) != 0xD) continue;
				int nlen = ep[0] & 0xF;
				if((int)strlen(tok) != nlen) continue;
				int match = 1;
				for(int ci = 0; ci < nlen && match; ci++) {
					char a = ep[1+ci]; if(a>='a'&&a<='z') a -= 32;
					char b = tok[ci];  if(b>='a'&&b<='z') b -= 32;
					if(a != b) match = 0;
				}
				if(match) {
					dir_blk = ep[17] | ((word32)ep[18] << 8);
					found = 1; break;
				}
			}
			first_blk = 0;
			cur = next;
		}
		if(!found) return 0;
		tok = strtok(NULL, "/");
	}
	return dir_blk;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Disk slot enumeration
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef struct { Disk *disk; char label[16]; } DiskSlot;
#define MAX_SLOTS (4 + MAX_C7_DISKS)

static int enum_disks(DiskSlot slots[MAX_SLOTS]) {
	int n = 0;
	for(int i = 0; i < 2; i++) {
		Disk *d = &g_iwm.drive525[i];
		if(d->name_ptr && d->name_ptr[0]) {
			snprintf(slots[n].label, sizeof(slots[n].label), "s6d%d", i+1);
			slots[n++].disk = d;
		}
	}
	for(int i = 0; i < 2; i++) {
		Disk *d = &g_iwm.drive35[i];
		if(d->name_ptr && d->name_ptr[0]) {
			snprintf(slots[n].label, sizeof(slots[n].label), "s5d%d", i+1);
			slots[n++].disk = d;
		}
	}
	for(int i = 0; i < MAX_C7_DISKS; i++) {
		Disk *d = &g_iwm.smartport[i];
		if(d->name_ptr && d->name_ptr[0]) {
			snprintf(slots[n].label, sizeof(slots[n].label), "sp%d", i);
			slots[n++].disk = d;
		}
	}
	return n;
}

static DiskSlot *find_slot(DiskSlot *slots, int n, const char *label) {
	for(int i = 0; i < n; i++) {
		if(strcmp(slots[i].label, label) == 0) return &slots[i];
	}
	return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PSR flag helpers
 * ═══════════════════════════════════════════════════════════════════════════ */
static void append_psr_flags(Jb *jb, word32 psr) {
	jb_printf(jb,
	    ",\"N\":%d,\"V\":%d,\"M\":%d,\"X_flag\":%d"
	    ",\"D\":%d,\"I\":%d,\"Z\":%d,\"C\":%d",
	    (psr>>7)&1, (psr>>6)&1, (psr>>5)&1, (psr>>4)&1,
	    (psr>>3)&1, (psr>>2)&1, (psr>>1)&1,  psr    &1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Command handlers
 * ═══════════════════════════════════════════════════════════════════════════ */

static void cmd_get_registers(Jb *jb) {
	/* Reading the engine struct from a non-main thread is safe on x86:
	 * word-aligned reads are atomic, and worst-case we get a mid-frame
	 * snapshot—acceptable for debugging. */
	word32 kpc    = engine.kpc;
	word32 acc    = engine.acc;
	word32 xreg   = engine.xreg;
	word32 yreg   = engine.yreg;
	word32 stack  = engine.stack;
	word32 dbank  = engine.dbank;
	word32 direct = engine.direct;
	word32 psr    = engine.psr;

	jb_printf(jb,
	    "{\"ok\":true"
	    ",\"pc\":\"%02X/%04X\""
	    ",\"acc\":\"$%04X\",\"x\":\"$%04X\",\"y\":\"$%04X\""
	    ",\"sp\":\"$%04X\",\"dp\":\"$%04X\",\"db\":\"$%02X\",\"psr\":\"$%02X\"",
	    (kpc >> 16) & 0xFF, kpc & 0xFFFF,
	    acc & 0xFFFF, xreg & 0xFFFF, yreg & 0xFFFF,
	    stack & 0xFFFF, direct & 0xFFFF, dbank & 0xFF, psr & 0xFF);
	append_psr_flags(jb, psr);
	jb_printf(jb, ",\"halted\":%s}", g_halt_sim ? "true" : "false");
}

static void cmd_read_memory(Jb *jb, word32 addr, int len) {
	if(len <= 0 || len > 65536) len = 256;
	byte *buf = (byte *)malloc(len);
	if(!buf) { jb_cat(jb, "{\"ok\":false,\"error\":\"oom\"}"); return; }
	for(int i = 0; i < len; i++) buf[i] = (byte)get_memory_c(addr + i);

	jb_printf(jb, "{\"ok\":true,\"addr\":\"%02X/%04X\",\"len\":%d,\"data_b64\":",
	          (addr >> 16) & 0xFF, addr & 0xFFFF, len);
	jb_b64(jb, buf, len);
	jb_cat(jb, ",\"data_hex\":\"");
	for(int i = 0; i < len; i++) {
		char h[5];
		snprintf(h, sizeof(h), i ? " %02X" : "%02X", buf[i]);
		jb_cat(jb, h);
	}
	jb_cat(jb, "\"}");
	free(buf);
}

static void cmd_search_memory(Jb *jb, word32 start, word32 end,
                               const byte *pat, int pat_len) {
	if(pat_len <= 0 || pat_len > 64) {
		jb_cat(jb, "{\"ok\":false,\"error\":\"invalid pattern length\"}");
		return;
	}
	jb_printf(jb, "{\"ok\":true,\"matches\":[");
	int first = 1;
	for(word32 a = start; a + (word32)pat_len - 1 <= end; a++) {
		int hit = 1;
		for(int i = 0; i < pat_len && hit; i++) {
			if((byte)get_memory_c(a + i) != pat[i]) hit = 0;
		}
		if(hit) {
			if(!first) jb_cat(jb, ",");
			first = 0;
			jb_printf(jb, "\"%02X/%04X\"", (a >> 16) & 0xFF, a & 0xFFFF);
		}
	}
	jb_cat(jb, "]}");
}

static void cmd_halt(Jb *jb) {
	g_halt_sim    = 1;
	g_dcycles_end = 0;	/* stop inner CPU loop immediately, mirroring set_halt_act() */
	jb_printf(jb, "{\"ok\":true,\"pc\":\"%02X/%04X\"}",
	          (engine.kpc >> 16) & 0xFF, engine.kpc & 0xFFFF);
}

static void cmd_continue(Jb *jb) {
	g_halt_sim = 0;
	g_stepping = 0;
	jb_cat(jb, "{\"ok\":true}");
}

static void cmd_step(Jb *jb) {
	g_stepping = 1;
	g_halt_sim = 0;
	jb_cat(jb, "{\"ok\":true}");
}

static void cmd_get_break_info(Jb *jb) {
	pthread_mutex_lock(&g_break_mutex);
	BreakState bs = g_break_state;
	pthread_mutex_unlock(&g_break_mutex);

	if(!bs.valid) {
		jb_cat(jb, "{\"ok\":true,\"broken\":false}");
		return;
	}
	static const char *reasons[] = {
		"BRK", "breakpoint", "halt_printf", "explicit", "unknown"
	};
	int ri = (bs.reason >= 0 && bs.reason <= 3) ? bs.reason : 4;
	jb_printf(jb,
	    "{\"ok\":true,\"broken\":true,\"reason\":\"%s\""
	    ",\"pc\":\"%02X/%04X\""
	    ",\"acc\":\"$%04X\",\"x\":\"$%04X\",\"y\":\"$%04X\""
	    ",\"sp\":\"$%04X\",\"dp\":\"$%04X\",\"db\":\"$%02X\",\"psr\":\"$%02X\"",
	    reasons[ri],
	    (bs.pc >> 16) & 0xFF, bs.pc & 0xFFFF,
	    bs.acc & 0xFFFF, bs.xreg & 0xFFFF, bs.yreg & 0xFFFF,
	    bs.stack & 0xFFFF, bs.direct & 0xFFFF, bs.dbank & 0xFF, bs.psr & 0xFF);
	append_psr_flags(jb, bs.psr);
	jb_cat(jb, "}");
}

static void cmd_debugger_command(Jb *jb, const char *cmd) {
	/* This command must execute on the main thread. We queue it and wait. */
	pthread_mutex_lock(&g_main_mutex);
	strncpy(g_main_cmd.cmd, cmd, sizeof(g_main_cmd.cmd) - 1);
	g_main_cmd.cmd[sizeof(g_main_cmd.cmd)-1] = 0;
	g_main_cmd.resp = NULL;
	g_main_cmd.done = 0;
	g_main_cmd_pending = 1;
	pthread_cond_signal(&g_main_ready);
	while(!g_main_cmd.done) {
		pthread_cond_wait(&g_main_done, &g_main_mutex);
	}
	char *resp = g_main_cmd.resp;
	g_main_cmd_pending = 0;
	pthread_mutex_unlock(&g_main_mutex);

	jb_cat(jb, "{\"ok\":true,\"output\":");
	jb_str(jb, resp ? resp : "");
	jb_cat(jb, "}");
	free(resp);
}

static void cmd_list_volumes(Jb *jb) {
	DiskSlot slots[MAX_SLOTS];
	int n = enum_disks(slots);
	static const char *type_names[] = {
		"unknown","prodos","dos33","dynapro","nib","woz"
	};

	jb_cat(jb, "{\"ok\":true,\"volumes\":[");
	for(int i = 0; i < n; i++) {
		if(i) jb_cat(jb, ",");
		Disk *d = slots[i].disk;
		int ti = (d->image_type >= 0 && d->image_type <= 5) ?
		          d->image_type : 0;
		jb_printf(jb, "{\"slot\":\"%s\"", slots[i].label);
		jb_cat(jb, ",\"path\":"); jb_str(jb, d->name_ptr ? d->name_ptr : "");
		if(d->partition_name && d->partition_name[0]) {
			jb_cat(jb, ",\"partition\":"); jb_str(jb, d->partition_name);
		}
		jb_printf(jb, ",\"image_type\":\"%s\"", type_names[ti]);
		jb_printf(jb, ",\"write_prot\":%s", d->write_prot ? "true" : "false");
		jb_printf(jb, ",\"dirty\":%s", d->disk_dirty ? "true" : "false");
		/* For ProDOS images, pull the volume name from the VHB */
		if(d->image_type == DSK_TYPE_PRODOS) {
			byte vblk[PRODOS_BLOCK_SIZE];
			if(prodos_read_block(d, 2, vblk) == 0) {
				int nlen = vblk[4] & 0xF;
				char vname[16] = {0};
				memcpy(vname, vblk + 5, nlen);
				jb_cat(jb, ",\"vol_name\":"); jb_str(jb, vname);
				word32 total = vblk[37] | ((word32)vblk[38] << 8);
				jb_printf(jb, ",\"total_blocks\":%u", total);
			}
		}
		jb_cat(jb, "}");
	}
	jb_cat(jb, "]}");
}

static void cmd_list_files(Jb *jb, const char *slot_label, const char *path, int recursive) {
	DiskSlot slots[MAX_SLOTS];
	int n = enum_disks(slots);
	DiskSlot *slot = find_slot(slots, n, slot_label);
	if(!slot) {
		jb_cat(jb, "{\"ok\":false,\"error\":\"volume not found\"}");
		return;
	}
	Disk *disk = slot->disk;
	if(disk->image_type != DSK_TYPE_PRODOS) {
		jb_cat(jb, "{\"ok\":false,\"error\":\"only ProDOS images supported\"}");
		return;
	}
	word32 dir_blk = prodos_find_dir(disk, path);
	if(!dir_blk) {
		jb_cat(jb, "{\"ok\":false,\"error\":\"path not found\"}");
		return;
	}
	jb_printf(jb, "{\"ok\":true,\"slot\":\"%s\",\"path\":", slot_label);
	jb_str(jb, path ? path : "/");
	jb_cat(jb, ",\"files\":[");
	int first = 1;
	prodos_walk_dir(jb, disk, dir_blk, 0, recursive, &first);
	jb_cat(jb, "]}");
}

static void cmd_read_file(Jb *jb, const char *slot_label, const char *path) {
	DiskSlot slots[MAX_SLOTS];
	int n = enum_disks(slots);
	DiskSlot *slot = find_slot(slots, n, slot_label);
	if(!slot) {
		jb_cat(jb, "{\"ok\":false,\"error\":\"volume not found\"}");
		return;
	}
	Disk *disk = slot->disk;
	if(disk->image_type != DSK_TYPE_PRODOS) {
		jb_cat(jb, "{\"ok\":false,\"error\":\"only ProDOS images supported\"}");
		return;
	}

	/* Split path into parent dir and filename */
	char pbuf[512];
	strncpy(pbuf, path ? path : "", sizeof(pbuf) - 1);
	pbuf[sizeof(pbuf)-1] = 0;
	char *slash = strrchr(pbuf, '/');
	const char *filename;
	if(slash) { *slash = 0; filename = slash + 1; }
	else       { filename = pbuf; pbuf[0] = 0; }

	word32 dir_blk = prodos_find_dir(disk, pbuf[0] ? pbuf : "/");
	if(!dir_blk) {
		jb_cat(jb, "{\"ok\":false,\"error\":\"directory not found\"}");
		return;
	}

	/* Search directory for the file */
	byte blk[PRODOS_BLOCK_SIZE];
	word32 cur = dir_blk;
	int first_blk = 1;
	while(cur) {
		if(prodos_read_block(disk, cur, blk) < 0) break;
		word32 next = blk[2] | ((word32)blk[3] << 8);
		for(int ei = first_blk ? 1 : 0; ei < 13; ei++) {
			byte *ep = blk + 4 + ei * 0x27;
			int stype = (ep[0] >> 4) & 0xF;
			if(stype == 0) continue;
			int nlen = ep[0] & 0xF;
			int flen = (int)strlen(filename);
			if(flen != nlen) continue;
			int match = 1;
			for(int ci = 0; ci < nlen && match; ci++) {
				char a = ep[1+ci];    if(a>='a'&&a<='z') a -= 32;
				char b = filename[ci]; if(b>='a'&&b<='z') b -= 32;
				if(a != b) match = 0;
			}
			if(!match) continue;

			byte   ftype   = ep[16];
			word32 key_blk = ep[17] | ((word32)ep[18] << 8);
			word32 eof     = ep[21] | ((word32)ep[22] << 8) | ((word32)ep[23] << 16);
			word32 aux     = ep[31] | ((word32)ep[32] << 8);
			int dlen = 0;
			byte *fdata = prodos_read_file_data(disk, key_blk, ep[0], eof, &dlen);

			jb_cat(jb, "{\"ok\":true,\"name\":");
			jb_str(jb, filename);
			jb_printf(jb, ",\"type\":\"%s\",\"ftype\":\"$%02X\""
			          ",\"eof\":%u,\"aux\":\"$%04X\"",
			          prodos_type_name(ftype), ftype, eof, aux);
			if(dlen == -1) {
				jb_printf(jb, ",\"error\":\"file exceeds 1MB (%u bytes)\"", eof);
			} else if(fdata && dlen > 0) {
				jb_cat(jb, ",\"data_b64\":"); jb_b64(jb, fdata, dlen);
				free(fdata);
			} else {
				jb_cat(jb, ",\"data_b64\":\"\"");
			}
			jb_cat(jb, "}");
			return;
		}
		first_blk = 0;
		cur = next;
	}
	jb_cat(jb, "{\"ok\":false,\"error\":\"file not found\"}");
}

/* ── Volume extraction to host filesystem ───────────────────────────────── */

typedef struct {
	int  file_count;
	long byte_count;
	Jb   tree;          /* text lines: "/PATH/NAME  [TYPE, N bytes]\n" */
} ExtractStats;

static void extract_prodos_tree(Disk *disk, word32 dir_blk,
                                 const char *host_dir, const char *vol_prefix,
                                 ExtractStats *stats) {
	byte blk[PRODOS_BLOCK_SIZE];
	word32 cur = dir_blk;
	int is_vol_blk = 1;

	while(cur) {
		if(prodos_read_block(disk, cur, blk) < 0) break;
		word32 next = blk[2] | ((word32)blk[3] << 8);

		for(int ei = is_vol_blk ? 1 : 0; ei < 13; ei++) {
			byte *ep = blk + 4 + ei * 0x27;
			int stype = (ep[0] >> 4) & 0xF;
			int nlen  =  ep[0]       & 0xF;
			if(stype == 0) continue;

			/* Build lowercase host filename from ProDOS name */
			char fname[16] = {0};
			for(int i = 0; i < nlen; i++) {
				char c = ep[1+i];
				fname[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
			}

			char host_path[1024];
			snprintf(host_path, sizeof(host_path), "%s/%s", host_dir, fname);
			char vol_path[512];
			snprintf(vol_path, sizeof(vol_path), "%s/%s", vol_prefix, fname);

			byte   ftype   = ep[16];
			word32 key_blk = ep[17] | ((word32)ep[18] << 8);
			word32 eof     = ep[21] | ((word32)ep[22] << 8) | ((word32)ep[23] << 16);

			if(stype == 0xD) {
				/* Subdirectory: create host dir and recurse */
				mkdir(host_path, 0755);
				jb_printf(&stats->tree, "%s/\n", vol_path);
				extract_prodos_tree(disk, key_blk, host_path, vol_path, stats);
			} else {
				/* File: read data and write to host */
				int dlen = 0;
				byte *fdata = prodos_read_file_data(disk, key_blk, ep[0], eof, &dlen);
				jb_printf(&stats->tree, "%s  [%s, %u bytes]\n",
				          vol_path, prodos_type_name(ftype), eof);
				if(fdata && dlen > 0) {
					int fd = open(host_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
					if(fd >= 0) {
						write(fd, fdata, dlen);
						close(fd);
					}
					free(fdata);
					stats->byte_count += dlen;
				} else if(dlen == -1) {
					/* File exceeds MAX_FILE_READ_BYTES — noted in tree, not extracted */
				}
				stats->file_count++;
			}
		}
		is_vol_blk = 0;
		cur = next;
	}
}

static void cmd_read_volume(Jb *jb, const char *slot_label) {
	DiskSlot slots[MAX_SLOTS];
	int n = enum_disks(slots);
	DiskSlot *slot = find_slot(slots, n, slot_label);
	if(!slot) {
		jb_cat(jb, "{\"ok\":false,\"error\":\"volume not found\"}");
		return;
	}
	Disk *disk = slot->disk;
	if(disk->image_type != DSK_TYPE_PRODOS) {
		jb_cat(jb, "{\"ok\":false,\"error\":\"only ProDOS images supported\"}");
		return;
	}

	/* Read volume header from block 2 */
	byte vblk[PRODOS_BLOCK_SIZE];
	if(prodos_read_block(disk, 2, vblk) < 0) {
		jb_cat(jb, "{\"ok\":false,\"error\":\"cannot read volume directory\"}");
		return;
	}
	int vnamelen = vblk[4] & 0xF;
	char vname[16] = {0};
	memcpy(vname, vblk + 5, vnamelen);
	word32 total_blocks = vblk[37] | ((word32)vblk[38] << 8);

	/* Create output directory: /tmp/gsplus_VOLNAME_PID */
	char out_dir[256];
	snprintf(out_dir, sizeof(out_dir), "/tmp/gsplus_%s_%d", vname, (int)getpid());
	/* Lowercase the vol name portion for the host path */
	for(char *p = out_dir + 12; *p && *p != '_'; p++) {
		if(*p >= 'A' && *p <= 'Z') *p += 32;
	}
	if(mkdir(out_dir, 0755) < 0 && errno != EEXIST) {
		jb_printf(jb, "{\"ok\":false,\"error\":\"mkdir failed: %s\"}", strerror(errno));
		return;
	}

	/* Extract the full ProDOS tree to host filesystem */
	ExtractStats stats;
	memset(&stats, 0, sizeof(stats));
	jb_init(&stats.tree);
	extract_prodos_tree(disk, 2, out_dir, "", &stats);

	/* Write a text manifest of the tree to the output directory */
	char manifest_path[320];
	snprintf(manifest_path, sizeof(manifest_path), "%s/_manifest.txt", out_dir);
	int mfd = open(manifest_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if(mfd >= 0) {
		write(mfd, stats.tree.buf, stats.tree.len);
		close(mfd);
	}

	/* Return metadata only — no binary data in the response */
	jb_printf(jb, "{\"ok\":true,\"slot\":\"%s\"", slot_label);
	jb_cat(jb, ",\"vol_name\":"); jb_str(jb, vname);
	jb_printf(jb, ",\"total_blocks\":%u", total_blocks);
	jb_printf(jb, ",\"total_files\":%d", stats.file_count);
	jb_printf(jb, ",\"total_bytes\":%ld", stats.byte_count);
	jb_cat(jb, ",\"output_dir\":"); jb_str(jb, out_dir);
	jb_cat(jb, ",\"manifest\":"); jb_str(jb, manifest_path);
	jb_cat(jb, ",\"tree\":"); jb_str(jb, stats.tree.buf ? stats.tree.buf : "");
	jb_cat(jb, "}");
	free(stats.tree.buf);
}

static void cmd_read_volume_raw(Jb *jb, const char *slot_label) {
	DiskSlot slots[MAX_SLOTS];
	int n = enum_disks(slots);
	DiskSlot *slot = find_slot(slots, n, slot_label);
	if(!slot) {
		jb_cat(jb, "{\"ok\":false,\"error\":\"volume not found\"}");
		return;
	}
	Disk *disk = slot->disk;

	/* Determine image bounds */
	dword64 img_start = disk->dimage_start;
	dword64 img_size  = disk->dimage_size;
	if(img_size == 0) img_size = disk->raw_dsize;
	if(img_size == 0) {
		jb_cat(jb, "{\"ok\":false,\"error\":\"image size unknown\"}");
		return;
	}

	/* Build output path: /tmp/gsplus_raw_SLOT_PID.img */
	char out_path[256];
	snprintf(out_path, sizeof(out_path), "/tmp/gsplus_raw_%s_%d.img",
	         slot_label, (int)getpid());

	int out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if(out_fd < 0) {
		jb_printf(jb, "{\"ok\":false,\"error\":\"cannot create output file: %s\"}",
		          strerror(errno));
		return;
	}

	long bytes_written = 0;

	if(disk->raw_data) {
		/* Image is already in memory — write directly */
		bytes_written = (long)write(out_fd, disk->raw_data + img_start, (size_t)img_size);
	} else if(disk->name_ptr && disk->name_ptr[0]) {
		/* Read from the source file in chunks */
		int src_fd = open(disk->name_ptr, O_RDONLY | O_BINARY);
		if(src_fd < 0) {
			close(out_fd);
			jb_printf(jb, "{\"ok\":false,\"error\":\"cannot open source image: %s\"}",
			          strerror(errno));
			return;
		}
		lseek(src_fd, (off_t)img_start, SEEK_SET);

		#define RAW_CHUNK (64 * 1024)
		byte *chunk = (byte *)malloc(RAW_CHUNK);
		if(!chunk) {
			close(src_fd); close(out_fd);
			jb_cat(jb, "{\"ok\":false,\"error\":\"oom\"}");
			return;
		}
		dword64 remaining = img_size;
		while(remaining > 0) {
			int to_read = (remaining > RAW_CHUNK) ? RAW_CHUNK : (int)remaining;
			int got = (int)read(src_fd, chunk, to_read);
			if(got <= 0) break;
			write(out_fd, chunk, got);
			bytes_written += got;
			remaining -= got;
		}
		free(chunk);
		close(src_fd);
		#undef RAW_CHUNK
	} else {
		close(out_fd);
		jb_cat(jb, "{\"ok\":false,\"error\":\"no source data available for this disk\"}");
		return;
	}

	close(out_fd);

	static const char *type_names[] = {
		"unknown","prodos","dos33","dynapro","nib","woz"
	};
	int ti = (disk->image_type >= 0 && disk->image_type <= 5) ?
	          disk->image_type : 0;

	jb_printf(jb, "{\"ok\":true,\"slot\":\"%s\"", slot_label);
	jb_cat(jb, ",\"source_path\":"); jb_str(jb, disk->name_ptr ? disk->name_ptr : "");
	jb_printf(jb, ",\"image_type\":\"%s\"", type_names[ti]);
	jb_printf(jb, ",\"image_size_bytes\":%llu", (unsigned long long)img_size);
	jb_printf(jb, ",\"bytes_written\":%ld", bytes_written);
	jb_cat(jb, ",\"output_path\":"); jb_str(jb, out_path);

	/* For ProDOS images, include volume header metadata from block 2 */
	if(disk->image_type == DSK_TYPE_PRODOS) {
		byte vblk[PRODOS_BLOCK_SIZE];
		if(prodos_read_block(disk, 2, vblk) == 0) {
			int nlen = vblk[4] & 0xF;
			char vname[16] = {0};
			memcpy(vname, vblk + 5, nlen);
			word32 total_blocks = vblk[37] | ((word32)vblk[38] << 8);
			jb_cat(jb, ",\"vol_name\":"); jb_str(jb, vname);
			jb_printf(jb, ",\"total_blocks\":%u", total_blocks);
			jb_printf(jb, ",\"vol_size_bytes\":%u", total_blocks * PRODOS_BLOCK_SIZE);
		}
	}

	jb_cat(jb, "}");
}

static void cmd_restart(Jb *jb) {
	/* Queue a reset to run on the main thread via debug_server_poll() */
	pthread_mutex_lock(&g_main_mutex);
	g_restart_pending = 1;
	pthread_mutex_unlock(&g_main_mutex);
	jb_cat(jb, "{\"ok\":true,\"message\":\"restart queued\"}");
}

static void cmd_get_log(Jb *jb, int n_lines) {
	if(n_lines <= 0)   n_lines = 50;
	if(n_lines > 1000) n_lines = 1000;

	pthread_mutex_lock(&g_log_mutex);

	/* Snapshot ring buffer into a linear allocation */
	int total = g_log_wrapped ? LOG_BUF_SIZE : g_log_pos;
	char *linear = (char *)malloc(total + 1);
	if(!linear) {
		pthread_mutex_unlock(&g_log_mutex);
		jb_cat(jb, "{\"ok\":false,\"error\":\"oom\"}");
		return;
	}
	if(g_log_wrapped) {
		int tail = LOG_BUF_SIZE - g_log_pos;
		memcpy(linear,        g_log_buf + g_log_pos, tail);
		memcpy(linear + tail, g_log_buf,              g_log_pos);
	} else {
		memcpy(linear, g_log_buf, total);
	}
	linear[total] = 0;

	pthread_mutex_unlock(&g_log_mutex);

	/* Walk backward to find the start of the last n_lines */
	int newlines = 0;
	int start = 0;
	for(int i = total - 1; i >= 0; i--) {
		if(linear[i] == '\n') {
			newlines++;
			if(newlines == n_lines) {
				start = i + 1;
				break;
			}
		}
	}

	jb_printf(jb, "{\"ok\":true,\"lines_requested\":%d,"
	              "\"total_buffered_bytes\":%d,\"log\":", n_lines, total);
	jb_str(jb, linear + start);
	jb_cat(jb, "}");
	free(linear);
}

/* Apply enabled value to one trap index; silently ignores index 0. */
static void wdm_trap_set_one(int idx, int enabled) {
	if(idx < 0 || idx > 127) return;
	if(idx == 0) return;	/* $00 always off */
	g_wdm_trap_enabled[idx] = enabled ? 1 : 0;
}

static void cmd_set_wdm_trap(Jb *jb, const char *req) {
	/* enabled: bool or 0/1 */
	const char *ev = jparse_find(req, "enabled");
	int enabled = 1;
	if(ev) {
		if(strncmp(ev, "false", 5) == 0 || *ev == '0') enabled = 0;
	}

	/* trap: "all" | integer | [int, int, ...] */
	const char *tv = jparse_find(req, "trap");
	if(!tv) {
		jb_cat(jb, "{\"ok\":false,\"error\":\"missing 'trap' field\"}");
		return;
	}

	int noted_zero = 0;	/* did caller try to set trap $00? */

	if(*tv == '"') {
		/* String — only "all" is accepted */
		tv++;
		if(strncmp(tv, "all", 3) == 0) {
			for(int i = 1; i < 128; i++) {
				g_wdm_trap_enabled[i] = enabled ? 1 : 0;
			}
		} else {
			jb_cat(jb, "{\"ok\":false,\"error\":\"unknown trap value (use integer, list, or \\\"all\\\")\"}");
			return;
		}
	} else if(*tv == '[') {
		/* JSON array of integers */
		tv++;
		while(*tv && *tv != ']') {
			while(*tv == ' ' || *tv == ',' || *tv == '\t') tv++;
			if(*tv == ']' || !*tv) break;
			char *end;
			long idx = strtol(tv, &end, 0);
			if(end == tv) break;
			if(idx == 0) noted_zero = 1;
			wdm_trap_set_one((int)idx, enabled);
			tv = end;
		}
	} else {
		/* Single integer */
		long idx = strtol(tv, NULL, 0);
		if(idx == 0) noted_zero = 1;
		wdm_trap_set_one((int)idx, enabled);
	}

	if(noted_zero) {
		jb_printf(jb, "{\"ok\":true,\"note\":\"trap $00 is always disabled and cannot be changed\"}");
	} else {
		jb_cat(jb, "{\"ok\":true}");
	}
}

static void cmd_send_keys(Jb *jb, const char *keys) {
	int	i, ret;

	if(!keys || !keys[0]) {
		jb_cat(jb, "{\"ok\":false,\"error\":\"empty key string\"}");
		return;
	}
	for(i = 0; keys[i]; i++) {
		ret = adb_paste_add_buf((word32)(unsigned char)keys[i]);
		if(ret) {
			jb_printf(jb, "{\"ok\":false,\"error\":\"paste buffer full at char %d\"}", i);
			return;
		}
	}
	jb_printf(jb, "{\"ok\":true,\"chars_sent\":%d}", i);
}

static void cmd_get_wdm_traps(Jb *jb) {
	jb_cat(jb, "{\"ok\":true,\"traps\":[");
	for(int i = 0; i < 128; i++) {
		if(i > 0) jb_cat(jb, ",");
		jb_cat(jb, g_wdm_trap_enabled[i] ? "1" : "0");
	}
	jb_cat(jb, "]}");
}

static void cmd_get_symbols(Jb *jb) {
	int n = symbols_file_count();
	jb_printf(jb, "{\"ok\":true,\"count\":%d,\"files\":[", n);
	for(int i = 0; i < n; i++) {
		const char *path = "", *target = "";
		word32 symsig = 0, length = 0;
		int nsyms = 0;
		if(symbols_get_file(i, &path, &target, &symsig, &length,
		                    &nsyms) != 0) continue;
		if(i > 0) jb_cat(jb, ",");
		jb_cat(jb, "{\"path\":");      jb_str(jb, path);
		jb_cat(jb, ",\"target\":");    jb_str(jb, target);
		jb_printf(jb, ",\"symsig\":\"$%08X\"", symsig);
		jb_printf(jb, ",\"length\":%u", length);
		jb_printf(jb, ",\"n_symbols\":%d", nsyms);
		jb_cat(jb, "}");
	}
	jb_cat(jb, "]}");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Request dispatch
 * ═══════════════════════════════════════════════════════════════════════════ */
static void dispatch(const char *req, Jb *jb) {
	char cmd[64];
	jparse_str(req, "cmd", cmd, sizeof(cmd));

	if(strcmp(cmd, "get_registers") == 0) {
		cmd_get_registers(jb);

	} else if(strcmp(cmd, "read_memory") == 0) {
		word32 addr = (word32)jparse_int(req, "addr", 0);
		int    len  = (int)   jparse_int(req, "len",  256);
		cmd_read_memory(jb, addr, len);

	} else if(strcmp(cmd, "search_memory") == 0) {
		word32 start = (word32)jparse_int(req, "start", 0x000000);
		word32 end   = (word32)jparse_int(req, "end",   0x00FFFF);
		byte   pat[64];
		int    plen  = jparse_hexbytes(req, "pattern", pat, sizeof(pat));
		cmd_search_memory(jb, start, end, pat, plen);

	} else if(strcmp(cmd, "halt") == 0) {
		cmd_halt(jb);

	} else if(strcmp(cmd, "continue") == 0) {
		cmd_continue(jb);

	} else if(strcmp(cmd, "step") == 0) {
		cmd_step(jb);

	} else if(strcmp(cmd, "get_break_info") == 0) {
		cmd_get_break_info(jb);

	} else if(strcmp(cmd, "debugger_command") == 0) {
		char text[512];
		jparse_str(req, "text", text, sizeof(text));
		cmd_debugger_command(jb, text);

	} else if(strcmp(cmd, "list_volumes") == 0) {
		cmd_list_volumes(jb);

	} else if(strcmp(cmd, "list_files") == 0) {
		char slot[32], path[512];
		jparse_str(req, "slot", slot, sizeof(slot));
		jparse_str(req, "path", path, sizeof(path));
		int recursive = jparse_int(req, "recursive", 0);
		cmd_list_files(jb, slot, path[0] ? path : "/", recursive);

	} else if(strcmp(cmd, "read_file") == 0) {
		char slot[32], path[512];
		jparse_str(req, "slot", slot, sizeof(slot));
		jparse_str(req, "path", path, sizeof(path));
		cmd_read_file(jb, slot, path);

	} else if(strcmp(cmd, "read_volume") == 0) {
		char slot[32];
		jparse_str(req, "slot", slot, sizeof(slot));
		cmd_read_volume(jb, slot);

	} else if(strcmp(cmd, "read_volume_raw") == 0) {
		char slot[32];
		jparse_str(req, "slot", slot, sizeof(slot));
		cmd_read_volume_raw(jb, slot);

	} else if(strcmp(cmd, "restart") == 0) {
		cmd_restart(jb);

	} else if(strcmp(cmd, "get_log") == 0) {
		int n = (int)jparse_int(req, "lines", 50);
		cmd_get_log(jb, n);

	} else if(strcmp(cmd, "set_wdm_trap") == 0) {
		cmd_set_wdm_trap(jb, req);

	} else if(strcmp(cmd, "get_wdm_traps") == 0) {
		cmd_get_wdm_traps(jb);

	} else if(strcmp(cmd, "get_symbols") == 0) {
		cmd_get_symbols(jb);

	} else if(strcmp(cmd, "send_keys") == 0) {
		char keys[4096];
		jparse_str(req, "keys", keys, sizeof(keys));
		cmd_send_keys(jb, keys);

	} else {
		jb_printf(jb, "{\"ok\":false,\"error\":\"unknown command: %s\"}", cmd);
	}
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Background server thread
 * ═══════════════════════════════════════════════════════════════════════════ */
static void *server_thread_fn(void *arg) {
	(void)arg;

	int srv = socket(AF_UNIX, SOCK_STREAM, 0);
	if(srv < 0) {
		fprintf(stderr, "debug_server: socket: %s\n", strerror(errno));
		return NULL;
	}
	unlink(SOCK_PATH);

	struct sockaddr_un sa;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	strncpy(sa.sun_path, SOCK_PATH, sizeof(sa.sun_path) - 1);

	if(bind(srv, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		fprintf(stderr, "debug_server: bind: %s\n", strerror(errno));
		close(srv); return NULL;
	}
	chmod(SOCK_PATH, 0600);
	listen(srv, 8);
	printf("debug_server: listening on %s\n", SOCK_PATH);
	fflush(stdout);

	char *req_buf = (char *)malloc(MAX_REQ_SIZE);
	if(!req_buf) { close(srv); return NULL; }

	while(1) {
		int cli = accept(srv, NULL, NULL);
		if(cli < 0) continue;

		/* Read newline-terminated request */
		int rlen = 0;
		while(rlen < MAX_REQ_SIZE - 1) {
			char c;
			if(recv(cli, &c, 1, 0) <= 0) break;
			if(c == '\n') break;
			req_buf[rlen++] = c;
		}
		req_buf[rlen] = 0;

		long req_id = jparse_int(req_buf, "id", -1);

		Jb jb;
		jb_init(&jb);
		dispatch(req_buf, &jb);

		/* Inject the request id back into the response */
		if(req_id >= 0 && jb.len > 1) {
			char id_frag[32];
			int id_len = snprintf(id_frag, sizeof(id_frag), "\"id\":%ld,", req_id);
			jb_grow(&jb, id_len + 2);
			memmove(jb.buf + 1 + id_len, jb.buf + 1, jb.len);
			memcpy(jb.buf + 1, id_frag, id_len);
			jb.len += id_len;
		}

		/* Append newline and send */
		jb_grow(&jb, 2);
		jb.buf[jb.len++] = '\n';
		jb.buf[jb.len]   = 0;
		send(cli, jb.buf, jb.len, 0);
		free(jb.buf);
		close(cli);
	}
	free(req_buf);
	close(srv);
	return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API called from the emulator
 * ═══════════════════════════════════════════════════════════════════════════ */

void debug_server_init(void) {
	memset(&g_break_state, 0, sizeof(g_break_state));
	memset(&g_main_cmd,    0, sizeof(g_main_cmd));
	g_main_cmd_pending = 0;

	pthread_t tid;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if(pthread_create(&tid, &attr, server_thread_fn, NULL) != 0) {
		fprintf(stderr, "debug_server: pthread_create: %s\n", strerror(errno));
	}
	pthread_attr_destroy(&attr);
}

void debug_server_poll(void) {
	/* Called from the emulator's 16ms loop (main thread).
	 * Handles pending debugger_command requests (with captured output) and
	 * restart requests. Both must run on the main thread. */
	pthread_mutex_lock(&g_main_mutex);
	int has_cmd     = g_main_cmd_pending;
	int do_restart  = g_restart_pending;
	g_restart_pending = 0;
	if(!has_cmd && !do_restart) {
		pthread_mutex_unlock(&g_main_mutex);
		return;
	}
	char cmd_copy[512] = {0};
	if(has_cmd) {
		strncpy(cmd_copy, g_main_cmd.cmd, sizeof(cmd_copy) - 1);
	}
	pthread_mutex_unlock(&g_main_mutex);

	/* Process restart immediately (before debugger_command) */
	if(do_restart) {
		do_reset();
	}

	if(has_cmd) {
		/* Arm the dbg_printf capture buffer */
		int cap_sz = 65536;
		g_dbg_capture = (char *)malloc(cap_sz);
		if(g_dbg_capture) {
			g_dbg_capture[0]   = 0;
			g_dbg_capture_len  = 0;
			g_dbg_capture_cap  = cap_sz;
		}

		do_debug_cmd(cmd_copy);

		/* Disarm and hand off result */
		char *result      = g_dbg_capture;
		g_dbg_capture     = NULL;
		g_dbg_capture_len = 0;
		g_dbg_capture_cap = 0;

		pthread_mutex_lock(&g_main_mutex);
		g_main_cmd.resp = result;
		g_main_cmd.done = 1;
		pthread_cond_signal(&g_main_done);
		pthread_mutex_unlock(&g_main_mutex);
	}
}

void debug_server_notify_break(word32 pc, word32 acc, word32 xreg, word32 yreg,
                                word32 stack, word32 direct, word32 dbank,
                                word32 psr, int reason) {
	pthread_mutex_lock(&g_break_mutex);
	g_break_state.valid  = 1;
	g_break_state.pc     = pc;
	g_break_state.acc    = acc;
	g_break_state.xreg   = xreg;
	g_break_state.yreg   = yreg;
	g_break_state.stack  = stack;
	g_break_state.direct = direct;
	g_break_state.dbank  = dbank;
	g_break_state.psr    = psr;
	g_break_state.reason = reason;
	pthread_mutex_unlock(&g_break_mutex);
}
