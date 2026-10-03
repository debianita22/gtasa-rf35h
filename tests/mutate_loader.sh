#!/bin/sh
# mutate_loader.sh -- prove the loader test catches broken shims and loader
#
#   tests/mutate_loader.sh BUILD_DIR   (from run-tests.sh: build/*.o, the
#                                       test library and test_loader.o made,
#                                       QEMU, XF and LIBS set)
#
# Each mutant rebuilds one object with one behaviour broken, relinks
# test_loader and runs it under qemu (Cortex-A35); it must fail. These are
# the faults qemu can show: misaligned atomics, a thread without its own
# alternate signal stack, a fortify shim with the wrong argument order, the
# monotonic condition initializer and bionic's stack minimum ignored, names
# no longer found whatever their case, and damaged ELF files the loader
# would have trusted.
set -u

cd "$(dirname "$0")/.."
B=${1:?usage: mutate_loader.sh BUILD_DIR}
: "${QEMU:?run from tests/run-tests.sh}" "${XF:?}" "${LIBS:?}"
CROSS=${CROSS:-aarch64-linux-gnu-}
M="$B/lomutants"
mkdir -p "$M"
fail=0
MCFLAGS="-std=gnu11 -O2 -g -fPIE -fno-omit-frame-pointer -fno-strict-aliasing -march=armv8-a+crc \
  -mtune=cortex-a35 -w -Isrc -Isrc/compat -Iupstream/source -Ibuild $XF -DGTASA_VERSION=\"mutant\""

mutant() {
  name=$1 file=$2
  shift 2
  base=$(basename "$file" .c)
  mkdir -p "$M/$name"
  sed "$@" "$file" > "$M/$name/$base.c"
  if cmp -s "$file" "$M/$name/$base.c"; then
    echo "mutant $name: the mutation no longer applies to $file"
    fail=1
    return
  fi
  # shellcheck disable=SC2086
  ${CROSS}gcc $MCFLAGS -c -o "$M/$name/$base.o" "$M/$name/$base.c" || { fail=1; return; }
  objs=$(ls build/*.o | grep -v -E 'tls_guard|/main.o|up_game|_stub' | sed "s|^build/$base\.o\$|$M/$name/$base.o|")
  # shellcheck disable=SC2086
  ${CROSS}gcc -o "$M/$name/test_loader" build/tls_guard.o "$B/test_loader.o" $objs $LIBS || { fail=1; return; }
  # shellcheck disable=SC2086
  $QEMU "$M/$name/test_loader" "$B/bionic_lib.so" > "$M/$name.log" 2>&1
  rc=$?
  if [ $rc = 0 ]; then
    echo "mutant $name: NOT detected"
    fail=1
  else
    why=$(grep -m1 FAIL "$M/$name.log")
    echo "mutant $name: detected (${why:-exit status $rc})"
  fi
}

mutant pthread-8byte src/bionic_pthread.c \
  -e 's|  return __atomic_load_n((uint32_t \*)obj, __ATOMIC_ACQUIRE);|  return (uint32_t)__atomic_load_n((uint64_t *)obj, __ATOMIC_ACQUIRE);|'
mutant no-thread-altstack src/bionic_pthread.c -e 's|^  crash_thread_init(); .*|  ;|'
mutant fgets-chk-args src/bionic_libc.c \
  -e 's|{ (void)bos; return fgets(s, size, R(f)); }|{ (void)size; return fgets(s, (int)(uintptr_t)f, R((FILE *)bos)); }|'
mutant cond-realtime src/bionic_pthread.c -e 's|cond_new((init \& 0x2) != 0)|cond_new(0)|'
mutant glibc-stack-min src/bionic_pthread.c -e 's|^#define BIONIC_STACK_MIN 16384|#define BIONIC_STACK_MIN PTHREAD_STACK_MIN|'
mutant no-case-fopen src/bionic_libc.c -e "s/  if (!f \&\& mode \&\& mode\[0\] == 'r' \&\& errno == ENOENT) {/  if (0) {/"
mutant case-sensitive-walk src/casefold.c -e 's/        if (!strcasecmp(e->d_name, comp) \&\& /        if (!strcmp(e->d_name, comp) \&\& /'
mutant miss-cached-forever src/casefold.c \
  -e 's|  if (e->asked \&\& e->miss_dir \&\& stat(e->miss_dir, \&st) == 0 \&\& same_time(\&st.st_mtim, \&e->mtime))|  if (e->asked \&\& e->miss_dir)|'
mutant slash-dropped src/casefold.c -e "s|  if (plen \&\& path\[plen - 1\] == '/' \&\& |  if (0 \&\& |"
mutant index-roots-literal src/android.c \
  -e 's|    const char \*dir = case_resolve(roots\[i\], spelled, sizeof(spelled));|    const char *dir = NULL;|'
mutant no-name-check src/loader.c -e 's|    names_ok = mod->syms\[i\].st_name < dynstr_size;|    names_ok = 1;|'
mutant no-wrap-check src/loader.c -e 's/ || p->p_memsz > LOAD_MAX || p->p_vaddr > LOAD_MAX - p->p_memsz / /'

[ $fail = 0 ] && echo "loader mutants: all detected" || { echo "loader mutants: FAILED"; exit 1; }
