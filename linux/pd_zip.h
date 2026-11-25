/* Zipped games: the first .gb / .gbc / .gba / .nds inside a .zip, found from its central directory and unpacked with
 * zlib's puff (stored or deflated; no encryption, no zip64). The game list shows the zip by the game inside it, and the
 * host plays a copy unpacked into DATA/unzipped (kept, so a big DS game is unpacked once). */
#ifndef PD_ZIP_H
#define PD_ZIP_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "puff.h"

struct pd_zip_entry {
  char name[256]; // the game's file name, without the folders it sits in inside the zip
  unsigned method; // 0 stored, 8 deflated
  uint32_t csize, usize, local;
};

static inline uint32_t pd_zip_u16(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static inline uint32_t pd_zip_u32(const unsigned char *p) { return pd_zip_u16(p) | pd_zip_u16(p + 2) << 16; }

static inline bool pd_zip_is_zip(const char *path) {
  const char *ext = strrchr(path, '.');
  return ext && !strcasecmp(ext, ".zip");
}

static inline bool pd_zip_game_name(const char *name) {
  const char *ext = strrchr(name, '.');
  return ext && (!strcasecmp(ext, ".gb") || !strcasecmp(ext, ".gbc") || !strcasecmp(ext, ".gba") || !strcasecmp(ext, ".nds"));
}

static inline bool pd_zip_game(const char *zip, struct pd_zip_entry *out) {
  FILE *f = fopen(zip, "rb");
  if (!f)
    return false;
  bool found = false;
  unsigned char *tail = NULL, *cd = NULL;
  if (fseek(f, 0, SEEK_END))
    goto done;
  long size = ftell(f);
  long tailN = size < 65557 ? size : 65557; // the end record, and a comment up to 64 KB before it
  if (size < 22 || !(tail = malloc((size_t)tailN)) || fseek(f, size - tailN, SEEK_SET) ||
      fread(tail, 1, (size_t)tailN, f) != (size_t)tailN)
    goto done;
  long at = -1;
  for (long i = tailN - 22; i >= 0 && at < 0; i--)
    if (pd_zip_u32(tail + i) == 0x06054b50u)
      at = i;
  if (at < 0)
    goto done;
  uint32_t cdSize = pd_zip_u32(tail + at + 12), cdAt = pd_zip_u32(tail + at + 16);
  unsigned entries = pd_zip_u16(tail + at + 10);
  if ((long)cdAt + (long)cdSize > size || cdSize > (64u << 20) || !(cd = malloc(cdSize ? cdSize : 1)) ||
      fseek(f, (long)cdAt, SEEK_SET) || fread(cd, 1, cdSize, f) != cdSize)
    goto done;
  for (uint32_t p = 0; entries-- && p + 46 <= cdSize && pd_zip_u32(cd + p) == 0x02014b50u;) {
    unsigned flags = pd_zip_u16(cd + p + 8), method = pd_zip_u16(cd + p + 10);
    uint32_t nameN = pd_zip_u16(cd + p + 28), extraN = pd_zip_u16(cd + p + 30), commentN = pd_zip_u16(cd + p + 32);
    if (p + 46 + nameN > cdSize)
      break;
    char name[512];
    snprintf(name, sizeof name, "%.*s", (int)nameN, (const char *)cd + p + 46);
    const char *slash = strrchr(name, '/'), *base = slash ? slash + 1 : name;
    // Not a folder, not macOS's resource forks (__MACOSX/, ._name), not encrypted.
    if (!(flags & 1) && (method == 0 || method == 8) && *base && strncmp(name, "__MACOSX/", 9) && strncmp(base, "._", 2) &&
        pd_zip_game_name(base) && strlen(base) < sizeof out->name) {
      snprintf(out->name, sizeof out->name, "%s", base);
      out->method = method;
      out->csize = pd_zip_u32(cd + p + 20);
      out->usize = pd_zip_u32(cd + p + 24);
      out->local = pd_zip_u32(cd + p + 42);
      found = true;
      break;
    }
    p += 46 + nameN + extraN + commentN;
  }
done:
  free(tail);
  free(cd);
  fclose(f);
  return found;
}

/* The game's bytes written to dest; false if the zip can't be read or the data doesn't unpack to its stated size. */
static inline bool pd_zip_extract(const char *zip, const struct pd_zip_entry *e, const char *dest) {
  if (e->usize > (1u << 30) || e->csize > (1u << 30))
    return false;
  FILE *f = fopen(zip, "rb");
  if (!f)
    return false;
  unsigned char head[30], *packed = NULL, *data = NULL;
  bool ok = false;
  if (fseek(f, (long)e->local, SEEK_SET) || fread(head, 1, 30, f) != 30 || pd_zip_u32(head) != 0x04034b50u ||
      fseek(f, (long)(pd_zip_u16(head + 26) + pd_zip_u16(head + 28)), SEEK_CUR))
    goto done;
  packed = malloc(e->csize ? e->csize : 1);
  if (!packed || fread(packed, 1, e->csize, f) != e->csize)
    goto done;
  if (e->method == 0) {
    if (e->csize != e->usize)
      goto done;
    data = packed, packed = NULL;
  } else {
    unsigned long outN = e->usize, inN = e->csize;
    data = malloc(e->usize ? e->usize : 1);
    if (!data || puff(data, &outN, packed, &inN) != 0 || outN != e->usize)
      goto done;
  }
  FILE *o = fopen(dest, "wb");
  if (!o)
    goto done;
  ok = fwrite(data, 1, e->usize, o) == e->usize;
  ok = !fclose(o) && ok;
  if (!ok)
    remove(dest);
done:
  free(packed);
  free(data);
  fclose(f);
  return ok;
}

#endif
