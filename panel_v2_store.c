/* panel_v2_store.c - M2 storage layer (part 1/3): helpers + auxiliary KV.
 *
 * Two interchangeable backends behind one vtable:
 *   PV2_IMPL_SQLITE   sqlite3 amalgamation, WAL + configurable synchronous level
 *   PV2_IMPL_JOURNAL  append-only log with per-transaction COMMIT records
 *                     (the "transactional recovery log" fallback of sec.4)
 * Both implement the same primitives, so duplicate detection, conflict
 * detection, revision assignment, change sequencing and tombstone bookkeeping
 * live exactly once in pv2_store_apply() (part 3).
 */
#include "panel_v2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>

#ifdef _WIN32
#include <direct.h>          /* _mkdir, host build only */
#include <io.h>              /* _commit, host build only */
#include <windows.h>
#include <bcrypt.h>          /* BCryptGenRandom, host build only (link -lbcrypt) */
#endif

#ifdef PV2_HAVE_SQLITE
#include "sqlite3.h"
#endif

/* T05B: the MSVCRT/mingw open() defaults to TEXT mode, which expands every
 * 0x0a byte to 0x0d 0x0a on write. Journal records are binary (CRCs over
 * timestamps routinely contain 0x0a), so a text-mode fd silently corrupts the
 * log and the next load truncates it. Force binary on Windows; POSIX has no
 * O_BINARY. */
#ifndef O_BINARY
#define O_BINARY 0
#endif

/* Durability shims: the panel target is POSIX (fdatasync/fsync); the Windows
 * host build used by the M2 harness only offers _commit(). */
#ifdef _WIN32
static int pv2_fdatasync(int fd) { return _commit(fd); }
static int pv2_fsync(int fd)     { return _commit(fd); }
#else
static int pv2_fdatasync(int fd) { return fdatasync(fd); }
static int pv2_fsync(int fd)     { return fsync(fd); }
#endif

/* T04C (F06): same-directory atomic replace for the KV flush pipeline.
 * POSIX rename() replaces an existing destination; the zig mingw runtime is
 * strict-POSIX and fails with EEXIST instead, so the Windows host build uses
 * MoveFileExW with the replace flag (WRITE_THROUGH for durability). */
#ifdef _WIN32
static int kv_replace(const char *tmp, const char *path)
{
    wchar_t wtmp[640], wpath[640];
    if (MultiByteToWideChar(CP_UTF8, 0, tmp, -1, wtmp, 640) == 0) return -1;
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 640) == 0) return -1;
    if (!MoveFileExW(wtmp, wpath,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return -1;
    return 0;
}
#else
static int kv_replace(const char *tmp, const char *path)
{
    return rename(tmp, path);
}
#endif

/* ------------------------------------------------------------------ utils */
long long pv2_now_ms(void)
{
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
#endif
    return (long long)time(NULL) * 1000LL;
}

long long pv2_wall_seconds(void) { return (long long)time(NULL); }

long pv2_rss_kb(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    long kb = -1;
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) { sscanf(line + 6, "%ld", &kb); break; }
    }
    fclose(f);
    return kb;
}

/* T04B (F05 fix): the ONLY randomness source is the OS CSPRNG. The previous
 * xorshift(PRNG seeded from urandom+time+pid) is gone; there is no time/pid
 * fallback. A short read or unavailable source is a hard failure so callers
 * refuse to mint secrets (security-v2.md §4.2, SEC-07). */
static int os_random_bytes(unsigned char *out, size_t n)
{
#if defined(_WIN32)
    if (n == 0) return 0;
    /* NTSTATUS success is 0; mingw's bcrypt.h does not define STATUS_SUCCESS */
    if (BCryptGenRandom(NULL, (PUCHAR)out, (ULONG)n,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return -1;
    return 0;
#else
    /* POSIX/musl (cortex-a7 board): full reads from /dev/urandom; EINTR
     * retries, short reads continue, EOF or any error is fatal. */
    int fd, ok = 0;
    size_t got = 0;
    if (n == 0) return 0;
    do {
        fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return -1;
    while (got < n) {
        ssize_t r = read(fd, out + got, n - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            ok = -1;
            break;
        }
        if (r == 0) { ok = -1; break; }          /* unexpected EOF */
        got += (size_t)r;
    }
    close(fd);
    return ok ? -1 : 0;
#endif
}

/* Test-only hook (never set by production code): lets the harness install a
 * failing / short-reading / deterministic source. */
static pv2_random_fn g_rand_inject = NULL;

int pv2_random_bytes(unsigned char *out, size_t n)
{
    if (!out && n) return -1;
    if (g_rand_inject) return g_rand_inject(out, n);
    return os_random_bytes(out, n);
}

void pv2_random_set_inject(pv2_random_fn fn) { g_rand_inject = fn; }

int pv2_random_hex(char *out, size_t nbytes)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char buf[64];
    size_t i;
    if (nbytes == 0) { out[0] = '\0'; return 0; }
    if (nbytes > sizeof buf) return -1;
    if (pv2_random_bytes(buf, nbytes) != 0) return -1;
    for (i = 0; i < nbytes; i++) {
        out[i * 2]     = hex[buf[i] >> 4];
        out[i * 2 + 1] = hex[buf[i] & 0x0F];
    }
    out[nbytes * 2] = '\0';
    return 0;
}

int pv2_const_time_eq(const char *a, const char *b)
{
    size_t la, lb, i;
    unsigned char diff;
    if (!a || !b) return 0;
    la = strlen(a); lb = strlen(b);
    diff = (unsigned char)(la ^ lb);
    for (i = 0; i < la && i < lb; i++) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

/* Portable UTC breakdown: gmtime_r on POSIX targets (board), gmtime on the
 * Windows host build used for the M2 host verification harness. */
static void pv2_gmtime_utc(time_t t, struct tm *out)
{
#ifdef _WIN32
    struct tm *p = gmtime(&t);
    if (p) *out = *p; else memset(out, 0, sizeof *out);
#else
    if (gmtime_r(&t, out) == NULL) memset(out, 0, sizeof *out);
#endif
}

const char *pv2_clock_iso(long long epoch_sec, char *buf, size_t n)
{
    time_t t = (time_t)epoch_sec;
    struct tm tmv;
    pv2_gmtime_utc(t, &tmv);
    snprintf(buf, n, "%04d-%02d-%02dT%02d:%02d:%02dZ",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

const char *pv2_date_of(long long epoch_sec, char *buf, size_t n)
{
    time_t t = (time_t)epoch_sec;
    struct tm tmv;
    pv2_gmtime_utc(t, &tmv);
    snprintf(buf, n, "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    return buf;
}

int pv2_uri_contains_token(const char *uri, const char *token)
{
    if (!uri || !token || token[0] == '\0') return 0;
    return strstr(uri, token) != NULL;
}

const char *pv2_entity_name(int entity)
{
    switch (entity) {
    case PV2_ENT_WORD:       return "word";
    case PV2_ENT_ANNOTATION: return "annotation";
    case PV2_ENT_PLAN:       return "daily_plan";
    case PV2_ENT_REVIEW:     return "review_event";
    case PV2_ENT_BRIEFING:   return "briefing";
    case PV2_ENT_COURSE:     return "course_occurrence";   /* N3 */
    case PV2_ENT_PLAN_ITEM:  return "plan_item";           /* N3 */
    case PV2_ENT_WINDOW:     return "schedule_window";     /* N3 */
    default:                 return "unknown";
    }
}

int pv2_entity_from_name(const char *name)
{
    if (!name) return 0;
    if (!strcmp(name, "word"))              return PV2_ENT_WORD;
    if (!strcmp(name, "annotation"))        return PV2_ENT_ANNOTATION;
    if (!strcmp(name, "daily_plan"))        return PV2_ENT_PLAN;
    if (!strcmp(name, "review_event"))      return PV2_ENT_REVIEW;
    if (!strcmp(name, "briefing"))          return PV2_ENT_BRIEFING;
    if (!strcmp(name, "course_occurrence")) return PV2_ENT_COURSE;    /* N3 */
    if (!strcmp(name, "plan_item"))         return PV2_ENT_PLAN_ITEM;/* N3 */
    if (!strcmp(name, "schedule_window"))   return PV2_ENT_WINDOW;   /* N3 */
    return 0;
}

const char *pv2_op_status_name(int status)
{
    switch (status) {
    case PV2_ST_APPLIED:   return "applied";
    case PV2_ST_DUPLICATE: return "duplicate";
    case PV2_ST_CONFLICT:  return "conflict";
    case PV2_ST_REJECTED:  return "rejected";
    default:               return "storage_failure";
    }
}

int pv2_http_for_status(int status)
{
    switch (status) {
    case PV2_ST_APPLIED:   return 200;
    case PV2_ST_DUPLICATE: return 200;
    case PV2_ST_CONFLICT:  return 409;
    case PV2_ST_REJECTED:  return 422;
    default:               return 500;
    }
}

/* POSIX mkdir(path, mode); the Windows host build only has _mkdir(path). */
static void pv2_mkdir(const char *path)
{
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0700);
#endif
}

static void mkdir_p(const char *path)
{
    char tmp[600];
    size_t n = strlen(path), i;
    if (n >= sizeof tmp) return;
    memcpy(tmp, path, n + 1);
    for (i = 1; i < n; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char c = tmp[i];
            tmp[i] = '\0';
            pv2_mkdir(tmp);
            tmp[i] = c;
        }
    }
    pv2_mkdir(tmp);
}

static long long file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (long long)st.st_size;
}

static uint32_t crc32_of(const unsigned char *d, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        crc ^= d[i];
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(~((int)(crc & 1) - 1)));
    }
    return ~crc;
}

/* ================================================================ aux KV */
struct pv2_kv {
    char      path[600];
    char     *keys[512];
    char     *vals[512];
    int       n;
    long long bytes;
};

static int kv_index(pv2_kv *kv, const char *key)
{
    int i;
    for (i = 0; i < kv->n; i++)
        if (!strcmp(kv->keys[i], key)) return i;
    return -1;
}

/* T04C (F06 fix): the whole persistence pipeline is error-checked — write,
 * file fsync, close, same-directory rename and directory sync. Any failure
 * removes the temp file and returns -1; callers must refuse to report a
 * "persisted" success. Memory mutations are rolled back by pv2_kv_set/del on
 * flush failure so memory and disk never diverge. */
static int g_kv_inject_fail = 0;   /* test-only hook (never set in production) */

void pv2_kv_set_inject_fail(int fail) { g_kv_inject_fail = fail; }

static int write_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int kv_put_rec(int fd, const char *k, const char *v)
{
    char hdr[32];
    snprintf(hdr, sizeof hdr, "%u\n", (unsigned)strlen(k));
    if (write_all(fd, hdr, strlen(hdr)) != 0) return -1;
    if (write_all(fd, k, strlen(k)) != 0) return -1;
    if (write_all(fd, "\n", 1) != 0) return -1;
    snprintf(hdr, sizeof hdr, "%u\n", (unsigned)strlen(v));
    if (write_all(fd, hdr, strlen(hdr)) != 0) return -1;
    if (write_all(fd, v, strlen(v)) != 0) return -1;
    if (write_all(fd, "\n", 1) != 0) return -1;
    return 0;
}

static void kv_dirname(const char *path, char *out, size_t n)
{
    char *slash;
    snprintf(out, n, "%s", path);
    slash = strrchr(out, '/');
    if (!slash) slash = strrchr(out, '\\');
    if (slash) *slash = '\0';
    else snprintf(out, n, ".");
}

static int kv_flush(pv2_kv *kv)
{
    char tmp[640], dir[640];
    int fd, i, bad = 0;
    if (!kv) return -1;
    /* test-only injection (T04C): forces the flush pipeline to fail */
    if (g_kv_inject_fail) return -1;
    snprintf(tmp, sizeof tmp, "%s.tmp", kv->path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0600);
    if (fd < 0) return -1;
    for (i = 0; i < kv->n && !bad; i++)
        if (kv_put_rec(fd, kv->keys[i], kv->vals[i]) != 0) bad = 1;
    if (!bad && pv2_fsync(fd) != 0) bad = 1;
    if (close(fd) != 0 && !bad) bad = 1;
    if (!bad && kv_replace(tmp, kv->path) != 0) bad = 1;
#if !defined(_WIN32)
    /* directory sync so the rename itself survives power loss (POSIX only;
     * the Windows host build has no directory fsync semantics) */
    if (!bad) {
        int dfd;
        kv_dirname(kv->path, dir, sizeof dir);
        dfd = open(dir, O_RDONLY);
        if (dfd >= 0) {
            if (fsync(dfd) != 0) bad = 1;
            close(dfd);
        }
        /* an unreadable directory here is reported via the next flush; the
         * file rename itself already succeeded */
    }
#endif
    if (bad) unlink(tmp);
    else chmod(kv->path, 0600);
    return bad ? -1 : 0;
}

pv2_kv *pv2_kv_open(const char *path, char *err, size_t errn)
{
    pv2_kv *kv = (pv2_kv *)calloc(1, sizeof *kv);
    FILE *f;
    if (!kv) { if (err) snprintf(err, errn, "kv alloc failed"); return NULL; }
    snprintf(kv->path, sizeof kv->path, "%s", path);
    f = fopen(path, "rb");
    if (f) {
        while (kv->n < 512) {
            char lbuf[32];
            unsigned long kl, vl;
            char *k, *v;
            if (!fgets(lbuf, sizeof lbuf, f)) break;
            kl = strtoul(lbuf, NULL, 10);
            if (kl == 0 || kl > 4096) break;
            k = (char *)malloc(kl + 1);
            if (fread(k, 1, kl, f) != kl) { free(k); break; }
            k[kl] = '\0';
            if (fgetc(f) == EOF) { free(k); break; }
            if (!fgets(lbuf, sizeof lbuf, f)) { free(k); break; }
            vl = strtoul(lbuf, NULL, 10);
            if (vl > 262144) { free(k); break; }
            v = (char *)malloc(vl + 1);
            if (fread(v, 1, vl, f) != vl) { free(k); free(v); break; }
            v[vl] = '\0';
            if (fgetc(f) == EOF) { free(k); free(v); break; }
            kv->keys[kv->n] = k;
            kv->vals[kv->n] = v;
            kv->bytes += (long long)(kl + vl);
            kv->n++;
        }
        fclose(f);
    }
    (void)err; (void)errn;
    return kv;
}

void pv2_kv_close(pv2_kv *kv)
{
    int i;
    if (!kv) return;
    for (i = 0; i < kv->n; i++) { free(kv->keys[i]); free(kv->vals[i]); }
    free(kv);
}

/* return 0 = persisted, -1 = flush failure (memory rolled back to the exact
 * on-disk state; T04C) */
int pv2_kv_set(pv2_kv *kv, const char *key, const char *val)
{
    int i, existed;
    char *old_val = NULL;
    char *nk = NULL, *nv = NULL;
    if (!kv) return -1;
    i = kv_index(kv, key);
    existed = (i >= 0);
    if (!existed && kv->n >= 512) return -1;
    /* 1. mutate memory */
    if (existed) {
        old_val = kv->vals[i];
        nv = strdup(val ? val : "");
        if (!nv) return -1;
        kv->bytes -= (long long)strlen(old_val);
        kv->vals[i] = nv;
        kv->bytes += (long long)strlen(nv);
    } else {
        nk = strdup(key);
        nv = strdup(val ? val : "");
        if (!nk || !nv) { free(nk); free(nv); return -1; }
        kv->keys[kv->n] = nk;
        kv->vals[kv->n] = nv;
        kv->bytes += (long long)(strlen(nk) + strlen(nv));
        kv->n++;
    }
    /* 2. persist; 3. roll back on failure so disk stays authoritative */
    if (kv_flush(kv) != 0) {
        if (existed) {
            /* kv->vals[i] is nv here; hand the old value back */
            kv->bytes -= (long long)strlen(kv->vals[i]);
            kv->vals[i] = old_val;
            kv->bytes += (long long)strlen(old_val);
        } else {
            kv->n--;
            kv->bytes -= (long long)(strlen(nk) + strlen(nv));
            free(nk);
            free(nv);
        }
        return -1;
    }
    if (existed) free(old_val);
    return 0;
}

const char *pv2_kv_get(pv2_kv *kv, const char *key)
{
    int i;
    if (!kv) return NULL;
    i = kv_index(kv, key);
    return i >= 0 ? kv->vals[i] : NULL;
}

/* return 1 = deleted and persisted, 0 = key absent, -1 = flush failure
 * (memory entry restored; T04C) */
int pv2_kv_del(pv2_kv *kv, const char *key)
{
    int i, j;
    char *dk = NULL, *dv = NULL;
    long long freed = 0;
    if (!kv) return 0;
    i = kv_index(kv, key);
    if (i < 0) return 0;
    dk = kv->keys[i];
    dv = kv->vals[i];
    freed = (long long)(strlen(dk) + strlen(dv));
    for (j = i; j + 1 < kv->n; j++) { kv->keys[j] = kv->keys[j + 1]; kv->vals[j] = kv->vals[j + 1]; }
    kv->n--;
    kv->bytes -= freed;
    if (kv_flush(kv) != 0) {
        /* put the entry back at the tail; KV has no ordering guarantee and
         * readers match by key, so tail re-insertion is a faithful restore */
        kv->keys[kv->n] = dk;
        kv->vals[kv->n] = dv;
        kv->n++;
        kv->bytes += freed;
        return -1;
    }
    free(dk);
    free(dv);
    return 1;
}

int pv2_kv_count_prefix(pv2_kv *kv, const char *prefix)
{
    int i, n = 0;
    size_t pl;
    if (!kv) return 0;
    pl = strlen(prefix);
    for (i = 0; i < kv->n; i++)
        if (!strncmp(kv->keys[i], prefix, pl)) n++;
    return n;
}

int pv2_kv_list(pv2_kv *kv, const char *prefix, char **keys, int max)
{
    int i, n = 0;
    size_t pl;
    if (!kv) return 0;
    pl = strlen(prefix);
    for (i = 0; i < kv->n && n < max; i++)
        if (!strncmp(kv->keys[i], prefix, pl)) keys[n++] = kv->keys[i];
    return n;
}

long long pv2_kv_bytes(const pv2_kv *kv) { return kv ? kv->bytes : 0; }

/* ============================================================ journal backend
 * Append-only log. A transaction is  BEGIN <records...> COMMIT{nrec}  and is
 * applied only when the framing, the CRC of every record and the declared
 * record count all check out. Anything after the last intact COMMIT is a torn
 * tail: it is reported (pv2_store_torn_tail) and truncated, never applied.
 */
#define J_MAGIC "PV2J"
#define J_VER   1

enum {
    JT_BEGIN = 1, JT_RECEIPT, JT_ENTITY, JT_CHANGE, JT_TOMBSTONE,
    JT_COMMIT, JT_CONFLICT, JT_GC, JT_TOMB_CONFIRM
};

typedef struct {
    char     magic[4];
    uint16_t ver;
    uint16_t type;
    uint32_t len;
    uint32_t crc;
} pv2_jrec;

typedef struct {
    int      entity;
    char     id[48];
    long     revision;
    long long updated_at;
    int      deleted;
    char    *payload;
    size_t   plen;
} j_ent;

typedef struct {
    char op_id[48];
    int  status;
    long revision;
    long seq;
    long long applied_at;
} j_rcpt;

typedef struct {
    long seq;
    int  entity;
    char id[48];
    long revision;
    int  deleted;
} j_chg;

typedef struct {
    int      entity;
    char     id[48];
    long     revision;
    long long deleted_at;
    int      confirmed;      /* T05B: main phone confirmed the deletion */
} j_tomb;

typedef struct {
    char dir[512];
    char log_path[600];
    int  fd;
    int  sync_mode;          /* 0 = no fsync (crash-unsafe, test only)
                              * 1 = fdatasync before COMMIT and after COMMIT
                              * 2 = single fsync after the whole transaction  */
    j_ent   *ents;  size_t n_ents,  cap_ents;
    j_rcpt  *rcpts; size_t n_rcpts, cap_rcpts;
    j_chg   *chgs;  size_t n_chgs,  cap_chgs;
    j_tomb  *tombs; size_t n_tombs, cap_tombs;
    long     next_seq;
    long     min_change_seq;
    long     retention_floor;
    long     n_conflicts;
    int      in_tx;
    long     tx_start_off;
    long     tx_nrec;
    int      torn_tail;
} jstore;

/* --- little-endian serialisation helpers --- */
static void wr_u32(unsigned char *p, size_t *off, uint32_t v) { memcpy(p + *off, &v, 4); *off += 4; }
static void wr_i32(unsigned char *p, size_t *off, int32_t v)  { memcpy(p + *off, &v, 4); *off += 4; }
static void wr_i64(unsigned char *p, size_t *off, int64_t v)  { memcpy(p + *off, &v, 8); *off += 8; }
static void wr_str(unsigned char *p, size_t *off, const char *s, size_t len)
{
    wr_u32(p, off, (uint32_t)len);
    if (len) memcpy(p + *off, s, len);
    *off += len;
}

static int rd_u32(const unsigned char *p, size_t *off, size_t len, uint32_t *v)
{
    if (*off + 4 > len) return -1;
    memcpy(v, p + *off, 4); *off += 4; return 0;
}
static int rd_i32(const unsigned char *p, size_t *off, size_t len, int32_t *v)
{
    if (*off + 4 > len) return -1;
    memcpy(v, p + *off, 4); *off += 4; return 0;
}
static int rd_i64(const unsigned char *p, size_t *off, size_t len, int64_t *v)
{
    if (*off + 8 > len) return -1;
    memcpy(v, p + *off, 8); *off += 8; return 0;
}
static int rd_str(const unsigned char *p, size_t *off, size_t len, char *out, size_t cap)
{
    uint32_t n;
    if (rd_u32(p, off, len, &n) != 0) return -1;
    if (n >= cap) return -1;
    if (*off + n > len) return -1;
    if (n) memcpy(out, p + *off, n);
    out[n] = '\0';
    *off += n;
    return 0;
}

static int j_write_record(jstore *js, uint16_t type, const unsigned char *payload, size_t len)
{
    pv2_jrec h;
    unsigned char *buf = (unsigned char *)malloc(len + sizeof h);
    size_t total = len + sizeof h;
    ssize_t w;
    if (!buf) return -1;
    memcpy(h.magic, J_MAGIC, 4);
    h.ver = J_VER;
    h.type = type;
    h.len = (uint32_t)len;
    h.crc = crc32_of(payload, len);
    memcpy(buf, &h, sizeof h);
    if (len) memcpy(buf + sizeof h, payload, len);
    w = write(js->fd, buf, total);
    free(buf);
    if (w != (ssize_t)total) return -1;
    if (js->in_tx && type != JT_COMMIT) js->tx_nrec++;
    return 0;
}

/* --- in-memory model --- */
static j_ent *j_find_ent(jstore *js, int entity, const char *id)
{
    size_t i;
    for (i = 0; i < js->n_ents; i++)
        if (js->ents[i].entity == entity && !strcmp(js->ents[i].id, id))
            return &js->ents[i];
    return NULL;
}

static j_ent *j_upsert_ent(jstore *js, int entity, const char *id)
{
    j_ent *e = j_find_ent(js, entity, id);
    if (e) return e;
    if (js->n_ents == js->cap_ents) {
        size_t ncap = js->cap_ents ? js->cap_ents * 2 : 64;
        j_ent *nb = (j_ent *)realloc(js->ents, ncap * sizeof(j_ent));
        if (!nb) return NULL;
        js->ents = nb;
        js->cap_ents = ncap;
    }
    e = &js->ents[js->n_ents++];
    memset(e, 0, sizeof *e);
    e->entity = entity;
    snprintf(e->id, sizeof e->id, "%s", id);
    return e;
}

static void j_add_chg(jstore *js, long seq, int entity, const char *id, long rev, int deleted)
{
    j_chg *c;
    if (js->n_chgs == js->cap_chgs) {
        size_t ncap = js->cap_chgs ? js->cap_chgs * 2 : 128;
        j_chg *nb = (j_chg *)realloc(js->chgs, ncap * sizeof(j_chg));
        if (!nb) return;
        js->chgs = nb;
        js->cap_chgs = ncap;
    }
    c = &js->chgs[js->n_chgs++];
    c->seq = seq; c->entity = entity; c->revision = rev; c->deleted = deleted;
    snprintf(c->id, sizeof c->id, "%s", id);
    if (seq > js->next_seq) js->next_seq = seq;
    if (js->min_change_seq == 0 || seq < js->min_change_seq) js->min_change_seq = seq;
}

static void j_add_rcpt(jstore *js, const j_rcpt *r)
{
    if (js->n_rcpts == js->cap_rcpts) {
        size_t ncap = js->cap_rcpts ? js->cap_rcpts * 2 : 64;
        j_rcpt *nb = (j_rcpt *)realloc(js->rcpts, ncap * sizeof(j_rcpt));
        if (!nb) return;
        js->rcpts = nb;
        js->cap_rcpts = ncap;
    }
    js->rcpts[js->n_rcpts++] = *r;
}

static void j_add_tomb(jstore *js, int entity, const char *id, long rev, long long at)
{
    j_tomb *t;
    size_t i;
    for (i = 0; i < js->n_tombs; i++) {
        if (js->tombs[i].entity == entity && !strcmp(js->tombs[i].id, id)) {
            js->tombs[i].revision = rev;
            js->tombs[i].deleted_at = at;
            js->tombs[i].confirmed = 0;   /* a fresh delete needs fresh confirmation */
            return;
        }
    }
    if (js->n_tombs == js->cap_tombs) {
        size_t ncap = js->cap_tombs ? js->cap_tombs * 2 : 64;
        j_tomb *nb = (j_tomb *)realloc(js->tombs, ncap * sizeof(j_tomb));
        if (!nb) return;
        js->tombs = nb;
        js->cap_tombs = ncap;
    }
    t = &js->tombs[js->n_tombs++];
    memset(t, 0, sizeof *t);
    t->entity = entity;
    snprintf(t->id, sizeof t->id, "%s", id);
    t->revision = rev;
    t->deleted_at = at;
}

static void j_free_model(jstore *js)
{
    size_t i;
    for (i = 0; i < js->n_ents; i++) free(js->ents[i].payload);
    free(js->ents);  js->ents = NULL;  js->n_ents = 0;  js->cap_ents = 0;
    free(js->rcpts); js->rcpts = NULL; js->n_rcpts = 0; js->cap_rcpts = 0;
    free(js->chgs);  js->chgs = NULL;  js->n_chgs = 0;  js->cap_chgs = 0;
    free(js->tombs); js->tombs = NULL; js->n_tombs = 0; js->cap_tombs = 0;
    js->next_seq = 0;
    js->min_change_seq = 0;
    js->retention_floor = 0;
    js->n_conflicts = 0;
}

/* T05B: a JT_GC record replays the purge the GC run performed — confirmed
 * tombstones past the tombstone cutoff and receipts past the receipt cutoff
 * are dropped from the replayed model, so a restart reproduces the exact
 * post-GC state. Zero cutoffs (old-format records) purge nothing. */
static void j_replay_gc(jstore *js, long long tomb_cut, long long rcpt_cut)
{
    size_t i, w;
    w = 0;
    for (i = 0; i < js->n_tombs; i++) {
        if (js->tombs[i].confirmed && tomb_cut > 0 && js->tombs[i].deleted_at < tomb_cut)
            continue;
        js->tombs[w++] = js->tombs[i];
    }
    js->n_tombs = w;
    w = 0;
    for (i = 0; i < js->n_rcpts; i++) {
        if (rcpt_cut > 0 && js->rcpts[i].applied_at < rcpt_cut)
            continue;
        js->rcpts[w++] = js->rcpts[i];
    }
    js->n_rcpts = w;
}

static int j_apply_record(jstore *js, uint16_t type, const unsigned char *p, size_t len)
{
    size_t off = 0;
    switch (type) {
    case JT_BEGIN:
        return 0;
    case JT_RECEIPT: {
        j_rcpt r;
        int32_t status;
        int64_t rev, seq, at;
        memset(&r, 0, sizeof r);
        if (rd_str(p, &off, len, r.op_id, sizeof r.op_id) != 0) return -1;
        if (rd_i32(p, &off, len, &status) != 0) return -1;
        if (rd_i64(p, &off, len, &rev) != 0) return -1;
        if (rd_i64(p, &off, len, &seq) != 0) return -1;
        if (rd_i64(p, &off, len, &at) != 0) return -1;
        r.status = status; r.revision = (long)rev; r.seq = (long)seq; r.applied_at = at;
        j_add_rcpt(js, &r);
        return 0;
    }
    case JT_ENTITY: {
        int32_t entity, deleted;
        int64_t rev, upd;
        uint32_t plen;
        char id[48];
        j_ent *e;
        if (rd_i32(p, &off, len, &entity) != 0) return -1;
        if (rd_str(p, &off, len, id, sizeof id) != 0) return -1;
        if (rd_i64(p, &off, len, &rev) != 0) return -1;
        if (rd_i32(p, &off, len, &deleted) != 0) return -1;
        if (rd_i64(p, &off, len, &upd) != 0) return -1;
        if (rd_u32(p, &off, len, &plen) != 0) return -1;
        if (off + plen > len) return -1;
        e = j_upsert_ent(js, entity, id);
        if (!e) return -1;
        free(e->payload);
        e->payload = (char *)malloc(plen + 1);
        if (!e->payload) return -1;
        memcpy(e->payload, p + off, plen);
        e->payload[plen] = '\0';
        e->plen = plen;
        e->revision = (long)rev;
        e->deleted = deleted;
        e->updated_at = upd;
        return 0;
    }
    case JT_CHANGE: {
        int64_t seq, rev;
        int32_t entity, deleted;
        char id[48];
        if (rd_i64(p, &off, len, &seq) != 0) return -1;
        if (rd_i32(p, &off, len, &entity) != 0) return -1;
        if (rd_str(p, &off, len, id, sizeof id) != 0) return -1;
        if (rd_i64(p, &off, len, &rev) != 0) return -1;
        if (rd_i32(p, &off, len, &deleted) != 0) return -1;
        j_add_chg(js, (long)seq, entity, id, (long)rev, deleted);
        return 0;
    }
    case JT_TOMBSTONE: {
        int32_t entity;
        int64_t rev, at;
        char id[48];
        if (rd_i32(p, &off, len, &entity) != 0) return -1;
        if (rd_str(p, &off, len, id, sizeof id) != 0) return -1;
        if (rd_i64(p, &off, len, &rev) != 0) return -1;
        if (rd_i64(p, &off, len, &at) != 0) return -1;
        j_add_tomb(js, entity, id, (long)rev, at);
        return 0;
    }
    case JT_GC: {
        int64_t now, floor, tomb_cut = 0, rcpt_cut = 0;
        int32_t keep;
        if (rd_i64(p, &off, len, &now) != 0) return -1;
        if (rd_i32(p, &off, len, &keep) != 0) return -1;
        if (rd_i64(p, &off, len, &floor) != 0) return -1;
        /* T05B: extended payload carries the purge cutoffs; old-format
         * records (24 B) replay with zero cutoffs = purge nothing. */
        if (off + 8 <= len) { if (rd_i64(p, &off, len, &tomb_cut) != 0) return -1; }
        if (off + 8 <= len) { if (rd_i64(p, &off, len, &rcpt_cut) != 0) return -1; }
        if ((long)floor > js->retention_floor) js->retention_floor = (long)floor;
        j_replay_gc(js, tomb_cut, rcpt_cut);
        return 0;
    }
    case JT_TOMB_CONFIRM: {
        /* T05B: payload: entity,id — main phone confirmed the deletion */
        int32_t entity;
        char id[48];
        size_t i;
        if (rd_i32(p, &off, len, &entity) != 0) return -1;
        if (rd_str(p, &off, len, id, sizeof id) != 0) return -1;
        for (i = 0; i < js->n_tombs; i++) {
            if (js->tombs[i].entity == entity && !strcmp(js->tombs[i].id, id))
                js->tombs[i].confirmed = 1;
        }
        return 0;
    }
    case JT_CONFLICT: {
        /* payload: entity,id,copy_id,client_id,base,server,at,plen,payload */
        int32_t entity;
        int64_t base_rev, server_rev, at;
        uint32_t plen;
        char id[48], copy_id[128], client_id[48];
        if (rd_i32(p, &off, len, &entity) != 0) return -1;
        if (rd_str(p, &off, len, id, sizeof id) != 0) return -1;
        if (rd_str(p, &off, len, copy_id, sizeof copy_id) != 0) return -1;
        if (rd_str(p, &off, len, client_id, sizeof client_id) != 0) return -1;
        if (rd_i64(p, &off, len, &base_rev) != 0) return -1;
        if (rd_i64(p, &off, len, &server_rev) != 0) return -1;
        if (rd_i64(p, &off, len, &at) != 0) return -1;
        if (rd_u32(p, &off, len, &plen) != 0) return -1;
        if (off + plen > len) return -1;
        (void)entity; (void)id; (void)copy_id; (void)client_id;
        (void)base_rev; (void)server_rev; (void)at; (void)plen;
        js->n_conflicts++;
        return 0;
    }
    case JT_COMMIT:
        return 0;
    default:
        return -1;
    }
}

/* Two passes: pass 1 validates framing and locates the last intact COMMIT;
 * pass 2 replays the intact prefix into a fresh model. A torn tail is
 * truncated and flagged. */
static int j_load(jstore *js)
{
    FILE *f;
    long off = 0, good_end = 0, dangling = 0;
    int in_tx = 0;
    long recs = 0;

    j_free_model(js);
    if (js->fd >= 0) close(js->fd);
    js->fd = open(js->log_path, O_RDWR | O_CREAT | O_APPEND | O_BINARY, 0600);
    if (js->fd < 0) return -1;

    f = fopen(js->log_path, "rb");
    if (f) {
        for (;;) {
            pv2_jrec h;
            unsigned char *p;
            long rec_off = off;
            if (fread(&h, sizeof h, 1, f) != 1) break;
            if (memcmp(h.magic, J_MAGIC, 4) != 0 || h.ver != J_VER || h.len > 262144) {
                dangling = 1; break;
            }
            p = (unsigned char *)malloc(h.len ? h.len : 1);
            if (!p) { dangling = 1; break; }
            if (h.len && fread(p, 1, h.len, f) != h.len) { free(p); dangling = 1; break; }
            if (crc32_of(p, h.len) != h.crc) { free(p); dangling = 1; break; }
            off = (long)(rec_off + sizeof h + h.len);
            if (h.type == JT_BEGIN) {
                in_tx = 1; recs = 0;
            } else if (h.type == JT_COMMIT) {
                uint32_t nrec = 0;
                size_t o = 0;
                if (!in_tx || rd_u32(p, &o, h.len, &nrec) != 0 || (long)nrec != recs) {
                    free(p); dangling = 1; break;
                }
                in_tx = 0;
                good_end = off;
            } else if (!in_tx) {
                free(p); dangling = 1; break;      /* record outside a transaction */
            } else {
                recs++;
            }
            free(p);
        }
        fclose(f);
    }

    if (off > good_end) {
        dangling = 1;
        if (ftruncate(js->fd, good_end) != 0) { /* best effort */ }
    }
    if (dangling) js->torn_tail = 1;

    f = fopen(js->log_path, "rb");
    if (f) {
        long o2 = 0;
        while (o2 < good_end) {
            pv2_jrec h;
            unsigned char *p;
            if (fread(&h, sizeof h, 1, f) != 1) break;
            p = (unsigned char *)malloc(h.len ? h.len : 1);
            if (!p) break;
            if (h.len && fread(p, 1, h.len, f) != h.len) { free(p); break; }
            j_apply_record(js, h.type, p, h.len);
            free(p);
            o2 = (long)(o2 + sizeof h + h.len);
        }
        fclose(f);
    }
    lseek(js->fd, 0, SEEK_END);
    return 0;
}

/* --- journal primitives --- */
static int j_p_begin(void *h)
{
    jstore *js = (jstore *)h;
    int r;
    if (js->in_tx) return -1;
    js->tx_nrec = 0;
    js->tx_start_off = (long)lseek(js->fd, 0, SEEK_END);
    /* T05B fix: write JT_BEGIN before arming in_tx so the BEGIN record is NOT
     * counted into tx_nrec. It used to be counted, so every COMMIT carried
     * nrec = records+1 while j_load pass 1 compared it against the plain data
     * record count — a guaranteed mismatch that made ANY journal restart with
     * committed transactions truncate the whole log down to an empty store. */
    r = j_write_record(js, JT_BEGIN, NULL, 0);
    js->in_tx = 1;
    return r;
}

static int j_p_commit(void *h)
{
    jstore *js = (jstore *)h;
    unsigned char buf[8];
    uint32_t nrec;
    int r;
    if (!js->in_tx) return -1;
    nrec = (uint32_t)js->tx_nrec;
    wr_u32(buf, &(size_t){0}, nrec);
    if (js->sync_mode == 1) (void)pv2_fdatasync(js->fd);
    r = j_write_record(js, JT_COMMIT, buf, 4);
    js->in_tx = 0;
    if (js->sync_mode >= 1) (void)pv2_fdatasync(js->fd);
    return r;
}

static void j_p_rollback(void *h)
{
    jstore *js = (jstore *)h;
    if (!js->in_tx) return;
    js->in_tx = 0;
    if (ftruncate(js->fd, js->tx_start_off) != 0) { /* best effort */ }
    lseek(js->fd, 0, SEEK_END);
    j_load(js);
}

static int j_p_find_receipt(void *h, const char *op_id, pv2_receipt *out)
{
    jstore *js = (jstore *)h;
    size_t i;
    for (i = 0; i < js->n_rcpts; i++) {
        if (!strcmp(js->rcpts[i].op_id, op_id)) {
            memset(out, 0, sizeof *out);
            snprintf(out->op_id, sizeof out->op_id, "%s", js->rcpts[i].op_id);
            out->status = js->rcpts[i].status;
            out->revision = js->rcpts[i].revision;
            out->seq = js->rcpts[i].seq;
            return 1;
        }
    }
    return 0;
}

static void j_p_put_receipt(void *h, const pv2_receipt *r, const char *client_id, long long now)
{
    jstore *js = (jstore *)h;
    unsigned char buf[512];
    size_t off = 0;
    j_rcpt rec;
    int64_t rev = r->revision, seq = r->seq, at = now;
    int32_t status = r->status;
    (void)client_id;
    wr_str(buf, &off, r->op_id, strlen(r->op_id));
    wr_i32(buf, &off, status);
    wr_i64(buf, &off, rev);
    wr_i64(buf, &off, seq);
    wr_i64(buf, &off, at);
    j_write_record(js, JT_RECEIPT, buf, off);
    memset(&rec, 0, sizeof rec);
    snprintf(rec.op_id, sizeof rec.op_id, "%s", r->op_id);
    rec.status = r->status; rec.revision = r->revision; rec.seq = r->seq; rec.applied_at = now;
    j_add_rcpt(js, &rec);
}

static long j_p_entity_revision(void *h, int entity, const char *id)
{
    j_ent *e = j_find_ent((jstore *)h, entity, id);
    return e ? e->revision : 0;
}

static void j_p_put_entity(void *h, int entity, const char *id, long rev,
                           const char *payload, size_t plen, int deleted, long long now)
{
    jstore *js = (jstore *)h;
    unsigned char *buf = (unsigned char *)malloc(plen + 512);
    size_t off = 0;
    j_ent *e;
    int32_t enc = entity, del = deleted;
    int64_t erev = rev, eupd = now;
    if (!buf) return;
    wr_i32(buf, &off, enc);
    wr_str(buf, &off, id, strlen(id));
    wr_i64(buf, &off, erev);
    wr_i32(buf, &off, del);
    wr_i64(buf, &off, eupd);
    wr_u32(buf, &off, (uint32_t)plen);
    if (plen) memcpy(buf + off, payload, plen);
    off += plen;
    j_write_record(js, JT_ENTITY, buf, off);
    free(buf);
    e = j_upsert_ent(js, entity, id);
    if (e) {
        free(e->payload);
        e->payload = (char *)malloc(plen + 1);
        if (e->payload) {
            memcpy(e->payload, payload ? payload : "", plen);
            e->payload[plen] = '\0';
            e->plen = plen;
        } else {
            e->plen = 0;
        }
        e->revision = rev; e->deleted = deleted; e->updated_at = now;
    }
}

static long j_p_next_seq(void *h)
{
    jstore *js = (jstore *)h;
    return ++js->next_seq;
}

static void j_p_put_change(void *h, long seq, int entity, const char *id, long rev, int deleted)
{
    jstore *js = (jstore *)h;
    unsigned char buf[256];
    size_t off = 0;
    int64_t s = seq, r = rev;
    int32_t e = entity, d = deleted;
    wr_i64(buf, &off, s);
    wr_i32(buf, &off, e);
    wr_str(buf, &off, id, strlen(id));
    wr_i64(buf, &off, r);
    wr_i32(buf, &off, d);
    j_write_record(js, JT_CHANGE, buf, off);
    j_add_chg(js, seq, entity, id, rev, deleted);
}

static void j_p_put_tombstone(void *h, int entity, const char *id, long rev, long long now)
{
    jstore *js = (jstore *)h;
    unsigned char buf[256];
    size_t off = 0;
    int32_t e = entity;
    int64_t r = rev, at = now;
    wr_i32(buf, &off, e);
    wr_str(buf, &off, id, strlen(id));
    wr_i64(buf, &off, r);
    wr_i64(buf, &off, at);
    j_write_record(js, JT_TOMBSTONE, buf, off);
    j_add_tomb(js, entity, id, rev, now);
}

static int j_p_read_change(void *h, long seq, int *entity, char *id, size_t idn,
                           long *rev, int *deleted)
{
    jstore *js = (jstore *)h;
    size_t i;
    for (i = 0; i < js->n_chgs; i++) {
        if (js->chgs[i].seq == seq) {
            *entity = js->chgs[i].entity;
            snprintf(id, idn, "%s", js->chgs[i].id);
            *rev = js->chgs[i].revision;
            *deleted = js->chgs[i].deleted;
            return 1;
        }
    }
    return 0;
}

static long j_p_min_seq(void *h)
{
    jstore *js = (jstore *)h;
    long m = js->min_change_seq;
    if (js->retention_floor > m) m = js->retention_floor;
    return m;
}

static long j_p_max_seq(void *h) { return ((jstore *)h)->next_seq; }

static long j_p_retention_floor(void *h) { return ((jstore *)h)->retention_floor; }

static int j_p_count(void *h, int entity, int include_deleted)
{
    jstore *js = (jstore *)h;
    size_t i;
    int n = 0;
    for (i = 0; i < js->n_ents; i++)
        if ((entity == 0 || js->ents[i].entity == entity) &&
            (include_deleted || !js->ents[i].deleted)) n++;
    return n;
}

static int j_p_list(void *h, int entity, long offset, pv2_entity_info *out, int max)
{
    jstore *js = (jstore *)h;
    size_t i;
    int n = 0;
    long seen = 0;
    for (i = 0; i < js->n_ents && n < max; i++) {
        if (js->ents[i].entity != entity) continue;
        if (seen++ < offset) continue;
        memset(&out[n], 0, sizeof out[n]);
        snprintf(out[n].id, sizeof out[n].id, "%s", js->ents[i].id);
        out[n].entity = js->ents[i].entity;
        out[n].revision = js->ents[i].revision;
        out[n].deleted = js->ents[i].deleted;
        out[n].updated_at = js->ents[i].updated_at;
        n++;
    }
    return n;
}

static int j_p_get_payload(void *h, int entity, const char *id, char *buf, size_t cap,
                           size_t *out_len, long *out_rev, int *out_deleted)
{
    j_ent *e = j_find_ent((jstore *)h, entity, id);
    if (!e) return 0;
    if (buf) {
        size_t n = (e->plen < cap - 1) ? e->plen : cap - 1;
        memcpy(buf, e->payload ? e->payload : "", n);
        buf[n] = '\0';
        if (out_len) *out_len = n;
    } else if (out_len) {
        *out_len = e->plen;
    }
    if (out_rev) *out_rev = e->revision;
    if (out_deleted) *out_deleted = e->deleted;
    return 1;
}

static void j_p_put_conflict(void *h, int entity, const char *id, const char *client_id,
                             long base_rev, long server_rev, const char *payload,
                             size_t plen, const char *copy_id, long long now)
{
    jstore *js = (jstore *)h;
    unsigned char *buf = (unsigned char *)malloc(plen + 768);
    size_t off = 0;
    int32_t e = entity;
    int64_t b = base_rev, s = server_rev, at = now;
    if (!buf) return;
    wr_i32(buf, &off, e);
    wr_str(buf, &off, id, strlen(id));
    wr_str(buf, &off, copy_id, strlen(copy_id));
    wr_str(buf, &off, client_id ? client_id : "", client_id ? strlen(client_id) : 0);
    wr_i64(buf, &off, b);
    wr_i64(buf, &off, s);
    wr_i64(buf, &off, at);
    wr_u32(buf, &off, (uint32_t)plen);
    if (plen) memcpy(buf + off, payload ? payload : "", plen);
    off += plen;
    j_write_record(js, JT_CONFLICT, buf, off);
    js->n_conflicts++;
    free(buf);
}

static long j_p_conflict_count(void *h)
{
    jstore *js = (jstore *)h;
    if (!js) return 0;
    return js->n_conflicts;
}

static int j_p_gc(void *h, long long now, int keep_days, long *out_t, long *out_c)
{
    jstore *js = (jstore *)h;
    unsigned char buf[64];
    size_t off = 0, w, i, j;
    long long cutoff = now - (long long)keep_days * 86400LL;
    long long rcpt_cut = now - (long long)PV2_RECEIPT_KEEP_DAYS * 86400LL;
    long floor = js->retention_floor, removed = 0, purged = 0;
    int64_t n64 = now, f64, t64, r64;
    int32_t k32 = keep_days;

    /* T05B: the tombstone ledger purges only rows that are BOTH past the
     * retention period AND confirmed by the main phone (storage-design
     * sec.4.4); receipts are purged after the fixed 90 d op_id window. */
    w = 0;
    for (i = 0; i < js->n_tombs; i++) {
        if (js->tombs[i].confirmed && js->tombs[i].deleted_at < cutoff) {
            removed++;
            for (j = 0; j < js->n_chgs; j++) {
                if (js->chgs[j].entity == js->tombs[i].entity &&
                    !strcmp(js->chgs[j].id, js->tombs[i].id) &&
                    js->chgs[j].deleted && js->chgs[j].seq > floor) {
                    floor = js->chgs[j].seq;
                    purged++;
                }
            }
            continue;
        }
        js->tombs[w++] = js->tombs[i];
    }
    js->n_tombs = w;

    w = 0;
    for (i = 0; i < js->n_rcpts; i++) {
        if (js->rcpts[i].applied_at < rcpt_cut) continue;
        js->rcpts[w++] = js->rcpts[i];
    }
    js->n_rcpts = w;

    if (floor > js->retention_floor) js->retention_floor = floor;

    /* Persist inside ONE transaction. T05B fix: the GC record used to be
     * written bare (outside any transaction), which j_load pass 1 flags as
     * dangling — the next restart truncated every record after the last
     * COMMIT. The extended JT_GC payload replays the purge via j_replay_gc. */
    if (js->in_tx) return -1;
    if (j_p_begin(h) != 0) return -1;
    n64 = now;
    f64 = js->retention_floor;
    t64 = cutoff;
    r64 = rcpt_cut;
    wr_i64(buf, &off, n64);
    wr_i32(buf, &off, k32);
    wr_i64(buf, &off, f64);
    wr_i64(buf, &off, t64);
    wr_i64(buf, &off, r64);
    if (j_write_record(js, JT_GC, buf, off) != 0) { j_p_rollback(h); return -1; }
    if (j_p_commit(h) != 0) { j_p_rollback(h); return -1; }
    (void)pv2_fdatasync(js->fd);
    if (out_t) *out_t = removed;
    if (out_c) *out_c = purged;
    return 0;
}

/* T05B: main-phone confirmation for one tombstone; persisted as its own
 * transactional record so a restart keeps the confirmed state. */
static int j_p_confirm_tombstone(void *h, int entity, const char *id, long long now)
{
    jstore *js = (jstore *)h;
    unsigned char buf[96];
    size_t off = 0;
    int32_t e = entity;
    (void)now;
    if (js->in_tx) return -1;
    if (j_p_begin(h) != 0) return -1;
    wr_i32(buf, &off, e);
    wr_str(buf, &off, id, strlen(id));
    if (j_write_record(js, JT_TOMB_CONFIRM, buf, off) != 0) { j_p_rollback(h); return -1; }
    if (j_p_commit(h) != 0) { j_p_rollback(h); return -1; }
    j_apply_record(js, JT_TOMB_CONFIRM, buf, off);
    return 0;
}

static int j_p_integrity(void *h, char *msg, size_t msgn)
{
    jstore *js = (jstore *)h;
    size_t i, j;
    long expect = 0;
    for (i = 0; i < js->n_chgs; i++) {
        if (js->chgs[i].seq <= expect) {
            snprintf(msg, msgn, "change sequence not increasing at seq=%ld", js->chgs[i].seq);
            return -1;
        }
        expect = js->chgs[i].seq;
    }
    for (i = 0; i < js->n_ents; i++) {
        int matched = 0;
        if (js->ents[i].revision <= 0) {
            snprintf(msg, msgn, "entity %s has non-positive revision", js->ents[i].id);
            return -1;
        }
        for (j = 0; j < js->n_chgs; j++) {
            if (js->chgs[j].entity == js->ents[i].entity &&
                !strcmp(js->chgs[j].id, js->ents[i].id) &&
                js->chgs[j].revision == js->ents[i].revision) {
                matched = 1;
                break;
            }
        }
        if (!matched) {
            snprintf(msg, msgn, "entity %s rev %ld has no matching change row (torn transaction)",
                     js->ents[i].id, js->ents[i].revision);
            return -1;
        }
    }
    snprintf(msg, msgn, "ok entities=%zu changes=%zu receipts=%zu torn_tail=%d",
             js->n_ents, js->n_chgs, js->n_rcpts, js->torn_tail);
    return 0;
}

static int j_p_sync(void *h) { return pv2_fdatasync(((jstore *)h)->fd); }
static long long j_p_bytes(void *h) { return file_size(((jstore *)h)->log_path); }
static int j_p_torn_tail(void *h) { return ((jstore *)h)->torn_tail; }

static void j_p_free(void *h)
{
    jstore *js = (jstore *)h;
    if (!js) return;
    j_free_model(js);
    if (js->fd >= 0) close(js->fd);
    free(js);
}

/* ============================================================= sqlite backend
 * The unified database: words / annotations / daily plans / review events /
 * briefings / op receipts / conflicts / tombstones all live in one file and
 * are written in one transaction, so the change-log sequence cannot drift away
 * from the entity rows. WAL mode; synchronous level is configurable (F=0/1/2).
 */
#ifdef PV2_HAVE_SQLITE
typedef struct {
    sqlite3 *db;
    char     dir[512];
    char     db_path[600];
    int      sync_mode;
} sqstore;

static const char *SQ_SCHEMA =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA foreign_keys=ON;"
    "CREATE TABLE IF NOT EXISTS entities("
    "  entity INTEGER NOT NULL, id TEXT NOT NULL, revision INTEGER NOT NULL,"
    "  deleted INTEGER NOT NULL DEFAULT 0, updated_at INTEGER NOT NULL, payload TEXT,"
    "  PRIMARY KEY(entity,id));"
    "CREATE TABLE IF NOT EXISTS changes("
    "  seq INTEGER PRIMARY KEY, entity INTEGER NOT NULL, id TEXT NOT NULL,"
    "  revision INTEGER NOT NULL, deleted INTEGER NOT NULL, applied_at INTEGER NOT NULL);"
    "CREATE INDEX IF NOT EXISTS ix_changes_ent ON changes(entity,id,revision);"
    "CREATE TABLE IF NOT EXISTS ops("
    "  op_id TEXT PRIMARY KEY, client_id TEXT, status INTEGER NOT NULL,"
    "  revision INTEGER NOT NULL, seq INTEGER NOT NULL, applied_at INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS conflicts("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT, entity INTEGER NOT NULL, ent_id TEXT NOT NULL,"
    "  copy_id TEXT NOT NULL, client_id TEXT, base_revision INTEGER, server_revision INTEGER,"
    "  payload TEXT, created_at INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS tombstones("
    "  entity INTEGER NOT NULL, id TEXT NOT NULL, revision INTEGER NOT NULL,"
    "  deleted_at INTEGER NOT NULL, confirmed INTEGER NOT NULL DEFAULT 0,"
    "  PRIMARY KEY(entity,id));"
    "CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY, v TEXT NOT NULL);";

static int sq_exec(sqstore *s, const char *sql)
{
    return sqlite3_exec(s->db, sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

static void sq_maybe_checkpoint(sqstore *s)
{
    char wal[640];
    snprintf(wal, sizeof wal, "%s-wal", s->db_path);
    if (file_size(wal) > (1024 * 1024)) {
        if (sq_exec(s, "PRAGMA wal_checkpoint(TRUNCATE);") != 0) {
            (void)sq_exec(s, "PRAGMA wal_checkpoint(PASSIVE);");
        }
    }
}

static long sq_floor(sqstore *s)
{
    sqlite3_stmt *st = NULL;
    long v = 0;
    if (sqlite3_prepare_v2(s->db, "SELECT v FROM meta WHERE k='retention_floor';", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) v = (long)sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    return v;
}

static void sq_set_floor(sqstore *s, long floor)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db,
            "INSERT INTO meta(k,v) VALUES('retention_floor',?) "
            "ON CONFLICT(k) DO UPDATE SET v=excluded.v;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)floor);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
}

static int sq_open(sqstore **out, const char *dir, int sync_mode, char *err, size_t errn)
{
    sqstore *s = (sqstore *)calloc(1, sizeof *s);
    char *emsg = NULL;
    const char *prag;
    if (!s) return -1;
    snprintf(s->dir, sizeof s->dir, "%s", dir);
    mkdir_p(dir);
    snprintf(s->db_path, sizeof s->db_path, "%s/panel_v2.db", dir);
    s->sync_mode = sync_mode;
    if (sqlite3_open(s->db_path, &s->db) != SQLITE_OK) {
        snprintf(err, errn, "sqlite3_open failed: %s", sqlite3_errmsg(s->db));
        return -1;
    }
    if (sqlite3_exec(s->db, SQ_SCHEMA, NULL, NULL, &emsg) != SQLITE_OK) {
        snprintf(err, errn, "schema init failed: %s", emsg ? emsg : "?");
        sqlite3_free(emsg);
        return -1;
    }
    /* T05B migration: tombstones.confirmed on DBs created before it existed */
    {
        sqlite3_stmt *st = NULL;
        int has_col = 0;
        if (sqlite3_prepare_v2(s->db, "PRAGMA table_info(tombstones);", -1, &st, NULL) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                const unsigned char *cn = sqlite3_column_text(st, 1);
                if (cn && strcmp((const char *)cn, "confirmed") == 0) { has_col = 1; break; }
            }
            sqlite3_finalize(st);
        }
        if (!has_col &&
            sqlite3_exec(s->db,
                         "ALTER TABLE tombstones ADD COLUMN confirmed INTEGER NOT NULL DEFAULT 0;",
                         NULL, NULL, NULL) != SQLITE_OK) {
            snprintf(err, errn, "tombstones migration failed");
            return -1;
        }
    }
    sq_maybe_checkpoint(s);
    switch (sync_mode) {
    case 0:  prag = "PRAGMA synchronous=OFF;PRAGMA wal_autocheckpoint=0;";    break;
    case 2:  prag = "PRAGMA synchronous=NORMAL;PRAGMA wal_autocheckpoint=0;"; break;
    default: prag = "PRAGMA synchronous=FULL;PRAGMA wal_autocheckpoint=0;";   break;
    }
    if (sq_exec(s, prag) != 0) {
        snprintf(err, errn, "pragma failed: %s", sqlite3_errmsg(s->db));
        return -1;
    }
    (void)sqlite3_busy_timeout(s->db, 5000);
    *out = s;
    return 0;
}

static int sq_p_begin(void *h) { return sq_exec((sqstore *)h, "BEGIN IMMEDIATE;"); }
static int sq_p_commit(void *h)
{
    sqstore *s = (sqstore *)h;
    int r = sq_exec(s, "COMMIT;");
    if (r == 0) sq_maybe_checkpoint(s);
    return r;
}
static void sq_p_rollback(void *h) { (void)sq_exec((sqstore *)h, "ROLLBACK;"); }

static int sq_p_find_receipt(void *h, const char *op_id, pv2_receipt *out)
{
    sqstore *s = (sqstore *)h;
    sqlite3_stmt *st = NULL;
    int found = 0;
    if (sqlite3_prepare_v2(s->db, "SELECT status,revision,seq FROM ops WHERE op_id=?1;", -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(st, 1, op_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        memset(out, 0, sizeof *out);
        snprintf(out->op_id, sizeof out->op_id, "%s", op_id);
        out->status = sqlite3_column_int(st, 0);
        out->revision = (long)sqlite3_column_int64(st, 1);
        out->seq = (long)sqlite3_column_int64(st, 2);
        found = 1;
    }
    sqlite3_finalize(st);
    return found;
}

static void sq_p_put_receipt(void *h, const pv2_receipt *r, const char *client_id, long long now)
{
    sqstore *s = (sqstore *)h;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db,
            "INSERT OR REPLACE INTO ops(op_id,client_id,status,revision,seq,applied_at) "
            "VALUES(?1,?2,?3,?4,?5,?6);", -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, r->op_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, client_id ? client_id : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, r->status);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)r->revision);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)r->seq);
    sqlite3_bind_int64(st, 6, (sqlite3_int64)now);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

static long sq_p_entity_revision(void *h, int entity, const char *id)
{
    sqstore *s = (sqstore *)h;
    sqlite3_stmt *st = NULL;
    long rev = 0;
    if (sqlite3_prepare_v2(s->db, "SELECT revision FROM entities WHERE entity=?1 AND id=?2;", -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_int(st, 1, entity);
    sqlite3_bind_text(st, 2, id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) rev = (long)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return rev;
}

static void sq_p_put_entity(void *h, int entity, const char *id, long rev,
                            const char *payload, size_t plen, int deleted, long long now)
{
    sqstore *s = (sqstore *)h;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db,
            "INSERT OR REPLACE INTO entities(entity,id,revision,deleted,updated_at,payload) "
            "VALUES(?1,?2,?3,?4,?5,?6);", -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_int(st, 1, entity);
    sqlite3_bind_text(st, 2, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)rev);
    sqlite3_bind_int(st, 4, deleted);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)now);
    sqlite3_bind_text(st, 6, payload ? payload : "", (int)plen, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

static long sq_p_next_seq(void *h)
{
    sqstore *s = (sqstore *)h;
    sqlite3_stmt *st = NULL;
    long v = 1;
    if (sqlite3_prepare_v2(s->db, "SELECT IFNULL(MAX(seq),0)+1 FROM changes;", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) v = (long)sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    return v;
}

static void sq_p_put_change(void *h, long seq, int entity, const char *id, long rev, int deleted)
{
    sqstore *s = (sqstore *)h;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db,
            "INSERT INTO changes(seq,entity,id,revision,deleted,applied_at) VALUES(?1,?2,?3,?4,?5,?6);",
            -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)seq);
    sqlite3_bind_int(st, 2, entity);
    sqlite3_bind_text(st, 3, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)rev);
    sqlite3_bind_int(st, 5, deleted);
    sqlite3_bind_int64(st, 6, (sqlite3_int64)pv2_wall_seconds());
    sqlite3_step(st);
    sqlite3_finalize(st);
}

static void sq_p_put_tombstone(void *h, int entity, const char *id, long rev, long long now)
{
    sqstore *s = (sqstore *)h;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db,
            "INSERT OR REPLACE INTO tombstones(entity,id,revision,deleted_at) VALUES(?1,?2,?3,?4);",
            -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_int(st, 1, entity);
    sqlite3_bind_text(st, 2, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)rev);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)now);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* T05B: main-phone confirmation for one tombstone. A re-delete resets the
 * flag via INSERT OR REPLACE (confirmed falls back to its DEFAULT 0). */
static int sq_p_confirm_tombstone(void *h, int entity, const char *id, long long now)
{
    sqstore *s = (sqstore *)h;
    sqlite3_stmt *st = NULL;
    int rc;
    (void)now;
    if (sqlite3_prepare_v2(s->db,
            "UPDATE tombstones SET confirmed=1 WHERE entity=?1 AND id=?2;",
            -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(st, 1, entity);
    sqlite3_bind_text(st, 2, id, -1, SQLITE_TRANSIENT);
    rc = (sqlite3_step(st) == SQLITE_DONE) ? 0 : -1;
    sqlite3_finalize(st);
    return rc;
}

static int sq_p_read_change(void *h, long seq, int *entity, char *id, size_t idn,
                            long *rev, int *deleted)
{
    sqstore *s = (sqstore *)h;
    sqlite3_stmt *st = NULL;
    int found = 0;
    if (sqlite3_prepare_v2(s->db, "SELECT entity,id,revision,deleted FROM changes WHERE seq=?1;", -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)seq);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(st, 1);
        *entity = sqlite3_column_int(st, 0);
        snprintf(id, idn, "%s", t ? (const char *)t : "");
        *rev = (long)sqlite3_column_int64(st, 2);
        *deleted = sqlite3_column_int(st, 3);
        found = 1;
    }
    sqlite3_finalize(st);
    return found;
}

static long sq_p_min_seq(void *h)
{
    sqstore *s = (sqstore *)h;
    sqlite3_stmt *st = NULL;
    long v = 0;
    if (sqlite3_prepare_v2(s->db, "SELECT IFNULL(MIN(seq),0) FROM changes;", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) v = (long)sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    if (sq_floor(s) > v) v = sq_floor(s);
    return v;
}

static long sq_p_max_seq(void *h)
{
    sqstore *s = (sqstore *)h;
    sqlite3_stmt *st = NULL;
    long v = 0;
    if (sqlite3_prepare_v2(s->db, "SELECT IFNULL(MAX(seq),0) FROM changes;", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) v = (long)sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    return v;
}

static long sq_p_retention_floor(void *h) { return sq_floor((sqstore *)h); }

static int sq_p_count(void *h, int entity, int include_deleted)
{
    sqstore *s = (sqstore *)h;
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (sqlite3_prepare_v2(s->db,
            "SELECT COUNT(*) FROM entities WHERE (?1=0 OR entity=?1) AND (?2=1 OR deleted=0);",
            -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, entity);
    sqlite3_bind_int(st, 2, include_deleted);
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

static int sq_p_list(void *h, int entity, long offset, pv2_entity_info *out, int max)
{
    sqstore *s = (sqstore *)h;
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (sqlite3_prepare_v2(s->db,
            "SELECT id,entity,revision,deleted,updated_at FROM entities WHERE entity=?1 "
            "ORDER BY id LIMIT ?2 OFFSET ?3;", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, entity);
    sqlite3_bind_int(st, 2, max);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)offset);
    while (n < max && sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(st, 0);
        memset(&out[n], 0, sizeof out[n]);
        snprintf(out[n].id, sizeof out[n].id, "%s", t ? (const char *)t : "");
        out[n].entity = sqlite3_column_int(st, 1);
        out[n].revision = (long)sqlite3_column_int64(st, 2);
        out[n].deleted = sqlite3_column_int(st, 3);
        out[n].updated_at = (long long)sqlite3_column_int64(st, 4);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

static int sq_p_get_payload(void *h, int entity, const char *id, char *buf, size_t cap,
                            size_t *out_len, long *out_rev, int *out_deleted)
{
    sqstore *s = (sqstore *)h;
    sqlite3_stmt *st = NULL;
    int found = 0;
    if (sqlite3_prepare_v2(s->db, "SELECT payload,revision,deleted FROM entities WHERE entity=?1 AND id=?2;",
                           -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, entity);
    sqlite3_bind_text(st, 2, id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(st, 0);
        int len = sqlite3_column_bytes(st, 0);
        if (buf) {
            size_t n = ((size_t)len < cap - 1) ? (size_t)len : cap - 1;
            if (t && n) memcpy(buf, t, n);
            buf[n] = '\0';
            if (out_len) *out_len = n;
        } else if (out_len) {
            *out_len = (size_t)len;
        }
        if (out_rev) *out_rev = (long)sqlite3_column_int64(st, 1);
        if (out_deleted) *out_deleted = sqlite3_column_int(st, 2);
        found = 1;
    }
    sqlite3_finalize(st);
    return found;
}

static void sq_p_put_conflict(void *h, int entity, const char *id, const char *client_id,
                              long base_rev, long server_rev, const char *payload,
                              size_t plen, const char *copy_id, long long now)
{
    sqstore *s = (sqstore *)h;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db,
            "INSERT INTO conflicts(entity,ent_id,copy_id,client_id,base_revision,server_revision,payload,created_at) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8);", -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_int(st, 1, entity);
    sqlite3_bind_text(st, 2, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, copy_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, client_id ? client_id : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)base_rev);
    sqlite3_bind_int64(st, 6, (sqlite3_int64)server_rev);
    sqlite3_bind_text(st, 7, payload ? payload : "", (int)plen, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 8, (sqlite3_int64)now);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

static long sq_p_conflict_count(void *h)
{
    sqstore *s = (sqstore *)h;
    sqlite3_stmt *st = NULL;
    long n = 0;
    if (sqlite3_prepare_v2(s->db, "SELECT COUNT(*) FROM conflicts;", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) n = (long)sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    return n;
}

static int sq_p_gc(void *h, long long now, int keep_days, long *out_t, long *out_c)
{
    sqstore *s = (sqstore *)h;
    sqlite3_stmt *st = NULL;
    long long cutoff = now - (long long)keep_days * 86400LL;
    long long rcpt_cut = now - (long long)PV2_RECEIPT_KEEP_DAYS * 86400LL;
    long removed = 0, purged = 0, floor = sq_floor(s), new_floor = 0;

    /* T05B: purge only tombstones that are BOTH past retention AND confirmed
     * by the main phone (storage-design sec.4.4). */
    if (sqlite3_prepare_v2(s->db, "DELETE FROM tombstones WHERE confirmed=1 AND deleted_at < ?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)cutoff);
        if (sqlite3_step(st) == SQLITE_DONE) removed = (long)sqlite3_changes(s->db);
        sqlite3_finalize(st);
    }

    /* receipts: fixed 90 d op_id idempotency window */
    if (sqlite3_prepare_v2(s->db, "DELETE FROM ops WHERE applied_at < ?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)rcpt_cut);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }

    if (sqlite3_prepare_v2(s->db,
            "SELECT IFNULL(MAX(seq),0), COUNT(*) FROM changes WHERE deleted=1 AND applied_at < ?1;",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)cutoff);
        if (sqlite3_step(st) == SQLITE_ROW) {
            new_floor = (long)sqlite3_column_int64(st, 0);
            purged = (long)sqlite3_column_int64(st, 1);
        }
        sqlite3_finalize(st);
    }
    /* Retention is enforced logically: rows stay (so a resurrect cannot slip in
     * through a stale create) while every cursor below the floor is expired and
     * forced into a full resync. */
    if (new_floor > floor) { sq_set_floor(s, new_floor); floor = new_floor; }

    if (out_t) *out_t = removed;
    if (out_c) *out_c = purged;
    return 0;
}

static int sq_p_integrity(void *h, char *msg, size_t msgn)
{
    sqstore *s = (sqstore *)h;
    sqlite3_stmt *st = NULL;
    long orphan = 0, badrev = 0, ne = 0, nc = 0, no = 0, nk = 0;

    if (sqlite3_prepare_v2(s->db, "PRAGMA integrity_check;", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *t = sqlite3_column_text(st, 0);
            if (!t || strcmp((const char *)t, "ok") != 0) {
                snprintf(msg, msgn, "integrity_check: %s", t ? (const char *)t : "?");
                sqlite3_finalize(st);
                return -1;
            }
        }
        sqlite3_finalize(st);
    }

    if (sqlite3_prepare_v2(s->db,
            "SELECT COUNT(*) FROM entities e WHERE NOT EXISTS("
            "  SELECT 1 FROM changes c WHERE c.entity=e.entity AND c.id=e.id AND c.revision=e.revision);",
            -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) orphan = (long)sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    if (orphan > 0) {
        snprintf(msg, msgn, "torn transaction: %ld entity rows without a matching change row", orphan);
        return -1;
    }

    if (sqlite3_prepare_v2(s->db, "SELECT COUNT(*) FROM entities WHERE revision<=0;", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) badrev = (long)sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    if (badrev > 0) {
        snprintf(msg, msgn, "%ld entity rows with non-positive revision", badrev);
        return -1;
    }

    if (sqlite3_prepare_v2(s->db, "SELECT (SELECT COUNT(*) FROM entities),(SELECT COUNT(*) FROM changes),"
                                  "(SELECT COUNT(*) FROM ops),(SELECT COUNT(*) FROM conflicts);",
                           -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            ne = (long)sqlite3_column_int64(st, 0);
            nc = (long)sqlite3_column_int64(st, 1);
            no = (long)sqlite3_column_int64(st, 2);
            nk = (long)sqlite3_column_int64(st, 3);
        }
        sqlite3_finalize(st);
    }
    snprintf(msg, msgn, "ok entities=%ld changes=%ld receipts=%ld conflicts=%ld floor=%ld",
             ne, nc, no, nk, sq_floor(s));
    return 0;
}

static int sq_p_sync(void *h)
{
    sqstore *s = (sqstore *)h;
    char wal[640];
    int fd;
    (void)sq_exec(s, "PRAGMA wal_checkpoint(FULL);");
    snprintf(wal, sizeof wal, "%s-wal", s->db_path);
    fd = open(s->db_path, O_RDWR | O_BINARY);
    if (fd >= 0) { (void)pv2_fsync(fd); (void)close(fd); }
    fd = open(wal, O_RDWR | O_BINARY);
    if (fd >= 0) { (void)pv2_fsync(fd); (void)close(fd); }
    return 0;
}

static long long sq_p_bytes(void *h)
{
    sqstore *s = (sqstore *)h;
    char wal[640];
    snprintf(wal, sizeof wal, "%s-wal", s->db_path);
    return file_size(s->db_path) + file_size(wal);
}

static int sq_p_torn_tail(void *h) { (void)h; return 0; }

static void sq_p_free(void *h)
{
    sqstore *s = (sqstore *)h;
    if (!s) return;
    if (s->db) sqlite3_close(s->db);
    free(s);
}
#endif /* PV2_HAVE_SQLITE */

/* ============================================================ shared layer
 * Both backends expose the same primitives, so duplicate suppression, the
 * revision handshake, change sequencing, tombstone bookkeeping, the conflict
 * copy and the op receipt all live here exactly once.
 */
static void store_fill_journal(pv2_store *st)
{
    st->kind = "journal";
    st->impl = PV2_IMPL_JOURNAL;
    st->p_begin = j_p_begin;
    st->p_commit = j_p_commit;
    st->p_rollback = j_p_rollback;
    st->p_find_receipt = j_p_find_receipt;
    st->p_put_receipt = j_p_put_receipt;
    st->p_entity_revision = j_p_entity_revision;
    st->p_put_entity = j_p_put_entity;
    st->p_next_seq = j_p_next_seq;
    st->p_put_change = j_p_put_change;
    st->p_put_tombstone = j_p_put_tombstone;
    st->p_read_change = j_p_read_change;
    st->p_min_seq = j_p_min_seq;
    st->p_max_seq = j_p_max_seq;
    st->p_count = j_p_count;
    st->p_list = j_p_list;
    st->p_get_payload = j_p_get_payload;
    st->p_put_conflict = j_p_put_conflict;
    st->p_conflict_count = j_p_conflict_count;
    st->p_gc = j_p_gc;
    st->p_confirm_tombstone = j_p_confirm_tombstone;
    st->p_retention_floor = j_p_retention_floor;
    st->p_integrity = j_p_integrity;
    st->p_sync = j_p_sync;
    st->p_bytes = j_p_bytes;
    st->p_torn_tail = j_p_torn_tail;
    st->p_free = j_p_free;
}

#ifdef PV2_HAVE_SQLITE
static void store_fill_sqlite(pv2_store *st)
{
    st->kind = "sqlite";
    st->impl = PV2_IMPL_SQLITE;
    st->p_begin = sq_p_begin;
    st->p_commit = sq_p_commit;
    st->p_rollback = sq_p_rollback;
    st->p_find_receipt = sq_p_find_receipt;
    st->p_put_receipt = sq_p_put_receipt;
    st->p_entity_revision = sq_p_entity_revision;
    st->p_put_entity = sq_p_put_entity;
    st->p_next_seq = sq_p_next_seq;
    st->p_put_change = sq_p_put_change;
    st->p_put_tombstone = sq_p_put_tombstone;
    st->p_read_change = sq_p_read_change;
    st->p_min_seq = sq_p_min_seq;
    st->p_max_seq = sq_p_max_seq;
    st->p_count = sq_p_count;
    st->p_list = sq_p_list;
    st->p_get_payload = sq_p_get_payload;
    st->p_put_conflict = sq_p_put_conflict;
    st->p_conflict_count = sq_p_conflict_count;
    st->p_gc = sq_p_gc;
    st->p_confirm_tombstone = sq_p_confirm_tombstone;
    st->p_retention_floor = sq_p_retention_floor;
    st->p_integrity = sq_p_integrity;
    st->p_sync = sq_p_sync;
    st->p_bytes = sq_p_bytes;
    st->p_torn_tail = sq_p_torn_tail;
    st->p_free = sq_p_free;
}
#endif

int pv2_store_open(pv2_store *st, int impl, const char *dir, const char *server_epoch,
                   const char *library_id, int sync_mode, char *err, size_t errn)
{
    char e[256];
    if (!st || !dir) return -1;
    if (err && errn) err[0] = '\0';
    memset(st, 0, sizeof *st);
    if (!server_epoch || !server_epoch[0] || !library_id || !library_id[0]) {
        if (err && errn) snprintf(err, errn, "server_epoch and library_id are required");
        return -1;
    }
    e[0] = '\0';
    if (impl == PV2_IMPL_SQLITE) {
#ifdef PV2_HAVE_SQLITE
        sqstore *s = NULL;
        if (sq_open(&s, dir, sync_mode, e, sizeof e) != 0) {
            if (err && errn) snprintf(err, errn, "%s", e);
            return -1;
        }
        store_fill_sqlite(st);
        st->h = s;
        return 0;
#else
        if (err && errn)
            snprintf(err, errn, "sqlite backend not compiled in (rebuild with -DPV2_HAVE_SQLITE)");
        return -1;
#endif
    }
    {
        jstore *s = (jstore *)calloc(1, sizeof *s);
        if (!s) { if (err && errn) snprintf(err, errn, "out of memory"); return -1; }
        snprintf(s->dir, sizeof s->dir, "%s", dir);
        mkdir_p(dir);
        snprintf(s->log_path, sizeof s->log_path, "%s/panel_v2.log", dir);
        s->sync_mode = sync_mode;
        s->fd = -1;
        if (j_load(s) != 0) {
            if (err && errn) snprintf(err, errn, "journal open failed: %s", strerror(errno));
            free(s);
            return -1;
        }
        store_fill_journal(st);
        st->h = s;
        return 0;
    }
}

void pv2_store_close(pv2_store *st)
{
    if (!st || !st->h) return;
    if (st->p_free) st->p_free(st->h);
    memset(st, 0, sizeof *st);
}

const char *pv2_store_kind(const pv2_store *st)
{
    if (!st || !st->kind) return "none";
    return st->kind;
}

int pv2_store_begin(pv2_store *st) { return (st && st->p_begin) ? st->p_begin(st->h) : -1; }

int pv2_store_commit(pv2_store *st)
{
    int r = (st && st->p_commit) ? st->p_commit(st->h) : -1;
    if (st && r != 0 && st->p_rollback) st->p_rollback(st->h);
    return r;
}

void pv2_store_rollback(pv2_store *st) { if (st && st->p_rollback) st->p_rollback(st->h); }

static void store_receipt_commit(pv2_store *st, const pv2_op *op, int status,
                                 long revision, long seq, const char *client_id, long long now)
{
    pv2_receipt r;
    memset(&r, 0, sizeof r);
    snprintf(r.op_id, sizeof r.op_id, "%s", op->op_id);
    r.status = status;
    r.revision = revision;
    r.seq = seq;
    if (st->p_begin(st->h) != 0) return;
    if (st->p_put_receipt) st->p_put_receipt(st->h, &r, client_id, now);
    if (st->p_commit(st->h) != 0) st->p_rollback(st->h);
}

int pv2_store_apply(pv2_store *st, const pv2_op *op, const char *client_id, pv2_receipt *out)
{
    pv2_receipt dup;
    pv2_receipt rec;
    char payload[PV2_OP_PAYLOAD_BYTES + 1];
    char copy_id[96];
    size_t plen = 0;
    long server_rev = 0, new_rev = 0, seq = 0, copy_seq = 0;
    long long now;
    int have = 0, deleted_flag = 0, conflict = 0;

    if (!st || !st->h || !op || !out) return -1;
    memset(out, 0, sizeof *out);
    snprintf(out->op_id, sizeof out->op_id, "%s", op->op_id);
    out->status = PV2_ST_STORAGE_FAIL;
    snprintf(out->code, sizeof out->code, "%s", PV2_CODE_STORAGE);
    snprintf(out->message, sizeof out->message, "storage failure");

    /* 1. op_id idempotency: the receipt lives in the same transaction as the
     *    write, so a retry can never be applied twice. */
    memset(&dup, 0, sizeof dup);
    if (st->p_find_receipt && st->p_find_receipt(st->h, op->op_id, &dup)) {
        out->status = PV2_ST_DUPLICATE;
        out->revision = dup.revision;
        out->seq = dup.seq;
        snprintf(out->code, sizeof out->code, "duplicate");
        snprintf(out->message, sizeof out->message,
                 "op_id already committed with status %s", pv2_op_status_name(dup.status));
        return 0;
    }

    if (op->payload && op->payload_len) {
        size_t n = op->payload_len;
        if (n > PV2_OP_PAYLOAD_BYTES) n = PV2_OP_PAYLOAD_BYTES;
        memcpy(payload, op->payload, n);
        payload[n] = '\0';
        plen = n;
    } else {
        payload[0] = '\0';
    }

    now = pv2_wall_seconds();
    have = st->p_get_payload(st->h, op->entity, op->entity_id, NULL, 0, NULL, &server_rev, &deleted_flag);

    if (op->deleted) {
        if (!have) {
            /* already gone: a successful no-op so the client can clear the op */
            seq = st->p_max_seq(st->h);
            store_receipt_commit(st, op, PV2_ST_DUPLICATE, 0, seq, client_id, now);
            out->status = PV2_ST_DUPLICATE;
            out->revision = 0;
            out->seq = seq;
            snprintf(out->code, sizeof out->code, "already_absent");
            snprintf(out->message, sizeof out->message, "entity already absent");
            return 0;
        }
        if (op->base_revision != server_rev) conflict = 1;
        else new_rev = server_rev + 1;
    } else {
        if (!have) {
            if (op->base_revision != 0) conflict = 1;
            else new_rev = 1;
        } else if (deleted_flag) {
            if (op->base_revision != server_rev) conflict = 1;   /* edit of a deleted row */
            else new_rev = server_rev + 1;                       /* deliberate resurrection */
        } else if (op->base_revision != server_rev) {
            conflict = 1;
        } else {
            new_rev = server_rev + 1;
        }
    }

    if (conflict) {
        /* 409: the server row stays untouched and the losing client copy is
         * filed under a derived id and streamed back on the next pull, so both
         * copies are kept. Conflict row + copy + receipt share one transaction. */
        memset(copy_id, 0, sizeof copy_id);
        if (op->entity_id[0])
            snprintf(copy_id, sizeof copy_id, "%.20s@%.24s", op->entity_id,
                     op->op_id[0] ? op->op_id : "op");
        else
            snprintf(copy_id, sizeof copy_id, "copy@%.24s", op->op_id);
        if (st->p_begin(st->h) != 0) return -1;
        if (st->p_put_conflict)
            st->p_put_conflict(st->h, op->entity, op->entity_id, client_id,
                               op->base_revision, server_rev,
                               plen ? payload : NULL, plen, copy_id, now);
        if (!op->deleted && plen) {
            copy_seq = st->p_next_seq ? st->p_next_seq(st->h) : (st->p_max_seq(st->h) + 1);
            st->p_put_entity(st->h, op->entity, copy_id, 1, payload, plen, 0, now);
            st->p_put_change(st->h, copy_seq, op->entity, copy_id, 1, 0);
        }
        seq = st->p_max_seq(st->h);
        memset(&rec, 0, sizeof rec);
        snprintf(rec.op_id, sizeof rec.op_id, "%s", op->op_id);
        rec.status = PV2_ST_CONFLICT;
        rec.revision = server_rev;
        rec.seq = seq;
        if (st->p_put_receipt) st->p_put_receipt(st->h, &rec, client_id, now);
        if (st->p_commit(st->h) != 0) {
            st->p_rollback(st->h);
            out->status = PV2_ST_STORAGE_FAIL;
            return -1;
        }
        out->status = PV2_ST_CONFLICT;
        out->revision = server_rev;
        out->seq = seq;
        snprintf(out->conflict_copy_id, sizeof out->conflict_copy_id, "%s", copy_id);
        snprintf(out->code, sizeof out->code, "%s", PV2_CODE_CONFLICT);
        snprintf(out->message, sizeof out->message,
                 "version conflict: base=%ld server=%ld, client copy kept as %s",
                 op->base_revision, server_rev, copy_id);
        return 0;
    }

    /* 2. one transaction: entity row + change row + tombstone + receipt */
    if (st->p_begin(st->h) != 0) {
        out->status = PV2_ST_STORAGE_FAIL;
        return -1;
    }
    seq = st->p_next_seq ? st->p_next_seq(st->h) : (st->p_max_seq(st->h) + 1);
    st->p_put_entity(st->h, op->entity, op->entity_id, new_rev,
                     op->deleted ? NULL : payload, op->deleted ? 0 : plen,
                     op->deleted ? 1 : 0, now);
    st->p_put_change(st->h, seq, op->entity, op->entity_id, new_rev, op->deleted ? 1 : 0);
    if (op->deleted && st->p_put_tombstone)
        st->p_put_tombstone(st->h, op->entity, op->entity_id, new_rev, now);

    memset(&rec, 0, sizeof rec);
    snprintf(rec.op_id, sizeof rec.op_id, "%s", op->op_id);
    rec.status = PV2_ST_APPLIED;
    rec.revision = new_rev;
    rec.seq = seq;
    if (st->p_put_receipt) st->p_put_receipt(st->h, &rec, client_id, now);

    if (st->p_commit(st->h) != 0) {
        st->p_rollback(st->h);
        out->status = PV2_ST_STORAGE_FAIL;
        return -1;
    }
    out->status = PV2_ST_APPLIED;
    out->revision = new_rev;
    out->seq = seq;
    snprintf(out->code, sizeof out->code, "ok");
    snprintf(out->message, sizeof out->message, "applied revision %ld", new_rev);
    return 0;
}

int pv2_store_seed(pv2_store *st, int entity, const char *id, const char *payload,
                   size_t plen, int deleted)
{
    pv2_op op;
    pv2_receipt rc;
    long rev;
    if (!st || !st->h) return -1;
    memset(&op, 0, sizeof op);
    rev = st->p_entity_revision ? st->p_entity_revision(st->h, entity, id) : 0;
    op.entity = entity;
    snprintf(op.entity_id, sizeof op.entity_id, "%s", id);
    op.base_revision = rev;
    op.payload = payload;
    op.payload_len = plen;
    op.deleted = deleted ? 1 : 0;
    snprintf(op.op_id, sizeof op.op_id, "seed:%.8s:%.24s", pv2_entity_name(entity), id);
    return pv2_store_apply(st, &op, "board", &rc) == 0 ? 0 : -1;
}

long pv2_store_last_seq(pv2_store *st)
{
    return (st && st->p_max_seq) ? st->p_max_seq(st->h) : 0;
}

long pv2_store_min_seq(pv2_store *st)
{
    return (st && st->p_min_seq) ? st->p_min_seq(st->h) : 0;
}

int pv2_store_read_change(pv2_store *st, long seq, int *entity, char *id, size_t idn,
                          long *rev, int *deleted)
{
    if (!st || !st->p_read_change) return 0;
    return st->p_read_change(st->h, seq, entity, id, idn, rev, deleted);
}

int pv2_store_count(pv2_store *st, int entity, int include_deleted)
{
    if (!st || !st->p_count) return 0;
    return st->p_count(st->h, entity, include_deleted);
}

long pv2_store_entity_revision(pv2_store *st, int entity, const char *id)
{
    if (!st || !st->p_entity_revision) return 0;
    return st->p_entity_revision(st->h, entity, id);
}

int pv2_store_integrity(pv2_store *st, char *msg, size_t msgn)
{
    if (!st || !st->p_integrity) { if (msg && msgn) snprintf(msg, msgn, "store not open"); return -1; }
    return st->p_integrity(st->h, msg, msgn);
}

int pv2_store_sync(pv2_store *st)
{
    if (!st || !st->p_sync) return -1;
    return st->p_sync(st->h);
}

long long pv2_store_bytes(pv2_store *st)
{
    return (st && st->p_bytes) ? st->p_bytes(st->h) : 0;
}

int pv2_store_torn_tail(pv2_store *st)
{
    if (!st || !st->p_torn_tail) return 0;
    return st->p_torn_tail(st->h);
}

int pv2_store_gc(pv2_store *st, long long now, int keep_days,
                 long *out_tombstones, long *out_changes)
{
    if (!st || !st->p_gc) return -1;
    return st->p_gc(st->h, now, keep_days, out_tombstones, out_changes);
}

int pv2_store_confirm_tombstone(pv2_store *st, int entity, const char *id, long long now)
{
    if (!st || !st->p_confirm_tombstone) return -1;
    return st->p_confirm_tombstone(st->h, entity, id, now);
}

/* --------------------------------------------------------------- context */
int pv2_ctx_init(pv2_ctx *c, const char *dir, int impl, const char *board_id,
                 const char *library_id, const char *server_epoch,
                 const char *control_pin, int sync_mode, char *err, size_t errn)
{
    char kvpath[700];
    if (!c || !dir) return -1;
    memset(c, 0, sizeof *c);
    snprintf(c->dir, sizeof c->dir, "%s", dir);
    snprintf(c->cred_dir, sizeof c->cred_dir, "%s/auth", dir);
    mkdir_p(c->dir);
    mkdir_p(c->cred_dir);
    if (pv2_store_open(&c->store, impl, dir, server_epoch, library_id, sync_mode, err, errn) != 0)
        return -1;
    /* T04C: "aux.kv" is unusable on the Windows host — AUX is a reserved DOS
     * device name (like CON/NUL/PRN), so runtimes that resolve names through
     * the NT namespace (zig mingw CRT open, PS provider) fail on "aux.*" with
     * ERROR_ACCESS_DENIED. The POSIX board target is unaffected; the name is
     * changed everywhere so host and board share one layout. */
    snprintf(kvpath, sizeof kvpath, "%s/pv2aux.kv", c->cred_dir);
    c->aux = pv2_kv_open(kvpath, err, errn);
    if (!c->aux) { pv2_store_close(&c->store); return -1; }
    snprintf(c->library_id, sizeof c->library_id, "%s", library_id ? library_id : "");
    snprintf(c->server_epoch, sizeof c->server_epoch, "%s", server_epoch ? server_epoch : "");
    snprintf(c->board_id, sizeof c->board_id, "%s", board_id ? board_id : "marves-panel");
    snprintf(c->pin, sizeof c->pin, "%s", control_pin ? control_pin : "");
    c->auth_enabled = 1;    /* M9A: 默认启用鉴权；由宿主(service.c)按配置覆盖 */
    c->sync_mode = sync_mode;
    c->now = pv2_wall_seconds();
    return 0;
}

void pv2_ctx_close(pv2_ctx *c)
{
    if (!c) return;
    if (c->aux) { pv2_kv_close(c->aux); c->aux = NULL; }
    pv2_store_close(&c->store);
}

