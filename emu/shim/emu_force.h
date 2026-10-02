/* Force-included (-include) into the phone project's own C sources.
 *  - redirects fopen("/fs/...") to the emulated SPIFFS directory
 *  - (EMU_TRACK_ALLOC) routes malloc/free through the emulator heap accounting */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#ifdef __cplusplus
extern "C" {
#endif
FILE *emu_fopen(const char *path, const char *mode);
FILE *emu_freopen(const char *path, const char *mode, FILE *f);
void *emu_malloc(size_t n);
void *emu_calloc(size_t n, size_t m);
void *emu_realloc(void *p, size_t n);
void  emu_free(void *p);
#ifdef __cplusplus
}
#endif
#define fopen(p, m)      emu_fopen((p), (m))
#define freopen(p, m, f) emu_freopen((p), (m), (f))
#ifdef EMU_TRACK_ALLOC
#define malloc(n)        emu_malloc(n)
#define calloc(n, m)     emu_calloc((n), (m))
#define realloc(p, n)    emu_realloc((p), (n))
#define free(p)          emu_free(p)
#endif
