/* loader.h -- Linux extensions to the gtasa_nx so_util API
 *
 * upstream/source/so_util.h is the interface the vendored game hooks are
 * written against; loader.c implements it on Linux. The declarations below
 * are the extras the Linux platform layer needs on top of it.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef GTASA_LOADER_H
#define GTASA_LOADER_H

#include <stddef.h>
#include <stdint.h>

#include "so_util.h"

/* Called for an import that is neither in the import table nor exported by
 * another loaded module. Returns the address to bind, or 0 to leave it
 * unresolved (it then traps with its name if ever called). `is_object` is set
 * when the importing symbol is a data object rather than a function. */
typedef uintptr_t (*so_fallback_resolver)(const char *name, int is_object);
void so_set_fallback_resolver(so_fallback_resolver fn);

/* Imports that ended up unresolved in the last so_resolve() call. */
int so_unresolved_count(void);

/* Module and offset for an address, for crash reports and the stall
 * sampler; NULL when the address is outside every loaded module.
 * Async-signal-safe (reads only). */
const so_module *so_module_at(uintptr_t addr);

/* Name of the nearest exported symbol at or below `addr` inside `mod`, and
 * the offset from it; NULL when none. Async-signal-safe. */
const char *so_symbol_at(const so_module *mod, uintptr_t addr, uintptr_t *off);

#endif
