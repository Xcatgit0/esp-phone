#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "dns_server.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "dns_server";

#define DNS_PORT        53
#define DNS_MAX_PACKET  512
#define DNS_FWD_TIMEOUT_SEC 3

typedef struct {
    bool used;
    char hostname[64];
    uint32_t ip; /* network byte order */
} dns_record_t;

/*
 * Open-addressing hash table for the (potentially huge) block-list.
 * Each slot is 8 bytes (hash + ip); occupancy/tombstone state lives in
 * two separate 1-bit-per-slot bitmaps rather than inside the slot
 * struct, so the per-entry cost stays at 8 bytes + 2 bits instead of
 * ballooning to 12+ bytes from struct padding around a bool field.
 *
 * s_hash_occupied[i] = 0 -> slot i has never been used (probe stops here)
 * s_hash_occupied[i] = 1, s_hash_tombstone[i] = 0 -> slot i holds live data
 * s_hash_occupied[i] = 1, s_hash_tombstone[i] = 1 -> slot i was deleted
 *   (probe must continue past it, but inserts may reuse it)
 */
typedef struct {
    uint64_t hash;
    uint32_t ip; /* network byte order */
} dns_hash_slot_t;

static dns_record_t s_records[DNS_SERVER_MAX_RECORDS];

static dns_hash_slot_t *s_hash_table = NULL;
static uint8_t *s_hash_occupied = NULL;
static uint8_t *s_hash_tombstone = NULL;
static uint32_t s_hash_capacity = 0;
static uint32_t s_hash_count = 0;      /* live entries (excludes tombstones) */

static char s_upstream[DNS_SERVER_MAX_UPSTREAM][16];
static int s_upstream_count = 0;

static SemaphoreHandle_t s_mutex = NULL;
static TaskHandle_t s_task = NULL;
static int s_sock = -1;
static volatile bool s_running = false;

#define LOCK()   xSemaphoreTake(s_mutex, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mutex)

static inline bool bitmap_get(const uint8_t *bm, uint32_t i) { return (bm[i >> 3] >> (i & 7)) & 1; }
static inline void bitmap_set(uint8_t *bm, uint32_t i)   { bm[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static inline void bitmap_clear(uint8_t *bm, uint32_t i) { bm[i >> 3] &= (uint8_t)~(1u << (i & 7)); }

static void ensure_mutex(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
    }
}

/* ---- wire format helpers ------------------------------------------- */

/* Reads a (non-compressed) QNAME starting at buf[offset], writes the
 * dotted-string form into out. Returns the offset right after the
 * terminating zero byte, or -1 on a malformed/oversized name. */
static int parse_qname(const uint8_t *buf, int len, int offset, char *out, int out_len)
{
    int opos = 0;
    while (offset < len) {
        uint8_t l = buf[offset++];
        if (l == 0) {
            out[opos] = '\0';
            return offset;
        }
        if (l & 0xC0) {
            /* compression pointer -- not expected/needed in a question */
            return -1;
        }
        if (offset + l > len || opos + l + 1 >= out_len) {
            return -1;
        }
        if (opos > 0) out[opos++] = '.';
        memcpy(out + opos, buf + offset, l);
        opos += l;
        offset += l;
    }
    return -1;
}

static void to_lower_inplace(char *s)
{
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

/* FNV-1a 64-bit -- matches a JS implementation using
 * offset basis 0xcbf29ce484222325n and prime 0x100000001b3n exactly,
 * byte-for-byte, as long as the same lowercase UTF-8 bytes are hashed
 * on both sides. IMPORTANT: the caller must lowercase the domain
 * before hashing -- dns_server_add_hash() takes a raw number with no
 * hostname to normalize, and the live lookup in dns_task() always
 * hashes the lowercased query name. */
static uint64_t fnv1a_64(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 0x100000001b3ULL; }
    return h;
}

/* Builds a single-answer A-record reply in place, reusing the original
 * query's header/question section up to `question_end` (right after
 * QTYPE/QCLASS) -- so the transaction ID and question survive
 * untouched. Deliberately does NOT preserve anything beyond the
 * question (e.g. an EDNS0 OPT pseudo-record in the query's Additional
 * section): NSCOUNT/ARCOUNT are forced to 0, since our synthetic
 * single-answer reply has no authority/additional records of its own.
 * Returns the total reply length. */
static int build_answer(uint8_t *pkt, int question_end, uint32_t ip_net_order)
{
    pkt[2] = 0x84 | (pkt[2] & 0x01); /* QR=1, AA=1, keep original RD bit */
    pkt[3] = 0x80;                   /* RA=1, RCODE=0 */
    pkt[6] = 0x00; pkt[7] = 0x01;    /* ANCOUNT = 1 */
    pkt[8] = 0x00; pkt[9] = 0x00;    /* NSCOUNT = 0 */
    pkt[10] = 0x00; pkt[11] = 0x00;  /* ARCOUNT = 0 (drops any EDNS OPT etc.) */

    int p = question_end;
    pkt[p++] = 0xC0; pkt[p++] = 0x0C; /* name = pointer to offset 12 */
    pkt[p++] = 0x00; pkt[p++] = 0x01; /* TYPE = A */
    pkt[p++] = 0x00; pkt[p++] = 0x01; /* CLASS = IN */
    pkt[p++] = 0x00; pkt[p++] = 0x00; pkt[p++] = 0x00; pkt[p++] = 0x3C; /* TTL = 60s */
    pkt[p++] = 0x00; pkt[p++] = 0x04; /* RDLENGTH = 4 */
    memcpy(&pkt[p], &ip_net_order, 4);
    p += 4;
    return p;
}

/* ---- local record lookup -------------------------------------------- */

static bool lookup_hostname_locked(const char *hostname, uint32_t *ip_out)
{
    for (int i = 0; i < DNS_SERVER_MAX_RECORDS; i++) {
        if (s_records[i].used && strcmp(s_records[i].hostname, hostname) == 0) {
            *ip_out = s_records[i].ip;
            return true;
        }
    }
    return false;
}

/* Finds the slot for `hash`. If for_insert is true and no live entry
 * matches, returns a slot usable for insertion (preferring the first
 * tombstone seen over a never-used slot, to keep clusters short).
 * Returns false if not found (lookup) or the table is full (insert). */
static bool hash_find_slot(uint64_t hash, uint32_t *out_index, bool for_insert)
{
    if (s_hash_capacity == 0) return false;

    uint32_t start = hash % s_hash_capacity;
    uint32_t first_tombstone = UINT32_MAX;

    for (uint32_t probe = 0; probe < s_hash_capacity; probe++) {
        uint32_t i = (start + probe) % s_hash_capacity;

        if (!bitmap_get(s_hash_occupied, i)) {
            /* never used -- end of any probe chain that could contain
             * this key */
            if (for_insert) {
                *out_index = (first_tombstone != UINT32_MAX) ? first_tombstone : i;
                return true;
            }
            return false;
        }
        if (bitmap_get(s_hash_tombstone, i)) {
            if (for_insert && first_tombstone == UINT32_MAX) first_tombstone = i;
            continue; /* deleted slot: keep probing */
        }
        if (s_hash_table[i].hash == hash) {
            *out_index = i;
            return true;
        }
    }
    /* probed the whole table without finding an empty slot */
    if (for_insert && first_tombstone != UINT32_MAX) {
        *out_index = first_tombstone;
        return true;
    }
    return false;
}

static bool lookup_hash_locked(uint64_t hash, uint32_t *ip_out)
{
    uint32_t i;
    if (!hash_find_slot(hash, &i, false)) return false;
    *ip_out = s_hash_table[i].ip;
    return true;
}

/* ---- forwarding ------------------------------------------------------*/

static void forward_query(const uint8_t *query, int qlen,
                           const struct sockaddr_in *client, socklen_t client_len)
{
    char servers[DNS_SERVER_MAX_UPSTREAM][16];
    int count;
    LOCK();
    count = s_upstream_count;
    memcpy(servers, s_upstream, sizeof(servers));
    UNLOCK();

    if (count == 0) {
        ESP_LOGW(TAG, "no upstream configured, dropping unresolved query");
        return;
    }

    int fsock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fsock < 0) return;

    struct timeval tv = { .tv_sec = DNS_FWD_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(fsock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t reply[DNS_MAX_PACKET];
    for (int i = 0; i < count; i++) {
        struct sockaddr_in up = { 0 };
        up.sin_family = AF_INET;
        up.sin_port = htons(DNS_PORT);
        up.sin_addr.s_addr = inet_addr(servers[i]);

        if (sendto(fsock, query, qlen, 0, (struct sockaddr *)&up, sizeof(up)) < 0) {
            continue;
        }
        int rn = recvfrom(fsock, reply, sizeof(reply), 0, NULL, NULL);
        if (rn > 0) {
            sendto(s_sock, reply, rn, 0, (struct sockaddr *)client, client_len);
            close(fsock);
            return;
        }
        ESP_LOGW(TAG, "upstream %s timed out", servers[i]);
    }
    close(fsock);
}

/* ---- server task ------------------------------------------------------*/

static void dns_task(void *arg)
{
    (void)arg;
    uint8_t buf[DNS_MAX_PACKET];
    ESP_LOGI(TAG, "started");

    while (s_running) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(s_sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &from_len);
        if (n < 0) {
            continue; /* SO_RCVTIMEO expiry -- just re-check s_running */
        }
        if (n < 12) continue; /* shorter than a DNS header */

        uint16_t qdcount = (buf[4] << 8) | buf[5];
        if (qdcount != 1) {
            /* only single-question queries are handled, matching what
             * every mainstream stub resolver sends */
            continue;
        }

        char qname[256];
        int after_name = parse_qname(buf, n, 12, qname, sizeof(qname));
        if (after_name < 0 || after_name + 4 > n) continue;

        uint16_t qtype = (buf[after_name] << 8) | buf[after_name + 1];
        int question_end = after_name + 4; /* right after QTYPE+QCLASS */
        to_lower_inplace(qname);

        uint32_t ip_net = 0;
        bool found = false;

        if (qtype == 1 /* A */) {
            LOCK();
            found = lookup_hostname_locked(qname, &ip_net);
            if (!found) {
                found = lookup_hash_locked(fnv1a_64(qname), &ip_net);
            }
            UNLOCK();
        }

        if (found) {
            /* Build the reply on top of just the question (dropping any
             * trailing Additional-section data from the query, e.g. an
             * EDNS0 OPT record) -- see build_answer()'s comment. */
            int reply_len = build_answer(buf, question_end, ip_net);
            sendto(s_sock, buf, reply_len, 0, (struct sockaddr *)&from, from_len);
        } else {
            /* Forwarding still sends the client's original query
             * byte-for-byte (including any OPT record) so upstream sees
             * exactly what the client asked for. */
            forward_query(buf, n, &from, from_len);
        }
    }

    ESP_LOGI(TAG, "task exiting");
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ---- public API ------------------------------------------------------*/

esp_err_t dns_server_start(void)
{
    ensure_mutex();
    if (s_running) return ESP_OK;

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        return ESP_FAIL;
    }

    int reuse = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DNS_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind() to port 53 failed");
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }

    /* 1s recv timeout so the task wakes up periodically to check
     * s_running -- this is the clean-shutdown mechanism, not a busy
     * poll: the task is blocked in recvfrom() the rest of the time. */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    s_running = true;
    BaseType_t ok = xTaskCreatePinnedToCore(dns_task, "dns_server", 4096, NULL, 5, &s_task, 0);
    if (ok != pdPASS) {
        s_running = false;
        close(s_sock);
        s_sock = -1;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t dns_server_stop(void)
{
    if (!s_running) return ESP_OK;
    s_running = false;
    /* give the task up to ~1.2s to notice and self-delete */
    for (int i = 0; i < 12 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_sock >= 0) { close(s_sock); s_sock = -1; }
    return ESP_OK;
}

bool dns_server_is_running(void)
{
    return s_running;
}

esp_err_t dns_server_set_upstream(const char *servers[], int count)
{
    ensure_mutex();
    if (count < 0) count = 0;
    if (count > DNS_SERVER_MAX_UPSTREAM) count = DNS_SERVER_MAX_UPSTREAM;

    LOCK();
    s_upstream_count = 0;
    for (int i = 0; i < count; i++) {
        struct in_addr test;
        if (inet_aton(servers[i], &test) == 0) continue; /* skip invalid */
        strncpy(s_upstream[s_upstream_count], servers[i], sizeof(s_upstream[0]) - 1);
        s_upstream_count++;
    }
    int applied = s_upstream_count;
    UNLOCK();
    return applied > 0 ? ESP_OK : ESP_ERR_INVALID_ARG;
}

int dns_server_get_upstream(char out[][16], int max)
{
    ensure_mutex();
    LOCK();
    int n = s_upstream_count > max ? max : s_upstream_count;
    for (int i = 0; i < n; i++) {
        strncpy(out[i], s_upstream[i], 16);
    }
    UNLOCK();
    return n;
}

esp_err_t dns_server_add(const char *hostname, const char *ip)
{
    ensure_mutex();
    struct in_addr addr;
    if (inet_aton(ip, &addr) == 0) return ESP_ERR_INVALID_ARG;
    if (strlen(hostname) >= sizeof(s_records[0].hostname)) return ESP_ERR_INVALID_ARG;

    char lower[64];
    strncpy(lower, hostname, sizeof(lower) - 1);
    lower[sizeof(lower) - 1] = '\0';
    to_lower_inplace(lower);

    LOCK();
    int free_slot = -1;
    for (int i = 0; i < DNS_SERVER_MAX_RECORDS; i++) {
        if (s_records[i].used && strcmp(s_records[i].hostname, lower) == 0) {
            s_records[i].ip = addr.s_addr;
            UNLOCK();
            return ESP_OK;
        }
        if (!s_records[i].used && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) { UNLOCK(); return ESP_ERR_NO_MEM; }
    s_records[free_slot].used = true;
    strncpy(s_records[free_slot].hostname, lower, sizeof(s_records[free_slot].hostname) - 1);
    s_records[free_slot].ip = addr.s_addr;
    UNLOCK();
    return ESP_OK;
}

esp_err_t dns_server_remove(const char *hostname)
{
    ensure_mutex();
    char lower[64];
    strncpy(lower, hostname, sizeof(lower) - 1);
    lower[sizeof(lower) - 1] = '\0';
    to_lower_inplace(lower);

    LOCK();
    for (int i = 0; i < DNS_SERVER_MAX_RECORDS; i++) {
        if (s_records[i].used && strcmp(s_records[i].hostname, lower) == 0) {
            s_records[i].used = false;
            UNLOCK();
            return ESP_OK;
        }
    }
    UNLOCK();
    return ESP_ERR_NOT_FOUND;
}

void dns_server_clear(void)
{
    ensure_mutex();
    LOCK();
    memset(s_records, 0, sizeof(s_records));
    if (s_hash_capacity > 0) {
        memset(s_hash_occupied, 0, (s_hash_capacity + 7) / 8);
        memset(s_hash_tombstone, 0, (s_hash_capacity + 7) / 8);
        s_hash_count = 0;
    }
    UNLOCK();
}

void dns_server_lookup(const char *hostname, char out_ip[16])
{
    ensure_mutex();
    out_ip[0] = '\0';
    char lower[64];
    strncpy(lower, hostname, sizeof(lower) - 1);
    lower[sizeof(lower) - 1] = '\0';
    to_lower_inplace(lower);

    LOCK();
    uint32_t ip;
    if (lookup_hostname_locked(lower, &ip)) {
        struct in_addr a = { .s_addr = ip };
        strncpy(out_ip, inet_ntoa(a), 16);
    }
    UNLOCK();
}

esp_err_t dns_server_hash_init(uint32_t capacity)
{
    ensure_mutex();
    if (capacity < 8) capacity = 8;
    /* keep load factor under ~70% at the caller's requested capacity so
     * probe chains stay short */
    uint32_t alloc_capacity = capacity + (capacity / 3) + 1;
	dns_hash_slot_t *table = heap_caps_calloc(
	    alloc_capacity,
	    sizeof(dns_hash_slot_t),
		    MALLOC_CAP_SPIRAM
	);
//    dns_hash_slot_t *table = calloc(alloc_capacity, sizeof(dns_hash_slot_t));
    uint32_t bitmap_bytes = (alloc_capacity + 7) / 8;
//    uint8_t *occ = calloc(bitmap_bytes, 1);
//    uint8_t *tomb = calloc(bitmap_bytes, 1);

uint8_t *occ = heap_caps_calloc(
    bitmap_bytes, 1,
    MALLOC_CAP_SPIRAM
);

uint8_t *tomb = heap_caps_calloc(
    bitmap_bytes, 1,
    MALLOC_CAP_SPIRAM
);

    if (table == NULL || occ == NULL || tomb == NULL) {
        free(table); free(occ); free(tomb);
        ESP_LOGE(TAG, "hash table alloc failed for capacity=%u", (unsigned)capacity);
        return ESP_ERR_NO_MEM;
    }

    LOCK();
    free(s_hash_table);
    free(s_hash_occupied);
    free(s_hash_tombstone);
    s_hash_table = table;
    s_hash_occupied = occ;
    s_hash_tombstone = tomb;
    s_hash_capacity = alloc_capacity;
    s_hash_count = 0;
    UNLOCK();

    ESP_LOGI(TAG, "hash table ready: capacity=%u entries (~%u KB)",
             (unsigned)alloc_capacity,
             (unsigned)((alloc_capacity * sizeof(dns_hash_slot_t) + 2 * bitmap_bytes) / 1024));
    return ESP_OK;
}

esp_err_t dns_server_add_hash(uint64_t hash, const char *ip)
{
    ensure_mutex();
    struct in_addr addr;
    if (inet_aton(ip, &addr) == 0) return ESP_ERR_INVALID_ARG;

    if (s_hash_capacity == 0) {
        esp_err_t ie = dns_server_hash_init(DNS_SERVER_DEFAULT_HASH_CAPACITY);
        if (ie != ESP_OK) return ie;
    }

    LOCK();
    uint32_t i;
    if (!hash_find_slot(hash, &i, true)) {
        UNLOCK();
        ESP_LOGW(TAG, "hash table full (capacity=%u) -- call dns_server_hash_init() "
                      "with a larger capacity before adding more entries", (unsigned)s_hash_capacity);
        return ESP_ERR_NO_MEM;
    }

    bool is_new = !(bitmap_get(s_hash_occupied, i) && !bitmap_get(s_hash_tombstone, i));
    s_hash_table[i].hash = hash;
    s_hash_table[i].ip = addr.s_addr;
    bitmap_set(s_hash_occupied, i);
    bitmap_clear(s_hash_tombstone, i);
    if (is_new) s_hash_count++;
    UNLOCK();
    return ESP_OK;
}

esp_err_t dns_server_remove_hash(uint64_t hash)
{
    ensure_mutex();
    LOCK();
    uint32_t i;
    if (!hash_find_slot(hash, &i, false)) {
        UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    bitmap_set(s_hash_tombstone, i);
    if (s_hash_count > 0) s_hash_count--;
    UNLOCK();
    return ESP_OK;
}

void dns_server_hash_lookup(uint64_t hash, char out_ip[16])
{
    ensure_mutex();
    out_ip[0] = '\0';
    LOCK();
    uint32_t ip;
    if (lookup_hash_locked(hash, &ip)) {
        struct in_addr a = { .s_addr = ip };
        strncpy(out_ip, inet_ntoa(a), 16);
    }
    UNLOCK();
}

uint32_t dns_server_hash_count(void)
{
    ensure_mutex();
    LOCK(); uint32_t c = s_hash_count; UNLOCK();
    return c;
}

uint32_t dns_server_hash_capacity(void)
{
    ensure_mutex();
    LOCK(); uint32_t c = s_hash_capacity; UNLOCK();
    return c;
}
