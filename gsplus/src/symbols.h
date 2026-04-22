/* symbols.h - host-side symbol map for relocatable IIGS segments.
 *
 * The ORCA linker (with -DgsplusSymbols=1) emits <target>.symbols JSON
 * files and appends a 19-byte footer to the end of each output CODE
 * segment's LCONST:
 *
 *     offset  size  content
 *     ------  ----  -----------------------------------------------
 *       0     10    "_$GSPSYM$_"          ASCII magic, no NUL
 *      10      4    sfSig                 LE uint32 — matches .symbols "symsig"
 *      14      4    length                LE uint32 — total LCONST length
 *                                         including the footer
 *      18      1    segNum                uint8    — pre-ExpressLoad-remap
 *                                         segment number; matches
 *                                         symbols[].segment
 *
 * symbols_scan_and_bind() walks emulator RAM, finds every footer, and
 * binds (sfSig, segNum) → seg_base = (magic_addr + 19) - length, where
 * the indexed .symbols file with matching sfSig supplies the symbol
 * names.
 *
 * Symbol resolution is wired into WDM $00-$7F: each of those traps
 * calls the throttled scanner before emitting its log line so the
 * symbol binding info is always fresh. WDM $0F is the same thing with
 * no adornment — an explicit "scan now" trigger the program can emit
 * at startup. The tracer window also rescans on every halt transition.
 */

#ifndef GSPLUS_SYMBOLS_H
#define GSPLUS_SYMBOLS_H

#include <stdint.h>

void  symbols_init(void);
void  symbols_rescan(void);
void  symbols_register_from_sig(uint32_t load_base, uint32_t symsig);
void  symbols_dump(void);

/* RAM scanner: walks emulator memory for __GSPLUSSYMBOLS__ footers and
 * binds each found segment. Returns the number of bindings installed.
 * Unthrottled — use symbols_scan_and_bind_throttled() in hot paths. */
int   symbols_scan_and_bind(void);

/* Same as above, but a no-op if the last scan ran within the throttle
 * window (~0.5s). Safe to call from every WDM trap. */
int   symbols_scan_and_bind_throttled(void);

/* Index accessors, used by the MCP server to report what symbol files are
 * currently indexed. Returns 0 on success, nonzero if i is out of range.
 * Any output pointer may be NULL. Strings belong to symbols.c — do not
 * free; they stay valid until the next symbols_rescan(). */
int   symbols_file_count(void);
int   symbols_get_file(int i,
                       const char **path_out,
                       const char **target_out,
                       uint32_t    *symsig_out,
                       uint32_t    *length_out,
                       int         *nsyms_out);

/* Resolve a 24-bit PC to "target!name+$NN" form. Returns a pointer to a
 * static buffer (not thread-safe) on hit, or NULL if no segment covers
 * the PC. */
const char *symbols_describe_pc(uint32_t pc);

#endif
