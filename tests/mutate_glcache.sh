#!/bin/sh
# mutate_glcache.sh -- prove test_glcache catches broken caches
#
#   tests/mutate_glcache.sh BUILD_DIR
#
# Each mutant is src/gl_wrap.c with one invalidation or comparison removed;
# test_glcache must fail on every one of them. A mutant the test passes, or a
# mutation that no longer applies to the source, fails this script.
set -u

cd "$(dirname "$0")/.."
B=${1:?usage: mutate_glcache.sh BUILD_DIR}
mkdir -p "$B/mutants"
fail=0

mutant() {
  name=$1
  shift
  src="$B/mutants/gl_wrap_$name.c"
  sed "$@" src/gl_wrap.c > "$src"
  if cmp -s src/gl_wrap.c "$src"; then
    echo "mutant $name: the mutation no longer applies to src/gl_wrap.c"
    fail=1
    return
  fi
  gcc -std=gnu11 -O2 -w -Isrc -Isrc/compat -Iupstream/source -o "$B/mutants/t_$name" \
    tests/test_glcache.c "$src" -lEGL -lGLESv2 -lpthread || { fail=1; return; }
  if LIBGL_ALWAYS_SOFTWARE=1 "$B/mutants/t_$name" 20000 > "$B/mutants/$name.log" 2>&1; then
    echo "mutant $name: NOT detected"
    fail=1
  else
    echo "mutant $name: detected ($(head -1 "$B/mutants/$name.log"))"
  fi
}

# a deleted texture that was bound stays "bound" in the cache
mutant deltex -e 's|  glc.tex_ok = 0; /\* a deleted bound texture reverts to 0 \*/|  /* mutant */|'
# binding only the draw or read half keeps the FRAMEBUFFER binding cached
mutant drawread -e 's|    glc.fbo_ok = 0; /\* GL_DRAW/READ_FRAMEBUFFER: one half changes \*/|    /* mutant */|'
# a deleted framebuffer stays "bound"
mutant delfbo -e '/void glw_glDeleteFramebuffers/,/^}/ s|  glc.fbo_ok = 0;|  /* mutant */|'
# glBlendFuncSeparate with unequal halves leaves glBlendFunc cached
mutant blendsep -e '/void glw_glBlendFuncSeparate/,/^}/ s|  glc.blend_ok = 0;|  /* mutant */|'
# same for the blend equation
mutant beqsep -e '/void glw_glBlendEquationSeparate/,/^}/ s|  glc.beq_ok = 0;|  /* mutant */|'
# texture bindings not kept per unit
mutant unit -e 's|const unsigned u = (unsigned)(glc.active - GL_TEXTURE0);|const unsigned u = 0;|'
# two capabilities sharing a bit
mutant capbit -e 's|case GL_SAMPLE_COVERAGE:          return 8;|case GL_SAMPLE_COVERAGE:          return 7;|'
# viewport compared on its origin only
mutant viewport -e 's|glc.vp\[0\] == x && glc.vp\[1\] == y && glc.vp\[2\] == w && glc.vp\[3\] == h|glc.vp[0] == x \&\& glc.vp[1] == y|'

[ $fail = 0 ] && echo "mutants: all detected" || { echo "mutants: FAILED"; exit 1; }
