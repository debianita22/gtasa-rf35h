/* gl_wrap.h -- GL entry points the game is bound to, with the state cache
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef GTASA_GL_WRAP_H
#define GTASA_GL_WRAP_H

#include <stddef.h>
#include <stdint.h>
#include <GLES2/gl2.h>

typedef struct {
  uint64_t draws;          /* glDraw* calls */
  uint64_t vertices;       /* vertices or indices submitted */
  uint64_t state_calls;    /* cached state setters called by the game */
  uint64_t state_skipped;  /* ... of which dropped as redundant */
  uint64_t uniforms;       /* glUniform* */
  uint64_t tex_binds;      /* glBindTexture reaching the driver */
  uint64_t prog_binds;     /* glUseProgram reaching the driver */
  uint64_t fbo_binds;      /* glBindFramebuffer reaching the driver */
  uint64_t tex_upload;     /* bytes through glTex*Image2D */
  uint64_t buf_upload;     /* bytes through glBuffer*Data */
  uint64_t compiles;       /* glCompileShader */
  uint64_t clears;         /* glClear */
  uint64_t syncs;          /* queries that wait for Mesa's glthread worker */
} GlCounters;

/* Turn the state cache and the counters on or off (both default off). */
void glw_configure(int state_cache, int counters);

/* Forget the calling thread's cached state: after a real context switch,
 * and at every frame boundary (the overlay draws behind the cache). */
void glw_reset(void);

/* Cumulative counters since start. */
void glw_counters(GlCounters *out);

/* trilinear override, from gtasa_nx's update_imports() */
void glw_force_trilinear(int on);

/* with gl_no_error: glGetError answers GL_NO_ERROR without a glthread sync */
void glw_skip_get_error(int on);

/* texture_lod_bias: 1 keeps the game's negative LOD bias on the diffuse
 * texture, 0 takes it out of the shader sources (the default is to keep). */
void glw_keep_lod_bias(int keep);

/* The rewrite glShaderSource applies with the bias off: every
 * "texture2D(Diffuse, Out_Tex0, -N)" becomes "texture2D(Diffuse, Out_Tex0)".
 * out needs strlen(in) + 1 bytes. Returns the number of calls changed. */
size_t glw_drop_lod_bias(const char *in, char *out);

void glw_glEnable(GLenum cap);
void glw_glDisable(GLenum cap);
void glw_glBlendFunc(GLenum s, GLenum d);
void glw_glBlendFuncSeparate(GLenum sc, GLenum dc, GLenum sa, GLenum da);
void glw_glBlendEquation(GLenum e);
void glw_glBlendEquationSeparate(GLenum ec, GLenum ea);
void glw_glDepthFunc(GLenum f);
void glw_glDepthMask(GLboolean m);
void glw_glCullFace(GLenum m);
void glw_glFrontFace(GLenum m);
void glw_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a);
void glw_glActiveTexture(GLenum unit);
void glw_glBindTexture(GLenum target, GLuint tex);
void glw_glDeleteTextures(GLsizei n, const GLuint *t);
void glw_glUseProgram(GLuint p);
void glw_glDeleteProgram(GLuint p);
void glw_glLinkProgram(GLuint p);
void glw_glBindBuffer(GLenum target, GLuint b);
void glw_glDeleteBuffers(GLsizei n, const GLuint *b);
void glw_glBindFramebuffer(GLenum target, GLuint fb);
void glw_glDeleteFramebuffers(GLsizei n, const GLuint *fb);
void glw_glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
void glw_glDrawArrays(GLenum mode, GLint first, GLsizei count);
void glw_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *idx);
void glw_glClear(GLbitfield mask);
void glw_glCompileShader(GLuint s);
void glw_glShaderSource(GLuint s, GLsizei count, const GLchar *const *str, const GLint *len);
void glw_glTexImage2D(GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLint b, GLenum f,
                      GLenum y, const void *p);
void glw_glTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f,
                         GLenum ty, const void *p);
void glw_glCompressedTexImage2D(GLenum t, GLint l, GLenum i, GLsizei w, GLsizei h, GLint b,
                                GLsizei s, const void *d);
void glw_glCompressedTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h,
                                   GLenum f, GLsizei s, const void *d);
void glw_glTexParameteri(GLenum target, GLenum pname, GLint value);
void glw_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
void glw_glBufferSubData(GLenum target, GLintptr off, GLsizeiptr size, const void *data);
void glw_glUniform1f(GLint l, GLfloat a);
void glw_glUniform2f(GLint l, GLfloat a, GLfloat b);
void glw_glUniform3f(GLint l, GLfloat a, GLfloat b, GLfloat c);
void glw_glUniform4f(GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d);
void glw_glUniform1i(GLint l, GLint a);
void glw_glUniform1fv(GLint l, GLsizei n, const GLfloat *v);
void glw_glUniform2fv(GLint l, GLsizei n, const GLfloat *v);
void glw_glUniform3fv(GLint l, GLsizei n, const GLfloat *v);
void glw_glUniform4fv(GLint l, GLsizei n, const GLfloat *v);
void glw_glUniformMatrix3fv(GLint l, GLsizei n, GLboolean t, const GLfloat *v);
void glw_glUniformMatrix4fv(GLint l, GLsizei n, GLboolean t, const GLfloat *v);
void glw_glGetShaderInfoLog(GLuint shader, GLsizei max, GLsizei *len, GLchar *log);

/* queries: each one makes the game's thread wait for glthread to catch up */
GLenum glw_glGetError(void);
void glw_glGetIntegerv(GLenum pname, GLint *v);
void glw_glGetFloatv(GLenum pname, GLfloat *v);
void glw_glGetBooleanv(GLenum pname, GLboolean *v);
GLboolean glw_glIsEnabled(GLenum cap);
GLint glw_glGetUniformLocation(GLuint prog, const GLchar *name);
GLint glw_glGetAttribLocation(GLuint prog, const GLchar *name);
GLenum glw_glCheckFramebufferStatus(GLenum target);
void glw_glGetShaderiv(GLuint shader, GLenum pname, GLint *v);
void glw_glGetProgramiv(GLuint prog, GLenum pname, GLint *v);
void glw_glFinish(void);
void glw_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *p);

#endif
