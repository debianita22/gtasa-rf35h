#!/bin/sh
# mutate_rewrite.sh -- prove test_shader_rewrite catches a broken rewrite
#
#   tests/mutate_rewrite.sh BUILD_DIR
set -u

cd "$(dirname "$0")/.."
B=${1:?usage: mutate_rewrite.sh BUILD_DIR}
M="$B/rwmutants"
mkdir -p "$M"
fail=0

mutant() {
  name=$1
  shift
  mkdir -p "$M/$name"
  sed "$@" src/gl_wrap.c > "$M/$name/gl_wrap.c"
  if cmp -s src/gl_wrap.c "$M/$name/gl_wrap.c"; then
    echo "mutant $name: the mutation no longer applies"
    fail=1
    return
  fi
  gcc -std=gnu11 -O2 -w -Isrc -Isrc/compat -Iupstream/source -o "$M/$name/test_shader_rewrite" \
    tests/test_shader_rewrite.c "$M/$name/gl_wrap.c" src/util.c src/tls_guard.c src/platform_config.c \
    upstream/source/config.c -Wl,--wrap=glShaderSource -lGLESv2 -lpthread || { fail=1; return; }
  if "$M/$name/test_shader_rewrite" > "$M/$name.log" 2>&1; then
    echo "mutant $name: NOT detected"
    fail=1
  else
    echo "mutant $name: detected ($(grep -m1 FAIL "$M/$name.log" | sed 's/^ *//'))"
  fi
}

mutant keeps-comma -e 's|      memcpy(o, p, head - 2); /\* up to "Out_Tex0", without ", " \*/|      memcpy(o, p, head);|' \
  -e 's|      o += head - 2;|      o += head;|'
mutant rewrites-when-kept -e 's|  if (!drop_lod_bias \|\| count <= 0 \|\| !str) {|  if (count <= 0 \|\| !str) {|'
mutant ignores-lengths -e 's|    total += len \&\& len\[i\] >= 0 ? (size_t)len\[i\] : strlen(str\[i\]);|    total += strlen(str[i]);|' \
  -e 's|      const size_t n = len \&\& len\[i\] >= 0 ? (size_t)len\[i\] : strlen(str\[i\]);|      const size_t n = strlen(str[i]);|'
mutant any-argument -e "s|    if (arg\[0\] == '-' \&\& digits \&\& \*q == ')') {|    if (*q == ')' \|\| strchr(arg, ')')) { q = strchr(arg, ')');|"

[ $fail = 0 ] && echo "rewrite mutants: all detected" || { echo "rewrite mutants: FAILED"; exit 1; }
