/* test_loader.c -- load tests/bionic_lib.so with the real loader and import
 * table and run its checks. Runs on aarch64 (under qemu-aarch64 on a PC).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <elf.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "so_util.h"
#include "util.h"

#include "import_table.h"
#include "loader.h"
#include "platform_config.h"
#include "platform_util.h"
#include "tls_guard.h"
#include "crash.h"

/* the game's pthread_create, as libGame.so gets it */
int bionic_pthread_create(pthread_t *t, const void *attr, void *(*fn)(void *), void *arg);

/* the platform layer's main.c provides these to the rest of the code */
so_module game_mod;
void hard_exit(void) { _exit(0); }

static int failures;

/* rc is 0 or the failing line in `file` */
static void report_in(const char *file, const char *name, int rc) {
  printf("%-16s %s", name, rc == 0 ? "ok\n" : "FAIL");
  if (rc)
    printf(" (%s line %d)\n", file, rc);
  failures += rc != 0;
}

static void report(const char *name, int rc) { report_in("bionic_lib.c", name, rc); }

/* so_load must refuse a damaged file instead of reading out of bounds:
 * kind 0 puts a symbol name outside .dynstr, kind 1 makes a segment wrap
 * around the address space */
static int damaged_refused(const char *path, int kind) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return __LINE__;
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = malloc((size_t)size);
  const int got = buf && fread(buf, 1, (size_t)size, f) == (size_t)size;
  fclose(f);
  if (!got)
    return __LINE__;
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)buf;
  int changed = 0;
  if (kind == 0) {
    Elf64_Shdr *sh = (Elf64_Shdr *)(buf + eh->e_shoff);
    const char *names = buf + sh[eh->e_shstrndx].sh_offset;
    for (int i = 0; i < eh->e_shnum && !changed; i++)
      if (!strcmp(names + sh[i].sh_name, ".dynsym")) {
        ((Elf64_Sym *)(buf + sh[i].sh_offset))[1].st_name = 0x7fffff00u;
        changed = 1;
      }
  } else {
    Elf64_Phdr *ph = (Elf64_Phdr *)(buf + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum && !changed; i++)
      if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr) {
        ph[i].p_memsz = UINT64_MAX - ph[i].p_vaddr + 0x11; /* end wraps to 0x10 */
        changed = 1;
      }
  }
  char tmp[64];
  snprintf(tmp, sizeof(tmp), "tests/build/damaged-%d.so", kind);
  f = fopen(tmp, "wb");
  const int wrote = f && fwrite(buf, 1, (size_t)size, f) == (size_t)size;
  if (f)
    fclose(f);
  free(buf);
  if (!changed || !wrote)
    return __LINE__;
  so_module m;
  memset(&m, 0, sizeof(m));
  if (so_load(&m, tmp, NULL, 0) >= 0)
    return __LINE__;
  return 0;
}

/* ---- the crash reporter, in a child process ---- */

/* SIGSEGV with x29 = the argument: the frame walk starts from garbage */
void crash_with_fp(uintptr_t fp);
__asm__(".text\n"
        ".global crash_with_fp\n"
        ".type crash_with_fp, %function\n"
        "crash_with_fp:\n"
        "  mov x29, x0\n"
        "  mov x9, #0\n"
        "  str xzr, [x9]\n"
        "  ret\n");

static int __attribute__((noinline)) dive(int n) {
  volatile char buf[2048];
  buf[0] = (char)n;
  buf[2047] = (char)n;
  if (n < 0)
    return 0;
  return dive(n + 1) + buf[0] + buf[2047];
}

static void *overflow_thread(void *arg) {
  (void)arg;
  return (void *)(intptr_t)dive(0);
}

static int file_has(const char *path, const char *needle) {
  char buf[8192];
  FILE *f = fopen(path, "r");
  if (!f)
    return 0;
  const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = 0;
  return strstr(buf, needle) != NULL;
}

/* kind 0: a fault with a frame pointer into unmapped memory (the walk must
 * stop, not fault); kind 1: a stack overflow on a thread made by the
 * game's pthread_create (its own alternate stack is what lets the handler
 * run). Either way: the report, last-error.txt, exit status 128 + SIGSEGV. */
static int crash_report(int kind) {
  const char *dir = kind ? "tests/build/crash-overflow" : "tests/build/crash-badfp";
  mkdir(dir, 0755);
  char log[128], err[128];
  snprintf(log, sizeof(log), "%s/crash.log", dir);
  snprintf(err, sizeof(err), "%s/last-error.txt", dir);
  unlink(log);
  unlink(err);
  fflush(stdout);
  const pid_t pid = fork();
  if (pid == 0) {
    if (chdir(dir) < 0)
      _exit(1);
    log_open("crash.log");
    crash_install();
    if (kind == 0) {
      void *gone = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      munmap(gone, 4096);
      crash_with_fp((uintptr_t)gone);
    } else {
      pthread_t t;
      if (bionic_pthread_create(&t, NULL, overflow_thread, NULL) == 0)
        pthread_join(t, NULL);
    }
    _exit(0);
  }
  int st = 0;
  if (waitpid(pid, &st, 0) != pid)
    return __LINE__;
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 128 + SIGSEGV) {
    printf("  child: %s %d\n", WIFEXITED(st) ? "exit" : "signal",
           WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st));
    return __LINE__;
  }
  if (!file_has(log, "*** CRASH: SIGSEGV") || !file_has(log, "  pc "))
    return __LINE__;
  if (!file_has(err, "Il gioco si e' chiuso per un errore"))
    return __LINE__;
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: test_loader path/to/bionic_lib.so\n");
    return 2;
  }
  char why[128];
  if (tls_guard_check(why, sizeof(why)) < 0) {
    printf("tls_guard: FAIL (%s)\n", why);
    return 1;
  }
  printf("tls_guard        ok (canary slot at TPIDR_EL0+0x28)\n");

  pconfig_defaults();
  so_set_fallback_resolver(imports_fallback);
  import_table_configure();

  so_module mod;
  if (so_load(&mod, argv[1], NULL, 0) < 0) {
    printf("so_load: FAIL\n");
    return 1;
  }
  so_relocate(&mod);
  so_resolve(&mod, import_table, import_table_count, 1);
  printf("imports          %d unresolved\n", so_unresolved_count());
  failures += so_unresolved_count() != 0;
  so_finalize(&mod);
  so_flush_caches(&mod);
  so_execute_init_array(&mod);

#define FN(name) ((int (*)())so_find_addr_rx(&mod, name))
  report("relocations", FN("test_relocs")());
  report("abs relocations", FN("test_abs_relocs")());
  report("errno", FN("test_errno")());
  report("stdio", FN("test_stdio")());
  report("fortify", ((int (*)(const char *))FN("test_fortify"))("tests/build/fortify.txt"));
  report("ctype", FN("test_ctype")());
  report("sysconf", FN("test_sysconf")());
  report("stat", ((int (*)(const char *))FN("test_stat"))(argv[1]));
  report("readdir", ((int (*)(const char *, const char *))FN("test_readdir"))(".", "Makefile"));
  mkdir("tests/build/CaseDir", 0755);
  mkdir("tests/build/CaseDir/Sub", 0755);
  unlink("tests/build/CaseDir/Sub/later.TXT"); /* test_casefold creates it */
  FILE *mixed = fopen("tests/build/CaseDir/Sub/MixedName.txt", "w");
  if (mixed) {
    fputs("abc", mixed);
    fclose(mixed);
  }
  report("case of names", FN("test_casefold")());
  /* AAsset from a game folder whose roots are spelled otherwise */
  mkdir("tests/build/aasset", 0755);
  mkdir("tests/build/aasset/DATA", 0755);
  mkdir("tests/build/aasset/DATA/Models", 0755);
  mkdir("tests/build/aasset/es2", 0755);
  FILE *thing = fopen("tests/build/aasset/DATA/Models/Thing.dat", "w");
  if (thing) {
    fputs("xyz", thing);
    fclose(thing);
  }
  FILE *shader = fopen("tests/build/aasset/es2/Shader.TXT", "w");
  if (shader)
    fclose(shader);
  char here[1024];
  if (getcwd(here, sizeof(here)) && chdir("tests/build/aasset") == 0) {
    const int rc = FN("test_aasset")();
    if (chdir(here) < 0)
      return 1;
    report("aasset, case", rc);
  } else {
    report_in("test_loader.c", "aasset, case", __LINE__);
  }
  report("mutex", FN("test_mutex")());
  report("threads", ((int (*)(int))FN("test_threads"))(4));
  report("timedwait", FN("test_timedwait")());
  report("timedwait mono", FN("test_timedwait_monotonic")());
  report("small stack", FN("test_small_stack")());
  report("misaligned", FN("test_misaligned")());
  report("setjmp", FN("test_setjmp")());
  report("signal", FN("test_signal")());
  report("canary", FN("test_canary")());

  report_in("test_loader.c", "crash, bad fp", crash_report(0));
  report_in("test_loader.c", "crash, overflow", crash_report(1));
  report_in("test_loader.c", "bad symbol name", damaged_refused(argv[1], 0));
  report_in("test_loader.c", "wrapping segment", damaged_refused(argv[1], 1));

  /* the symbol lookup the crash reporter uses */
  uintptr_t off = 0;
  const uintptr_t fn = so_find_addr_rx(&mod, "test_errno");
  const so_module *m = so_module_at(fn + 8);
  const char *sym = m ? so_symbol_at(m, fn + 8, &off) : NULL;
  report_in("test_loader.c", "symbolize", (m == &mod && sym && !strcmp(sym, "test_errno") && off == 8) ? 0 : __LINE__);

  printf("%s\n", failures ? "FAILED" : "ALL PASSED");
  return failures ? 1 : 0;
}
