/* error.c -- fatal errors on a handheld with no console
 *
 * The message goes to the log and to last-error.txt next to it, where the
 * launcher picks it up to show it in RetroArch, and the process exits with
 * status 2 so the launcher can tell a setup error from a crash.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "error.h"
#include "util.h"
#include "platform_util.h"

void fatal_error(const char *fmt, ...) {
  char msg[1024];
  va_list va;
  va_start(va, fmt);
  vsnprintf(msg, sizeof(msg), fmt, va);
  va_end(va);

  debugPrintf("FATAL: %s\n", msg);
  FILE *f = fopen("last-error.txt", "w");
  if (f) {
    fputs(msg, f);
    fputc('\n', f);
    fclose(f);
  }
  fprintf(stderr, "gtasa: %s\n", msg);
  tune_restore_all();
  _exit(2);
}
