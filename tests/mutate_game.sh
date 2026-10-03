#!/bin/sh
# mutate_game.sh -- prove test_game catches broken game tuning
#
#   tests/mutate_game.sh BUILD_DIR GAME_DIR   (from run-tests.sh: build/*.o and
#                                              test_game.o made; QEMU, XF, LIBS set)
#
# Like mutate_loader.sh, against the user's own libGame.so: each mutant
# rebuilds one object with one behaviour broken and test_game must fail.
set -u

cd "$(dirname "$0")/.."
B=${1:?usage: mutate_game.sh BUILD_DIR GAME_DIR}
GAME=${2:?usage: mutate_game.sh BUILD_DIR GAME_DIR}
: "${QEMU:?run from tests/run-tests.sh}" "${XF:?}" "${LIBS:?}"
CROSS=${CROSS:-aarch64-linux-gnu-}
M="$B/gamemutants"
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
  ${CROSS}gcc -o "$M/$name/test_game" build/tls_guard.o "$B/test_game.o" $objs $LIBS || { fail=1; return; }
  # shellcheck disable=SC2086
  $QEMU "$M/$name/test_game" "$GAME" "$M/$name/shaders" > "$M/$name.log" 2>&1
  rc=$?
  if [ $rc = 0 ]; then
    echo "mutant $name: NOT detected"
    fail=1
  else
    why=$(grep -m1 FAIL "$M/$name.log")
    echo "mutant $name: detected (${why:-exit status $rc})"
  fi
}

mutant profile-ignored src/game_tuning.c -e 's|^  if (value < 0)$|  if (1)|'
mutant overwrites-player src/game_tuning.c -e 's|^  if (!(\*loaded \& 1))$|  if (1)|'
mutant no-range-check src/game_tuning.c -e 's|^  if (value < s->min \|\| value > s->max) {$|  if (0) {|'
mutant game-branch-dropped src/game_tuning.c -e 's|^  if (caps\[CAP_OLD_GPU\] == 1) {$|  if (1) {|'
mutant bias-kept src/gl_wrap.c -e "s|    if (arg\[0\] == '-' \&\& digits \&\& \*q == ')') {|    if (0) {|"

[ $fail = 0 ] && echo "game mutants: all detected" || { echo "game mutants: FAILED"; exit 1; }
