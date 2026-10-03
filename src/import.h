/* import.h -- install the game from the user's own APK and OBB
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef GTASA_IMPORT_H
#define GTASA_IMPORT_H

/* Take data_dir's import lock (until the process exits): 0, or -1 when
 * another import holds it. import_run takes it itself; calling this first
 * keeps a refused import from touching anything, its log included. */
int import_lock(const char *data_dir);

/* Number of APK/OBB archives waiting in data_dir or data_dir/import. */
int import_pending(const char *data_dir);

/* Extract libraries, assets and OBB data into data_dir (see import.c).
 * full_shaders: replace the short shader lists with scache.txt.
 * Returns 0, 2 with the reason in import-status.txt and last-error.txt, or
 * 3 when another import is running in data_dir (nothing touched). */
int import_run(const char *data_dir, int full_shaders);

#endif
