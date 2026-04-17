/* symbols.h - host-side symbol map for relocatable IIGS segments.
 *
 * The ORCA linker (with -DgsplusSymbols=1) emits <target>.symbols JSON
 * files and injects an 8-byte WDM $0F prologue at the start of segment 1
 * of each linked binary. The prologue carries a 32-bit signature that
 * matches the "symsig" field in the JSON file.
 *
 * At reset (and whenever the symbols-path config changes), symbols_rescan()
 * walks g_cfg_symbols_path, parses every *.symbols file, and indexes them
 * by symsig. When a segment fires its WDM $0F prologue, the handler reads
 * the signature and calls symbols_register_from_sig() to bind the indexed
 * symbol table to the segment's runtime load base.
 */

#ifndef GSPLUS_SYMBOLS_H
#define GSPLUS_SYMBOLS_H

#include <stdint.h>

void  symbols_init(void);
void  symbols_rescan(void);
void  symbols_register_from_sig(uint32_t load_base, uint32_t symsig);
void  symbols_dump(void);

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
