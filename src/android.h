/* android.h -- NDK entry points emulated in android.c
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef GTASA_ANDROID_H
#define GTASA_ANDROID_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

void *AAssetManager_fromJava_fake(void *env, void *mgr);
void *AAssetManager_open_fake(void *mgr, const char *path, int mode);
void AAsset_close_fake(void *asset);
int AAsset_read_fake(void *asset, void *buf, size_t count);
long AAsset_seek_fake(void *asset, long off, int whence);
int64_t AAsset_seek64_fake(void *asset, int64_t off, int whence);
long AAsset_getLength_fake(void *asset);
int64_t AAsset_getLength64_fake(void *asset);
long AAsset_getRemainingLength_fake(void *asset);
int64_t AAsset_getRemainingLength64_fake(void *asset);
const void *AAsset_getBuffer_fake(void *asset);
int AAsset_openFileDescriptor_fake(void *asset, long *start, long *length);
int AAsset_isAllocated_fake(void *asset);
void *AAssetManager_openDir_fake(void *mgr, const char *dir);
const char *AAssetDir_getNextFileName_fake(void *dir);
void AAssetDir_rewind_fake(void *dir);
void AAssetDir_close_fake(void *dir);

void *ANativeWindow_fromSurface_fake(void *env, void *surface);
int ANativeWindow_getWidth_fake(void *win);
int ANativeWindow_getHeight_fake(void *win);
void ANativeWindow_acquire_fake(void *win);
void ANativeWindow_release_fake(void *win);
int ANativeWindow_setBuffersGeometry_fake(void *win, int w, int h, int format);

int __android_log_print_fake(int prio, const char *tag, const char *fmt, ...);
int __android_log_vprint_fake(int prio, const char *tag, const char *fmt, va_list va);
int __android_log_write_fake(int prio, const char *tag, const char *text);
void __android_log_assert_fake(const char *cond, const char *tag, const char *fmt, ...)
    __attribute__((noreturn));
void __assert2_fake(const char *file, int line, const char *func, const char *expr)
    __attribute__((noreturn));
void android_set_abort_message_fake(const char *msg);
int __system_property_get_fake(const char *name, char *value);

extern void *SL_IID_fake;
unsigned slCreateEngine_fake(void);

#endif
