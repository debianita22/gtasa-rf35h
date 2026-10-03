/* test_import.c -- driver for tests/test_import.py: run the import on a folder
 *
 *   test_import DATA_DIR
 *
 * Exit status and import-status.txt are what the test checks.
 */

#include <stdio.h>
#include <unistd.h>

#include "import.h"
#include "platform_config.h"
#include "platform_util.h"

void hard_exit(void) { _exit(0); }

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: test_import DATA_DIR\n");
    return 64;
  }
  pconfig_defaults();
  log_open("/dev/null");
  if (argc > 2) /* "pending": just count */
    return printf("%d\n", import_pending(argv[1])) < 0;
  return import_run(argv[1], 1);
}
