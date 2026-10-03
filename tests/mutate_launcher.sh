#!/bin/sh
# mutate_launcher.sh -- prove test_launcher catches a broken launcher core
#
#   tests/mutate_launcher.sh BUILD_DIR
set -u

cd "$(dirname "$0")/.."
B=${1:?usage: mutate_launcher.sh BUILD_DIR}
M="$B/lmutants"
SRC=packaging/lakka/gtasa/launcher/gtasa_libretro.c
mkdir -p "$M"
fail=0

mutant() {
  name=$1
  shift
  mkdir -p "$M/$name"
  sed "$@" "$SRC" > "$M/$name/gtasa_libretro.c"
  if cmp -s "$SRC" "$M/$name/gtasa_libretro.c"; then
    echo "mutant $name: the mutation no longer applies"
    fail=1
    return
  fi
  cp packaging/lakka/gtasa/launcher/libretro.h "$M/$name/"
  gcc -std=gnu11 -O2 -w -fPIC -shared -o "$M/$name/core.so" "$M/$name/gtasa_libretro.c" || { fail=1; return; }
  rm -rf "$M/$name/work"
  if "$B/test_launcher" "$M/$name/core.so" "$M/$name/work" > "$M/$name.log" 2>&1; then
    echo "mutant $name: NOT detected"
    fail=1
  else
    echo "mutant $name: detected ($(grep -m1 FAIL "$M/$name.log" | sed 's/^ *//'))"
  fi
}

mutant unchecked-options -e 's/        return v;$/        return v;\n  return v;/'
mutant ignore-last-error -e 's/        if (line\[0\]) {/        if (0) {/'
mutant no-start-button -e 's/  const int edge = held \&\& !start_held;/  const int edge = 0;/'
mutant no-progress -e 's/      } else if (!strncmp(line, "run ", 4)) {/      } else if (0) {/'
mutant start-failure-ignored -e 's/      if (child_done() \&\& child_status > 0) {/      if (0) {/'
mutant wrong-default -e 's/checked("gtasa_scale", "100")/checked("gtasa_scale", "90")/'
mutant installed-first -e 's/      } else if (archives_waiting() \&\& !skip_import) {/      } else if (archives_waiting() \&\& !skip_import \&\& !installed()) {/'
mutant no-import -e 's/      } else if (archives_waiting() \&\& !skip_import) {/      } else if (0) {/'
mutant no-attach -e 's/      if (import_running()) {/      if (0) {/'
mutant race-not-followed -e 's/      if (done \&\& !attached \&\& (child_status == 3 || (child_status < 0 \&\& import_running()))) {/      if (0) {/'
mutant interrupted-unnamed -e 's/: !strncmp(line, "run ", 4) ? "interrotta prima della fine"/: 0 ? ""/'
mutant reset-restarts -e 's/^  if (child_done())$/  if (1)/'
mutant unknown-status-failed -e 's/        if ((attached || child_status <= 0) \&\& !strncmp(line, "done ", 5)) {/        if ((attached || child_status == 0) \&\& !strncmp(line, "done ", 5)) {/'
mutant unknown-start-failed -e 's/      if (child_done() \&\& child_status > 0) {/      if (child_done() \&\& child_status != 0) {/'
mutant no-skip -e 's/        skip_import = import_failed \&\& installed();/        skip_import = 0;/'

[ $fail = 0 ] && echo "launcher mutants: all detected" || { echo "launcher mutants: FAILED"; exit 1; }
