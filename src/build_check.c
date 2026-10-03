/* build_check.c -- is this libGame.so the build the hooks were written for?
 *
 * gtasa_nx's patches poke fixed offsets inside functions of the 2.11.311
 * arm64-v8a libGame.so; any other build would be patched in the wrong places
 * and fail in ways that look like random crashes. Without a published hash,
 * the check uses what upstream documented: the load addresses its stub
 * comments give for seven functions, and the original instructions its
 * patch comments describe at nine patch sites.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdio.h>
#include <string.h>

#include "build_check.h"

typedef struct {
  const char *symbol;
  uintptr_t value; /* st_value in the 2.11.311 arm64 build */
} KnownAddress;

/* from the comments in upstream/source/hooks/<name>.s
 * (address of +offset minus offset) */
static const KnownAddress addresses[] = {
  { "_ZN12CPostEffects12MobileRenderEv", 0x5e2c9c },        /* colorfilter_ps2_stub.s */
  { "_ZN8CCoronas6RenderEv", 0x5ce048 },                   /* corona_ps2_stub.s */
  { "_ZN6CPlane20ProcessControlInputsEh", 0x6cc7c8 },       /* cplane_rudder_stub.s */
  { "_ZN11CAutomobile16HydraulicControlEv", 0x6a1f8c },     /* hydraulics_stub.s */
  { "_ZN4CPad18AimWeaponLeftRightEP4CPedPb", 0x49fd2c },    /* hydraulics_stub.s */
  { "_ZN4CPad15AimWeaponUpDownEP4CPedPb", 0x4a0108 },       /* hydraulics_stub.s */
  { "_ZN14MainMenuScreen11AddAllItemsEv", 0x70e8a4 },       /* mainmenu_exit_stub.s */
};

typedef struct {
  const char *symbol;
  uint32_t offset;
  uint32_t mask;
  uint32_t value;
  const char *insn;
} KnownInsn;

/* original instructions named in upstream/source/hooks/game.c and the stubs;
 * encodings checked against GNU as by tests/check_fingerprints.py */
static const KnownInsn insns[] = {
  { "_Z11RenderSceneb", 0x68, 0xffffffff, 0x1e2a2920, "fadd s0, s9, s10" },
  { "_Z16BuildPixelSourcej", 0x244, 0xffffffff, 0x110602c8, "add w8, w22, #0x180" },
  { "_ZN15CTaskSimpleSwim14ProcessEffectsEP4CPed", 0x5a8, 0xffffffff, 0xb940f808, "ldr w8, [x0, #248]" },
  { "_ZN15CTaskSimpleSwim14ProcessEffectsEP4CPed", 0x5ac, 0xffffffff, 0xf9407809, "ldr x9, [x0, #240]" },
  { "_ZN12CPostEffects12MobileRenderEv", 0x614, 0xffffffff, 0xbc1d03b4, "stur s20, [x29, #-48]" },
  { "_ZN11CPopulation9ManagePedEP4CPedRK7CVector", 0x1ec, 0xffffffff, 0x540004cd, "b.le +0x98" },
  { "_ZN14CTrafficLights18DisplayActualLightEP7CEntity", 0x268, 0xff00001f, 0x5400000b, "b.lt" },
  { "_ZN11CAutomobile9PreRenderEv", 0x864, 0xff00001f, 0x5400000b, "b.lt" },
  { "_ZN7CCamera7ProcessEv", 0xdc4, 0xffe0fc00, 0x1e201800, "fdiv" },
};

static const Elf64_Sym *find_sym(const so_module *m, const char *name) {
  for (int i = 0; i < m->num_syms; i++)
    if (m->syms[i].st_shndx != SHN_UNDEF && !strcmp(m->dynstrtab + m->syms[i].st_name, name))
      return &m->syms[i];
  return NULL;
}

int build_check(const so_module *game, BuildCheck *out) {
  memset(out, 0, sizeof(*out));
  for (size_t i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++) {
    out->checked++;
    const Elf64_Sym *s = find_sym(game, addresses[i].symbol);
    if (s && s->st_value == addresses[i].value) {
      out->matched++;
    } else if (!out->first_mismatch[0]) {
      snprintf(out->first_mismatch, sizeof(out->first_mismatch), "%s at %#lx, expected %#lx",
               addresses[i].symbol, s ? (unsigned long)s->st_value : 0ul,
               (unsigned long)addresses[i].value);
    }
  }
  for (size_t i = 0; i < sizeof(insns) / sizeof(insns[0]); i++) {
    out->checked++;
    const Elf64_Sym *s = find_sym(game, insns[i].symbol);
    if (s && s->st_size >= insns[i].offset + 4 &&
        s->st_value + insns[i].offset + 4 <= game->load_size) {
      uint32_t word;
      memcpy(&word, (const uint8_t *)game->load_base + s->st_value + insns[i].offset, 4);
      if ((word & insns[i].mask) == insns[i].value) {
        out->matched++;
        continue;
      }
      if (!out->first_mismatch[0])
        snprintf(out->first_mismatch, sizeof(out->first_mismatch), "%s+%#x is %08x, expected %s",
                 insns[i].symbol, insns[i].offset, word, insns[i].insn);
    } else if (!out->first_mismatch[0]) {
      snprintf(out->first_mismatch, sizeof(out->first_mismatch), "%s missing or too short",
               insns[i].symbol);
    }
  }
  return out->matched == out->checked ? 0 : -1;
}
