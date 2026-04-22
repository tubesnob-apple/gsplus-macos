/* symbols.c - host-side symbol map for relocatable IIGS segments.
 *
 * Protocol (toolchain side):
 *   The ORCA linker, when built with gsplusSymbols=1, injects an 8-byte
 *   prologue at offset 0 of segment 1 of each linked binary:
 *     +0  $42 $0F   WDM $0F
 *     +2  $80 $04   BRA +4
 *     +4  <sig>     32-bit link signature, little-endian
 *   and writes a <target>.symbols JSON file whose top-level "symsig"
 *   field matches the signature embedded at offsets 4-7.
 *
 * Host side:
 *   symbols_rescan() walks g_cfg_symbols_path, parses every *.symbols
 *   file, and indexes them by symsig. On WDM $0F, the CPU handler reads
 *   the signature at PC+2 and calls symbols_register_from_sig() with the
 *   load base (the WDM instruction's address) to bind an indexed symbol
 *   table to runtime memory.
 *
 *   The index is rebuilt on every emulator reset/restart so dropping a
 *   new .symbols file into the configured directory takes effect on the
 *   next reset, without having to relaunch the host.
 */

#include "defc.h"
#include "symbols.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

extern char *g_cfg_symbols_path;
extern char *g_argv0_path;
extern word32 g_c035_shadow_reg;
extern word32 g_c068_statereg;

typedef struct {
	word32  rel_offset;
	char   *name;
	/* Pre-ExpressLoad-remap OMF segment number (1..N) this symbol
	 * belongs to. 0 means "not declared" — legacy files that didn't
	 * emit a segment key get matched against any binding, preserving
	 * the original behavior. Binding-side filtering (seg_map_t.seg_num)
	 * only kicks in for scanner-bound segments where we know the
	 * segNum from the runtime footer. */
	uint8_t segment;
} sym_entry_t;

/* One declared visibility range. A file may declare more than one — e.g.
 * iigs.symbols uses separate ranges for bank $00 I/O ($C000-$CFFF), the
 * bank-$00 ROM mirror ($E000-$FFFF), bank $E0, bank $E1, etc.
 *
 * Softswitch predicates: a segment can gate its visibility on the live
 * values of $C035 (shadow register) and $C068 (IIgs state register). The
 * segment is considered "active" only when, for every predicate set,
 *   (live_reg & mask) == value
 * A mask of 0 means the predicate is unused — the default — so existing
 * .symbols files without predicates always match.
 *
 *   shadow_mask/value: typically used by bank-$00/$01 duplicates of
 *   hardware symbols. The hardware only gates through to these aliases
 *   when the corresponding shadow bit is *clear* (1 = inhibit shadow).
 *   Example: $00:C030 is SPKR only when $C035 bit 6 is 0.
 *
 *   statereg_mask/value: used for ROM/LC bank gating. Example: the
 *   bank-$00 monitor ROM at $E000-$FFFF is only meaningful when the LC
 *   read bit ($C068 bit 1) is clear (reads come from ROM, not LC RAM). */
typedef struct {
	word32  load_base;       /* start address of the range */
	word32  length;          /* bytes covered */
	int     has_load_base;   /* 0 → segment inherits WDM-provided base */
	word32  shadow_mask;
	word32  shadow_value;
	word32  statereg_mask;
	word32  statereg_value;
} sym_segment_t;

/* One parsed .symbols file. Owns its path/target/entries memory. */
typedef struct {
	char           *path;      /* absolute path to the .symbols file */
	word32          symsig;
	char           *target;    /* from JSON "target" field; display name */
	sym_entry_t    *entries;   /* sorted by rel_offset */
	int             n_entries;
	int             has_load_base;  /* non-zero if load_base is set */
	word32          load_base;      /* file-level load_base; the anchor
	                                 * against which symbol offsets are
	                                 * measured. Used both as the implicit
	                                 * base for segments that don't set
	                                 * their own load_base, and as the
	                                 * file_base stored on each binding. */
	word32          max_offset;     /* cap on symbol+offset display distance.
	                                 * 0 = unlimited. Sparse files set this
	                                 * so a distant PC doesn't latch onto a
	                                 * random neighboring symbol. */
	sym_segment_t  *segments;       /* visibility ranges */
	int             n_segments;
} sym_file_t;

/* One binding: a sym_file_t range placed at a runtime address. A single
 * file can produce multiple bindings when it declares multiple segments.
 * Bindings point into g_index; they don't own their symbols.
 *
 * Softswitch predicates are copied from the source segment so the lookup
 * path doesn't have to follow the file pointer to find them. */
typedef struct {
	const sym_file_t *file;
	word32            load_base;   /* start of this binding in memory */
	word32            length;      /* extent */
	word32            file_base;   /* anchor for rel = pc - file_base
	                                * (so symbol offsets stay stable even
	                                * when multiple bindings place the same
	                                * file at different runtime addresses) */
	word32            shadow_mask;
	word32            shadow_value;
	word32            statereg_mask;
	word32            statereg_value;
	uint8_t           seg_num;     /* OMF pre-remap segment number (1..N)
	                                * for scanner-installed bindings; 0 =
	                                * no filter (legacy iigs.symbols and
	                                * WDM-$0F-registered files). When
	                                * nonzero, only symbols with the
	                                * matching segment field are eligible
	                                * for a hit inside this binding. */
	uint8_t           from_scan;   /* 1 = installed by symbols_scan_and_bind,
	                                * so the next scan can atomically drop
	                                * and replace it. */
} seg_map_t;

static sym_file_t *g_index       = NULL;
static int         g_n_index     = 0;
static int         g_index_cap   = 0;

static seg_map_t  *g_segments    = NULL;
static int         g_n_segments  = 0;
static int         g_seg_cap     = 0;

/* ---- minimal JSON helpers ---- */

static const char *
json_skip_ws(const char *p)
{
	while(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	return p;
}

static const char *
json_parse_str(const char *p, char *out, int maxlen)
{
	int i = 0;
	if(*p != '"') return NULL;
	p++;
	while(*p && *p != '"') {
		if(*p == '\\') {
			p++;
			if(!*p) return NULL;
		}
		if(i < maxlen - 1) out[i++] = *p;
		p++;
	}
	out[i] = '\0';
	if(*p == '"') p++;
	return p;
}

static const char *
json_skip_value(const char *p)
{
	p = json_skip_ws(p);
	if(*p == '"') {
		p++;
		while(*p && *p != '"') {
			if(*p == '\\') p++;
			if(*p) p++;
		}
		if(*p == '"') p++;
		return p;
	}
	if(*p == '{' || *p == '[') {
		char open = *p, close = (open == '{') ? '}' : ']';
		int depth = 1;
		p++;
		while(*p && depth > 0) {
			if(*p == '"') {
				p++;
				while(*p && *p != '"') {
					if(*p == '\\') p++;
					if(*p) p++;
				}
				if(*p) p++;
			} else {
				if(*p == open) depth++;
				else if(*p == close) depth--;
				p++;
			}
		}
		return p;
	}
	while(*p && *p != ',' && *p != '}' && *p != ']'
	       && *p != ' ' && *p != '\t')
		p++;
	return p;
}

static const char *
json_find_key(const char *p, const char *key)
{
	p = json_skip_ws(p);
	if(*p == '{') p++;

	while(*p) {
		char kbuf[256];

		p = json_skip_ws(p);
		if(*p == '}') return NULL;
		if(*p == ',') { p++; continue; }

		p = json_parse_str(p, kbuf, sizeof(kbuf));
		if(!p) return NULL;

		p = json_skip_ws(p);
		if(*p != ':') return NULL;
		p++;
		p = json_skip_ws(p);

		if(strcmp(kbuf, key) == 0)
			return p;

		p = json_skip_value(p);
		if(!p) return NULL;
	}
	return NULL;
}

static word32
json_parse_hex(const char *s)
{
	if(s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	return (word32)strtoul(s, NULL, 16);
}

/* ---- state teardown ---- */

static int
sym_cmp(const void *a, const void *b)
{
	const sym_entry_t *ea = a;
	const sym_entry_t *eb = b;
	if(ea->rel_offset < eb->rel_offset) return -1;
	if(ea->rel_offset > eb->rel_offset) return  1;
	return 0;
}

static void
free_sym_file(sym_file_t *f)
{
	int i;
	if(!f) return;
	for(i = 0; i < f->n_entries; i++) {
		free(f->entries[i].name);
	}
	free(f->entries);
	free(f->segments);
	free(f->target);
	free(f->path);
	memset(f, 0, sizeof(*f));
}

static void
clear_index(void)
{
	int i;
	for(i = 0; i < g_n_index; i++) {
		free_sym_file(&g_index[i]);
	}
	free(g_index);
	g_index = NULL;
	g_n_index = 0;
	g_index_cap = 0;
}

static void
clear_bindings(void)
{
	/* seg_map_t entries don't own any memory; they reference g_index. */
	free(g_segments);
	g_segments = NULL;
	g_n_segments = 0;
	g_seg_cap = 0;
}

/* ---- file I/O ---- */

static char *
read_file(const char *path)
{
	FILE *fp;
	long  len;
	char *buf;

	fp = fopen(path, "r");
	if(!fp) return NULL;

	fseek(fp, 0, SEEK_END);
	len = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	if(len <= 0 || len > 4 * 1024 * 1024) {
		fclose(fp);
		return NULL;
	}

	buf = malloc(len + 1);
	if(!buf) { fclose(fp); return NULL; }

	fread(buf, 1, len, fp);
	buf[len] = '\0';
	fclose(fp);
	return buf;
}

/* ---- parse one .symbols file into a sym_file_t ---- */

static int
parse_symbols_file(const char *path, sym_file_t *out)
{
	char *json;
	const char *start, *v, *segs, *syms, *p;
	char ver[32] = "", sigstr[32] = "", target[256] = "";
	word32 seg_length = 0;
	int seg_count = 0;
	sym_entry_t *arr = NULL;
	int cap = 0, n = 0;

	memset(out, 0, sizeof(*out));

	json = read_file(path);
	if(!json) return 0;

	start = json_skip_ws(json);
	if(*start != '{') { free(json); return 0; }

	v = json_find_key(json, "orca_symbols_version");
	if(!v) { free(json); return 0; }
	json_parse_str(v, ver, sizeof(ver));
	if(json_parse_hex(ver) != 1) {
		dbg_printf("symbols: %s: unknown version %s\n", path, ver);
		free(json);
		return 0;
	}

	v = json_find_key(json, "symsig");
	if(!v) { free(json); return 0; }
	json_parse_str(v, sigstr, sizeof(sigstr));
	out->symsig = json_parse_hex(sigstr);

	v = json_find_key(json, "target");
	if(v) json_parse_str(v, target, sizeof(target));

	/* Optional load_base: if present, the file is auto-bound at this
	 * 24-bit address during symbols_rescan(). Intended for shipped
	 * files like iigs.symbols whose addresses are fixed in hardware. */
	v = json_find_key(json, "load_base");
	if(v) {
		char lbstr[32] = "";
		json_parse_str(v, lbstr, sizeof(lbstr));
		out->has_load_base = 1;
		out->load_base     = json_parse_hex(lbstr) & 0xffffff;
	}

	/* Optional max_offset: caps the symbol+offset match distance. */
	v = json_find_key(json, "max_offset");
	if(v) {
		char mostr[32] = "";
		json_parse_str(v, mostr, sizeof(mostr));
		out->max_offset = json_parse_hex(mostr);
	}

	/* Parse the full segments array. ORCA-linked programs have a single
	 * segment with just a length field (relocated via WDM $0F). Files like
	 * iigs.symbols declare multiple segments, each with its own load_base
	 * for absolute binding. Ignore the segment "number" field (per spec,
	 * pass-1 numbering can differ from pass-2 by one). */
	{
		sym_segment_t *segs_arr = NULL;
		int segs_cap = 0, segs_n = 0;
		segs = json_find_key(json, "segments");
		if(segs && *segs == '[') {
			p = segs + 1;
			while(*p) {
				char lenstr[32] = "", lbstr[32] = "";
				const char *obj_start, *lv;
				int has_lb = 0;

				p = json_skip_ws(p);
				if(*p == ']') break;
				if(*p == ',') { p++; continue; }
				if(*p != '{') break;

				obj_start = p;
				seg_count++;

				lv = json_find_key(obj_start, "length");
				if(lv) json_parse_str(lv, lenstr, sizeof(lenstr));

				lv = json_find_key(obj_start, "load_base");
				if(lv) {
					json_parse_str(lv, lbstr, sizeof(lbstr));
					has_lb = 1;
				}

				if(segs_n == segs_cap) {
					int newcap = segs_cap ? segs_cap * 2 : 4;
					sym_segment_t *np = realloc(segs_arr,
					    newcap * sizeof(*np));
					if(!np) break;
					segs_arr = np;
					segs_cap = newcap;
				}
				segs_arr[segs_n].load_base     = has_lb ?
				    (json_parse_hex(lbstr) & 0xffffff) : 0;
				segs_arr[segs_n].length        = json_parse_hex(lenstr);
				segs_arr[segs_n].has_load_base = has_lb;
				segs_arr[segs_n].shadow_mask     = 0;
				segs_arr[segs_n].shadow_value    = 0;
				segs_arr[segs_n].statereg_mask   = 0;
				segs_arr[segs_n].statereg_value  = 0;

				/* Optional softswitch predicates. Both mask+value
				 * pairs must be present for the predicate to apply;
				 * mask=0 disables the predicate. */
				{
					char tmp[32] = "";
					const char *kv;
					kv = json_find_key(obj_start, "shadow_mask");
					if(kv) {
						json_parse_str(kv, tmp, sizeof(tmp));
						segs_arr[segs_n].shadow_mask =
						    json_parse_hex(tmp) & 0xff;
					}
					kv = json_find_key(obj_start, "shadow_value");
					if(kv) {
						json_parse_str(kv, tmp, sizeof(tmp));
						segs_arr[segs_n].shadow_value =
						    json_parse_hex(tmp) & 0xff;
					}
					kv = json_find_key(obj_start, "statereg_mask");
					if(kv) {
						json_parse_str(kv, tmp, sizeof(tmp));
						segs_arr[segs_n].statereg_mask =
						    json_parse_hex(tmp) & 0xff;
					}
					kv = json_find_key(obj_start, "statereg_value");
					if(kv) {
						json_parse_str(kv, tmp, sizeof(tmp));
						segs_arr[segs_n].statereg_value =
						    json_parse_hex(tmp) & 0xff;
					}
				}
				segs_n++;

				if(seg_count == 1) seg_length = json_parse_hex(lenstr);

				p = json_skip_value(obj_start);
				if(!p) break;
			}
		}
		out->segments   = segs_arr;
		out->n_segments = segs_n;
	}
	(void)seg_length;  /* length of segment 0 is now stored in segments[0] */

	/* Collect every symbol. The pass-1 "segment" number on each symbol
	 * is what the scanner footer reports (pre-ExpressLoad-remap); we
	 * store it so per-segment bindings can filter by it. Legacy files
	 * without the key get segment=0 which means "no filter". */
	syms = json_find_key(json, "symbols");
	if(syms && *syms == '[') {
		p = syms + 1;
		while(*p) {
			char sname[256] = "";
			char offstr[32] = "";
			char segstr[32] = "";
			const char *obj_start, *sv;

			p = json_skip_ws(p);
			if(*p == ']') break;
			if(*p == ',') { p++; continue; }
			if(*p != '{') break;

			obj_start = p;

			sv = json_find_key(obj_start, "name");
			if(sv) json_parse_str(sv, sname, sizeof(sname));

			sv = json_find_key(obj_start, "offset");
			if(sv) json_parse_str(sv, offstr, sizeof(offstr));

			sv = json_find_key(obj_start, "segment");
			if(sv) {
				/* Accept both bare decimal ("2") and quoted
				 * hex-ish forms. json_parse_str handles the
				 * quoted case; bare numbers need a raw read. */
				if(*sv == '"') {
					json_parse_str(sv, segstr, sizeof(segstr));
				} else {
					int si = 0;
					while(*sv && *sv != ',' && *sv != '}'
					       && *sv != ' ' && *sv != '\t'
					       && *sv != '\n' && *sv != '\r'
					       && si < (int)sizeof(segstr) - 1) {
						segstr[si++] = *sv++;
					}
					segstr[si] = '\0';
				}
			}

			if(n == cap) {
				int newcap = cap ? cap * 2 : 64;
				sym_entry_t *np = realloc(arr,
				    newcap * sizeof(*np));
				if(!np) {
					int k;
					for(k = 0; k < n; k++) free(arr[k].name);
					free(arr);
					free(json);
					return 0;
				}
				arr = np;
				cap = newcap;
			}
			arr[n].rel_offset = json_parse_hex(offstr);
			arr[n].name       = strdup(sname);
			/* segment is a small decimal number in the linker's
			 * JSON (1..N). Use base-10 strtoul; 0x-prefixed values
			 * fall through to base-16 for robustness. */
			if(segstr[0]) {
				const char *sp = segstr;
				int base = 10;
				if(sp[0] == '0' && (sp[1] == 'x' || sp[1] == 'X')) {
					sp += 2;
					base = 16;
				}
				arr[n].segment = (uint8_t)
				    (strtoul(sp, NULL, base) & 0xff);
			} else {
				arr[n].segment = 0;
			}
			n++;

			p = json_skip_value(obj_start);
			if(!p) break;
		}
	}

	if(n > 1) qsort(arr, n, sizeof(*arr), sym_cmp);

	out->path      = strdup(path);
	out->target    = strdup(target);
	out->entries   = arr;
	out->n_entries = n;

	free(json);
	return 1;
}

/* ---- index rebuild ---- */

#define SYM_MAX_RECURSE_DEPTH 8

static int
entry_is_dir(const char *path, const struct dirent *ent)
{
#ifdef DT_DIR
	if(ent->d_type == DT_DIR) return 1;
	if(ent->d_type == DT_REG || ent->d_type == DT_LNK) {
		/* DT_LNK: fall through so we can stat() and follow symlinks
		 * to directories. */
		if(ent->d_type == DT_REG) return 0;
	}
#endif
	{
		struct stat st;
		if(stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 1;
	}
	return 0;
}

static void
scan_dir(const char *dirpath, int depth)
{
	DIR *dir;
	struct dirent *ent;

	if(depth > SYM_MAX_RECURSE_DEPTH) return;

	dir = opendir(dirpath);
	if(!dir) {
		if(depth == 0) {
			dbg_printf("symbols: cannot open dir %s\n", dirpath);
		}
		return;
	}

	while((ent = readdir(dir)) != NULL) {
		const char *dot;
		char path[1024];
		sym_file_t f;

		if(ent->d_name[0] == '.') continue;  /* skip . .. hidden */

		snprintf(path, sizeof(path), "%s/%s", dirpath, ent->d_name);

		if(entry_is_dir(path, ent)) {
			scan_dir(path, depth + 1);
			continue;
		}

		dot = strrchr(ent->d_name, '.');
		if(!dot || strcmp(dot, ".symbols") != 0) continue;

		if(!parse_symbols_file(path, &f)) continue;

		if(g_n_index == g_index_cap) {
			int newcap = g_index_cap ? g_index_cap * 2 : 8;
			sym_file_t *np = realloc(g_index,
			    newcap * sizeof(*np));
			if(!np) { free_sym_file(&f); closedir(dir); return; }
			g_index = np;
			g_index_cap = newcap;
		}
		g_index[g_n_index++] = f;
	}
	closedir(dir);
}

/* Scan a host path that ships symbol files alongside the binary. Tried
 * unconditionally so the default iigs.symbols loads whether or not the
 * user has set Symbols Path. Silent failure if the path doesn't exist.
 *
 * On macOS, parse_argv is called with slashes_to_find=3 so g_argv0_path
 * points to the .app bundle root and the resources live inside
 * Contents/Resources. On X11/Windows builds, parse_argv uses
 * slashes_to_find=1 so g_argv0_path already IS the executable's
 * directory, which is where shipped files live. */
static void
scan_system_paths(void)
{
	char buf[1024];

	if(!g_argv0_path || !*g_argv0_path) return;

#ifdef MAC
	snprintf(buf, sizeof(buf), "%s/Contents/Resources", g_argv0_path);
#else
	snprintf(buf, sizeof(buf), "%s", g_argv0_path);
#endif
	scan_dir(buf, 0);
}

/* Bind every indexed file that has an explicit load_base to that address.
 * Used for files like iigs.symbols whose symbols live at fixed hardware
 * addresses and don't need a WDM $0F prologue to establish relocation. */
static void
autobind_fixed_files(void)
{
	int i;
	for(i = 0; i < g_n_index; i++) {
		if(g_index[i].has_load_base) {
			symbols_register_from_sig(g_index[i].load_base,
			                          g_index[i].symsig);
		}
	}
}

void
symbols_rescan(void)
{
	clear_bindings();
	clear_index();

	scan_system_paths();

	if(g_cfg_symbols_path && *g_cfg_symbols_path) {
		scan_dir(g_cfg_symbols_path, 0);
	}

	autobind_fixed_files();

	dbg_printf("symbols: indexed %d .symbols file(s)\n", g_n_index);
}

/* ---- WDM $0F handler ---- */

/* Append one binding to g_segments. Returns 0 on OOM. Predicate pointer
 * may be NULL (no constraints). */
static int
append_seg_map(const sym_file_t *file, word32 load_base,
               word32 length, word32 file_base,
               const sym_segment_t *seg)
{
	seg_map_t *b;
	if(g_n_segments == g_seg_cap) {
		int newcap = g_seg_cap ? g_seg_cap * 2 : 16;
		seg_map_t *p = realloc(g_segments, newcap * sizeof(*p));
		if(!p) return 0;
		g_segments = p;
		g_seg_cap = newcap;
	}
	b = &g_segments[g_n_segments++];
	b->file           = file;
	b->load_base      = load_base & 0xffffff;
	b->length         = length;
	b->file_base      = file_base & 0xffffff;
	b->shadow_mask    = seg ? seg->shadow_mask    : 0;
	b->shadow_value   = seg ? seg->shadow_value   : 0;
	b->statereg_mask  = seg ? seg->statereg_mask  : 0;
	b->statereg_value = seg ? seg->statereg_value : 0;
	b->seg_num        = 0;
	b->from_scan      = 0;
	return 1;
}

/* Drop every existing binding for the given file. Used before rebinding so
 * a re-fired WDM $0F or a re-triggered autobind doesn't leave stale
 * entries for earlier load addresses. */
static void
drop_bindings_for(const sym_file_t *file)
{
	int src, dst = 0;
	for(src = 0; src < g_n_segments; src++) {
		if(g_segments[src].file != file) {
			if(dst != src) g_segments[dst] = g_segments[src];
			dst++;
		}
	}
	g_n_segments = dst;
}

void
symbols_register_from_sig(word32 load_base, word32 symsig)
{
	sym_file_t *match = NULL;
	int i;
	int file_has_abs_seg = 0;

	load_base &= 0xffffff;

	for(i = 0; i < g_n_index; i++) {
		if(g_index[i].symsig == symsig) {
			match = &g_index[i];
			break;
		}
	}

	if(!match) {
		dbg_printf("symbols: no .symbols file for sig $%08x "
		           "(base %06x)\n", symsig, load_base);
		return;
	}

	for(i = 0; i < match->n_segments; i++) {
		if(match->segments[i].has_load_base) {
			file_has_abs_seg = 1;
			break;
		}
	}

	drop_bindings_for(match);

	if(file_has_abs_seg) {
		/* File declares its own per-segment addresses (iigs.symbols
		 * style). Each segment becomes its own binding. file_base is
		 * the file-level load_base (anchor for symbol offsets —
		 * typically 0 for files with absolute offsets). */
		word32 anchor = match->has_load_base ? match->load_base : 0;
		for(i = 0; i < match->n_segments; i++) {
			const sym_segment_t *seg = &match->segments[i];
			if(!seg->has_load_base) continue;
			append_seg_map(match, seg->load_base, seg->length,
			               anchor, seg);
		}
		dbg_printf("symbols: bound %s (sig $%08x) across %d segment(s)"
		           " (%d syms)\n",
		           match->target ? match->target : "",
		           symsig, match->n_segments, match->n_entries);
	} else {
		/* Single-segment ORCA-linked file: one binding at the
		 * relocated load_base. file_base == load_base so symbol
		 * offsets stay relative to the segment start. */
		word32 len = match->n_segments > 0
		    ? match->segments[0].length : 0;
		const sym_segment_t *seg0 = match->n_segments > 0
		    ? &match->segments[0] : NULL;
		append_seg_map(match, load_base, len, load_base, seg0);
		dbg_printf("symbols: bound %s (sig $%08x) @ %06x len=%06x "
		           "(%d syms)\n",
		           match->target ? match->target : "",
		           symsig, load_base, len, match->n_entries);
	}
}

/* ---- Footer-based scanner: scan RAM for _$GSPSYM$_ markers ----
 *
 * The linker appends a 19-byte footer to the end of every CODE segment's
 * LCONST (with -DgsplusSymbols=1):
 *
 *   off 0..9    "_$GSPSYM$_"          ASCII magic (10 bytes)
 *   off 10..13  sfSig                 LE uint32 (matches .symbols symsig)
 *   off 14..17  length                LE uint32 (total LCONST incl. footer)
 *   off 18      segNum                pre-ExpressLoad-remap segment #
 *
 *   seg_base = (magic_addr + 19) - length
 *
 * The scanner walks fast RAM (g_memory_ptr, 0..g_mem_size_total-1 = banks
 * starting from $00) and slow RAM (g_slow_memory_ptr, banks $E0/$E1), finds
 * every magic, sanity-checks the footer, and installs one binding per
 * valid footer via symbols_register_segment(). Scanner-installed bindings
 * are flagged from_scan=1 so the next scan atomically replaces them. */

static const char SCAN_MAGIC[] = "_$GSPSYM$_";
#define SCAN_MAGIC_LEN  10
#define SCAN_FOOTER_LEN 19

/* Extern declarations for the host-side RAM pointers. */
extern byte   *g_memory_ptr;
extern byte   *g_slow_memory_ptr;
extern word32  g_mem_size_total;

/* Drop every binding installed by an earlier scan. Files registered via
 * the legacy WDM-$0F path or the auto-bind-fixed-files path for
 * iigs.symbols stay put. */
static void
drop_scan_bindings(void)
{
	int src, dst = 0;
	for(src = 0; src < g_n_segments; src++) {
		if(!g_segments[src].from_scan) {
			if(dst != src) g_segments[dst] = g_segments[src];
			dst++;
		}
	}
	g_n_segments = dst;
}

/* Install one scanner-derived binding. Returns 1 on success, 0 if the
 * sfSig has no matching .symbols in g_index (nothing to bind). */
static int
register_scanned_segment(word32 sfSig, word32 load_base, word32 length,
                         uint8_t seg_num)
{
	sym_file_t *match = NULL;
	int i;
	for(i = 0; i < g_n_index; i++) {
		if(g_index[i].symsig == sfSig) {
			match = &g_index[i];
			break;
		}
	}
	if(!match) return 0;
	if(!append_seg_map(match, load_base, length, load_base, NULL))
		return 0;
	g_segments[g_n_segments - 1].seg_num   = seg_num;
	g_segments[g_n_segments - 1].from_scan = 1;
	return 1;
}

/* Sweep `buf` (length `buf_len`) looking for SCAN_MAGIC. Each match's
 * 24-bit emulator address is computed as `addr_base + (hit - buf)`.
 * Returns the number of valid footers that installed a binding. */
static int
scan_region(const byte *buf, size_t buf_len, word32 addr_base)
{
	int installed = 0;
	const byte *p   = buf;
	const byte *end = buf + buf_len;

	while(p + SCAN_FOOTER_LEN <= end) {
		const byte *hit = memmem(p, end - p,
		                         SCAN_MAGIC, SCAN_MAGIC_LEN);
		word32 sfSig, length, addr, seg_base;
		uint8_t segNum;
		if(!hit) break;

		/* Full 19-byte footer must fit within buf. If the magic
		 * lands in the last 9 bytes, skip it (shouldn't happen
		 * in practice — a real footer is always followed by its
		 * own 9-byte sfSig+length+segNum tail). */
		if(hit + SCAN_FOOTER_LEN > end) break;

		addr = addr_base + (word32)(hit - buf);
		sfSig  = (word32)hit[10]
		       | ((word32)hit[11] << 8)
		       | ((word32)hit[12] << 16)
		       | ((word32)hit[13] << 24);
		length = (word32)hit[14]
		       | ((word32)hit[15] << 8)
		       | ((word32)hit[16] << 16)
		       | ((word32)hit[17] << 24);
		segNum = hit[18];

		/* Sanity checks: footer length must be at least 26 bytes
		 * (the footer itself) and small enough to fit in one
		 * bank. segNum must be a plausible OMF segment index. */
		if(length > SCAN_FOOTER_LEN && length <= 0x10000 &&
		   segNum >= 1 && segNum <= 32) {
			seg_base = (addr + SCAN_FOOTER_LEN) - length;
			/* seg_base must lie in the same bank as the
			 * magic — segments can't span banks on the IIgs. */
			if((seg_base >> 16) == (addr >> 16)) {
				if(register_scanned_segment(sfSig, seg_base,
				                             length, segNum)) {
					installed++;
				}
			}
		}

		p = hit + 1;
	}
	return installed;
}

int
symbols_scan_and_bind(void)
{
	int installed = 0;

	drop_scan_bindings();

	if(g_memory_ptr && g_mem_size_total > 0) {
		installed += scan_region(g_memory_ptr,
		                          (size_t)g_mem_size_total,
		                          0x000000);
	}
	/* Slow RAM is 128 KB laid out as banks $E0 (first 64 KB) and
	 * $E1 (second 64 KB). A single scan covers both. */
	if(g_slow_memory_ptr) {
		installed += scan_region(g_slow_memory_ptr, 128 * 1024,
		                          0xE00000);
	}

	if(installed > 0) {
		dbg_printf("symbols: scanner bound %d segment(s)\n", installed);
	}
	return installed;
}

/* Throttled variant: returns immediately if the last scan was within
 * ~0.5s. Safe to call from every WDM trap without crushing the
 * emulator's effective cycle rate. */
int
symbols_scan_and_bind_throttled(void)
{
	static double last = -1.0;
	struct timespec ts;
	double now;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	now = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
	if(last >= 0.0 && (now - last) < 0.5) {
		return 0;
	}
	last = now;
	return symbols_scan_and_bind();
}

/* ---- PC lookup ---- */

static const sym_entry_t *
find_entry(const sym_file_t *f, word32 rel)
{
	int lo = 0, hi = f->n_entries - 1, best = -1;
	while(lo <= hi) {
		int mid = (lo + hi) >> 1;
		if(f->entries[mid].rel_offset <= rel) {
			best = mid;
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}
	if(best < 0) return NULL;
	return &f->entries[best];
}

const char *
symbols_describe_pc(word32 pc)
{
	static char buf[160];
	int i;

	pc &= 0xffffff;
	for(i = 0; i < g_n_segments; i++) {
		const seg_map_t  *s = &g_segments[i];
		const sym_file_t *f = s->file;
		const char *tgt;
		int suppress_tgt;
		if(!f) continue;
		if(pc >= s->load_base && pc < s->load_base + s->length) {
			word32 rel;
			const sym_entry_t *e;
			/* Softswitch gating: a segment can require certain bits
			 * in $C035 (shadow) or $C068 (state) to hold specific
			 * values before its symbols are considered live. E.g.
			 * the bank-$00 alias of TEXT_PAGE1 only means "text
			 * screen" while $C035 bit 0 is clear; if the app
			 * inhibits text-page shadowing, the bank-$00 address
			 * becomes ordinary RAM and the symbol must not match. */
			if(s->shadow_mask &&
			   (g_c035_shadow_reg & s->shadow_mask) != s->shadow_value) {
				continue;
			}
			if(s->statereg_mask &&
			   (g_c068_statereg & s->statereg_mask) != s->statereg_value) {
				continue;
			}
			/* Offset within the file's symbol table is measured
			 * against file_base, not the binding's load_base — that
			 * lets one file declare multiple segments at different
			 * runtime addresses while sharing one symbol table with
			 * absolute offsets (iigs.symbols style). */
			rel = (pc - s->file_base) & 0xffffff;
			e = find_entry(f, rel);
			/* Sparse-coverage guard: if the nearest symbol is farther
			 * than the file's max_offset cap, reject the hit so we
			 * don't advertise a random "IWM_Q7H+$1c4028" for a PC
			 * that's nowhere near any real iigs symbol. */
			if(e && f->max_offset != 0 &&
			   (rel - e->rel_offset) > f->max_offset) {
				continue;
			}
			/* Per-segment filter: scanner-installed bindings record
			 * the OMF pre-remap segNum from the footer, and only
			 * symbols whose "segment" field matches belong to this
			 * binding. Entries with segment=0 are legacy (no key
			 * in the JSON) and match any binding. Legacy bindings
			 * (seg_num=0) in turn match any symbol. On mismatch,
			 * skip to the next binding — another binding may cover
			 * the same PC for the right segment. */
			if(e && s->seg_num != 0 && e->segment != 0 &&
			   e->segment != s->seg_num) {
				continue;
			}
			tgt = f->target ? f->target : "";
			/* Suppress the target prefix for the built-in iigs
			 * symbol file — hardware and ROM names are standalone
			 * identifiers, and "iigs!SHADOW+$0" is just noise. User-
			 * linked ORCA programs keep the prefix so "app!main+$0"
			 * still namespaces across multiple loaded files. */
			suppress_tgt = (tgt[0] != 0 && strcmp(tgt, "iigs") == 0);
			if(e) {
				word32 off = rel - e->rel_offset;
				/* Drop "+$0" when the lookup landed exactly on
				 * a named symbol — "SHADOW" reads better than
				 * "SHADOW+$0". */
				if(suppress_tgt) {
					if(off == 0) snprintf(buf, sizeof(buf),
					                      "%s", e->name);
					else         snprintf(buf, sizeof(buf),
					                      "%s+$%x", e->name, off);
				} else {
					if(off == 0) snprintf(buf, sizeof(buf),
					                      "%s!%s", tgt, e->name);
					else         snprintf(buf, sizeof(buf),
					                      "%s!%s+$%x",
					                      tgt, e->name, off);
				}
			} else {
				if(suppress_tgt) {
					snprintf(buf, sizeof(buf), "+$%x", rel);
				} else {
					snprintf(buf, sizeof(buf), "%s+$%x",
					         tgt, rel);
				}
			}
			return buf;
		}
	}
	return NULL;
}

void
symbols_init(void)
{
	/* zero-init; symbols_rescan() gets called from do_reset() once
	 * config_init() has populated g_cfg_symbols_path. */
}

int
symbols_file_count(void)
{
	return g_n_index;
}

int
symbols_get_file(int i,
                 const char **path_out,
                 const char **target_out,
                 word32      *symsig_out,
                 word32      *length_out,
                 int         *nsyms_out)
{
	const sym_file_t *f;

	if(i < 0 || i >= g_n_index) return -1;
	f = &g_index[i];
	if(path_out)   *path_out   = f->path   ? f->path   : "";
	if(target_out) *target_out = f->target ? f->target : "";
	if(symsig_out) *symsig_out = f->symsig;
	if(length_out) *length_out = (f->n_segments > 0)
	                             ? f->segments[0].length : 0;
	if(nsyms_out)  *nsyms_out  = f->n_entries;
	return 0;
}

void
symbols_dump(void)
{
	int i;

	if(g_n_index == 0) {
		dbg_printf("symbols: no .symbols files indexed");
		if(g_cfg_symbols_path && *g_cfg_symbols_path) {
			dbg_printf(" (path: %s)\n", g_cfg_symbols_path);
		} else {
			dbg_printf(" (Symbols Path not set)\n");
		}
		return;
	}

	dbg_printf("%d .symbols file(s) indexed from %s:\n",
	           g_n_index,
	           g_cfg_symbols_path ? g_cfg_symbols_path : "");
	dbg_printf("  %-10s %-18s %-8s %6s  %s\n",
	           "symsig", "target", "length", "syms", "path");
	for(i = 0; i < g_n_index; i++) {
		const sym_file_t *f = &g_index[i];
		word32 len0 = (f->n_segments > 0) ? f->segments[0].length : 0;
		dbg_printf("  $%08x %-18s %06x  %6d  %s\n",
		           f->symsig,
		           f->target ? f->target : "",
		           len0,
		           f->n_entries,
		           f->path ? f->path : "");
	}
}
