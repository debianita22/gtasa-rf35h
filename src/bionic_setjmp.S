/* bionic_setjmp.S -- setjmp/longjmp for bionic's jmp_buf (long[32])
 *
 * glibc's aarch64 jmp_buf is 312 bytes and its setjmp saves the signal mask
 * past the 256 bytes bionic code reserves, so the game cannot use glibc's.
 * These save the AAPCS64 callee-saved state in the first 22 words and never
 * touch the signal mask; sigsetjmp/siglongjmp are the same functions.
 *
 * Layout: x19..x30 (words 0-11), sp (12), d8..d15 (13-20), cookie (21).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#if defined(__aarch64__)

#define COOKIE 0x6774617361736a6d /* "gtasajm", marks a buffer we filled */

  .text
  .align 2

  .global bionic_setjmp
  .type bionic_setjmp, %function
bionic_setjmp:
  stp x19, x20, [x0, #0]
  stp x21, x22, [x0, #16]
  stp x23, x24, [x0, #32]
  stp x25, x26, [x0, #48]
  stp x27, x28, [x0, #64]
  stp x29, x30, [x0, #80]
  mov x2, sp
  str x2, [x0, #96]
  stp d8, d9, [x0, #104]
  stp d10, d11, [x0, #120]
  stp d12, d13, [x0, #136]
  stp d14, d15, [x0, #152]
  ldr x2, =COOKIE
  str x2, [x0, #168]
  mov w0, #0
  ret
  .size bionic_setjmp, .-bionic_setjmp

  .global bionic_longjmp
  .type bionic_longjmp, %function
bionic_longjmp:
  ldr x2, [x0, #168]
  ldr x3, =COOKIE
  cmp x2, x3
  b.ne 1f
  ldp x19, x20, [x0, #0]
  ldp x21, x22, [x0, #16]
  ldp x23, x24, [x0, #32]
  ldp x25, x26, [x0, #48]
  ldp x27, x28, [x0, #64]
  ldp x29, x30, [x0, #80]
  ldr x2, [x0, #96]
  mov sp, x2
  ldp d8, d9, [x0, #104]
  ldp d10, d11, [x0, #120]
  ldp d12, d13, [x0, #136]
  ldp d14, d15, [x0, #152]
  cmp w1, #0
  csinc w0, w1, wzr, ne   /* longjmp(buf, 0) returns 1, per the C standard */
  ret
1:
  /* a buffer setjmp never filled: crash loudly here, not somewhere random */
  brk #0x6a
  .size bionic_longjmp, .-bionic_longjmp

  .ltorg

#endif

  .section .note.GNU-stack, "", %progbits
