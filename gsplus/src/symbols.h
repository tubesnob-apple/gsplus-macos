/* symbols.h - host-side symbol map for relocatable IIGS segments
 *
 * Segments register themselves at runtime via WDM $0F + a stack-passed
 * descriptor pointer. The handler reads <name>.symbols from the host
 * filesystem and builds a PC -> symbol map for debug output.
 */

#ifndef GSPLUS_SYMBOLS_H
#define GSPLUS_SYMBOLS_H

#include <stdint.h>

void  symbols_init(void);
void  symbols_register_from_desc(uint32_t desc_addr);
void  symbols_unregister_by_name(const char *name);

/* Resolve a 24-bit PC to "name+0xNN" form. Returns a pointer to a
 * static buffer (not thread-safe) on hit, or NULL if no segment covers
 * the PC. */
const char *symbols_describe_pc(uint32_t pc);

#endif
