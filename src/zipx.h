/* zipx.h -- reading ZIP archives (the APK and the OBB), zip64 included
 *
 * Only what the import needs: list the central directory, extract stored
 * and deflated entries with their CRC checked. Single-disk archives only.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef GTASA_ZIPX_H
#define GTASA_ZIPX_H

#include <stdint.h>
#include <stdio.h>

typedef struct {
  FILE *f;
  uint64_t size;      /* of the archive file */
  uint64_t entries;
  uint64_t cd_offset; /* central directory */
  uint64_t cd_size;
} Zip;

typedef struct {
  char name[1024];
  uint64_t comp_size;
  uint64_t size;
  uint64_t local_offset;
  uint32_t crc;
  uint16_t method; /* 0 stored, 8 deflate */
  uint16_t flags;
} ZipEntry;

/* 0, or -1 with a reason in `why` */
int zip_open(Zip *z, const char *path, char *why, size_t whylen);
void zip_close(Zip *z);

/* Calls `cb` for every entry in central-directory order; a nonzero return
 * from `cb` stops the walk and is returned. -1 on a damaged directory. */
int zip_foreach(Zip *z, int (*cb)(Zip *z, const ZipEntry *e, void *arg), void *arg);

/* Look one entry up by name; 0 when found. */
int zip_find(Zip *z, const char *name, ZipEntry *out);

/* Extract to `dest` (written as dest.part, renamed when complete and its
 * CRC matches). `progress` gets the bytes written so far, may be NULL.
 * 0, or -1 with a reason in `why`. */
int zip_extract(Zip *z, const ZipEntry *e, const char *dest,
                void (*progress)(uint64_t done, void *arg), void *arg, char *why, size_t whylen);

/* A name that is safe to create under a directory: relative, no "..",
 * no backslashes or control characters. */
int zip_name_safe(const char *name);

#endif
