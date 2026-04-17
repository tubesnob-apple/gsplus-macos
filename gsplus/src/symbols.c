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

extern char *g_cfg_symbols_path;

typedef struct {
	word32  rel_offset;
	char   *name;
} sym_entry_t;

/* One parsed .symbols file. Owns its path/target/entries memory. */
typedef struct {
	char        *path;      /* absolute path to the .symbols file */
	word32       symsig;
	char        *target;    /* from JSON "target" field; display name */
	word32       length;    /* from segments[0].length */
	sym_entry_t *entries;   /* sorted by rel_offset */
	int          n_entries;
} sym_file_t;

/* A loaded segment, bound to a load_base. Points into g_index; does not
 * own its symbols. */
typedef struct {
	const sym_file_t *file;
	word32            load_base;
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

	/* Take the first segment's length as the bound length. Most ORCA
	 * programs link to a single segment (see spec). */
	segs = json_find_key(json, "segments");
	if(segs && *segs == '[') {
		p = segs + 1;
		while(*p) {
			char lenstr[32] = "";
			const char *obj_start, *lv;

			p = json_skip_ws(p);
			if(*p == ']') break;
			if(*p == ',') { p++; continue; }
			if(*p != '{') break;

			obj_start = p;
			seg_count++;

			if(seg_count == 1) {
				lv = json_find_key(obj_start, "length");
				if(lv) json_parse_str(lv, lenstr, sizeof(lenstr));
				seg_length = json_parse_hex(lenstr);
			}

			p = json_skip_value(obj_start);
			if(!p) break;
		}
	}

	/* Collect every symbol. Per spec, the pass-1 segment number in the
	 * symbol entries may differ from the pass-2 segments[].number by one
	 * (express-load quirk); for address resolution we treat all symbols
	 * as belonging to the single output segment. */
	syms = json_find_key(json, "symbols");
	if(syms && *syms == '[') {
		p = syms + 1;
		while(*p) {
			char sname[256] = "";
			char offstr[32] = "";
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
			n++;

			p = json_skip_value(obj_start);
			if(!p) break;
		}
	}

	if(n > 1) qsort(arr, n, sizeof(*arr), sym_cmp);

	out->path      = strdup(path);
	out->target    = strdup(target);
	out->length    = seg_length;
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

void
symbols_rescan(void)
{
	clear_bindings();
	clear_index();

	if(!g_cfg_symbols_path || !*g_cfg_symbols_path)
		return;

	scan_dir(g_cfg_symbols_path, 0);

	dbg_printf("symbols: indexed %d .symbols file(s) from %s\n",
	           g_n_index, g_cfg_symbols_path);
}

/* ---- WDM $0F handler ---- */

void
symbols_register_from_sig(word32 load_base, word32 symsig)
{
	const sym_file_t *match = NULL;
	int i;

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

	/* If this symsig is already bound, rebind it in place. */
	for(i = 0; i < g_n_segments; i++) {
		if(g_segments[i].file && g_segments[i].file->symsig == symsig) {
			g_segments[i].load_base = load_base;
			dbg_printf("symbols: rebound %s (sig $%08x) to %06x\n",
			           match->target ? match->target : "",
			           symsig, load_base);
			return;
		}
	}

	if(g_n_segments == g_seg_cap) {
		int newcap = g_seg_cap ? g_seg_cap * 2 : 16;
		seg_map_t *p = realloc(g_segments, newcap * sizeof(*p));
		if(!p) return;
		g_segments = p;
		g_seg_cap = newcap;
	}
	g_segments[g_n_segments].file = match;
	g_segments[g_n_segments].load_base = load_base;
	g_n_segments++;

	dbg_printf("symbols: bound %s (sig $%08x) @ %06x len=%06x (%d syms)\n",
	           match->target ? match->target : "",
	           symsig, load_base, match->length, match->n_entries);
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
		if(!f) continue;
		if(pc >= s->load_base && pc < s->load_base + f->length) {
			word32 rel = pc - s->load_base;
			const sym_entry_t *e = find_entry(f, rel);
			const char *tgt = f->target ? f->target : "";
			if(e) {
				snprintf(buf, sizeof(buf), "%s!%s+$%x",
				         tgt, e->name, rel - e->rel_offset);
			} else {
				snprintf(buf, sizeof(buf), "%s+$%x",
				         tgt, rel);
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
	if(length_out) *length_out = f->length;
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
		dbg_printf("  $%08x %-18s %06x  %6d  %s\n",
		           f->symsig,
		           f->target ? f->target : "",
		           f->length,
		           f->n_entries,
		           f->path ? f->path : "");
	}
}
