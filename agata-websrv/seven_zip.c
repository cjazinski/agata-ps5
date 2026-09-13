/* seven_zip.c - 7z extraction for agata-ps5, wrapping the LZMA SDK C decoder.
 * Vendored from the public-domain LZMA SDK 23.01 (C/ directory).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#include "lzma/C/7z.h"
#include "lzma/C/7zAlloc.h"
#include "lzma/C/7zCrc.h"
#include "lzma/C/7zFile.h"

/* progress callback: (name, bytes_done) - return nonzero to cancel */
typedef int (*sz_progress)(const char* name, unsigned long done);

static ISzAlloc g_alloc = { SzAlloc, SzFree };
int (*g_cb)(const char*, unsigned long) = 0;
static unsigned long g_done_bytes = 0;
static int g_crc_ready = 0;

static int ensure_dir(const char* path) {
  char tmp[1024];
  snprintf(tmp, sizeof tmp, "%s", path);
  size_t len = strlen(tmp);
  if(len && tmp[len-1] == '/') tmp[len-1] = '\0';
  for(char* p = tmp + 1; *p; p++) {
    if(*p == '/') {
      *p = '\0';
      if(mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
      *p = '/';
    }
  }
  if(mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
  return 0;
}

/* Extract archive at arc_path into out_dir. Returns 0 on success. */
int extract_7z_progress(const char* arc_path, const char* out_dir,
               sz_progress cb, char* errbuf, size_t errcap) {
  g_cb = cb;
  g_done_bytes = 0;
  if(!g_crc_ready) { CrcGenerateTable(); g_crc_ready = 1; }

  CFileInStream archive;
  if(InFile_Open(&archive.file, arc_path) != 0) {
    snprintf(errbuf, errcap, "cannot open archive");
    return -1;
  }
  FileInStream_CreateVTable(&archive);
  CLookToRead2 lookStream;
  LookToRead2_CreateVTable(&lookStream, 0);
  lookStream.buf = (Byte*)ISzAlloc_Alloc(&g_alloc, 1 << 18);
  if(!lookStream.buf) { File_Close(&archive.file); snprintf(errbuf, errcap, "oom"); return -1; }
  lookStream.bufSize = 1 << 18;
  LookToRead2_INIT(&lookStream)
  lookStream.realStream = &archive.vt;

  ISzAlloc allocImp = g_alloc;
  ISzAlloc allocTempImp = g_alloc;

  CSzArEx db;
  SzArEx_Init(&db);

  UInt32 blockIndex = 0xFFFFFFFF;
  Byte* outBuffer = 0;
  size_t outBufferSize = 0;
  UInt16 name16[1024];

  SRes res = SzArEx_Open(&db, &lookStream.vt, &allocImp, &allocTempImp);
  if(res != SZ_OK) {
    snprintf(errbuf, errcap, "not a valid 7z (code %d)", res);
    goto fail;
  }

  if(ensure_dir(out_dir) != 0) {
    snprintf(errbuf, errcap, "cannot create output dir");
    goto fail;
  }


  for(UInt32 i = 0; i < db.NumFiles; i++) {
    size_t offset = 0, outSizeProcessed = 0;

    if(SzArEx_IsDir(&db, i)) {
      SzArEx_GetFileNameUtf16(&db, i, name16);
      char name[1024]; size_t j = 0;
      for(; name16[j] && j < sizeof name - 1; j++) name[j] = name16[j] < 0x80 ? (char)name16[j] : '?';
      name[j] = '\0';
      char full[2048];
      snprintf(full, sizeof full, "%s/%s", out_dir, name);
      ensure_dir(full);
      continue;
    }

    res = SzArEx_Extract(&db, &lookStream.vt, i,
                         &blockIndex, &outBuffer, &outBufferSize,
                         &offset, &outSizeProcessed,
                         &allocImp, &allocTempImp);
    if(res != SZ_OK) {
      snprintf(errbuf, errcap, "extract failed (file %u, code %d)", i, res);
      goto fail;
    }

    SzArEx_GetFileNameUtf16(&db, i, name16);
    char name[1024]; size_t j = 0;
    for(; name16[j] && j < sizeof name - 1; j++) name[j] = name16[j] < 0x80 ? (char)name16[j] : '?';
    name[j] = '\0';

    char full[2048];
    snprintf(full, sizeof full, "%s/%s", out_dir, name);
    char* slash = strrchr(full, '/');
    if(slash) { *slash = '\0'; ensure_dir(full); *slash = '/'; }

    FILE* f = fopen(full, "wb");
    if(!f) { snprintf(errbuf, errcap, "cannot write %s", full); goto fail; }
    if(outSizeProcessed) fwrite(outBuffer + offset, 1, outSizeProcessed, f);
    fclose(f);
    g_done_bytes += outSizeProcessed;

    if(g_cb && g_cb(name, g_done_bytes)) {
      snprintf(errbuf, errcap, "canceled");
      goto fail;
    }
  }

  if(outBuffer) ISzAlloc_Free(&g_alloc, outBuffer);
  SzArEx_Free(&db, &allocImp);
  ISzAlloc_Free(&g_alloc, lookStream.buf);
  File_Close(&archive.file);
  return 0;

fail:
  if(outBuffer) ISzAlloc_Free(&g_alloc, outBuffer);
  SzArEx_Free(&db, &allocImp);
  ISzAlloc_Free(&g_alloc, lookStream.buf);
  File_Close(&archive.file);
  return -1;
}
