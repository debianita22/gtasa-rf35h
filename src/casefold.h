/* casefold.h -- file names found the way a case-insensitive file system would
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef GTASA_CASEFOLD_H
#define GTASA_CASEFOLD_H

#include <stddef.h>

/* The on-disk spelling of a relative path that does not exist as given,
 * written to buf; NULL when no spelling of it exists (or path is absolute).
 * For lookups that failed with ENOENT. Thread-safe. */
const char *case_resolve(const char *path, char *buf, size_t len);

#endif
