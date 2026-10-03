#!/bin/sh
# mutate_import.sh -- prove test_import.py catches a broken importer
#
#   tests/mutate_import.sh BUILD_DIR
set -u

cd "$(dirname "$0")/.."
B=${1:?usage: mutate_import.sh BUILD_DIR}
M="$B/imutants"
mkdir -p "$M"
fail=0

mutant() {
  name=$1 file=$2
  shift 2
  mkdir -p "$M/$name/src" "$M/$name/build"
  sed "$@" "$file" > "$M/$name/$file"
  if cmp -s "$file" "$M/$name/$file"; then
    echo "mutant $name: the mutation no longer applies to $file"
    fail=1
    return
  fi
  srcs=$(echo src/import.c src/zipx.c | sed "s|$file|$M/$name/$file|")
  gcc -std=gnu11 -O2 -w -Isrc -Isrc/compat -Iupstream/source -o "$M/$name/build/test_import" \
    tests/test_import.c $srcs src/util.c src/platform_config.c src/tls_guard.c upstream/source/config.c -lz ||
    { fail=1; return; }
  if python3 tests/test_import.py "$M/$name/build" > "$M/$name.log" 2>&1; then
    echo "mutant $name: NOT detected"
    fail=1
  else
    echo "mutant $name: detected ($(grep -m1 FAIL "$M/$name.log" | sed 's/^ *//'))"
  fi
}

mutant zip64-extra src/zipx.c -e 's/    if (id == 0x0001) {/    if (0) {/'
mutant zip64-end src/zipx.c -e 's/  if (z->entries == 0xffff || z->cd_size == 0xffffffffu || z->cd_offset == 0xffffffffu) {/  if (0) {/'
mutant no-slip-check src/zipx.c -e 's/  if (!name\[0\] || name\[0\] == .\/.)/  return 1;\n  if (0)/'
mutant no-crc src/zipx.c -e 's/  if (ok \&\& (written != e->size || crc != e->crc)) {/  if (0) {/'
mutant patch-order src/import.c -e 's/  return strcasecmp(bx ? bx + 1 : x->path, by ? by + 1 : y->path);/  return -strcasecmp(bx ? bx + 1 : x->path, by ? by + 1 : y->path);/'
mutant all-libs src/import.c -e 's|  } else if (!strcmp(e->name, LIB_DIR "libc++_shared.so")) {\n    rel = "libc++_shared.so";|XX|' -e 's|    rel = e->name + 7;|    rel = e->name;|'

mutant no-lock src/import.c -e 's/  if (import_lock(data_dir) < 0) {/  if (0) {/'
mutant lock-own-fd src/import.c -e 's/  if (inherited \&\& \*inherited) {/  if (0) {/'

[ $fail = 0 ] && echo "import mutants: all detected" || { echo "import mutants: FAILED"; exit 1; }
