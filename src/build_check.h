/* build_check.h -- identify the libGame.so build before patching it
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef GTASA_BUILD_CHECK_H
#define GTASA_BUILD_CHECK_H

#include "so_util.h"

typedef struct {
  int checked;
  int matched;
  char first_mismatch[160];
} BuildCheck;

/* Run on the loaded (not yet patched) module. Returns 0 when every known
 * address and instruction matches the 2.11.311 arm64-v8a build. */
int build_check(const so_module *game, BuildCheck *out);

#endif
