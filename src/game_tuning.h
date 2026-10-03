/* game_tuning.h -- the game's first-run graphics settings for this hardware
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef GTASA_GAME_TUNING_H
#define GTASA_GAME_TUNING_H

#include "so_util.h"

/* Replace MobileSettings::SetRendererDefaults with one that applies the
 * game_* defaults of gtasa_nx.cfg (see game_tuning.c). Call before
 * so_finalize. Returns 0, or -1 when the game lacks the symbols. */
int game_tuning_install(so_module *game);

#endif
