#ifndef AUDIO_SOURCE_H
#define AUDIO_SOURCE_H
/*
 * audio_source.h - where the encoded bytes come from: a file ("/fs/test.mp3") or a network stream
 * ("http://..." / "https://..."). Network sources are read by their own task into a ring buffer
 * (prebuffered) so a slow network does not stall the I2S output. Same code on the device and in the emulator.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUDIO_SRC_EOF 0
#define AUDIO_SRC_ERROR (-1)
#define AUDIO_SRC_TIMEOUT (-2)
#define AUDIO_SRC_CANCELLED (-3)

typedef bool (*audio_cancel_fn)(void *arg);
typedef struct audio_source audio_source_t;

typedef struct
{
    int timeout_ms;          /* network: connect + per-read timeout (default 15000) */
    int n_hdr;               /* network: extra request headers */
    const char *hdr_name[8];
    const char *hdr_value[8];
    const char *ca_cert;     /* network: PEM of a private CA / self-signed server (NULL = built-in bundle) */
    size_t ca_cert_len;      /* including the terminating NUL */
    size_t prebuffer_bytes;  /* network: bytes to buffer before open() returns (default 32 KiB) */
    audio_cancel_fn cancel;  /* polled while blocking; return true to abort */
    void *cancel_arg;
} audio_source_opts_t;

bool audio_source_is_url(const char *uri);
/* May block (DNS, connect, TLS, prebuffer). On failure returns NULL and fills err. */
audio_source_t *audio_source_open(const char *uri, const audio_source_opts_t *opts, char *err, size_t errn);
/* > 0 bytes read | AUDIO_SRC_EOF | AUDIO_SRC_ERROR (see audio_source_error) | AUDIO_SRC_TIMEOUT | AUDIO_SRC_CANCELLED */
int audio_source_read(audio_source_t *s, uint8_t *buf, size_t n, uint32_t timeout_ms);
const char *audio_source_error(audio_source_t *s);
int64_t audio_source_size(audio_source_t *s); /* -1 = unknown / endless */
size_t audio_source_buffered(audio_source_t *s); /* network: bytes waiting in the ring buffer, files: 0 */
void audio_source_abort(audio_source_t *s);   /* wake a blocked read from another task */
void audio_source_close(audio_source_t *s);

#endif /* AUDIO_SOURCE_H */
