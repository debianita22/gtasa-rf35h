/* loader.c -- load Android arm64 .so modules into a glibc process
 *
 * Implements the so_util.h interface the gtasa_nx game hooks are written
 * against (upstream/source/so_util.h), on Linux: each module is mapped into
 * an anonymous mapping, relocated, linked against the import table, other
 * loaded modules and finally a fallback resolver, then mprotect()ed segment by
 * segment. The relocation and RELR logic follows gtasa_nx's so_util.c by Andy
 * Nguyen and fgsfds; the memory handling, the validation, the weak-symbol rule
 * and the named traps for unresolved imports are new.
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds (original so_util.c)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "loader.h"
#include "util.h"
#include "error.h"

/* no segment may end above this: keeps p_vaddr + p_memsz from wrapping */
#define LOAD_MAX ((uint64_t)1 << 32)

#ifndef DT_RELR
#define DT_RELRSZ 35
#define DT_RELR 36
#endif
#define DT_ANDROID_REL     0x6000000f
#define DT_ANDROID_RELA    0x60000011
#define DT_ANDROID_RELASZ  0x60000012
#define DT_ANDROID_RELR    0x6fffe000
#define DT_ANDROID_RELRSZ  0x6fffe001

/* Per-module state that does not fit upstream's so_module. It hangs off the
 * opaque load_memrv pointer, which on the Switch held a virtmem reservation. */
typedef struct {
  size_t page;
  Elf64_Rela *rela;      size_t rela_count;
  Elf64_Rela *jmprel;    size_t jmprel_count;
  Elf64_Rela *aps2;      size_t aps2_count;   /* decoded DT_ANDROID_RELA, heap */
  const Elf64_Xword *relr; size_t relr_size;
  uintptr_t init;                       /* DT_INIT, runtime address or 0 */
  uintptr_t *init_array; size_t init_array_count;
} so_linux;

static so_module *so_list;
static so_fallback_resolver fallback_resolver;
static int last_unresolved;

static so_linux *ext(const so_module *mod) { return (so_linux *)mod->load_memrv; }

void so_set_fallback_resolver(so_fallback_resolver fn) { fallback_resolver = fn; }
int so_unresolved_count(void) { return last_unresolved; }

/* --------------------------------------------------------------------------
 * traps for unresolved imports: a call lands in trap_called() with the name
 * of the symbol in x0, so the log says what is missing instead of jumping
 * into the middle of the module's own GOT.
 * ------------------------------------------------------------------------ */

#define TRAP_SIZE 32
#define TRAP_POOL 4096
static uint8_t *trap_pool;
static size_t trap_used;

static void trap_called(const char *name) {
  debugPrintf("FATAL: the game called an import that was not resolved: %s\n", name);
  fatal_error("Unresolved import called:\n%s", name);
}

static uintptr_t make_trap(const char *name) {
  if (!trap_pool) {
    trap_pool = mmap(NULL, TRAP_POOL * TRAP_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (trap_pool == MAP_FAILED) {
      trap_pool = NULL;
      return 0;
    }
  }
  if (trap_used >= TRAP_POOL)
    return 0;
  uint32_t *t = (uint32_t *)(trap_pool + trap_used++ * TRAP_SIZE);
  t[0] = 0x58000080u; /* ldr x0, #16  -> name     */
  t[1] = 0x580000b0u; /* ldr x16, #20 -> handler  */
  t[2] = 0xd61f0200u; /* br x16                   */
  t[3] = 0xd503201fu; /* nop (keeps the literals 8-byte aligned) */
  *(uint64_t *)(t + 4) = (uint64_t)(uintptr_t)name;
  *(uint64_t *)(t + 6) = (uint64_t)(uintptr_t)&trap_called;
  return (uintptr_t)t;
}

static void seal_traps(void) {
  if (trap_pool && trap_used) {
    __builtin___clear_cache((char *)trap_pool, (char *)trap_pool + TRAP_POOL * TRAP_SIZE);
    mprotect(trap_pool, TRAP_POOL * TRAP_SIZE, PROT_READ | PROT_EXEC);
  }
}

/* zero-filled stand-in for unresolved data imports */
static uint64_t unresolved_object[512];

/* -------------------------------------------------------------------------- */

void hook_arm64(uintptr_t addr, uintptr_t dst) {
  if (addr == 0)
    return;
  uint32_t *hook = (uint32_t *)addr;
  hook[0] = 0x58000051u; /* LDR X17, #0x8 */
  hook[1] = 0xd61f0220u; /* BR X17        */
  *(uint64_t *)(hook + 2) = dst;
}

void so_flush_caches(so_module *mod) {
  /* executable segments only: after so_finalize the pages between segments
   * are PROT_NONE, and cache maintenance on them faults (qemu cannot show
   * this: it does not model the caches) */
  for (int i = 0; i < mod->phnum; i++) {
    const Elf64_Phdr *p = &mod->phdr[i];
    if (p->p_type != PT_LOAD || !(p->p_flags & PF_X) || !p->p_memsz)
      continue;
    char *start = (char *)mod->load_virtbase + p->p_vaddr;
    __builtin___clear_cache(start, start + p->p_memsz);
  }
  seal_traps();
}

void so_free_temp(so_module *mod) {
  free(mod->so_base);
  mod->so_base = NULL;
  mod->elf_hdr = NULL;
  mod->prog_hdr = NULL;
  mod->sec_hdr = NULL;
  mod->shstrtab = NULL;
}

static int range_ok(size_t off, size_t len, size_t total) {
  return off <= total && len <= total - off;
}

static int read_whole_file(const char *path, void **out, size_t *out_size) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return -1;
  struct stat st;
  if (fstat(fd, &st) < 0 || st.st_size <= 0) {
    close(fd);
    return -1;
  }
  size_t size = (size_t)st.st_size;
  uint8_t *buf = malloc(size);
  if (!buf) {
    close(fd);
    return -2;
  }
  size_t got = 0;
  while (got < size) {
    ssize_t n = read(fd, buf + got, size - got);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    got += (size_t)n;
  }
  close(fd);
  if (got != size) {
    free(buf);
    return -1;
  }
  *out = buf;
  *out_size = size;
  return 0;
}

static const Elf64_Dyn *file_dynamic(const so_module *mod, size_t *count) {
  for (int i = 0; i < mod->phnum; i++) {
    const Elf64_Phdr *p = &mod->phdr[i];
    if (p->p_type != PT_DYNAMIC)
      continue;
    if (!range_ok(p->p_offset, p->p_filesz, mod->so_size))
      return NULL;
    *count = p->p_filesz / sizeof(Elf64_Dyn);
    return (const Elf64_Dyn *)((uint8_t *)mod->so_base + p->p_offset);
  }
  return NULL;
}

/* translate a link-time vaddr range into the mapping, or NULL if outside */
static void *vaddr_ptr(const so_module *mod, uintptr_t vaddr, size_t len) {
  if (!range_ok(vaddr, len, mod->load_size))
    return NULL;
  return (uint8_t *)mod->load_base + vaddr;
}

/* Android's packed relocations ("APS2", lld --pack-dyn-relocs=android):
 * SLEB128 groups that share r_info, offset delta or addend. Same algorithm
 * as bionic's packed_reloc_iterator. */
typedef struct {
  const uint8_t *p, *end;
  int error;
} Sleb;

static int64_t sleb_next(Sleb *d) {
  int64_t value = 0;
  unsigned shift = 0;
  uint8_t byte;
  do {
    if (d->p >= d->end || shift >= 64) {
      d->error = 1;
      return 0;
    }
    byte = *d->p++;
    value |= (int64_t)(byte & 0x7f) << shift;
    shift += 7;
  } while (byte & 0x80);
  if (shift < 64 && (byte & 0x40))
    value |= (int64_t)(~(uint64_t)0 << shift);
  return value;
}

enum {
  APS2_BY_INFO = 1,
  APS2_BY_OFFSET_DELTA = 2,
  APS2_BY_ADDEND = 4,
  APS2_HAS_ADDEND = 8,
};

static int decode_aps2(so_linux *x, const uint8_t *data, size_t size) {
  if (size < 4 || memcmp(data, "APS2", 4) != 0)
    return -1;
  Sleb d = { data + 4, data + size, 0 };
  const int64_t count = sleb_next(&d);
  if (d.error || count < 0 || count > 16 * 1024 * 1024)
    return -1;
  Elf64_Rela r = { 0 };
  r.r_offset = (Elf64_Addr)sleb_next(&d);
  Elf64_Rela *out = calloc((size_t)count ? (size_t)count : 1, sizeof(*out));
  if (!out)
    return -1;
  int64_t done = 0;
  while (done < count && !d.error) {
    const int64_t group_size = sleb_next(&d);
    const int64_t flags = sleb_next(&d);
    int64_t offset_delta = 0;
    if (flags & APS2_BY_OFFSET_DELTA)
      offset_delta = sleb_next(&d);
    if (flags & APS2_BY_INFO)
      r.r_info = (Elf64_Xword)sleb_next(&d);
    if ((flags & APS2_HAS_ADDEND) && (flags & APS2_BY_ADDEND))
      r.r_addend += sleb_next(&d);
    else if (!(flags & APS2_HAS_ADDEND))
      r.r_addend = 0;
    if (group_size <= 0 || group_size > count - done) {
      d.error = 1;
      break;
    }
    for (int64_t i = 0; i < group_size && !d.error; i++) {
      r.r_offset += (flags & APS2_BY_OFFSET_DELTA) ? (Elf64_Addr)offset_delta : (Elf64_Addr)sleb_next(&d);
      if (!(flags & APS2_BY_INFO))
        r.r_info = (Elf64_Xword)sleb_next(&d);
      if ((flags & APS2_HAS_ADDEND) && !(flags & APS2_BY_ADDEND))
        r.r_addend += sleb_next(&d);
      out[done++] = r;
    }
  }
  if (d.error || done != count) {
    free(out);
    return -1;
  }
  x->aps2 = out;
  x->aps2_count = (size_t)count;
  return 0;
}

static int parse_dynamic(so_module *mod) {
  so_linux *x = ext(mod);
  size_t n = 0;
  const Elf64_Dyn *dyn = file_dynamic(mod, &n);
  if (!dyn) {
    debugPrintf("%s: no PT_DYNAMIC\n", mod->name);
    return -1;
  }
  uintptr_t rela = 0, jmprel = 0, relr = 0, init_array = 0, aps2 = 0;
  size_t relasz = 0, pltrelsz = 0, relrsz = 0, init_arraysz = 0, aps2sz = 0;
  int64_t pltrel = DT_RELA;
  for (size_t i = 0; i < n && dyn[i].d_tag != DT_NULL; i++) {
    const Elf64_Xword v = dyn[i].d_un.d_val;
    switch (dyn[i].d_tag) {
      case DT_RELA:          rela = v; break;
      case DT_RELASZ:        relasz = v; break;
      case DT_JMPREL:        jmprel = v; break;
      case DT_PLTRELSZ:      pltrelsz = v; break;
      case DT_PLTREL:        pltrel = (int64_t)v; break;
      case DT_RELR:
      case DT_ANDROID_RELR:  relr = v; break;
      case DT_RELRSZ:
      case DT_ANDROID_RELRSZ: relrsz = v; break;
      case DT_INIT:          x->init = v; break;
      case DT_INIT_ARRAY:    init_array = v; break;
      case DT_INIT_ARRAYSZ:  init_arraysz = v; break;
      case DT_ANDROID_RELA:  aps2 = v; break;
      case DT_ANDROID_RELASZ: aps2sz = v; break;
      case DT_REL:
      case DT_ANDROID_REL:
        debugPrintf("%s: unsupported relocation table (tag %#llx)\n", mod->name,
                    (unsigned long long)dyn[i].d_tag);
        return -1;
      default:
        break;
    }
  }
  if (pltrel != DT_RELA && pltrelsz) {
    debugPrintf("%s: DT_PLTREL is not RELA\n", mod->name);
    return -1;
  }
  if (relasz) {
    x->rela = vaddr_ptr(mod, rela, relasz);
    x->rela_count = relasz / sizeof(Elf64_Rela);
  }
  if (pltrelsz) {
    x->jmprel = vaddr_ptr(mod, jmprel, pltrelsz);
    x->jmprel_count = pltrelsz / sizeof(Elf64_Rela);
  }
  if (relrsz) {
    x->relr = vaddr_ptr(mod, relr, relrsz);
    x->relr_size = relrsz;
  }
  if (init_arraysz) {
    x->init_array = vaddr_ptr(mod, init_array, init_arraysz);
    x->init_array_count = init_arraysz / sizeof(uintptr_t);
  }
  if ((relasz && !x->rela) || (pltrelsz && !x->jmprel) || (relrsz && !x->relr) ||
      (init_arraysz && !x->init_array)) {
    debugPrintf("%s: dynamic table points outside the image\n", mod->name);
    return -1;
  }
  if (aps2sz) {
    const uint8_t *packed = vaddr_ptr(mod, aps2, aps2sz);
    if (!packed || decode_aps2(x, packed, aps2sz) < 0) {
      debugPrintf("%s: bad packed relocation table\n", mod->name);
      return -1;
    }
  }
  if (x->init)
    x->init += (uintptr_t)mod->load_base;
  return 0;
}

int so_load(so_module *mod, const char *filename, void *base, size_t max_size) {
  memset(mod, 0, sizeof(*mod));
  const char *slash = strrchr(filename, '/');
  snprintf(mod->name, sizeof(mod->name), "%s", slash ? slash + 1 : filename);

  int rc = read_whole_file(filename, &mod->so_base, &mod->so_size);
  if (rc < 0)
    return rc;

  const Elf64_Ehdr *eh = mod->so_base;
  if (mod->so_size < sizeof(*eh) || memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
      eh->e_ident[EI_CLASS] != ELFCLASS64 || eh->e_ident[EI_DATA] != ELFDATA2LSB ||
      eh->e_machine != EM_AARCH64 || eh->e_type != ET_DYN ||
      eh->e_phentsize != sizeof(Elf64_Phdr) || eh->e_shentsize != sizeof(Elf64_Shdr) ||
      !range_ok(eh->e_phoff, (size_t)eh->e_phnum * sizeof(Elf64_Phdr), mod->so_size) ||
      !range_ok(eh->e_shoff, (size_t)eh->e_shnum * sizeof(Elf64_Shdr), mod->so_size) ||
      eh->e_shstrndx >= eh->e_shnum) {
    debugPrintf("so_load: %s is not an arm64 Android shared object\n", filename);
    rc = -1;
    goto fail;
  }
  if (eh->e_phnum > SO_MAX_SEGMENTS * 2) {
    debugPrintf("so_load: %s has too many program headers (%d)\n", filename, eh->e_phnum);
    rc = -4;
    goto fail;
  }

  mod->elf_hdr = (Elf64_Ehdr *)eh;
  mod->prog_hdr = (Elf64_Phdr *)((uint8_t *)mod->so_base + eh->e_phoff);
  mod->sec_hdr = (Elf64_Shdr *)((uint8_t *)mod->so_base + eh->e_shoff);
  const Elf64_Shdr *shstr = &mod->sec_hdr[eh->e_shstrndx];
  if (!range_ok(shstr->sh_offset, shstr->sh_size, mod->so_size)) {
    rc = -1;
    goto fail;
  }
  mod->shstrtab = (char *)mod->so_base + shstr->sh_offset;
  mod->phnum = eh->e_phnum;
  memcpy(mod->phdr, mod->prog_hdr, mod->phnum * sizeof(Elf64_Phdr));

  so_linux *x = calloc(1, sizeof(*x));
  if (!x) {
    rc = -2;
    goto fail;
  }
  mod->load_memrv = x;
  x->page = (size_t)sysconf(_SC_PAGESIZE);

  size_t top = 0;
  for (int i = 0; i < mod->phnum; i++) {
    const Elf64_Phdr *p = &mod->phdr[i];
    if (p->p_type == PT_TLS) {
      debugPrintf("so_load: %s uses ELF TLS, which this loader does not provide\n", filename);
      rc = -5;
      goto fail;
    }
    if (p->p_type != PT_LOAD)
      continue;
    if (p->p_filesz > p->p_memsz || p->p_memsz > LOAD_MAX || p->p_vaddr > LOAD_MAX - p->p_memsz ||
        !range_ok(p->p_offset, p->p_filesz, mod->so_size)) {
      rc = -1;
      goto fail;
    }
    if (p->p_vaddr + p->p_memsz > top)
      top = p->p_vaddr + p->p_memsz;
  }
  mod->load_size = ALIGN_MEM(top, x->page);
  if (!mod->load_size || (base && mod->load_size > max_size)) {
    rc = -3;
    goto fail;
  }

  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
  if (base)
    flags |= MAP_FIXED_NOREPLACE;
  void *map = mmap(base, mod->load_size, PROT_READ | PROT_WRITE, flags, -1, 0);
  if (map == MAP_FAILED) {
    debugPrintf("so_load: mmap of %zu KB failed: %s\n", mod->load_size / 1024, strerror(errno));
    rc = -2;
    goto fail;
  }
  mod->load_base = mod->load_virtbase = map;

  for (int i = 0; i < mod->phnum; i++) {
    const Elf64_Phdr *p = &mod->phdr[i];
    if (p->p_type == PT_LOAD)
      memcpy((uint8_t *)map + p->p_vaddr, (uint8_t *)mod->so_base + p->p_offset, p->p_filesz);
  }

  size_t dynstr_size = 0;
  for (int i = 0; i < eh->e_shnum; i++) {
    const Elf64_Shdr *s = &mod->sec_hdr[i];
    if (s->sh_name >= shstr->sh_size || !memchr(mod->shstrtab + s->sh_name, 0, shstr->sh_size - s->sh_name))
      continue;
    const char *sh_name = mod->shstrtab + s->sh_name;
    if (strcmp(sh_name, ".dynsym") == 0 && vaddr_ptr(mod, s->sh_addr, s->sh_size) &&
        s->sh_size / sizeof(Elf64_Sym) <= INT_MAX) {
      mod->syms = (Elf64_Sym *)((uint8_t *)map + s->sh_addr);
      mod->num_syms = (int)(s->sh_size / sizeof(Elf64_Sym));
    } else if (strcmp(sh_name, ".dynstr") == 0 && vaddr_ptr(mod, s->sh_addr, s->sh_size)) {
      mod->dynstrtab = (char *)map + s->sh_addr;
      dynstr_size = s->sh_size;
    }
  }
  /* every symbol name is read as mod->dynstrtab + st_name: all of them must
   * be inside a NUL-terminated table */
  int names_ok = mod->syms && mod->dynstrtab && dynstr_size && !mod->dynstrtab[dynstr_size - 1];
  for (int i = 0; names_ok && i < mod->num_syms; i++)
    names_ok = mod->syms[i].st_name < dynstr_size;
  if (!names_ok || parse_dynamic(mod) < 0) {
    debugPrintf("so_load: %s has a damaged symbol table\n", filename);
    rc = -2;
    goto fail_unmap;
  }

  debugPrintf("%s: mapped %zu KB at %p\n", mod->name, mod->load_size / 1024, map);

  mod->next = NULL;
  if (!so_list) {
    so_list = mod;
  } else {
    so_module *m = so_list;
    while (m->next)
      m = m->next;
    m->next = mod;
  }
  return 0;

fail_unmap:
  munmap(mod->load_base, mod->load_size);
  mod->load_base = mod->load_virtbase = NULL;
fail:
  free(mod->load_memrv);
  mod->load_memrv = NULL;
  free(mod->so_base);
  mod->so_base = NULL;
  return rc;
}

static int process_relr(so_module *mod, const Elf64_Xword *relr, size_t relrsz) {
  const uintptr_t base = (uintptr_t)mod->load_base;
  uintptr_t where = 0;
  for (size_t i = 0; i < relrsz / sizeof(Elf64_Xword); i++) {
    const Elf64_Xword entry = relr[i];
    if ((entry & 1) == 0) {
      where = (uintptr_t)entry;
      if (!range_ok(where, 8, mod->load_size))
        return -1;
      *(uint64_t *)(base + where) += base;
      where += 8;
    } else {
      for (int bit = 1; bit < 64; bit++) {
        if (!(entry & (1ull << bit)))
          continue;
        const uintptr_t at = where + (bit - 1) * 8;
        if (!range_ok(at, 8, mod->load_size))
          return -1;
        *(uint64_t *)(base + at) += base;
      }
      where += 63 * 8;
    }
  }
  return 0;
}

static int relocate_table(so_module *mod, Elf64_Rela *rels, size_t count) {
  const uintptr_t base = (uintptr_t)mod->load_base;
  for (size_t j = 0; j < count; j++) {
    const uint32_t type = ELF64_R_TYPE(rels[j].r_info);
    const uint32_t symi = ELF64_R_SYM(rels[j].r_info);
    if (type == R_AARCH64_NONE)
      continue;
    if (!range_ok(rels[j].r_offset, 8, mod->load_size) || symi >= (uint32_t)mod->num_syms) {
      debugPrintf("%s: relocation %zu out of range\n", mod->name, j);
      return -1;
    }
    uintptr_t *ptr = (uintptr_t *)(base + rels[j].r_offset);
    const Elf64_Sym *sym = &mod->syms[symi];
    switch (type) {
      case R_AARCH64_ABS64:
        /* imported ABS64 (RTTI and the like) is bound later, in so_resolve */
        *ptr = (sym->st_shndx == SHN_UNDEF) ? (uintptr_t)rels[j].r_addend
                                            : base + sym->st_value + rels[j].r_addend;
        break;
      case R_AARCH64_RELATIVE:
        *ptr = base + rels[j].r_addend;
        break;
      case R_AARCH64_GLOB_DAT:
      case R_AARCH64_JUMP_SLOT:
        if (sym->st_shndx != SHN_UNDEF)
          *ptr = base + sym->st_value + rels[j].r_addend;
        break;
      default:
        debugPrintf("%s: unsupported relocation type %u\n", mod->name, type);
        return -1;
    }
  }
  return 0;
}

int so_relocate(so_module *mod) {
  so_linux *x = ext(mod);
  if (relocate_table(mod, x->rela, x->rela_count) < 0 ||
      relocate_table(mod, x->aps2, x->aps2_count) < 0 ||
      relocate_table(mod, x->jmprel, x->jmprel_count) < 0 ||
      (x->relr && process_relr(mod, x->relr, x->relr_size) < 0))
    fatal_error("Error: could not relocate %s.", mod->name);
  return 0;
}

static uintptr_t lookup_export(const so_module *mod, const char *name) {
  for (int i = 0; i < mod->num_syms; i++) {
    const Elf64_Sym *s = &mod->syms[i];
    if (s->st_shndx == SHN_UNDEF || ELF64_ST_BIND(s->st_info) == STB_LOCAL)
      continue;
    const char *sname = mod->dynstrtab + s->st_name;
    if (sname[0] == name[0] && strcmp(sname, name) == 0)
      return (uintptr_t)mod->load_base + s->st_value;
  }
  return 0;
}

static int cmp_import(const void *a, const void *b) {
  return strcmp((*(DynLibFunction *const *)a)->symbol, (*(DynLibFunction *const *)b)->symbol);
}

static int cmp_import_key(const void *key, const void *elem) {
  return strcmp((const char *)key, (*(DynLibFunction *const *)elem)->symbol);
}

static int resolve_table(so_module *mod, Elf64_Rela *rels, size_t count,
                         DynLibFunction **index, int num_funcs, int taint) {
  int missing = 0;
  for (size_t j = 0; j < count; j++) {
    const uint32_t type = ELF64_R_TYPE(rels[j].r_info);
    if (type != R_AARCH64_ABS64 && type != R_AARCH64_GLOB_DAT && type != R_AARCH64_JUMP_SLOT)
      continue;
    const Elf64_Sym *sym = &mod->syms[ELF64_R_SYM(rels[j].r_info)];
    if (sym->st_shndx != SHN_UNDEF)
      continue;
    const char *name = mod->dynstrtab + sym->st_name;
    uintptr_t *ptr = (uintptr_t *)((uintptr_t)mod->load_base + rels[j].r_offset);
    uintptr_t addr = 0;

    DynLibFunction **hit = bsearch(name, index, num_funcs, sizeof(*index), cmp_import_key);
    if (hit)
      addr = (*hit)->func;
    for (so_module *m = so_list; !addr && m; m = m->next)
      if (m != mod)
        addr = lookup_export(m, name);
    const int is_object = ELF64_ST_TYPE(sym->st_info) == STT_OBJECT;
    if (!addr && fallback_resolver)
      addr = fallback_resolver(name, is_object);

    if (addr) {
      *ptr = addr + rels[j].r_addend;
      continue;
    }
    if (ELF64_ST_BIND(sym->st_info) == STB_WEAK) {
      *ptr = 0; /* weak undefined resolves to NULL, as on Android */
      continue;
    }
    missing++;
    debugPrintf("%s: unresolved import: %s\n", mod->name, name);
    if (taint)
      *ptr = is_object ? (uintptr_t)unresolved_object : make_trap(name);
  }
  return missing;
}

int so_resolve(so_module *mod, DynLibFunction *funcs, int num_funcs, int taint_missing_imports) {
  so_linux *x = ext(mod);
  DynLibFunction **index = malloc(sizeof(*index) * (num_funcs ? num_funcs : 1));
  if (!index)
    fatal_error("Error: out of memory resolving %s.", mod->name);
  for (int i = 0; i < num_funcs; i++)
    index[i] = &funcs[i];
  qsort(index, num_funcs, sizeof(*index), cmp_import);

  int missing = resolve_table(mod, x->rela, x->rela_count, index, num_funcs, taint_missing_imports);
  missing += resolve_table(mod, x->aps2, x->aps2_count, index, num_funcs, taint_missing_imports);
  missing += resolve_table(mod, x->jmprel, x->jmprel_count, index, num_funcs, taint_missing_imports);
  free(index);

  last_unresolved = missing;
  if (missing)
    debugPrintf("%s: %d unresolved imports\n", mod->name, missing);
  return 0;
}

void so_finalize(so_module *mod) {
  so_linux *x = ext(mod);
  const size_t pages = mod->load_size / x->page;
  uint8_t *prot = calloc(pages, 1);
  if (!prot)
    fatal_error("Error: out of memory in so_finalize.");

  /* a page shared by two segments gets the union of their permissions */
  for (int i = 0; i < mod->phnum; i++) {
    const Elf64_Phdr *p = &mod->phdr[i];
    if (p->p_type != PT_LOAD || !p->p_memsz)
      continue;
    uint8_t want = PROT_READ;
    if (p->p_flags & PF_W) want |= PROT_WRITE;
    if (p->p_flags & PF_X) want |= PROT_EXEC;
    const size_t first = p->p_vaddr / x->page;
    const size_t last = (p->p_vaddr + p->p_memsz - 1) / x->page;
    for (size_t pg = first; pg <= last && pg < pages; pg++)
      prot[pg] |= want;
  }
  for (size_t pg = 0; pg < pages;) {
    size_t end = pg + 1;
    while (end < pages && prot[end] == prot[pg])
      end++;
    const int p = prot[pg] ? prot[pg] : PROT_NONE;
    if (mprotect((uint8_t *)mod->load_base + pg * x->page, (end - pg) * x->page, p) < 0)
      fatal_error("Error: mprotect failed on %s: %s", mod->name, strerror(errno));
    pg = end;
  }
  free(prot);
}

void so_execute_init_array(so_module *mod) {
  so_linux *x = ext(mod);
  if (x->init)
    ((void (*)(void))x->init)();
  for (size_t j = 0; j < x->init_array_count; j++) {
    const uintptr_t fn = x->init_array[j];
    if (fn != 0 && fn != (uintptr_t)-1)
      ((void (*)(void))fn)();
  }
}

static uintptr_t find_defined(so_module *mod, const char *symbol) {
  for (int i = 0; i < mod->num_syms; i++) {
    const Elf64_Sym *s = &mod->syms[i];
    if (s->st_shndx == SHN_UNDEF)
      continue;
    if (strcmp(mod->dynstrtab + s->st_name, symbol) == 0)
      return (uintptr_t)mod->load_base + s->st_value;
  }
  return 0;
}

uintptr_t so_find_addr(so_module *mod, const char *symbol) {
  const uintptr_t addr = find_defined(mod, symbol);
  if (!addr)
    fatal_error("Error: could not find symbol:\n%s\n", symbol);
  return addr;
}

uintptr_t so_find_addr_rx(so_module *mod, const char *symbol) {
  return so_find_addr(mod, symbol);
}

uintptr_t so_try_find_addr_rx(so_module *mod, const char *symbol) {
  return find_defined(mod, symbol);
}

DynLibFunction *so_find_import(DynLibFunction *funcs, int num_funcs, const char *name) {
  for (int i = 0; i < num_funcs; ++i)
    if (!strcmp(funcs[i].symbol, name))
      return &funcs[i];
  return NULL;
}

int so_unload(so_module *mod) {
  if (!mod->load_base)
    return -1;
  so_free_temp(mod);
  munmap(mod->load_base, mod->load_size);
  mod->load_base = mod->load_virtbase = NULL;
  if (mod->load_memrv)
    free(ext(mod)->aps2);
  free(mod->load_memrv);
  mod->load_memrv = NULL;
  if (so_list == mod) {
    so_list = mod->next;
  } else {
    for (so_module *m = so_list; m; m = m->next)
      if (m->next == mod) {
        m->next = mod->next;
        break;
      }
  }
  return 0;
}

const so_module *so_module_at(uintptr_t addr) {
  for (const so_module *m = so_list; m; m = m->next)
    if (addr >= (uintptr_t)m->load_base && addr < (uintptr_t)m->load_base + m->load_size)
      return m;
  return NULL;
}

const char *so_symbol_at(const so_module *mod, uintptr_t addr, uintptr_t *off) {
  const uintptr_t rel = addr - (uintptr_t)mod->load_base;
  const Elf64_Sym *best = NULL;
  for (int i = 0; i < mod->num_syms; i++) {
    const Elf64_Sym *s = &mod->syms[i];
    if (s->st_shndx == SHN_UNDEF || ELF64_ST_TYPE(s->st_info) != STT_FUNC)
      continue;
    if (s->st_value <= rel && (!best || s->st_value > best->st_value))
      best = s;
  }
  if (!best)
    return NULL;
  *off = rel - best->st_value;
  return mod->dynstrtab + best->st_name;
}

/* matches the layout bionic and libunwind expect */
struct so_dl_phdr_info {
  Elf64_Addr dlpi_addr;
  const char *dlpi_name;
  const Elf64_Phdr *dlpi_phdr;
  Elf64_Half dlpi_phnum;
};

int so_dl_iterate_phdr(int (*callback)(void *info, size_t size, void *data), void *data) {
  int ret = 0;
  for (so_module *mod = so_list; mod; mod = mod->next) {
    struct so_dl_phdr_info info;
    info.dlpi_addr = (Elf64_Addr)(uintptr_t)mod->load_base;
    info.dlpi_name = mod->name;
    info.dlpi_phdr = mod->phdr; /* link-time vaddrs; + dlpi_addr = runtime */
    info.dlpi_phnum = (Elf64_Half)mod->phnum;
    ret = callback(&info, sizeof(info), data);
    if (ret)
      break;
  }
  return ret;
}
