#ifndef DNS_SERVER_H
#define DNS_SERVER_H

/*
 * dns_server: a small authoritative/forwarding DNS server for clients
 * connected to the SoftAP.
 *
 * ESP-IDF/lwIP ship a DNS *client* (resolver) but no DNS *server*. This
 * module implements the server side ourselves with a plain BSD UDP
 * socket + a dedicated FreeRTOS task -- there is no Lua involvement in
 * any of the packet parsing or forwarding.
 *
 * Behaviour per incoming query (A records / class IN only -- this is a
 * local ad-block/DNS-filter style resolver, not a general resolver):
 *   1. Local hostname-string record hit  -> answer immediately.
 *   2. Local hash-table record hit       -> answer immediately.
 *      (checked in that order; both are "local", checked before upstream)
 *   3. Miss -> the raw query is forwarded byte-for-byte to the first
 *      responding configured upstream server and the raw reply is
 *      relayed back to the original client. Because the original
 *      transaction ID is preserved end-to-end, no rewriting is needed.
 *
 * The hash table (dns_server_add_hash/hash_lookup) exists specifically
 * for large block-lists: it stores a 64-bit FNV-1a hash of the
 * lowercased hostname instead of the string itself (~16 bytes/entry:
 * 8-byte hash + 4-byte IP + padding, plus 2 bits/entry of
 * open-addressing bookkeeping) and is a real open-addressing hash
 * table -- O(1) average lookup, not a linear scan. 64-bit (rather than
 * 32-bit) keeps collision probability negligible even at tens of
 * thousands of entries (32-bit hashing ~50,000 domains already has a
 * meaningful collision chance by the birthday paradox). The hash MUST
 * be computed the same way the server does at lookup time: FNV-1a
 * 64-bit (offset basis 0xcbf29ce484222325, prime 0x100000001b3) over
 * the lowercased UTF-8 hostname bytes -- see fnv1a_64() in
 * dns_server.c. Because CONFIG_SPIRAM_USE_MALLOC=y and
 * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384 on this board, a single
 * malloc() for the table is automatically placed in PSRAM once its
 * size exceeds 16KB (i.e. capacity > ~1000 entries), so a "huge"
 * block-list (tens of thousands of domains) is not a problem -- it
 * just needs dns_server_hash_init() called with a large-enough
 * capacity up front. The small hostname-string table (32 entries) is
 * meant for a handful of exact overrides, not a big list.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DNS_SERVER_MAX_UPSTREAM   3
#define DNS_SERVER_MAX_RECORDS    32
/* Used only if dns_server_hash_init() is never called explicitly --
 * the first dns_server_add_hash() call will lazily init at this size. */
#define DNS_SERVER_DEFAULT_HASH_CAPACITY  1024

esp_err_t dns_server_start(void);
esp_err_t dns_server_stop(void);
bool dns_server_is_running(void);

/* Replaces the whole upstream list (1-3 entries). */
esp_err_t dns_server_set_upstream(const char *servers[], int count);
int dns_server_get_upstream(char out[][16], int max);

/* Local hostname records -- for a handful of exact-name overrides. */
esp_err_t dns_server_add(const char *hostname, const char *ip);
esp_err_t dns_server_remove(const char *hostname);
void dns_server_clear(void);
/* Returns "" (empty out_ip) if not found. */
void dns_server_lookup(const char *hostname, char out_ip[16]);

/* Local hash-keyed records (numeric key -> IP) for large block-lists.
 * Storing only a 32-bit hash instead of the hostname string is what
 * keeps a big list cheap in RAM -- see the file header comment.
 *
 * (Re)allocates the table for at least `capacity` entries (rounded up
 * internally). Calling this drops any existing hash entries. Not
 * required -- dns_server_add_hash() will lazily create a
 * DNS_SERVER_DEFAULT_HASH_CAPACITY-entry table on first use -- but
 * call it up front with your real expected size to avoid the default
 * table filling up (inserts past ~70% load factor start failing).
 */
esp_err_t dns_server_hash_init(uint32_t capacity);
esp_err_t dns_server_add_hash(uint64_t hash, const char *ip);
esp_err_t dns_server_remove_hash(uint64_t hash);
void dns_server_hash_lookup(uint64_t hash, char out_ip[16]);
uint32_t dns_server_hash_count(void);
uint32_t dns_server_hash_capacity(void);

#ifdef __cplusplus
}
#endif

#endif /* DNS_SERVER_H */
