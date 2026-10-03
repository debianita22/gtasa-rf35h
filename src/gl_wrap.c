/* gl_wrap.c -- redundant-state filter and counters in front of Mesa
 *
 * RenderWare and the mobile renderer re-send the same GL state many times
 * per frame; on the RK3326 every call that reaches Mesa costs CPU time on
 * the render thread (and, with glthread, a marshalled command). The filter
 * idea and its invalidation rules come from gtasa_nx's imports.c; here the
 * cache is per thread (one per current context), covers a few more setters
 * (blend equation, buffer and framebuffer bindings, viewport, 16 texture
 * units), and counts what goes through for the stats log.
 *
 * Correctness rules, as upstream: the cache is dropped on every real
 * eglMakeCurrent and at every frame boundary; deleting an object drops the
 * bindings that might name it; setters with arguments we do not model pass
 * through and invalidate what they could change.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <string.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>

#include "gl_wrap.h"
#include "util.h"

static int cache_on;
static int count_on;
static int force_trilinear;
static int skip_get_error;
static GlCounters counters;

#define COUNT(field, n) \
  do { if (count_on) __atomic_fetch_add(&counters.field, (uint64_t)(n), __ATOMIC_RELAXED); } while (0)

void glw_configure(int state_cache, int count) {
  cache_on = state_cache;
  count_on = count;
}

void glw_force_trilinear(int on) { force_trilinear = on; }
void glw_skip_get_error(int on) { skip_get_error = on; }

static int drop_lod_bias;

void glw_keep_lod_bias(int keep) { drop_lod_bias = !keep; }

void glw_counters(GlCounters *out) {
  uint64_t *dst = (uint64_t *)out;
  const uint64_t *src = (const uint64_t *)&counters;
  for (size_t i = 0; i < sizeof(*out) / sizeof(uint64_t); i++)
    dst[i] = __atomic_load_n(&src[i], __ATOMIC_RELAXED);
}

#define UNITS 16

typedef struct {
  uint32_t caps_known, caps_on;
  uint8_t blend_ok, beq_ok, dfunc_ok, dmask_ok, cull_ok, front_ok, cmask_ok;
  uint8_t active_ok, prog_ok, abuf_ok, fbo_ok, vp_ok;
  GLenum bsrc, bdst, beq, dfunc, cull, front, active;
  GLboolean dmask, cmask[4];
  GLuint prog, abuf, fbo;
  GLint vp[4];
  uint32_t tex_ok;
  GLuint tex[UNITS];
} GlCache;

static __thread GlCache glc;

void glw_reset(void) { memset(&glc, 0, sizeof(glc)); }

static int cap_bit(GLenum cap) {
  switch (cap) {
    case GL_BLEND:                    return 0;
    case GL_DEPTH_TEST:               return 1;
    case GL_CULL_FACE:                return 2;
    case GL_STENCIL_TEST:             return 3;
    case GL_SCISSOR_TEST:             return 4;
    case GL_POLYGON_OFFSET_FILL:      return 5;
    case GL_DITHER:                   return 6;
    case GL_SAMPLE_ALPHA_TO_COVERAGE: return 7;
    case GL_SAMPLE_COVERAGE:          return 8;
    default:                          return -1;
  }
}

/* returns 1 when the call is redundant and has been counted as skipped */
static inline int skip(int redundant) {
  COUNT(state_calls, 1);
  if (cache_on && redundant) {
    COUNT(state_skipped, 1);
    return 1;
  }
  return 0;
}

static void set_cap(GLenum cap, int on) {
  const int b = cap_bit(cap);
  if (b >= 0) {
    const uint32_t m = 1u << b;
    if (skip((glc.caps_known & m) && !!(glc.caps_on & m) == on))
      return;
    glc.caps_known |= m;
    glc.caps_on = on ? (glc.caps_on | m) : (glc.caps_on & ~m);
  }
  if (on)
    glEnable(cap);
  else
    glDisable(cap);
}

void glw_glEnable(GLenum cap) { set_cap(cap, 1); }
void glw_glDisable(GLenum cap) { set_cap(cap, 0); }

void glw_glBlendFunc(GLenum s, GLenum d) {
  if (skip(glc.blend_ok && glc.bsrc == s && glc.bdst == d))
    return;
  glc.blend_ok = 1;
  glc.bsrc = s;
  glc.bdst = d;
  glBlendFunc(s, d);
}

void glw_glBlendFuncSeparate(GLenum sc, GLenum dc, GLenum sa, GLenum da) {
  if (sc == sa && dc == da) {
    glw_glBlendFunc(sc, dc); /* same state as glBlendFunc(sc, dc) */
    return;
  }
  COUNT(state_calls, 1);
  glc.blend_ok = 0;
  glBlendFuncSeparate(sc, dc, sa, da);
}

void glw_glBlendEquation(GLenum e) {
  if (skip(glc.beq_ok && glc.beq == e))
    return;
  glc.beq_ok = 1;
  glc.beq = e;
  glBlendEquation(e);
}

void glw_glBlendEquationSeparate(GLenum ec, GLenum ea) {
  if (ec == ea) {
    glw_glBlendEquation(ec);
    return;
  }
  COUNT(state_calls, 1);
  glc.beq_ok = 0;
  glBlendEquationSeparate(ec, ea);
}

void glw_glDepthFunc(GLenum f) {
  if (skip(glc.dfunc_ok && glc.dfunc == f))
    return;
  glc.dfunc_ok = 1;
  glc.dfunc = f;
  glDepthFunc(f);
}

void glw_glDepthMask(GLboolean m) {
  if (skip(glc.dmask_ok && glc.dmask == m))
    return;
  glc.dmask_ok = 1;
  glc.dmask = m;
  glDepthMask(m);
}

void glw_glCullFace(GLenum m) {
  if (skip(glc.cull_ok && glc.cull == m))
    return;
  glc.cull_ok = 1;
  glc.cull = m;
  glCullFace(m);
}

void glw_glFrontFace(GLenum m) {
  if (skip(glc.front_ok && glc.front == m))
    return;
  glc.front_ok = 1;
  glc.front = m;
  glFrontFace(m);
}

void glw_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
  if (skip(glc.cmask_ok && glc.cmask[0] == r && glc.cmask[1] == g && glc.cmask[2] == b &&
           glc.cmask[3] == a))
    return;
  glc.cmask_ok = 1;
  glc.cmask[0] = r;
  glc.cmask[1] = g;
  glc.cmask[2] = b;
  glc.cmask[3] = a;
  glColorMask(r, g, b, a);
}

void glw_glActiveTexture(GLenum unit) {
  if (skip(glc.active_ok && glc.active == unit))
    return;
  glc.active_ok = 1;
  glc.active = unit;
  glActiveTexture(unit);
}

void glw_glBindTexture(GLenum target, GLuint tex) {
  if (target == GL_TEXTURE_2D && glc.active_ok) {
    const unsigned u = (unsigned)(glc.active - GL_TEXTURE0);
    if (u < UNITS) {
      if (skip((glc.tex_ok & (1u << u)) && glc.tex[u] == tex))
        return;
      glc.tex_ok |= 1u << u;
      glc.tex[u] = tex;
    }
  }
  COUNT(tex_binds, 1);
  glBindTexture(target, tex);
}

void glw_glDeleteTextures(GLsizei n, const GLuint *t) {
  glc.tex_ok = 0; /* a deleted bound texture reverts to 0 */
  glDeleteTextures(n, t);
}

void glw_glUseProgram(GLuint p) {
  if (skip(glc.prog_ok && glc.prog == p))
    return;
  glc.prog_ok = 1;
  glc.prog = p;
  COUNT(prog_binds, 1);
  glUseProgram(p);
}

void glw_glDeleteProgram(GLuint p) {
  if (glc.prog_ok && glc.prog == p)
    glc.prog_ok = 0;
  glDeleteProgram(p);
}

void glw_glLinkProgram(GLuint p) {
  glLinkProgram(p); /* relinking the current program keeps it current */
}

void glw_glBindBuffer(GLenum target, GLuint b) {
  if (target == GL_ARRAY_BUFFER) {
    if (skip(glc.abuf_ok && glc.abuf == b))
      return;
    glc.abuf_ok = 1;
    glc.abuf = b;
  }
  glBindBuffer(target, b);
}

void glw_glDeleteBuffers(GLsizei n, const GLuint *b) {
  glc.abuf_ok = 0;
  glDeleteBuffers(n, b);
}

void glw_glBindFramebuffer(GLenum target, GLuint fb) {
  if (target == GL_FRAMEBUFFER) {
    if (skip(glc.fbo_ok && glc.fbo == fb))
      return;
    glc.fbo_ok = 1;
    glc.fbo = fb;
  } else {
    glc.fbo_ok = 0; /* GL_DRAW/READ_FRAMEBUFFER: one half changes */
  }
  COUNT(fbo_binds, 1);
  glBindFramebuffer(target, fb);
}

void glw_glDeleteFramebuffers(GLsizei n, const GLuint *fb) {
  glc.fbo_ok = 0;
  glDeleteFramebuffers(n, fb);
}

void glw_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
  if (skip(glc.vp_ok && glc.vp[0] == x && glc.vp[1] == y && glc.vp[2] == w && glc.vp[3] == h))
    return;
  glc.vp_ok = 1;
  glc.vp[0] = x;
  glc.vp[1] = y;
  glc.vp[2] = w;
  glc.vp[3] = h;
  glViewport(x, y, w, h);
}

void glw_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
  COUNT(draws, 1);
  COUNT(vertices, count);
  glDrawArrays(mode, first, count);
}

void glw_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *idx) {
  COUNT(draws, 1);
  COUNT(vertices, count);
  glDrawElements(mode, count, type, idx);
}

void glw_glClear(GLbitfield mask) {
  COUNT(clears, 1);
  glClear(mask);
}

void glw_glCompileShader(GLuint s) {
  COUNT(compiles, 1);
  glCompileShader(s);
}

/* The game's shader generator (BuildPixelSource) samples the diffuse
 * texture with a -0.5 LOD bias (-1.5 for some materials): mip levels half a
 * step larger than the 640x480 picture needs. Without it the G31 fetches
 * from smaller levels: less memory traffic and less shimmer. */
size_t glw_drop_lod_bias(const char *in, char *out) {
  static const char call[] = "texture2D(Diffuse, Out_Tex0, ";
  const size_t call_len = sizeof(call) - 1;
  size_t changed = 0;
  const char *p = in;
  char *o = out;
  for (;;) {
    const char *hit = strstr(p, call);
    if (!hit)
      break;
    const char *arg = hit + call_len, *q = arg;
    int digits = 0;
    if (*q == '-')
      for (q++; (*q >= '0' && *q <= '9') || *q == '.'; q++)
        digits++;
    const size_t head = (size_t)(hit - p) + call_len;
    if (arg[0] == '-' && digits && *q == ')') {
      memcpy(o, p, head - 2); /* up to "Out_Tex0", without ", " */
      o += head - 2;
      *o++ = ')';
      p = q + 1;
      changed++;
    } else {
      memcpy(o, p, head);
      o += head;
      p = arg;
    }
  }
  strcpy(o, p);
  return changed;
}

void glw_glShaderSource(GLuint s, GLsizei count, const GLchar *const *str, const GLint *len) {
  if (!drop_lod_bias || count <= 0 || !str) {
    glShaderSource(s, count, str, len);
    return;
  }
  size_t total = 0;
  for (GLsizei i = 0; i < count; i++)
    total += len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
  char *src = malloc(total + 1), *out = malloc(total + 1);
  size_t changed = 0;
  if (src && out) {
    char *w = src;
    for (GLsizei i = 0; i < count; i++) {
      const size_t n = len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
      memcpy(w, str[i], n);
      w += n;
    }
    *w = 0;
    changed = glw_drop_lod_bias(src, out);
  }
  if (changed) {
    const GLchar *one = out;
    glShaderSource(s, 1, &one, NULL);
  } else {
    glShaderSource(s, count, str, len);
  }
  free(src);
  free(out);
}

static uint64_t pixel_bytes(GLsizei w, GLsizei h, GLenum format, GLenum type) {
  unsigned bpp = 4;
  if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 ||
      type == GL_UNSIGNED_SHORT_5_5_5_1)
    bpp = 2;
  else if (format == GL_RGB)
    bpp = 3;
  else if (format == GL_LUMINANCE_ALPHA)
    bpp = 2;
  else if (format == GL_LUMINANCE || format == GL_ALPHA)
    bpp = 1;
  return (uint64_t)w * (uint64_t)h * bpp;
}

void glw_glTexImage2D(GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLint b, GLenum f,
                      GLenum y, const void *p) {
  COUNT(tex_upload, pixel_bytes(w, h, f, y));
  glTexImage2D(t, l, i, w, h, b, f, y, p);
}

void glw_glTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f,
                         GLenum ty, const void *p) {
  COUNT(tex_upload, pixel_bytes(w, h, f, ty));
  glTexSubImage2D(t, l, x, y, w, h, f, ty, p);
}

void glw_glCompressedTexImage2D(GLenum t, GLint l, GLenum i, GLsizei w, GLsizei h, GLint b,
                                GLsizei s, const void *d) {
  COUNT(tex_upload, s);
  glCompressedTexImage2D(t, l, i, w, h, b, s, d);
}

void glw_glCompressedTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h,
                                   GLenum f, GLsizei s, const void *d) {
  COUNT(tex_upload, s);
  glCompressedTexSubImage2D(t, l, x, y, w, h, f, s, d);
}

void glw_glTexParameteri(GLenum target, GLenum pname, GLint value) {
  if (force_trilinear && value == GL_LINEAR_MIPMAP_NEAREST)
    value = GL_LINEAR_MIPMAP_LINEAR;
  glTexParameteri(target, pname, value);
}

void glw_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
  COUNT(buf_upload, size);
  glBufferData(target, size, data, usage);
}

void glw_glBufferSubData(GLenum target, GLintptr off, GLsizeiptr size, const void *data) {
  COUNT(buf_upload, size);
  glBufferSubData(target, off, size, data);
}

void glw_glUniform1f(GLint l, GLfloat a) { COUNT(uniforms, 1); glUniform1f(l, a); }
void glw_glUniform2f(GLint l, GLfloat a, GLfloat b) { COUNT(uniforms, 1); glUniform2f(l, a, b); }
void glw_glUniform3f(GLint l, GLfloat a, GLfloat b, GLfloat c) { COUNT(uniforms, 1); glUniform3f(l, a, b, c); }
void glw_glUniform4f(GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d) { COUNT(uniforms, 1); glUniform4f(l, a, b, c, d); }
void glw_glUniform1i(GLint l, GLint a) { COUNT(uniforms, 1); glUniform1i(l, a); }
void glw_glUniform1fv(GLint l, GLsizei n, const GLfloat *v) { COUNT(uniforms, 1); glUniform1fv(l, n, v); }
void glw_glUniform2fv(GLint l, GLsizei n, const GLfloat *v) { COUNT(uniforms, 1); glUniform2fv(l, n, v); }
void glw_glUniform3fv(GLint l, GLsizei n, const GLfloat *v) { COUNT(uniforms, 1); glUniform3fv(l, n, v); }
void glw_glUniform4fv(GLint l, GLsizei n, const GLfloat *v) { COUNT(uniforms, 1); glUniform4fv(l, n, v); }
void glw_glUniformMatrix3fv(GLint l, GLsizei n, GLboolean t, const GLfloat *v) { COUNT(uniforms, 1); glUniformMatrix3fv(l, n, t, v); }
void glw_glUniformMatrix4fv(GLint l, GLsizei n, GLboolean t, const GLfloat *v) { COUNT(uniforms, 1); glUniformMatrix4fv(l, n, t, v); }

void glw_glGetShaderInfoLog(GLuint shader, GLsizei max, GLsizei *len, GLchar *log) {
  glGetShaderInfoLog(shader, max, len, log);
  if (log && log[0])
    debugPrintf("shader %u info log:\n%s\n", shader, log);
}

/* ---- queries ---------------------------------------------------------------------- */

GLenum glw_glGetError(void) {
  if (skip_get_error)
    return GL_NO_ERROR; /* MESA_NO_ERROR context: there is nothing to report */
  COUNT(syncs, 1);
  return glGetError();
}

void glw_glGetIntegerv(GLenum pname, GLint *v) { COUNT(syncs, 1); glGetIntegerv(pname, v); }
void glw_glGetFloatv(GLenum pname, GLfloat *v) { COUNT(syncs, 1); glGetFloatv(pname, v); }
void glw_glGetBooleanv(GLenum pname, GLboolean *v) { COUNT(syncs, 1); glGetBooleanv(pname, v); }
GLboolean glw_glIsEnabled(GLenum cap) { COUNT(syncs, 1); return glIsEnabled(cap); }
GLint glw_glGetUniformLocation(GLuint p, const GLchar *n) { COUNT(syncs, 1); return glGetUniformLocation(p, n); }
GLint glw_glGetAttribLocation(GLuint p, const GLchar *n) { COUNT(syncs, 1); return glGetAttribLocation(p, n); }
GLenum glw_glCheckFramebufferStatus(GLenum t) { COUNT(syncs, 1); return glCheckFramebufferStatus(t); }
void glw_glGetShaderiv(GLuint s, GLenum pname, GLint *v) { COUNT(syncs, 1); glGetShaderiv(s, pname, v); }
void glw_glGetProgramiv(GLuint p, GLenum pname, GLint *v) { COUNT(syncs, 1); glGetProgramiv(p, pname, v); }
void glw_glFinish(void) { COUNT(syncs, 1); glFinish(); }
void glw_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void *p) {
  COUNT(syncs, 1);
  glReadPixels(x, y, w, h, f, t, p);
}
