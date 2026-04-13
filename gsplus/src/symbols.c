/* symbols.c - host-side symbol map for relocatable IIGS segments.
 *
 * Beacon protocol (toolchain side):
 *   At segment entry, before any other code, push the long address of
 *   a static descriptor and execute WDM $0F:
 *
 *       pea  seg_header>>16
 *       pea  seg_header
 *       wdm  $0f
 *       pla
 *       pla
 *
 *   The descriptor lives in the segment's data area:
 *
 *       seg_header:
 *           .word  $5347                  ; magic 'GS'
 *           .word  1                      ; version
 *           .word  seg_header - seg_start ; offset of this struct
 *           .long  seg_length             ; 24-bit length, low-3-bytes used
 *           .byte  name_len
 *           .byte  name[name_len]         ; not NUL-terminated
 *
 *   The C side computes load_base = desc_abs - desc_offset, then loads
 *   <g_cfg_symbols_path>/<name>.symbols (text format, one entry per line):
 *
 *       # any comment
 *       F 0000003a 00000018 init_heap
 *       V 00001200 00000080 g_state
 *       L 0000004f          .L_loop
 *
 *   Columns: kind (F/V/L), hex rel_offset, hex size or "-", name.
 */

#include "defc.h"
#include "symbols.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char *g_cfg_symbols_path;

#define SYM_MAGIC       0x5347
#define SYM_VERSION     1
#define MAX_NAME        63

typedef struct {
    word32  rel_offset;
    word32  size;       /* 0 if unknown */
    char   *name;       /* malloc'd */
    char    kind;       /* 'F', 'V', 'L' */
} sym_entry_t;

typedef struct {
    char        *seg_name;
    word32       load_base;
    word32       length;
    sym_entry_t *entries;
    int          n_entries;
} seg_map_t;

static seg_map_t *g_segments = NULL;
static int        g_n_segments = 0;
static int        g_seg_capacity = 0;

void
symbols_init(void)
{
    /* nothing for now */
}

/* ---- emulated-memory readers (bank-0 stack lives in slow memory) ---- */

static word32
read_emu8(word32 addr)
{
    return get_memory_c(addr & 0xffffff) & 0xff;
}

static word32
read_emu16(word32 addr)
{
    return read_emu8(addr) | (read_emu8(addr + 1) << 8);
}

static word32
read_emu24(word32 addr)
{
    return read_emu8(addr)
         | (read_emu8(addr + 1) << 8)
         | (read_emu8(addr + 2) << 16);
}

/* ---- segment map management ---- */

static void
free_seg_contents(seg_map_t *s)
{
    int i;
    if(!s) return;
    for(i = 0; i < s->n_entries; i++) {
        free(s->entries[i].name);
    }
    free(s->entries);
    free(s->seg_name);
    s->entries = NULL;
    s->seg_name = NULL;
    s->n_entries = 0;
}

void
symbols_unregister_by_name(const char *name)
{
    int i;
    for(i = 0; i < g_n_segments; i++) {
        if(g_segments[i].seg_name &&
           strcmp(g_segments[i].seg_name, name) == 0) {
            free_seg_contents(&g_segments[i]);
            /* swap-with-last */
            g_segments[i] = g_segments[g_n_segments - 1];
            g_n_segments--;
            return;
        }
    }
}

static seg_map_t *
seg_map_alloc_slot(void)
{
    if(g_n_segments == g_seg_capacity) {
        int newcap = g_seg_capacity ? g_seg_capacity * 2 : 16;
        seg_map_t *p = realloc(g_segments, newcap * sizeof(*p));
        if(!p) return NULL;
        g_segments = p;
        g_seg_capacity = newcap;
    }
    return &g_segments[g_n_segments++];
}

/* ---- .symbols file parsing ---- */

static int
sym_cmp(const void *a, const void *b)
{
    const sym_entry_t *ea = a;
    const sym_entry_t *eb = b;
    if(ea->rel_offset < eb->rel_offset) return -1;
    if(ea->rel_offset > eb->rel_offset) return  1;
    return 0;
}

static int
load_symbol_file(const char *seg_name, sym_entry_t **out_entries, int *out_n)
{
    char     path[1024];
    FILE    *fp;
    char     line[512];
    sym_entry_t *arr = NULL;
    int      cap = 0, n = 0;

    *out_entries = NULL;
    *out_n = 0;

    if(!g_cfg_symbols_path || !*g_cfg_symbols_path) {
        return 0;
    }
    snprintf(path, sizeof(path), "%s/%s.symbols",
             g_cfg_symbols_path, seg_name);

    fp = fopen(path, "r");
    if(!fp) {
        dbg_printf("symbols: %s: cannot open\n", path);
        return 0;
    }

    while(fgets(line, sizeof(line), fp)) {
        char     kind;
        unsigned rel, size;
        char     name[256];
        char     size_buf[32];
        int      matched;

        if(line[0] == '#' || line[0] == '\n' || line[0] == '\0') continue;

        /* Try "K HEX HEX NAME" first; size column may be "-" for unknown. */
        matched = sscanf(line, " %c %x %31s %255s",
                         &kind, &rel, size_buf, name);
        if(matched != 4) continue;
        if(kind != 'F' && kind != 'V' && kind != 'L') continue;

        if(size_buf[0] == '-') {
            size = 0;
        } else {
            size = (unsigned)strtoul(size_buf, NULL, 16);
        }

        if(n == cap) {
            int newcap = cap ? cap * 2 : 64;
            sym_entry_t *p = realloc(arr, newcap * sizeof(*p));
            if(!p) { free(arr); fclose(fp); return 0; }
            arr = p;
            cap = newcap;
        }
        arr[n].rel_offset = rel;
        arr[n].size       = size;
        arr[n].kind       = kind;
        arr[n].name       = strdup(name);
        n++;
    }

    fclose(fp);

    if(n > 1) qsort(arr, n, sizeof(*arr), sym_cmp);

    *out_entries = arr;
    *out_n = n;
    return 1;
}

/* ---- WDM $0F handler ---- */

void
symbols_register_from_desc(word32 desc_addr)
{
    word32  magic, version, desc_offset, seg_length;
    word32  name_len, i;
    char    name[MAX_NAME + 1];
    word32  load_base;
    seg_map_t *slot;

    desc_addr &= 0xffffff;

    magic       = read_emu16(desc_addr + 0);
    version     = read_emu16(desc_addr + 2);
    desc_offset = read_emu16(desc_addr + 4);
    seg_length  = read_emu24(desc_addr + 6);
    name_len    = read_emu8 (desc_addr + 9);

    if(magic != SYM_MAGIC) {
        dbg_printf("symbols: bad magic $%04x at %06x\n", magic, desc_addr);
        return;
    }
    if(version != SYM_VERSION) {
        dbg_printf("symbols: unsupported descriptor version %u\n", version);
        return;
    }
    if(name_len == 0 || name_len > MAX_NAME) {
        dbg_printf("symbols: bad name_len %u at %06x\n", name_len, desc_addr);
        return;
    }

    for(i = 0; i < name_len; i++) {
        name[i] = (char)read_emu8(desc_addr + 10 + i);
    }
    name[name_len] = '\0';

    load_base = (desc_addr - desc_offset) & 0xffffff;

    /* Reload semantics: replace any prior entry with the same name. */
    symbols_unregister_by_name(name);

    slot = seg_map_alloc_slot();
    if(!slot) return;
    memset(slot, 0, sizeof(*slot));
    slot->seg_name  = strdup(name);
    slot->load_base = load_base;
    slot->length    = seg_length;

    (void)load_symbol_file(name, &slot->entries, &slot->n_entries);

    dbg_printf("symbols: %s @ %06x len=%06x (%d syms)\n",
               name, load_base, seg_length, slot->n_entries);
}

/* ---- PC lookup ---- */

static const sym_entry_t *
find_entry(const seg_map_t *s, word32 rel)
{
    int lo = 0, hi = s->n_entries - 1, best = -1;
    while(lo <= hi) {
        int mid = (lo + hi) >> 1;
        if(s->entries[mid].rel_offset <= rel) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if(best < 0) return NULL;
    return &s->entries[best];
}

const char *
symbols_describe_pc(word32 pc)
{
    static char buf[160];
    int i;

    pc &= 0xffffff;
    for(i = 0; i < g_n_segments; i++) {
        seg_map_t *s = &g_segments[i];
        if(pc >= s->load_base && pc < s->load_base + s->length) {
            word32 rel = pc - s->load_base;
            const sym_entry_t *e = find_entry(s, rel);
            if(e) {
                snprintf(buf, sizeof(buf), "%s!%s+$%x",
                         s->seg_name, e->name, rel - e->rel_offset);
            } else {
                snprintf(buf, sizeof(buf), "%s+$%x", s->seg_name, rel);
            }
            return buf;
        }
    }
    return NULL;
}
