/* zipx.c -- reading ZIP archives (the APK and the OBB), zip64 included
 *
 * Layouts from PKWARE's APPNOTE.TXT (4.3.x): end of central directory
 * record, zip64 end record and locator, central and local file headers,
 * and the zip64 extended-information extra field (0x0001).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <zlib.h>

#include "zipx.h"

#define SIG_LOCAL  0x04034b50u
#define SIG_CENTRAL 0x02014b50u
#define SIG_EOCD   0x06054b50u
#define SIG_EOCD64 0x06064b50u
#define SIG_LOC64  0x07064b50u

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

static int read_at(FILE *f, uint64_t off, void *buf, size_t len) {
  if (fseeko(f, (off_t)off, SEEK_SET) != 0)
    return -1;
  return fread(buf, 1, len, f) == len ? 0 : -1;
}

int zip_open(Zip *z, const char *path, char *why, size_t whylen) {
  memset(z, 0, sizeof(*z));
  z->f = fopen(path, "rb");
  if (!z->f) {
    snprintf(why, whylen, "%s", strerror(errno));
    return -1;
  }
  setvbuf(z->f, NULL, _IOFBF, 256 * 1024);
  if (fseeko(z->f, 0, SEEK_END) != 0)
    goto bad;
  z->size = (uint64_t)ftello(z->f);
  if (z->size < 22)
    goto bad;

  /* the end record sits in the last 22 + 65535 (comment) bytes */
  const uint64_t tail = z->size < 22 + 65535 ? z->size : 22 + 65535;
  uint8_t *buf = malloc(tail);
  if (!buf || read_at(z->f, z->size - tail, buf, tail) < 0) {
    free(buf);
    goto bad;
  }
  int64_t eocd = -1;
  for (int64_t i = (int64_t)tail - 22; i >= 0; i--)
    if (le32(buf + i) == SIG_EOCD && (uint64_t)i + 22 + le16(buf + i + 20) <= tail) {
      eocd = i;
      break;
    }
  if (eocd < 0) {
    free(buf);
    snprintf(why, whylen, "not a ZIP archive (no end of central directory)");
    fclose(z->f);
    z->f = NULL;
    return -1;
  }
  const uint8_t *e = buf + eocd;
  const uint64_t eocd_off = z->size - tail + (uint64_t)eocd;
  if (le16(e + 4) != 0 || le16(e + 6) != 0) {
    free(buf);
    snprintf(why, whylen, "multi-disk archives are not supported");
    fclose(z->f);
    z->f = NULL;
    return -1;
  }
  z->entries = le16(e + 10);
  z->cd_size = le32(e + 12);
  z->cd_offset = le32(e + 16);
  free(buf);

  if (z->entries == 0xffff || z->cd_size == 0xffffffffu || z->cd_offset == 0xffffffffu) {
    uint8_t loc[20], rec[56];
    if (eocd_off < 20 || read_at(z->f, eocd_off - 20, loc, 20) < 0 || le32(loc) != SIG_LOC64 ||
        read_at(z->f, le64(loc + 8), rec, 56) < 0 || le32(rec) != SIG_EOCD64) {
      snprintf(why, whylen, "damaged zip64 end record");
      fclose(z->f);
      z->f = NULL;
      return -1;
    }
    z->entries = le64(rec + 32);
    z->cd_size = le64(rec + 40);
    z->cd_offset = le64(rec + 48);
  }
  if (z->cd_offset > z->size || z->cd_size > z->size - z->cd_offset)
    goto bad;
  return 0;

bad:
  snprintf(why, whylen, "damaged or truncated archive");
  if (z->f)
    fclose(z->f);
  z->f = NULL;
  return -1;
}

void zip_close(Zip *z) {
  if (z->f)
    fclose(z->f);
  z->f = NULL;
}

/* fill `out` from the central header at `p` (`avail` bytes); returns the
 * header's total length, or 0 when it is damaged */
static size_t parse_central(const uint8_t *p, size_t avail, ZipEntry *out) {
  if (avail < 46 || le32(p) != SIG_CENTRAL)
    return 0;
  const size_t nlen = le16(p + 28), xlen = le16(p + 30), clen = le16(p + 32);
  const size_t total = 46 + nlen + xlen + clen;
  if (total > avail || nlen == 0 || nlen >= sizeof(out->name))
    return 0;
  memset(out, 0, sizeof(*out));
  out->flags = le16(p + 8);
  out->method = le16(p + 10);
  out->crc = le32(p + 16);
  out->comp_size = le32(p + 20);
  out->size = le32(p + 24);
  out->local_offset = le32(p + 42);
  memcpy(out->name, p + 46, nlen);
  out->name[nlen] = 0;
  if (memchr(out->name, 0, nlen))
    return 0;
  /* zip64: the extra field holds, in order, the fields saturated above */
  const uint8_t *x = p + 46 + nlen, *xend = x + xlen;
  while (x + 4 <= xend) {
    const uint16_t id = le16(x), sz = le16(x + 2);
    const uint8_t *d = x + 4, *dend = d + sz;
    if (dend > xend)
      return 0;
    if (id == 0x0001) {
      if (out->size == 0xffffffffu) {
        if (d + 8 > dend) return 0;
        out->size = le64(d);
        d += 8;
      }
      if (out->comp_size == 0xffffffffu) {
        if (d + 8 > dend) return 0;
        out->comp_size = le64(d);
        d += 8;
      }
      if (out->local_offset == 0xffffffffu) {
        if (d + 8 > dend) return 0;
        out->local_offset = le64(d);
      }
    }
    x = dend;
  }
  return total;
}

int zip_foreach(Zip *z, int (*cb)(Zip *z, const ZipEntry *e, void *arg), void *arg) {
  if (z->cd_size > (64u << 20))
    return -1; /* no real APK or OBB has a 64 MB directory */
  uint8_t *cd = malloc(z->cd_size ? z->cd_size : 1);
  if (!cd || read_at(z->f, z->cd_offset, cd, z->cd_size) < 0) {
    free(cd);
    return -1;
  }
  size_t pos = 0;
  int rc = 0;
  for (uint64_t i = 0; i < z->entries; i++) {
    ZipEntry e;
    const size_t len = parse_central(cd + pos, z->cd_size - pos, &e);
    if (!len) {
      rc = -1;
      break;
    }
    pos += len;
    if ((rc = cb(z, &e, arg)) != 0)
      break;
  }
  free(cd);
  return rc;
}

typedef struct {
  const char *name;
  ZipEntry *out;
} FindArg;

static int find_cb(Zip *z, const ZipEntry *e, void *arg) {
  (void)z;
  FindArg *a = arg;
  if (strcmp(e->name, a->name) != 0)
    return 0;
  *a->out = *e;
  return 1;
}

int zip_find(Zip *z, const char *name, ZipEntry *out) {
  FindArg a = { name, out };
  return zip_foreach(z, find_cb, &a) == 1 ? 0 : -1;
}

int zip_name_safe(const char *name) {
  if (!name[0] || name[0] == '/')
    return 0;
  for (const char *p = name; *p; p++)
    if (*p == '\\' || (unsigned char)*p < 0x20)
      return 0;
  /* no ".." path component anywhere */
  for (const char *p = name; *p;) {
    const char *slash = strchr(p, '/');
    const size_t len = slash ? (size_t)(slash - p) : strlen(p);
    if (len == 2 && p[0] == '.' && p[1] == '.')
      return 0;
    if (!slash)
      break;
    p = slash + 1;
  }
  return 1;
}

int zip_extract(Zip *z, const ZipEntry *e, const char *dest,
                void (*progress)(uint64_t done, void *arg), void *arg, char *why, size_t whylen) {
  why[0] = 0;
  if (e->flags & 1) {
    snprintf(why, whylen, "%s is encrypted", e->name);
    return -1;
  }
  if (e->method != 0 && e->method != 8) {
    snprintf(why, whylen, "%s: compression method %u not supported", e->name, e->method);
    return -1;
  }
  uint8_t lh[30];
  if (read_at(z->f, e->local_offset, lh, 30) < 0 || le32(lh) != SIG_LOCAL) {
    snprintf(why, whylen, "%s: damaged local header", e->name);
    return -1;
  }
  const uint64_t data = e->local_offset + 30 + le16(lh + 26) + le16(lh + 28);
  if (data > z->size || e->comp_size > z->size - data) {
    snprintf(why, whylen, "%s: entry runs past the end of the archive", e->name);
    return -1;
  }
  if (fseeko(z->f, (off_t)data, SEEK_SET) != 0) {
    snprintf(why, whylen, "%s: %s", e->name, strerror(errno));
    return -1;
  }

  char part[4096];
  snprintf(part, sizeof(part), "%s.part", dest);
  FILE *out = fopen(part, "wb");
  if (!out) {
    snprintf(why, whylen, "%s: %s", part, strerror(errno));
    return -1;
  }
  setvbuf(out, NULL, _IOFBF, 256 * 1024);

  enum { CHUNK = 256 * 1024 };
  uint8_t *in = malloc(CHUNK), *buf = malloc(CHUNK);
  z_stream zs;
  memset(&zs, 0, sizeof(zs));
  int ok = in && buf && (e->method == 0 || inflateInit2(&zs, -MAX_WBITS) == Z_OK);
  uint64_t left = e->comp_size, written = 0;
  uint32_t crc = (uint32_t)crc32(0L, Z_NULL, 0);
  int zend = 0;
  while (ok && left > 0) {
    const size_t want = left < CHUNK ? (size_t)left : CHUNK;
    if (fread(in, 1, want, z->f) != want) {
      snprintf(why, whylen, "%s: read error", e->name);
      ok = 0;
      break;
    }
    left -= want;
    if (e->method == 0) {
      crc = (uint32_t)crc32(crc, in, (uInt)want);
      if (fwrite(in, 1, want, out) != want)
        ok = 0;
      written += want;
    } else {
      zs.next_in = in;
      zs.avail_in = (uInt)want;
      do {
        zs.next_out = buf;
        zs.avail_out = CHUNK;
        const int r = inflate(&zs, Z_NO_FLUSH);
        if (r == Z_BUF_ERROR && zs.avail_in == 0)
          break; /* output drained: wants more input */
        if (r != Z_OK && r != Z_STREAM_END) {
          snprintf(why, whylen, "%s: corrupt compressed data", e->name);
          ok = 0;
          break;
        }
        const size_t n = CHUNK - zs.avail_out;
        crc = (uint32_t)crc32(crc, buf, (uInt)n);
        if (n && fwrite(buf, 1, n, out) != n)
          ok = 0;
        written += n;
        if (r == Z_STREAM_END) {
          zend = 1;
          break;
        }
      } while (zs.avail_in > 0 || zs.avail_out == 0);
    }
    if (progress)
      progress(written, arg);
  }
  if (e->method == 8)
    inflateEnd(&zs);
  free(in);
  free(buf);
  if (fclose(out) != 0 && ok) {
    snprintf(why, whylen, "%s: %s", part, strerror(errno));
    ok = 0;
  }
  if (ok && e->method == 8 && !zend) {
    snprintf(why, whylen, "%s: compressed data ends early", e->name);
    ok = 0;
  }
  if (ok && (written != e->size || crc != e->crc)) {
    snprintf(why, whylen, "%s: CRC or size mismatch (damaged archive?)", e->name);
    ok = 0;
  }
  if (!ok) {
    if (!why[0])
      snprintf(why, whylen, "%s: write error (card full?)", dest);
    unlink(part);
    return -1;
  }
  if (rename(part, dest) != 0) {
    snprintf(why, whylen, "%s: %s", dest, strerror(errno));
    unlink(part);
    return -1;
  }
  return 0;
}
