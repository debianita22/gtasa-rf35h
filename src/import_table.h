/* import_table.h -- what the game's imports bind to
 *
 * Resolution order for an import: this table (wrappers and renamed
 * symbols), then the other loaded Android modules (the C++ runtime donor),
 * then the host libraries through dlsym, except for names whose ABI differs
 * between bionic and glibc, which must come from the table or stay
 * unresolved (and trap with their name if called).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef GTASA_IMPORT_TABLE_H
#define GTASA_IMPORT_TABLE_H

#include <stdint.h>

#include "so_util.h"

extern DynLibFunction import_table[];
extern const int import_table_count;

/* apply the options that change bindings (trilinear) */
void import_table_configure(void);

/* table only: our wrapper for `name`, or 0 */
uintptr_t imports_lookup(const char *name);

/* the loader's fallback: host libraries, minus the ABI-unsafe names */
uintptr_t imports_fallback(const char *name, int is_object);

/* full resolution as an import would get it (for the game's dlsym) */
uintptr_t imports_resolve(const char *name);

/* 1 when `name` must never be bound to the host library of the same name */
int imports_abi_unsafe(const char *name);

#endif
