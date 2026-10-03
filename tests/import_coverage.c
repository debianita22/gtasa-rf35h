/* import_coverage.c -- how each name in a list of imports would be bound.
 *
 *   import_coverage tests/upstream-imports.txt
 *
 * The list is every name gtasa_nx 3460c75 binds in its import table
 * (source/imports.c), i.e. what libGame.so 2.11.311 and libc++_shared.so
 * import from bionic, OpenAL, mpg123, EGL, GLES and the NDK, minus what the
 * two modules resolve from each other. Each name is reported as bound to
 * our table, bound to a host library through dlsym, or unbound (it would
 * trap with its name when called). Exit status 1 when anything is unbound.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "import_table.h"
#include "platform_config.h"

so_module game_mod;
void hard_exit(void) { _exit(0); }

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: import_coverage names.txt [-v]\n");
    return 2;
  }
  const int verbose = argc > 2 && !strcmp(argv[2], "-v");
  FILE *f = fopen(argv[1], "r");
  if (!f) {
    perror(argv[1]);
    return 2;
  }
  pconfig_defaults();
  import_table_configure();

  char line[256];
  int table = 0, host = 0, unbound = 0;
  while (fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\r\n")] = 0;
    if (!line[0] || line[0] == '#')
      continue;
    if (imports_lookup(line)) {
      table++;
      if (verbose)
        printf("table   %s\n", line);
    } else if (!imports_abi_unsafe(line) && dlsym(RTLD_DEFAULT, line)) {
      host++;
      if (verbose) {
        Dl_info di;
        printf("host    %s (%s)\n", line,
               dladdr(dlsym(RTLD_DEFAULT, line), &di) && di.dli_fname ? di.dli_fname : "?");
      }
    } else {
      unbound++;
      printf("UNBOUND %s%s\n", line, imports_abi_unsafe(line) ? " (ABI differs, needs a wrapper)" : "");
    }
  }
  fclose(f);
  printf("imports: %d table, %d host, %d unbound\n", table, host, unbound);
  return unbound ? 1 : 0;
}
