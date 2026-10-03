/* test_glcache.c -- the GL state cache must never change what GL ends up doing
 *
 * Two threads, each with its own GLES 3 context on Mesa (surfaceless EGL,
 * llvmpipe): thread A calls the entry points the game is bound to (glw_*,
 * cache on, counters on), thread B calls GL directly. Both run the same
 * random sequence of state changes, object deletions and re-creations,
 * frame boundaries, clears and draws, in lockstep. After every step the GL
 * state of both contexts is read back and compared, and at the end so are
 * the rendered pixels: a call the cache dropped that mattered shows up as a
 * difference at the step that caused it.
 *
 *   test_glcache [steps] [seed]
 *
 * Runs natively on the PC (it tests the cache logic, not the target ABI).
 */

#define _GNU_SOURCE

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include "gl_wrap.h"

int debugPrintf(char *fmt, ...) {
  (void)fmt;
  return 0;
}

enum { NTEX = 4, NCUBE = 2, NPROG = 2, NBUF = 3, NFBO = 2, UNITS = 6, W = 32, H = 32 };
enum { NSNAP = 64 };

typedef struct {
  int kind;
  int a, b, c, d;
  float f[4];
} Op;

typedef struct {
  int wrapped;
  EGLContext ctx;
  GLuint tex[NTEX], cube[NCUBE], prog[NPROG], buf[NBUF], fbo[NFBO], rb[NFBO], vao, vbo;
  GLuint vs, fs;
  GLint snap[NSNAP];
  unsigned char pixels[NFBO][W * H * 4];
} Side;

static EGLDisplay dpy;
static Op op;
static volatile int stop;
static pthread_barrier_t go, done;

/* side A goes through the wrappers, side B straight to GL */
#define CALL(s, fn, ...) do { if ((s)->wrapped) glw_##fn(__VA_ARGS__); else fn(__VA_ARGS__); } while (0)

static const GLenum caps[] = {
  GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE, GL_STENCIL_TEST, GL_SCISSOR_TEST,
  GL_POLYGON_OFFSET_FILL, GL_DITHER, GL_SAMPLE_ALPHA_TO_COVERAGE, GL_SAMPLE_COVERAGE,
  GL_RASTERIZER_DISCARD, /* not modelled by the cache: must pass through */
};
#define NCAPS (int)(sizeof(caps) / sizeof(caps[0]))
static const GLenum factors[] = { GL_ZERO, GL_ONE, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_COLOR };
static const GLenum equations[] = { GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX };
static const GLenum culls[] = { GL_FRONT, GL_BACK, GL_FRONT_AND_BACK };
static const GLenum fronts[] = { GL_CW, GL_CCW };

static const char *vs_src =
  "#version 300 es\n"
  "layout(location = 0) in vec2 pos;\n"
  "out vec2 uv;\n"
  "void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.25, 1.0); }\n";
static const char *fs_src[NPROG] = {
  "#version 300 es\nprecision mediump float;\nuniform sampler2D t;\nuniform vec4 c;\n"
  "in vec2 uv;\nout vec4 o;\nvoid main() { o = texture(t, uv) * c; }\n",
  "#version 300 es\nprecision mediump float;\nuniform vec4 c;\n"
  "in vec2 uv;\nout vec4 o;\nvoid main() { o = vec4(uv, 0.5, 1.0) + c * 0.25; }\n",
};

static GLuint shader(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetShaderInfoLog(s, sizeof(log), NULL, log);
    fprintf(stderr, "shader: %s\n", log);
    exit(2);
  }
  return s;
}

static GLuint make_program(Side *s, int which) {
  GLuint p = glCreateProgram();
  GLuint f = shader(GL_FRAGMENT_SHADER, fs_src[which]);
  glAttachShader(p, s->vs);
  glAttachShader(p, f);
  if (s->wrapped)
    glw_glLinkProgram(p);
  else
    glLinkProgram(p);
  glDeleteShader(f);
  return p;
}

static void make_fbo(Side *s, int i) {
  glGenFramebuffers(1, &s->fbo[i]);
  CALL(s, glBindFramebuffer, GL_FRAMEBUFFER, s->fbo[i]);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s->rb[i]);
}

static void setup(Side *s) {
  static const EGLint attribs[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
  eglBindAPI(EGL_OPENGL_ES_API);
  s->ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attribs);
  if (s->ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s->ctx)) {
    fprintf(stderr, "no GLES 3 context: %#x\n", eglGetError());
    exit(2);
  }
  glw_reset();
  s->vs = shader(GL_VERTEX_SHADER, vs_src);
  for (int i = 0; i < NPROG; i++)
    s->prog[i] = make_program(s, i);
  glGenTextures(NTEX, s->tex);
  glGenTextures(NCUBE, s->cube);
  glGenBuffers(NBUF, s->buf);
  glGenRenderbuffers(NFBO, s->rb);
  for (int i = 0; i < NFBO; i++) {
    glBindRenderbuffer(GL_RENDERBUFFER, s->rb[i]);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
    make_fbo(s, i);
  }
  /* attribute 0 of the default vertex array: a triangle covering most of the target */
  static const float tri[] = { -0.9f, -0.9f, 0.9f, -0.8f, -0.2f, 0.9f };
  glGenBuffers(1, &s->vbo);
  glBindBuffer(GL_ARRAY_BUFFER, s->vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
  glEnableVertexAttribArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glGenVertexArrays(1, &s->vao); /* an empty one to switch to */
  glw_reset(); /* setup went partly around the cache */
}

static void step(Side *s, const Op *o) {
  const int a = o->a, b = o->b, c = o->c, d = o->d;
  switch (o->kind) {
    case 0: CALL(s, glEnable, caps[a % NCAPS]); break;
    case 1: CALL(s, glDisable, caps[a % NCAPS]); break;
    case 2: CALL(s, glBlendFunc, factors[a % 5], factors[b % 5]); break;
    case 3: CALL(s, glBlendFuncSeparate, factors[a % 5], factors[b % 5], factors[c % 5], factors[d % 5]); break;
    case 4: CALL(s, glBlendEquation, equations[a % 5]); break;
    case 5: CALL(s, glBlendEquationSeparate, equations[a % 5], equations[b % 5]); break;
    case 6: CALL(s, glDepthFunc, GL_NEVER + a % 8); break;
    case 7: CALL(s, glDepthMask, (GLboolean)(a & 1)); break;
    case 8: CALL(s, glCullFace, culls[a % 3]); break;
    case 9: CALL(s, glFrontFace, fronts[a % 2]); break;
    case 10: CALL(s, glColorMask, (GLboolean)(a & 1), (GLboolean)(b & 1), (GLboolean)(c & 1), (GLboolean)(d & 1)); break;
    case 11: CALL(s, glActiveTexture, GL_TEXTURE0 + a % UNITS); break;
    case 12: CALL(s, glBindTexture, GL_TEXTURE_2D, a % (NTEX + 1) < NTEX ? s->tex[a % (NTEX + 1)] : 0); break;
    case 13: CALL(s, glBindTexture, GL_TEXTURE_CUBE_MAP, a % (NCUBE + 1) < NCUBE ? s->cube[a % (NCUBE + 1)] : 0); break;
    case 14:
      CALL(s, glDeleteTextures, 1, &s->tex[a % NTEX]);
      glGenTextures(1, &s->tex[a % NTEX]);
      break;
    case 15: CALL(s, glUseProgram, a % (NPROG + 1) < NPROG ? s->prog[a % (NPROG + 1)] : 0); break;
    case 16:
      CALL(s, glDeleteProgram, s->prog[a % NPROG]);
      s->prog[a % NPROG] = make_program(s, a % NPROG);
      break;
    case 17: CALL(s, glBindBuffer, GL_ARRAY_BUFFER, a % (NBUF + 1) < NBUF ? s->buf[a % (NBUF + 1)] : 0); break;
    case 18: CALL(s, glBindBuffer, GL_ELEMENT_ARRAY_BUFFER, a % (NBUF + 1) < NBUF ? s->buf[a % (NBUF + 1)] : 0); break;
    case 19:
      CALL(s, glDeleteBuffers, 1, &s->buf[a % NBUF]);
      glGenBuffers(1, &s->buf[a % NBUF]);
      break;
    case 20: CALL(s, glBindFramebuffer, GL_FRAMEBUFFER, a % (NFBO + 1) < NFBO ? s->fbo[a % (NFBO + 1)] : 0); break;
    case 21:
      CALL(s, glBindFramebuffer, (b & 1) ? GL_DRAW_FRAMEBUFFER : GL_READ_FRAMEBUFFER,
           a % (NFBO + 1) < NFBO ? s->fbo[a % (NFBO + 1)] : 0);
      break;
    case 22:
      CALL(s, glDeleteFramebuffers, 1, &s->fbo[a % NFBO]);
      make_fbo(s, a % NFBO);
      break;
    case 23: CALL(s, glViewport, a % 8, b % 8, 8 + c % 25, 8 + d % 25); break;
    case 24: /* frame boundary (eglSwapBuffers / eglMakeCurrent reset the cache) */
      if (s->wrapped)
        glw_reset();
      break;
    case 25: {
      GLint prog = 0;
      glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
      if (prog)
        CALL(s, glUniform4f, glGetUniformLocation((GLuint)prog, "c"), o->f[0], o->f[1], o->f[2], o->f[3]);
      CALL(s, glDrawArrays, GL_TRIANGLES, 0, 3);
      break;
    }
    case 26:
      glClearColor(o->f[0], o->f[1], o->f[2], o->f[3]);
      CALL(s, glClear, GL_COLOR_BUFFER_BIT);
      break;
    case 27: glBindVertexArray((a & 1) ? s->vao : 0); break;
    case 28: { /* give the texture bound on the active unit 4x4 texels */
      unsigned char texels[4 * 4 * 4];
      for (int i = 0; i < (int)sizeof(texels); i++)
        texels[i] = (unsigned char)(a * 37 + i * 11);
      CALL(s, glTexImage2D, GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
      CALL(s, glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      break;
    }
  }
}
#define NKINDS 29

static const char *const snap_names[NSNAP] = {
  "BLEND", "DEPTH_TEST", "CULL_FACE", "STENCIL_TEST", "SCISSOR_TEST", "POLYGON_OFFSET_FILL",
  "DITHER", "SAMPLE_ALPHA_TO_COVERAGE", "SAMPLE_COVERAGE", "RASTERIZER_DISCARD",
  "BLEND_SRC_RGB", "BLEND_DST_RGB", "BLEND_SRC_ALPHA", "BLEND_DST_ALPHA",
  "BLEND_EQUATION_RGB", "BLEND_EQUATION_ALPHA", "DEPTH_FUNC", "CULL_FACE_MODE", "FRONT_FACE",
  "ACTIVE_TEXTURE", "CURRENT_PROGRAM", "ARRAY_BUFFER_BINDING", "ELEMENT_ARRAY_BUFFER_BINDING",
  "DRAW_FRAMEBUFFER_BINDING", "READ_FRAMEBUFFER_BINDING", "VERTEX_ARRAY_BINDING",
  "DEPTH_WRITEMASK", "COLOR_WRITEMASK.r", "COLOR_WRITEMASK.g", "COLOR_WRITEMASK.b",
  "COLOR_WRITEMASK.a", "VIEWPORT.x", "VIEWPORT.y", "VIEWPORT.w", "VIEWPORT.h",
  "unit0 2D", "unit0 CUBE", "unit1 2D", "unit1 CUBE", "unit2 2D", "unit2 CUBE",
  "unit3 2D", "unit3 CUBE", "unit4 2D", "unit4 CUBE", "unit5 2D", "unit5 CUBE",
};

/* read back the state the cache models (direct GL; leaves the state as found) */
static int snapshot(GLint *v) {
  int k = 0;
  for (int i = 0; i < NCAPS; i++)
    v[k++] = glIsEnabled(caps[i]);
  static const GLenum ints[] = {
    GL_BLEND_SRC_RGB, GL_BLEND_DST_RGB, GL_BLEND_SRC_ALPHA, GL_BLEND_DST_ALPHA,
    GL_BLEND_EQUATION_RGB, GL_BLEND_EQUATION_ALPHA, GL_DEPTH_FUNC, GL_CULL_FACE_MODE,
    GL_FRONT_FACE, GL_ACTIVE_TEXTURE, GL_CURRENT_PROGRAM, GL_ARRAY_BUFFER_BINDING,
    GL_ELEMENT_ARRAY_BUFFER_BINDING, GL_DRAW_FRAMEBUFFER_BINDING, GL_READ_FRAMEBUFFER_BINDING,
    GL_VERTEX_ARRAY_BINDING,
  };
  for (size_t i = 0; i < sizeof(ints) / sizeof(ints[0]); i++)
    glGetIntegerv(ints[i], &v[k++]);
  GLboolean bv[4];
  glGetBooleanv(GL_DEPTH_WRITEMASK, bv);
  v[k++] = bv[0];
  glGetBooleanv(GL_COLOR_WRITEMASK, bv);
  for (int i = 0; i < 4; i++)
    v[k++] = bv[i];
  glGetIntegerv(GL_VIEWPORT, &v[k]);
  k += 4;
  GLint active = GL_TEXTURE0;
  glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
  for (int u = 0; u < UNITS; u++) {
    glActiveTexture(GL_TEXTURE0 + u);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &v[k++]);
    glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, &v[k++]);
  }
  glActiveTexture((GLenum)active);
  return k;
}

static void *side_thread(void *arg) {
  Side *s = arg;
  setup(s);
  for (;;) {
    pthread_barrier_wait(&go);
    if (stop)
      break;
    step(s, &op);
    snapshot(s->snap);
    pthread_barrier_wait(&done);
  }
  /* final image of each framebuffer object */
  for (int i = 0; i < NFBO; i++) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s->fbo[i]);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, s->pixels[i]);
  }
  pthread_barrier_wait(&done);
  return NULL;
}

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static unsigned rnd(void) {
  rng ^= rng << 13;
  rng ^= rng >> 7;
  rng ^= rng << 17;
  return (unsigned)(rng >> 16);
}

static const char *kind_name(int k) {
  static const char *n[NKINDS] = {
    "Enable", "Disable", "BlendFunc", "BlendFuncSeparate", "BlendEquation",
    "BlendEquationSeparate", "DepthFunc", "DepthMask", "CullFace", "FrontFace", "ColorMask",
    "ActiveTexture", "BindTexture(2D)", "BindTexture(CUBE)", "DeleteTextures+Gen", "UseProgram",
    "DeleteProgram+Create", "BindBuffer(ARRAY)", "BindBuffer(ELEMENT)", "DeleteBuffers+Gen",
    "BindFramebuffer", "BindFramebuffer(DRAW/READ)", "DeleteFramebuffers+Create", "Viewport",
    "frame boundary", "Uniform+DrawArrays", "Clear", "BindVertexArray", "TexImage2D",
  };
  return n[k];
}

int main(int argc, char **argv) {
  const int steps = argc > 1 ? atoi(argv[1]) : 20000;
  if (argc > 2)
    rng = (strtoull(argv[2], NULL, 0) + 1) * 0x9e3779b97f4a7c15ull | 1;

  dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
  EGLint major, minor;
  if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) {
    fprintf(stderr, "no surfaceless EGL display\n");
    return 2;
  }
  glw_configure(1, 1);

  static Side sa, sb;
  sa.wrapped = 1;
  pthread_barrier_init(&go, NULL, 3);
  pthread_barrier_init(&done, NULL, 3);
  pthread_t ta, tb;
  pthread_create(&ta, NULL, side_thread, &sa);
  pthread_create(&tb, NULL, side_thread, &sb);

  int diffs = 0, kinds_seen[NKINDS] = { 0 };
  static Op last[NKINDS];
  for (int i = 0; i < steps && !diffs; i++) {
    memset(&op, 0, sizeof(op));
    op.kind = (int)(rnd() % NKINDS);
    if (kinds_seen[op.kind] && (rnd() & 1)) {
      op = last[op.kind]; /* games re-send the same state constantly: half repeats */
    } else {
      op.a = (int)(rnd() % 64);
      op.b = (int)(rnd() % 64);
      if (op.kind == 3 || op.kind == 5) {
        /* half of the separate calls set equal halves (the cache folds those) */
        op.c = (rnd() & 1) ? op.a : (int)(rnd() % 64);
        op.d = (rnd() & 1) ? op.b : (int)(rnd() % 64);
      } else {
        op.c = (int)(rnd() % 64);
        op.d = (int)(rnd() % 64);
      }
      for (int j = 0; j < 4; j++)
        op.f[j] = (float)(rnd() % 256) / 255.0f;
      last[op.kind] = op;
    }
    kinds_seen[op.kind]++;

    pthread_barrier_wait(&go);
    pthread_barrier_wait(&done);
    for (int k = 0; k < NSNAP && snap_names[k]; k++) {
      if (sa.snap[k] != sb.snap[k]) {
        if (!diffs)
          printf("step %d (%s a=%d b=%d c=%d d=%d): state differs\n", i, kind_name(op.kind), op.a,
                 op.b, op.c, op.d);
        printf("  %s: cached %#x, direct %#x\n", snap_names[k], sa.snap[k], sb.snap[k]);
        diffs++;
      }
    }
  }
  stop = 1;
  pthread_barrier_wait(&go);
  pthread_barrier_wait(&done);
  pthread_join(ta, NULL);
  pthread_join(tb, NULL);

  int pixel_diffs = 0;
  for (int i = 0; i < NFBO; i++)
    pixel_diffs += memcmp(sa.pixels[i], sb.pixels[i], sizeof(sa.pixels[i])) != 0;

  GlCounters c;
  glw_counters(&c);
  int lit = 0;
  for (int i = 0; i < NFBO; i++)
    for (int p = 0; p < W * H * 4; p++)
      lit += sb.pixels[i][p] != 0;
  printf("glcache: %d steps, %llu state calls, %llu dropped as redundant, %llu draws\n", steps,
         (unsigned long long)c.state_calls, (unsigned long long)c.state_skipped,
         (unsigned long long)c.draws);
  printf("glcache: final images %s (%d nonzero bytes)\n", pixel_diffs ? "DIFFER" : "identical", lit);
  for (int k = 0; k < NKINDS; k++)
    if (!kinds_seen[k] && !diffs)
      printf("glcache: operation %s never drawn (too few steps)\n", kind_name(k));
  const int ok = !diffs && !pixel_diffs && c.state_skipped > 0 && lit > 0;
  printf("glcache: %s\n", ok ? "ok" : "FAIL");
  return ok ? 0 : 1;
}
