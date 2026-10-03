/* import_table.c -- bindings for the imports of libGame.so and libc++_shared.so
 *
 * Only symbols that need a wrapper, a different name or a data object are
 * listed; ABI-identical libc/libm/GL/EGL/OpenAL/mpg123 functions resolve to
 * the host library through the fallback, which logs each one it binds and
 * refuses the names in imports_abi_unsafe() (they trap with their name if
 * the game ever calls one that is missing here). The ABI checks behind the
 * pass-throughs are in tests/abi_check.c.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <dlfcn.h>
#include <netdb.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <AL/al.h>
#include <AL/alc.h>
#include <mpg123.h>

#include "config.h"
#include "so_util.h"
#include "util.h"

#include "android.h"
#include "bionic.h"
#include "egl_wrap.h"
#include "gl_wrap.h"
#include "import_table.h"
#include "loader.h"

extern int __cxa_atexit(void (*func)(void *), void *arg, void *dso);
extern int __cxa_thread_atexit_impl(void (*func)(void *), void *obj, void *dso);
extern char **environ;

/* gtasa_nx hooks/openal.c: 44.1 kHz context, device captured for shutdown */
extern ALCcontext *alcCreateContextHook(ALCdevice *dev, const ALCint *attr);
extern ALCdevice *alcOpenDeviceHook(const char *name);

/* FuzzySeek (gtasa_nx, after TheOfficialFloW): mpg123 skips useless data on
 * seeks, so radio stations switch faster and the SD card is read less. */
static int mpg123_param_fuzzy(mpg123_handle *mh, enum mpg123_parms type, long value,
                              double fvalue) {
  if (config.fuzzy_seek && (type == MPG123_FLAGS || type == MPG123_ADD_FLAGS))
    value |= MPG123_FUZZY | MPG123_SEEKBUFFER | MPG123_GAPLESS;
  return mpg123_param(mh, type, value, fvalue);
}

static const char *progname = "gtasa";
static int no_op_zero(void) { return 0; }

#define F(sym, fn) { (char *)(sym), (uintptr_t)(fn) }

DynLibFunction import_table[] = {
  /* ---- data objects ---- */
  F("__sF", bionic_sF),
  F("stdin", &bionic_stdin),
  F("stdout", &bionic_stdout),
  F("stderr", &bionic_stderr),
  F("_ctype_", &bionic_ctype_),
  F("_tolower_tab_", &bionic_tolower_tab_),
  F("_toupper_tab_", &bionic_toupper_tab_),
  F("__stack_chk_guard", &bionic_stack_chk_guard),
  F("environ", &environ),
  F("__progname", &progname),

  /* ---- process, errno, C++ runtime glue ---- */
  F("__errno", bionic___errno),
  F("__stack_chk_fail", bionic___stack_chk_fail),
  F("abort", bionic_abort),
  F("exit", bionic_exit),
  F("__cxa_atexit", __cxa_atexit),
  F("__cxa_finalize", bionic___cxa_finalize),
  F("__cxa_thread_atexit_impl", __cxa_thread_atexit_impl),
  F("__register_atfork", bionic___register_atfork),
  F("pthread_atfork", bionic___register_atfork),
  F("sysconf", bionic_sysconf),
  F("pathconf", bionic_pathconf),
  F("syscall", bionic_syscall),
  F("gettid", bionic_gettid),
  F("__ctype_get_mb_cur_max", bionic___ctype_get_mb_cur_max),
  F("strerror_r", bionic_strerror_r),
  F("__get_h_errno", __h_errno_location),
  F("dl_iterate_phdr", so_dl_iterate_phdr),
  F("dlopen", bionic_dlopen),
  F("dlsym", bionic_dlsym),
  F("dlclose", bionic_dlclose),
  F("dlerror", bionic_dlerror),
  F("dladdr", bionic_dladdr),
  /* bionic spells some LP64 functions with a 64 suffix */
  F("lseek64", lseek),
  F("stat64", stat),
  F("lstat64", lstat),
  F("fstat64", fstat),
  F("fstatat64", fstatat),
  F("readdir64", readdir),
  F("ftruncate64", ftruncate),
  F("pread64", pread),
  F("pwrite64", pwrite),
  F("mmap64", mmap),
  F("strlcpy", bionic_strlcpy),
  F("strlcat", bionic_strlcat),
  F("dirname", bionic_dirname),
  F("basename", bionic_basename),

  /* ---- _FORTIFY_SOURCE ---- */
  F("__memcpy_chk", bionic___memcpy_chk),
  F("__memmove_chk", bionic___memmove_chk),
  F("__memset_chk", bionic___memset_chk),
  F("__strcat_chk", bionic___strcat_chk),
  F("__strchr_chk", bionic___strchr_chk),
  F("__strrchr_chk", bionic___strrchr_chk),
  F("__strcpy_chk", bionic___strcpy_chk),
  F("__stpcpy_chk", bionic___stpcpy_chk),
  F("__strlen_chk", bionic___strlen_chk),
  F("__strncat_chk", bionic___strncat_chk),
  F("__strncpy_chk", bionic___strncpy_chk),
  F("__strncpy_chk2", bionic___strncpy_chk2),
  F("__vsnprintf_chk", bionic___vsnprintf_chk),
  F("__snprintf_chk", bionic___snprintf_chk),
  F("__vsprintf_chk", bionic___vsprintf_chk),
  F("__sprintf_chk", bionic___sprintf_chk),
  F("__read_chk", bionic___read_chk),
  F("__write_chk", bionic___write_chk),
  F("__fread_chk", bionic___fread_chk),
  F("__fgets_chk", bionic___fgets_chk),
  F("__FD_SET_chk", bionic___FD_SET_chk),
  F("__FD_CLR_chk", bionic___FD_CLR_chk),
  F("__FD_ISSET_chk", bionic___FD_ISSET_chk),
  F("__open_2", bionic___open_2),
  F("__openat_2", bionic___openat_2),
  F("open", bionic_open),
  F("openat", bionic_openat),
  F("stat", bionic_stat),
  F("lstat", bionic_lstat),
  F("opendir", bionic_opendir),

  /* ---- stdio ---- */
  F("printf", bionic_printf),
  F("vprintf", bionic_vprintf),
  F("puts", bionic_puts),
  F("putchar", bionic_putchar),
  F("getchar", bionic_getchar),
  F("fopen", bionic_fopen),
  F("fopen64", bionic_fopen),
  F("fdopen", bionic_fdopen),
  F("freopen", bionic_freopen),
  F("fclose", bionic_fclose),
  F("fflush", bionic_fflush),
  F("fread", bionic_fread),
  F("fwrite", bionic_fwrite),
  F("fgetc", bionic_fgetc),
  F("getc", bionic_fgetc),
  F("getc_unlocked", bionic_fgetc),
  F("fgets", bionic_fgets),
  F("fputc", bionic_fputc),
  F("putc", bionic_fputc),
  F("putc_unlocked", bionic_fputc),
  F("fputs", bionic_fputs),
  F("ungetc", bionic_ungetc),
  F("fprintf", bionic_fprintf),
  F("vfprintf", bionic_vfprintf),
  F("fscanf", bionic_fscanf),
  F("vfscanf", bionic_vfscanf),
  F("fseek", bionic_fseek),
  F("fseeko", bionic_fseeko),
  F("fseeko64", bionic_fseeko),
  F("ftell", bionic_ftell),
  F("ftello", bionic_ftello),
  F("ftello64", bionic_ftello),
  F("rewind", bionic_rewind),
  F("fgetpos", bionic_fgetpos),
  F("fsetpos", bionic_fsetpos),
  F("feof", bionic_feof),
  F("ferror", bionic_ferror),
  F("clearerr", bionic_clearerr),
  F("fileno", bionic_fileno),
  F("setvbuf", bionic_setvbuf),
  F("setbuf", bionic_setbuf),
  F("fgetwc", bionic_fgetwc),
  F("getwc", bionic_fgetwc),
  F("ungetwc", bionic_ungetwc),
  F("fputwc", bionic_fputwc),
  F("putwc", bionic_fputwc),
  F("fwide", bionic_fwide),
  F("getline", bionic_getline),
  F("getdelim", bionic_getdelim),
  F("flockfile", bionic_flockfile),
  F("funlockfile", bionic_funlockfile),

  /* ---- locale ---- */
  F("newlocale", bionic_newlocale),
  F("freelocale", bionic_freelocale),
  F("uselocale", bionic_uselocale),
  F("duplocale", bionic_uselocale),
  F("isalpha_l", bionic_isalpha_l),
  F("isdigit_l", bionic_isdigit_l),
  F("isxdigit_l", bionic_isxdigit_l),
  F("islower_l", bionic_islower_l),
  F("isupper_l", bionic_isupper_l),
  F("isspace_l", bionic_isspace_l),
  F("isprint_l", bionic_isprint_l),
  F("ispunct_l", bionic_ispunct_l),
  F("iscntrl_l", bionic_iscntrl_l),
  F("isalnum_l", bionic_isalnum_l),
  F("isblank_l", bionic_isblank_l),
  F("isgraph_l", bionic_isgraph_l),
  F("toupper_l", bionic_toupper_l),
  F("tolower_l", bionic_tolower_l),
  F("iswalpha_l", bionic_iswalpha_l),
  F("iswblank_l", bionic_iswblank_l),
  F("iswcntrl_l", bionic_iswcntrl_l),
  F("iswdigit_l", bionic_iswdigit_l),
  F("iswlower_l", bionic_iswlower_l),
  F("iswprint_l", bionic_iswprint_l),
  F("iswpunct_l", bionic_iswpunct_l),
  F("iswspace_l", bionic_iswspace_l),
  F("iswupper_l", bionic_iswupper_l),
  F("iswxdigit_l", bionic_iswxdigit_l),
  F("towlower_l", bionic_towlower_l),
  F("towupper_l", bionic_towupper_l),
  F("strcoll_l", bionic_strcoll_l),
  F("strxfrm_l", bionic_strxfrm_l),
  F("strftime_l", bionic_strftime_l),
  F("strtold_l", bionic_strtold_l),
  F("strtoll_l", bionic_strtoll_l),
  F("strtoull_l", bionic_strtoull_l),
  F("strtof_l", bionic_strtof_l),
  F("strtod_l", bionic_strtod_l),
  F("wcscoll_l", bionic_wcscoll_l),
  F("wcsxfrm_l", bionic_wcsxfrm_l),
  F("mbsnrtowcs", bionic_mbsnrtowcs),
  F("wcsnrtombs", bionic_wcsnrtombs),

  /* ---- network (closed unless network 1) ---- */
  F("socket", bionic_socket),
  F("getaddrinfo", bionic_getaddrinfo),
  F("freeaddrinfo", bionic_freeaddrinfo),
  F("gai_strerror", bionic_gai_strerror),
  F("gethostbyname", bionic_gethostbyname),
  F("gethostname", bionic_gethostname),

  /* ---- pthread ---- */
  F("pthread_create", bionic_pthread_create),
  F("pthread_attr_init", bionic_pthread_attr_init),
  F("pthread_attr_destroy", bionic_pthread_attr_destroy),
  F("pthread_attr_setdetachstate", bionic_pthread_attr_setdetachstate),
  F("pthread_attr_getdetachstate", bionic_pthread_attr_getdetachstate),
  F("pthread_attr_setstacksize", bionic_pthread_attr_setstacksize),
  F("pthread_attr_getstacksize", bionic_pthread_attr_getstacksize),
  F("pthread_attr_setstack", bionic_pthread_attr_setstack),
  F("pthread_attr_getstack", bionic_pthread_attr_getstack),
  F("pthread_attr_setguardsize", bionic_pthread_attr_setguardsize),
  F("pthread_attr_getguardsize", bionic_pthread_attr_getguardsize),
  F("pthread_attr_setschedpolicy", bionic_pthread_attr_setschedpolicy),
  F("pthread_attr_getschedpolicy", bionic_pthread_attr_getschedpolicy),
  F("pthread_attr_setschedparam", bionic_pthread_attr_setschedparam),
  F("pthread_attr_getschedparam", bionic_pthread_attr_getschedparam),
  F("pthread_attr_setscope", bionic_pthread_attr_setscope),
  F("pthread_attr_getscope", bionic_pthread_attr_getscope),
  F("pthread_attr_setinheritsched", bionic_pthread_attr_setinheritsched),
  F("pthread_getattr_np", bionic_pthread_getattr_np),
  F("pthread_setschedparam", bionic_pthread_setschedparam),
  F("pthread_getschedparam", bionic_pthread_getschedparam),
  F("pthread_setname_np", bionic_pthread_setname_np),
  F("pthread_gettid_np", bionic_pthread_gettid_np),
  F("pthread_self", pthread_self),
  F("pthread_equal", pthread_equal),
  F("pthread_join", pthread_join),
  F("pthread_detach", pthread_detach),
  F("pthread_exit", pthread_exit),
  F("pthread_kill", pthread_kill),
  F("pthread_once", pthread_once),
  F("pthread_key_create", pthread_key_create),
  F("pthread_key_delete", pthread_key_delete),
  F("pthread_getspecific", pthread_getspecific),
  F("pthread_setspecific", pthread_setspecific),
  F("pthread_mutexattr_init", bionic_pthread_mutexattr_init),
  F("pthread_mutexattr_destroy", bionic_pthread_mutexattr_destroy),
  F("pthread_mutexattr_settype", bionic_pthread_mutexattr_settype),
  F("pthread_mutexattr_gettype", bionic_pthread_mutexattr_gettype),
  F("pthread_mutexattr_setpshared", bionic_pthread_mutexattr_setpshared),
  F("pthread_mutex_init", bionic_pthread_mutex_init),
  F("pthread_mutex_destroy", bionic_pthread_mutex_destroy),
  F("pthread_mutex_lock", bionic_pthread_mutex_lock),
  F("pthread_mutex_trylock", bionic_pthread_mutex_trylock),
  F("pthread_mutex_unlock", bionic_pthread_mutex_unlock),
  F("pthread_mutex_timedlock", bionic_pthread_mutex_timedlock),
  F("pthread_mutex_lock_timeout_np", bionic_pthread_mutex_lock_timeout_np),
  F("pthread_condattr_init", bionic_pthread_condattr_init),
  F("pthread_condattr_destroy", bionic_pthread_condattr_destroy),
  F("pthread_condattr_setclock", bionic_pthread_condattr_setclock),
  F("pthread_condattr_getclock", bionic_pthread_condattr_getclock),
  F("pthread_condattr_setpshared", bionic_pthread_condattr_setpshared),
  F("pthread_cond_init", bionic_pthread_cond_init),
  F("pthread_cond_destroy", bionic_pthread_cond_destroy),
  F("pthread_cond_signal", bionic_pthread_cond_signal),
  F("pthread_cond_broadcast", bionic_pthread_cond_broadcast),
  F("pthread_cond_wait", bionic_pthread_cond_wait),
  F("pthread_cond_timedwait", bionic_pthread_cond_timedwait),
  F("pthread_cond_clockwait", bionic_pthread_cond_clockwait),
  F("pthread_cond_timedwait_monotonic_np", bionic_pthread_cond_timedwait_monotonic_np),
  F("pthread_cond_timedwait_monotonic", bionic_pthread_cond_timedwait_monotonic_np),
  F("pthread_cond_timedwait_relative_np", bionic_pthread_cond_timedwait_relative_np),
  F("pthread_rwlockattr_init", bionic_pthread_rwlockattr_init),
  F("pthread_rwlockattr_destroy", bionic_pthread_rwlockattr_destroy),
  F("pthread_rwlock_init", bionic_pthread_rwlock_init),
  F("pthread_rwlock_destroy", bionic_pthread_rwlock_destroy),
  F("pthread_rwlock_rdlock", bionic_pthread_rwlock_rdlock),
  F("pthread_rwlock_wrlock", bionic_pthread_rwlock_wrlock),
  F("pthread_rwlock_tryrdlock", bionic_pthread_rwlock_tryrdlock),
  F("pthread_rwlock_trywrlock", bionic_pthread_rwlock_trywrlock),
  F("pthread_rwlock_timedrdlock", bionic_pthread_rwlock_timedrdlock),
  F("pthread_rwlock_timedwrlock", bionic_pthread_rwlock_timedwrlock),
  F("pthread_rwlock_unlock", bionic_pthread_rwlock_unlock),
  F("sem_init", bionic_sem_init),
  F("sem_destroy", bionic_sem_destroy),
  F("sem_post", bionic_sem_post),
  F("sem_wait", bionic_sem_wait),
  F("sem_trywait", bionic_sem_trywait),
  F("sem_timedwait", bionic_sem_timedwait),
  F("sem_getvalue", bionic_sem_getvalue),
  F("sem_open", bionic_sem_open),
  F("sem_close", bionic_sem_close),
  F("sem_unlink", bionic_sem_unlink),

  /* ---- signals and non-local jumps ---- */
  F("sigaction", bionic_sigaction),
  F("signal", bionic_signal),
  F("bsd_signal", bionic_signal),
  F("sigemptyset", bionic_sigemptyset),
  F("sigfillset", bionic_sigfillset),
  F("sigaddset", bionic_sigaddset),
  F("sigdelset", bionic_sigdelset),
  F("sigismember", bionic_sigismember),
  F("sigprocmask", bionic_sigprocmask),
  F("pthread_sigmask", bionic_pthread_sigmask),
  F("setjmp", bionic_setjmp),
  F("_setjmp", bionic_setjmp),
  F("sigsetjmp", bionic_setjmp),
  F("longjmp", bionic_longjmp),
  F("_longjmp", bionic_longjmp),
  F("siglongjmp", bionic_longjmp),

  /* ---- Android NDK ---- */
  F("AAssetManager_fromJava", AAssetManager_fromJava_fake),
  F("AAssetManager_open", AAssetManager_open_fake),
  F("AAssetManager_openDir", AAssetManager_openDir_fake),
  F("AAsset_close", AAsset_close_fake),
  F("AAsset_read", AAsset_read_fake),
  F("AAsset_seek", AAsset_seek_fake),
  F("AAsset_seek64", AAsset_seek64_fake),
  F("AAsset_getLength", AAsset_getLength_fake),
  F("AAsset_getLength64", AAsset_getLength64_fake),
  F("AAsset_getRemainingLength", AAsset_getRemainingLength_fake),
  F("AAsset_getRemainingLength64", AAsset_getRemainingLength64_fake),
  F("AAsset_getBuffer", AAsset_getBuffer_fake),
  F("AAsset_openFileDescriptor", AAsset_openFileDescriptor_fake),
  F("AAsset_isAllocated", AAsset_isAllocated_fake),
  F("AAssetDir_getNextFileName", AAssetDir_getNextFileName_fake),
  F("AAssetDir_rewind", AAssetDir_rewind_fake),
  F("AAssetDir_close", AAssetDir_close_fake),
  F("ANativeWindow_fromSurface", ANativeWindow_fromSurface_fake),
  F("ANativeWindow_getWidth", ANativeWindow_getWidth_fake),
  F("ANativeWindow_getHeight", ANativeWindow_getHeight_fake),
  F("ANativeWindow_acquire", ANativeWindow_acquire_fake),
  F("ANativeWindow_release", ANativeWindow_release_fake),
  F("ANativeWindow_setBuffersGeometry", ANativeWindow_setBuffersGeometry_fake),
  F("__android_log_print", __android_log_print_fake),
  F("__android_log_vprint", __android_log_vprint_fake),
  F("__android_log_write", __android_log_write_fake),
  F("__android_log_assert", __android_log_assert_fake),
  F("__assert2", __assert2_fake),
  F("android_set_abort_message", android_set_abort_message_fake),
  F("__system_property_get", __system_property_get_fake),
  F("slCreateEngine", slCreateEngine_fake),
  F("SL_IID_ANDROIDCONFIGURATION", &SL_IID_fake),
  F("SL_IID_ANDROIDSIMPLEBUFFERQUEUE", &SL_IID_fake),
  F("SL_IID_BUFFERQUEUE", &SL_IID_fake),
  F("SL_IID_ENGINE", &SL_IID_fake),
  F("SL_IID_PLAY", &SL_IID_fake),
  F("SL_IID_RECORD", &SL_IID_fake),
  F("SL_IID_VOLUME", &SL_IID_fake),
  /* thread_local init wrapper the old OpenAL donor imported (gtasa_nx) */
  F("_ZTHN10ALCcontext13sLocalContextE", no_op_zero),

  /* ---- EGL ---- */
  F("eglGetDisplay", eglw_GetDisplay),
  F("eglInitialize", eglw_Initialize),
  F("eglTerminate", eglw_Terminate),
  F("eglChooseConfig", eglw_ChooseConfig),
  F("eglCreateWindowSurface", eglw_CreateWindowSurface),
  F("eglDestroySurface", eglw_DestroySurface),
  F("eglCreateContext", eglw_CreateContext),
  F("eglDestroyContext", eglw_DestroyContext),
  F("eglMakeCurrent", eglw_MakeCurrent),
  F("eglSwapBuffers", eglw_SwapBuffers),
  F("eglSwapInterval", eglw_SwapInterval),
  F("eglGetProcAddress", eglw_GetProcAddress),

  /* ---- GL setters and counters (the rest of GLES binds to Mesa directly) ---- */
  F("glEnable", glw_glEnable),
  F("glDisable", glw_glDisable),
  F("glBlendFunc", glw_glBlendFunc),
  F("glBlendFuncSeparate", glw_glBlendFuncSeparate),
  F("glBlendEquation", glw_glBlendEquation),
  F("glBlendEquationSeparate", glw_glBlendEquationSeparate),
  F("glDepthFunc", glw_glDepthFunc),
  F("glDepthMask", glw_glDepthMask),
  F("glCullFace", glw_glCullFace),
  F("glFrontFace", glw_glFrontFace),
  F("glColorMask", glw_glColorMask),
  F("glActiveTexture", glw_glActiveTexture),
  F("glBindTexture", glw_glBindTexture),
  F("glDeleteTextures", glw_glDeleteTextures),
  F("glUseProgram", glw_glUseProgram),
  F("glDeleteProgram", glw_glDeleteProgram),
  F("glLinkProgram", glw_glLinkProgram),
  F("glBindBuffer", glw_glBindBuffer),
  F("glDeleteBuffers", glw_glDeleteBuffers),
  F("glBindFramebuffer", glw_glBindFramebuffer),
  F("glDeleteFramebuffers", glw_glDeleteFramebuffers),
  F("glViewport", glw_glViewport),
  F("glDrawArrays", glw_glDrawArrays),
  F("glDrawElements", glw_glDrawElements),
  F("glClear", glw_glClear),
  F("glCompileShader", glw_glCompileShader),
  F("glShaderSource", glw_glShaderSource),
  F("glTexImage2D", glw_glTexImage2D),
  F("glTexSubImage2D", glw_glTexSubImage2D),
  F("glCompressedTexImage2D", glw_glCompressedTexImage2D),
  F("glCompressedTexSubImage2D", glw_glCompressedTexSubImage2D),
  F("glTexParameteri", glw_glTexParameteri),
  F("glBufferData", glw_glBufferData),
  F("glBufferSubData", glw_glBufferSubData),
  F("glUniform1f", glw_glUniform1f),
  F("glUniform2f", glw_glUniform2f),
  F("glUniform3f", glw_glUniform3f),
  F("glUniform4f", glw_glUniform4f),
  F("glUniform1i", glw_glUniform1i),
  F("glUniform1fv", glw_glUniform1fv),
  F("glUniform2fv", glw_glUniform2fv),
  F("glUniform3fv", glw_glUniform3fv),
  F("glUniform4fv", glw_glUniform4fv),
  F("glUniformMatrix3fv", glw_glUniformMatrix3fv),
  F("glUniformMatrix4fv", glw_glUniformMatrix4fv),
  F("glGetShaderInfoLog", glw_glGetShaderInfoLog),
  F("glGetError", glw_glGetError),
  F("glGetIntegerv", glw_glGetIntegerv),
  F("glGetFloatv", glw_glGetFloatv),
  F("glGetBooleanv", glw_glGetBooleanv),
  F("glIsEnabled", glw_glIsEnabled),
  F("glGetUniformLocation", glw_glGetUniformLocation),
  F("glGetAttribLocation", glw_glGetAttribLocation),
  F("glCheckFramebufferStatus", glw_glCheckFramebufferStatus),
  F("glGetShaderiv", glw_glGetShaderiv),
  F("glGetProgramiv", glw_glGetProgramiv),
  F("glFinish", glw_glFinish),
  F("glReadPixels", glw_glReadPixels),

  /* ---- OpenAL / mpg123: two hooks and one wrapper, the rest is direct ---- */
  F("alcOpenDevice", alcOpenDeviceHook),
  F("alcCreateContext", alcCreateContextHook),
  F("mpg123_param", mpg123_param_fuzzy),
};

const int import_table_count = sizeof(import_table) / sizeof(import_table[0]);

static int cmp_entry(const void *a, const void *b) {
  return strcmp(((const DynLibFunction *)a)->symbol, ((const DynLibFunction *)b)->symbol);
}

static int cmp_key(const void *key, const void *elem) {
  return strcmp((const char *)key, ((const DynLibFunction *)elem)->symbol);
}

void import_table_configure(void) {
  qsort(import_table, (size_t)import_table_count, sizeof(DynLibFunction), cmp_entry);
  for (int i = 1; i < import_table_count; i++)
    if (!strcmp(import_table[i - 1].symbol, import_table[i].symbol))
      debugPrintf("imports: duplicate entry %s\n", import_table[i].symbol);
  glw_force_trilinear(config.trilinear_filter);
}

uintptr_t imports_lookup(const char *name) {
  if (!name)
    return 0;
  const DynLibFunction *hit = bsearch(name, import_table, (size_t)import_table_count,
                                      sizeof(DynLibFunction), cmp_key);
  return hit ? hit->func : 0;
}

/* Names that exist in glibc with a different ABI (layouts, constants,
 * semantics), or that must come from the game's own C++ runtime. Binding
 * them to the host would corrupt memory far from the call. */
int imports_abi_unsafe(const char *n) {
  static const char *const exact[] = {
    "sysconf", "pathconf", "fpathconf", "nl_langinfo", "mallinfo", "glob", "globfree",
    "regcomp", "regexec", "regfree", "regerror", "getaddrinfo", "freeaddrinfo",
    "gai_strerror", "gethostbyname_r", "gethostbyaddr", "getservbyname", "sigaction",
    "signal", "bsd_signal", "sigprocmask", "sigsuspend", "sigwait", "sigtimedwait",
    "sigpending", "sigemptyset", "sigfillset", "sigaddset", "sigdelset", "sigismember",
    "setjmp", "_setjmp", "sigsetjmp", "longjmp", "_longjmp", "siglongjmp", "__sF",
    "stdin", "stdout", "stderr", "_ctype_", "_tolower_tab_", "_toupper_tab_", "__errno",
    "__stack_chk_guard", "__stack_chk_fail", "fork", "vfork", "clone", "execv", "execve",
    "execvp", "execl", "execlp", "execle", "system", "popen", "pclose", "posix_spawn",
    "posix_spawnp", "wait", "waitpid", "fgetpos", "fsetpos", "fpurge", "__fpending",
    "fputws", "fgetws", "fwprintf", "vfwprintf", "fwscanf", "vfwscanf", "wprintf",
    "vwprintf", "dlopen", "dlsym", "dlclose", "dlerror", "dladdr", "dl_iterate_phdr",
    "newlocale", "uselocale", "freelocale", "duplocale", "statfs", "fstatfs",
  };
  static const char *const prefix[] = {
    "pthread_", "sem_", "__system_property", "android_", "_Z", "__cxa_", "__gxx_",
    "_Unwind_", "__emutls_", "__aeabi_", "__gnu_", "__libc_", "__android_",
  };
  for (size_t i = 0; i < sizeof(exact) / sizeof(exact[0]); i++)
    if (!strcmp(n, exact[i]))
      return 1;
  for (size_t i = 0; i < sizeof(prefix) / sizeof(prefix[0]); i++)
    if (!strncmp(n, prefix[i], strlen(prefix[i])))
      return !(!strcmp(n, "__libc_current_sigrtmin") || !strcmp(n, "__libc_current_sigrtmax"));
  /* NDK APIs: AAsset*, ANativeWindow*, ALooper*, AConfiguration*, ... */
  if (n[0] == 'A' && n[1] >= 'A' && n[1] <= 'Z')
    return 1;
  return 0;
}

uintptr_t imports_fallback(const char *name, int is_object) {
  if (imports_abi_unsafe(name)) {
    debugPrintf("imports: %s is ABI-sensitive and has no binding; left unresolved\n", name);
    return 0;
  }
  void *addr = dlsym(RTLD_DEFAULT, name);
  if (!addr)
    return 0;
  if (is_object)
    debugPrintf("imports: data object %s bound to the host library\n", name);
  return (uintptr_t)addr;
}

uintptr_t imports_resolve(const char *name) {
  uintptr_t a = imports_lookup(name);
  if (a)
    return a;
  return imports_fallback(name, 0);
}
