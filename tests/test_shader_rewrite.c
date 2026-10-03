/* test_shader_rewrite.c -- texture_lod_bias: the shader source rewrite
 *
 * Native (x86-64), against src/gl_wrap.c with glShaderSource wrapped
 * (-Wl,--wrap=glShaderSource) so the test sees what would reach Mesa.
 * The sources are the shapes the game's generator writes (see
 * tools/shaderdump.c for the real ones).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GLES2/gl2.h>

#include "gl_wrap.h"

static int failures;
#define CHECK(cond, ...)                          \
  do {                                            \
    if (!(cond)) {                                \
      printf("  FAIL line %d: ", __LINE__);       \
      printf(__VA_ARGS__);                        \
      printf("\n");                               \
      failures++;                                 \
    }                                             \
  } while (0)

/* what glShaderSource received last */
static char seen[4096];
static int seen_count;
void __wrap_glShaderSource(GLuint s, GLsizei count, const GLchar *const *str, const GLint *len) {
  (void)s;
  seen_count = count;
  seen[0] = 0;
  for (GLsizei i = 0; i < count; i++) {
    const size_t n = len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
    strncat(seen, str[i], n);
  }
}

static void rewrite(const char *in, const char *want, size_t want_changed) {
  char out[1024];
  const size_t changed = glw_drop_lod_bias(in, out);
  CHECK(changed == want_changed, "%zu calls changed in \"%s\", expected %zu", changed, in, want_changed);
  CHECK(!strcmp(out, want), "\"%s\" -> \"%s\", expected \"%s\"", in, out, want);
}

int main(void) {
  rewrite("lowp vec4 diffuseColor = texture2D(Diffuse, Out_Tex0, -0.5);",
          "lowp vec4 diffuseColor = texture2D(Diffuse, Out_Tex0);", 1);
  rewrite("lowp vec4 diffuseColor = texture2D(Diffuse, Out_Tex0, -1.5);",
          "lowp vec4 diffuseColor = texture2D(Diffuse, Out_Tex0);", 1);
  rewrite("a = texture2D(Diffuse, Out_Tex0, -0.5); b = texture2D(Diffuse, Out_Tex0, -1.5);",
          "a = texture2D(Diffuse, Out_Tex0); b = texture2D(Diffuse, Out_Tex0);", 2);
  /* left alone: no bias, other samplers, a positive bias, not a number */
  rewrite("lowp vec4 diffuseColor = texture2D(Diffuse, Out_Tex0);",
          "lowp vec4 diffuseColor = texture2D(Diffuse, Out_Tex0);", 0);
  rewrite("lowp vec4 ReflTexture =  texture2D(EnvMap, ReflPos);",
          "lowp vec4 ReflTexture =  texture2D(EnvMap, ReflPos);", 0);
  rewrite("c = texture2D(Diffuse, Out_Tex0, 0.5);", "c = texture2D(Diffuse, Out_Tex0, 0.5);", 0);
  rewrite("c = texture2D(Diffuse, Out_Tex0, -bias);", "c = texture2D(Diffuse, Out_Tex0, -bias);", 0);
  rewrite("c = texture2D(Diffuse, Out_Tex0, -);", "c = texture2D(Diffuse, Out_Tex0, -);", 0);
  rewrite("", "", 0);
  printf("rewrite          %s\n", failures ? "FAIL" : "ok");

  /* through glShaderSource: pieces with and without lengths come out as
   * one rewritten string; with the bias kept nothing changes */
  const int before = failures;
  const char *pieces[] = { "precision mediump float;", "x = texture2D(Diffuse, Out_Tex0, -0.5);garbage",
                           "y = 1.0;" };
  const GLint lens[] = { -1, 39, -1 };
  glw_keep_lod_bias(0);
  glw_glShaderSource(1, 3, pieces, lens);
  CHECK(seen_count == 1, "the rewritten source went out in %d pieces", seen_count);
  CHECK(!strcmp(seen, "precision mediump float;x = texture2D(Diffuse, Out_Tex0);y = 1.0;"),
        "rewritten source: %s", seen);
  const char *plain[] = { "void main(){gl_FragColor = vec4(1.0);}" };
  glw_glShaderSource(2, 1, plain, NULL);
  CHECK(seen_count == 1 && !strcmp(seen, plain[0]), "a source without the bias changed: %s", seen);
  glw_keep_lod_bias(1);
  glw_glShaderSource(3, 3, pieces, lens);
  CHECK(seen_count == 3, "with the bias kept the pieces were merged (%d)", seen_count);
  CHECK(strstr(seen, "Out_Tex0, -0.5)") != NULL, "with the bias kept it was removed: %s", seen);
  printf("glShaderSource   %s\n", failures != before ? "FAIL" : "ok");

  printf("shader rewrite: %s\n", failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}
