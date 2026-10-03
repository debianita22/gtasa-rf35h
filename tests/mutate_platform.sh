#!/bin/sh
# mutate_platform.sh -- prove test_platform catches broken platform code
#
#   tests/mutate_platform.sh BUILD_DIR [sway]
#                    (from test_platform.sh: compositor running, PLATFORM_SRCS
#                    and CFLAGS set; with "sway", SWAY_ENV too)
#
# Each mutant breaks one behaviour test_platform claims to check; the test
# (both modes under weston; the scaled mode under sway for the sway mutants)
# must fail on every one. A mutant the test passes, or a mutation that no
# longer applies to the source, fails this script.
set -u

cd "$(dirname "$0")/.."
B=${1:?usage: mutate_platform.sh BUILD_DIR [sway]}
ON=${2:-weston}
: "${PLATFORM_SRCS:?run from tests/test_platform.sh}" "${CFLAGS:?}"
M="$B/pmutants"
mkdir -p "$M"
fail=0

mutant() {
  name=$1 file=$2
  shift 2
  mkdir -p "$M/$name/src"
  sed "$@" "$file" > "$M/$name/$file"
  if cmp -s "$file" "$M/$name/$file"; then
    echo "mutant $name: the mutation no longer applies to $file"
    fail=1
    return
  fi
  srcs=$(echo $PLATFORM_SRCS | sed "s|$file|$M/$name/$file|")
  gcc $CFLAGS -w -o "$M/$name/test_platform" tests/test_platform.c $srcs \
    -Wl,--wrap=eglMakeCurrent,--wrap=eglSwapInterval \
    -lSDL2 -lwayland-egl -lwayland-client -lEGL -lGLESv2 -lm -lpthread || { fail=1; return; }
  if [ "$ON" = sway ]; then
    env $SWAY_ENV "$M/$name/test_platform" scaled > "$M/$name.log" 2>&1
  else
    "$M/$name/test_platform" > "$M/$name.log" 2>&1 &&
      "$M/$name/test_platform" scaled >> "$M/$name.log" 2>&1
  fi
  rc=$?
  if [ $rc = 0 ]; then
    echo "mutant $name: NOT detected"
    fail=1
  else
    why=$(grep -m1 FAIL "$M/$name.log" | sed 's/^ *//')
    echo "mutant $name: detected (${why:-exit status $rc})"
  fi
}

if [ "$ON" = sway ]; then
  M="$B/pmutants-sway"
  mutant output-size-ignored src/platform.c -e 's|    output_logical_size(\&P.out_w, \&P.out_h);|    ;|'
  mutant output-transform-ignored src/platform.c -e 's|  if (P.out_transform \& 1) {|  if (0) {|'
  [ $fail = 0 ] && echo "platform mutants (sway): all detected" || { echo "platform mutants (sway): FAILED"; exit 1; }
  exit 0
fi

mutant egl-dedup src/egl_wrap.c -e '/current.ctx == ctx)$/{n;s/return EGL_TRUE;/;/}'
mutant egl-interval src/egl_wrap.c -e 's/    interval = pconfig.vsync;/    ;/'
mutant egl-token src/egl_wrap.c -e 's/if ((void \*)win == platform_anative_window() || !win)/if (0)/'
mutant pad-map src/gamenative.c -e 's/{ PAD_A, 0 }, { PAD_B, 1 }/{ PAD_A, 1 }, { PAD_B, 0 }/'
mutant pad-edge src/gamenative.c -e 's/    g_escape_pressed = 1;/    ;/'
mutant pad-swap src/gamenative.c -e 's/s.buttons = swap_bits(s.buttons, PAD_A, PAD_B);/;/'
mutant deadzone src/platform.c -e 's/if (mag <= dz) {/if (mag < 0) {/'
mutant exit-combo src/gamenative.c -e 's/return now - combo_since >= 1000000000ull;/return 1;/'
mutant pacer-catchup src/util.c -e 's|    p->next = end; /\* a frame or more behind: do not try to catch up \*/|    ;|'
mutant tune-group src/util.c -e 's|    if (group >= 0 \&\& tunes\[i\].group != group)|    if (0)|'
mutant tune-first-value src/util.c -e 's|      return write_line(path, value); /\* the first old value is the one to restore \*/|      { tunes[i].path[0] = 0; break; }|'
mutant no-viewport src/platform.c -e 's/  if (scaled \&\& !P.viewport)/  if (0)/'
mutant no-destination src/platform.c -e 's|    wp_viewport_set_destination(P.viewport, scaled ? P.out_w : -1, scaled ? P.out_h : -1);|    ;|'
mutant scale-ignored src/platform.c -e 's|    P.w = even(P.out_w \* scale / 100);|    P.w = P.out_w;|'
mutant geometry-ignored src/platform.c -e 's|  wl_egl_window_resize(P.egl_window, w, h, 0, 0);|  ;|'
mutant no-fullscreen src/platform.c -e 's|  xdg_toplevel_set_fullscreen(P.toplevel, NULL);|  ;|'
mutant pump-spin src/platform.c -e 's|    if (wl_display_dispatch_pending(d) < 0)|    if (wl_display_dispatch_pending(d) < -1)|'
mutant geometry0-output src/platform.c -e 's|    w = P.base_w;|    w = P.out_w;|' -e 's|    h = P.base_h;|    h = P.out_h;|'
mutant no-output src/platform.c -e 's|    P.output = wl_registry_bind(reg, name, \&wl_output_interface, min_u32(version, 2));|    return;|'

[ $fail = 0 ] && echo "platform mutants: all detected" || { echo "platform mutants: FAILED"; exit 1; }
