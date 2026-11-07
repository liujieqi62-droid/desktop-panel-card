/* panel_v2.h - Marves V3.2 / M2: board-side storage + protocol interfaces (C99).
 *
 * Scope of M2 (docs/requirements/Marves实施方案-单词学习与每日简报-2026-09-14.md):
 *   sec.4  storage: Android SQLite single-transaction commit; board SQLite+WAL
 *          viability (ARMv7 build, Tina filesystem, power-loss recovery) with a
 *          transactional recovery-log fallback store if the budget fails.
 *   sec.5  HTTP capacity: <=4096 B encoded request, <=32768 B response,
 *          20 items per page with byte-based early truncation, <=2048 B briefing
 *          paragraph records, import packing (<=10 items per pack).
 *   sec.5  /api/v2 surface: capabilities, pair, sync/status|push|pull, words,
 *          daily-plan, briefings, ai/jobs (+get/cancel), settings.
 *   sec.7  auth: control-PIN pairing, revocable random token, token never in URL,
 *          board credentials in a root-private 0600 file outside web root.
 *   sec.4  sync state machine: handshake -> push -> board txn receipt ->
 *          pull delta/cursor -> phone commits cursor; op_id idempotency,
 *          409 version conflict with both copies kept, tombstone deletion with
 *          >=30 day retention, expired cursor -> full re-sync.
 *
 * All identifiers, JSON keys and messages in this implementation are ASCII so
 * that the protocol layer is immune to the console/script code-page defects
 * documented for the M1 deploy gate (see docs/execution/MARVES-M2/report.md).
 */
#ifndef PANEL_V2_H
#define PANEL_V2_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ limits */
#define PV2_MAX_REQ_JSON          4096u    /* encoded request body (sec.5)      */
#define PV2_MAX_RESP_JSON         32768u   /* total response body (sec.5)       */
#define PV2_PAGE_ITEMS            20u      /* items per page (sec.5)            */
#define PV2_PAGE_BYTE_BUDGET      28672u   /* stop adding items before 32 KiB   */
#define PV2_BRIEF_PARA_BYTES      2048u    /* one briefing paragraph (sec.5)    */
#define PV2_PACK_MAX_OPS          10u      /* ops per request pack (sec.5)      */
#define PV2_IMPORT_MAX_ITEMS      1000u
#define PV2_IMPORT_MAX_BYTES      (1024u * 1024u)
#define PV2_LIBRARY_MAX_WORDS     5000u    /* first release library cap (sec.7) */
#define PV2_MAX_ANNOTATION_BYTES  2048u
#define PV2_AI_QUEUE_MAX          100u     /* queued+running jobs (sec.7)       */
#define PV2_TOMBSTONE_KEEP_DAYS   30       /* sec.4                             */
#define PV2_RECEIPT_KEEP_DAYS     90       /* op_id receipt retention (sec.4)   */
#define PV2_OP_PAYLOAD_BYTES      3072u    /* per-op payload budget             */
#define PV2_SCHEMA_VERSION        "2.0"

/* ------------------------------------------------------- entities (sec.4) */
typedef enum {
    PV2_ENT_WORD       = 1,
    PV2_ENT_ANNOTATION = 2,
    PV2_ENT_PLAN       = 3,
    PV2_ENT_REVIEW     = 4,
    PV2_ENT_BRIEFING   = 5,
    /* N3 (续工方案 §4.2): course timetable window + independent plan items.
     * Same generic apply contract; no dedicated push validation needed. */
    PV2_ENT_COURSE     = 6,     /* "course_occurrence"                      */
    PV2_ENT_PLAN_ITEM  = 7,     /* "plan_item"                              */
    PV2_ENT_WINDOW     = 8,     /* "schedule_window" (single row)           */
    PV2_ENT_MAX        = 8      /* capabilities list bound                  */
} pv2_entity;

const char *pv2_entity_name(int entity);
int         pv2_entity_from_name(const char *name);

/* --------------------------------------------- op status / HTTP mapping */
typedef enum {
    PV2_ST_APPLIED      = 0,
    PV2_ST_DUPLICATE    = 1,
    PV2_ST_CONFLICT     = 2,
    PV2_ST_REJECTED     = 3,
    PV2_ST_STORAGE_FAIL = 4
} pv2_op_status;

const char *pv2_op_status_name(int status);
int         pv2_http_for_status(int status);

/* stable error codes (sec.5 常见错误) */
#define PV2_CODE_BAD_REQUEST      "bad_request"
#define PV2_CODE_UNAUTHORIZED     "unauthorized"
#define PV2_CODE_FORBIDDEN        "forbidden"
#define PV2_CODE_CONFLICT         "version_conflict"
#define PV2_CODE_TOO_LARGE        "payload_too_large"
#define PV2_CODE_UNPROCESSABLE    "schema_invalid"
#define PV2_CODE_QUEUE_FULL       "queue_full"
#define PV2_CODE_LIBRARY_FULL     "library_full"
#define PV2_CODE_CURSOR_EXPIRED   "cursor_expired"
#define PV2_CODE_TOKEN_IN_URL     "token_in_url_forbidden"
#define PV2_CODE_RATE_LIMITED     "too_many_attempts"
#define PV2_CODE_STORAGE          "storage_failure"
#define PV2_CODE_SECRET_IN_BODY   "secret_in_ordinary_settings"
#define PV2_CODE_NOT_FOUND        "not_found"

/* ------------------------------------------------------------------- ops */
typedef struct {
    char        op_id[48];
    int         entity;              /* pv2_entity */
    char        entity_id[48];
    long        base_revision;       /* 0 = create */
    const char *payload;             /* JSON text (may be NULL for delete) */
    size_t      payload_len;
    int         deleted;             /* tombstone op */
    int         retry_count;
} pv2_op;

typedef struct {
    char  op_id[48];
    int   status;                    /* pv2_op_status */
    long  revision;
    long  seq;
    char  server_epoch[48];
    char  code[48];
    char  message[192];
    char  conflict_copy_id[96];      /* set when status == CONFLICT */
} pv2_receipt;

/* --------------------------------------------------------------- storage */
typedef enum { PV2_IMPL_JOURNAL = 1, PV2_IMPL_SQLITE = 2 } pv2_impl;

typedef struct pv2_store pv2_store;

typedef struct {
    char id[48];
    int  entity;
    long revision;
    int  deleted;
    long long updated_at;
} pv2_entity_info;

struct pv2_store {
    const char *kind;
    int         impl;
    void       *h;                   /* backend handle */

    /* backend primitives (see panel_v2_store.c) */
    int  (*p_begin)(void *h);
    int  (*p_commit)(void *h);
    void (*p_rollback)(void *h);
    int  (*p_find_receipt)(void *h, const char *op_id, pv2_receipt *out);
    void (*p_put_receipt)(void *h, const pv2_receipt *r, const char *client_id, long long now);
    long (*p_entity_revision)(void *h, int entity, const char *id);
    void (*p_put_entity)(void *h, int entity, const char *id, long rev,
                         const char *payload, size_t plen, int deleted, long long now);
    long (*p_next_seq)(void *h);
    void (*p_put_change)(void *h, long seq, int entity, const char *id, long rev, int deleted);
    void (*p_put_tombstone)(void *h, int entity, const char *id, long rev, long long now);
    int  (*p_read_change)(void *h, long seq, int *entity, char *id, size_t idn,
                          long *rev, int *deleted);
    long (*p_min_seq)(void *h);
    long (*p_max_seq)(void *h);
    int  (*p_count)(void *h, int entity, int include_deleted);
    int  (*p_list)(void *h, int entity, long offset, pv2_entity_info *out, int max);
    int  (*p_get_payload)(void *h, int entity, const char *id, char *buf, size_t cap,
                          size_t *out_len, long *out_rev, int *out_deleted);
    void (*p_put_conflict)(void *h, int entity, const char *id, const char *client_id,
                           long base_rev, long server_rev, const char *payload,
                           size_t plen, const char *copy_id, long long now);
    long (*p_conflict_count)(void *h);
    int  (*p_gc)(void *h, long long now, int keep_days, long *out_gc_tombstones, long *out_gc_changes);
    /* T05B: main-phone confirmation for a tombstone (storage-design sec.4.4:
     * GC needs >=30 d AND this confirmation). 0 = ok, <0 = storage error. */
    int  (*p_confirm_tombstone)(void *h, int entity, const char *id, long long now);
    long (*p_retention_floor)(void *h);
    int  (*p_integrity)(void *h, char *msg, size_t msgn);
    int  (*p_sync)(void *h);
    long long (*p_bytes)(void *h);
    int  (*p_torn_tail)(void *h);
    void (*p_free)(void *h);
};

/* open/close; err receives an ASCII diagnostic on failure */
int  pv2_store_open(pv2_store *st, int impl, const char *dir,
                    const char *server_epoch, const char *library_id,
                    int sync_mode, char *err, size_t errn);
void pv2_store_close(pv2_store *st);
const char *pv2_store_kind(const pv2_store *st);

/* transaction + apply (shared semantics for every backend) */
int  pv2_store_begin(pv2_store *st);
int  pv2_store_commit(pv2_store *st);
void pv2_store_rollback(pv2_store *st);
/* 0 = applied/duplicate/conflict (inspect out->status); <0 = storage failure */
int  pv2_store_apply(pv2_store *st, const pv2_op *op, const char *client_id, pv2_receipt *out);

long pv2_store_last_seq(pv2_store *st);
long pv2_store_min_seq(pv2_store *st);
int  pv2_store_read_change(pv2_store *st, long seq, int *entity, char *id, size_t idn,
                           long *rev, int *deleted);
int  pv2_store_count(pv2_store *st, int entity, int include_deleted);
long pv2_store_entity_revision(pv2_store *st, int entity, const char *id);
int  pv2_store_seed(pv2_store *st, int entity, const char *id, const char *payload,
                    size_t plen, int deleted);   /* fixture/seed path */
int  pv2_store_integrity(pv2_store *st, char *msg, size_t msgn);
int  pv2_store_sync(pv2_store *st);
long long pv2_store_bytes(pv2_store *st);
int  pv2_store_torn_tail(pv2_store *st);
int  pv2_store_gc(pv2_store *st, long long now, int keep_days,
                  long *out_tombstones, long *out_changes);
/* T05B: record the main-phone confirmation for one tombstone; a tombstone is
 * GC-eligible only after >=keep_days AND this confirmation. */
int  pv2_store_confirm_tombstone(pv2_store *st, int entity, const char *id, long long now);

/* -------------------------------------------------- daily plan list (N2) */
/* One materialized row of the phone's daily plan (pv2 entity "daily_plan").
 * The phone pushes one row per (date, word): payload {"word_id","date","seq",
 * "state","goal","revision"}; entity_id = "<worduuid>@<yyyymmdd>" (45 B <=
 * PV2 id cap).  The board never edits these rows - display only. */
typedef struct {
    char word_id[48];    /* full word entity id ("w-...") for text/annotation join */
    char state[16];      /* pending | remembered | review (verbatim from payload) */
    long seq;            /* phone-side stable order (payload "seq") */
} pv2_plan_row;

/* Materialize one business day's plan list (N2: board shows the phone's list,
 * not a date-hash pick).  Rows of `today` when any live row carries that date,
 * else the most recent earlier date (stale cache: caller renders the date).
 * Rows come back ordered by seq (ties keep store order).  date_used receives
 * the date the returned rows belong to ("YYYY-MM-DD", empty when none).
 * *goal = max "goal" seen among those rows (0 = none).  Returns the number of
 * live rows for date_used (rows beyond `max` are counted but not copied);
 * -1 on storage error, 0 with empty date_used when the store has no plan rows. */
int pv2_planlist_pick(pv2_store *st, const char *today, pv2_plan_row *rows, int max,
                      char *date_used, size_t dn, int *goal);

/* ------------------------------------------- course timetable window (N3) */
/* The phone materializes the selected course table into course_occurrence
 * rows from today for 14 days (续工方案 §4.2).  Rows carry the window
 * generation in the payload; one schedule_window row (id "schedule-window",
 * payload {"generation","table","from","to","count"}) declares the target
 * generation and its exact row count.  Packs are pushed in chunks of <=
 * PV2_PACK_MAX_OPS ops; the generation is ACTIVE only when the live row
 * count for that generation equals the window count (atomic switch, derived
 * by recount - no stored flag, safe across restarts and interruptions).
 * While incomplete the board keeps rendering rows of earlier generations
 * (the old table) and reports "syncing" - never a half-new table. */
typedef struct {
    char generation[24];  /* target generation verbatim from the window row  */
    char table[48];       /* course table name (may be CJK)                  */
    char from[12];        /* coverage start "YYYY-MM-DD"                     */
    char to[12];          /* coverage end (inclusive)                        */
    int  have_window;     /* live schedule_window row present                */
    int  target;          /* declared row count for the target generation    */
    int  have;            /* live course_occurrence rows in that generation  */
    int  active;          /* have_window && target > 0 && have == target     */
} pv2_window_info;

/* Read the window state (window row + recount of target-generation rows).
 * Returns 0 when no live window row exists (out zeroed except have_window=0),
 * 1 on success, -1 on storage error. */
int pv2_window_state(pv2_store *st, pv2_window_info *out);

/* --------------------------------------------------- auxiliary key/value */
/* Small file-backed KV used for tokens, settings, jobs and briefing
 * paragraphs. Kept outside the entity store so both storage backends share
 * identical auth/job semantics. File mode 0600, atomic tmp+rename writes. */
typedef struct pv2_kv pv2_kv;

pv2_kv *pv2_kv_open(const char *path, char *err, size_t errn);
void    pv2_kv_close(pv2_kv *kv);
/* T04C: set/del persist through the full pipeline (file fsync, rename, dir
 * sync). 0/1 = persisted, -1 = flush failure with memory rolled back to the
 * on-disk state — callers must refuse to report a persisted success. */
int     pv2_kv_set(pv2_kv *kv, const char *key, const char *val);
const char *pv2_kv_get(pv2_kv *kv, const char *key);
int     pv2_kv_del(pv2_kv *kv, const char *key);
int     pv2_kv_count_prefix(pv2_kv *kv, const char *prefix);
int     pv2_kv_list(pv2_kv *kv, const char *prefix, char **keys, int max);
long long pv2_kv_bytes(const pv2_kv *kv);
/* test-only injection: force every kv_flush to fail until cleared (T04C) */
void    pv2_kv_set_inject_fail(int fail);

/* --------------------------------------------------------- json writer */
typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    int    overflow;
} pv2_jw;

void pv2_jw_init(pv2_jw *w, char *buf, size_t cap);
void pv2_jw_raw(pv2_jw *w, const char *s);
void pv2_jw_ch(pv2_jw *w, char c);
void pv2_jw_str(pv2_jw *w, const char *s);              /* quoted + escaped */
void pv2_jw_i64(pv2_jw *w, long long v);
void pv2_jw_kv_str(pv2_jw *w, const char *k, const char *v);
void pv2_jw_kv_i64(pv2_jw *w, const char *k, long long v);
void pv2_jw_kv_bool(pv2_jw *w, const char *k, int v);
/* append a JSON string value with quotes only (no key) */
void pv2_jw_json_or_null(pv2_jw *w, const char *maybe_json);

/* --------------------------------------------------------------- context */
typedef struct {
    pv2_store store;
    pv2_kv   *aux;                  /* aux KV (tokens/settings/jobs/briefs) */
    char      dir[512];
    char      cred_dir[512];
    char      library_id[48];
    char      server_epoch[48];
    char      board_id[64];
    char      pin[16];
    int       auth_enabled;         /* M9A: 0=鉴权关闭(pair 任意 PIN/请求免 token)，默认 1 */
    int       sync_mode;
    long long now;
} pv2_ctx;

int  pv2_ctx_init(pv2_ctx *c, const char *dir, int impl, const char *board_id,
                  const char *library_id, const char *server_epoch,
                  const char *control_pin, int sync_mode, char *err, size_t errn);
void pv2_ctx_close(pv2_ctx *c);

/* ------------------------------------------------------ api dispatch */
typedef enum {
    PV2_ROUTE_CAPABILITIES = 1,
    PV2_ROUTE_PAIR,
    PV2_ROUTE_SYNC_STATUS,
    PV2_ROUTE_SYNC_PUSH,
    PV2_ROUTE_SYNC_PULL,
    PV2_ROUTE_WORDS,
    PV2_ROUTE_DAILY_PLAN,
    PV2_ROUTE_BRIEFINGS,
    PV2_ROUTE_AI_JOB_CREATE,
    PV2_ROUTE_AI_JOB_GET,
    PV2_ROUTE_AI_JOB_CANCEL,
    PV2_ROUTE_SETTINGS_GET,
    PV2_ROUTE_SETTINGS_POST,
    PV2_ROUTE_TOKEN_REVOKE
} pv2_route;

const char *pv2_route_path(pv2_route r);

typedef struct {
    pv2_route   route;
    const char *method;         /* "GET" / "POST" */
    const char *uri;            /* full request target, may carry a query     */
    const char *auth_header;    /* full "Bearer <token>" value or NULL        */
    const char *body;           /* request body text or NULL                  */
    size_t      body_len;
    const char *cursor;         /* ?cursor= */
    int         limit;          /* ?limit=, 0 = default                       */
    int         part;           /* ?part= briefing paragraph offset, 0-based  */
    const char *date;           /* ?date=YYYY-MM-DD */
    const char *job_id;         /* path parameter */
    const char *client_id;      /* push body field */
    const pv2_op *ops;          /* pre-parsed ops (client packer output)      */
    int         n_ops;
} pv2_request;

typedef struct {
    int http;
    const char *code;
    char message[192];
} pv2_api_err;

/* Single entry point used by the harness and (later) by the HTTP server.
 * Returns body length written, or -1 when the response buffer was too small. */
long pv2_dispatch(pv2_ctx *c, const pv2_request *req, char *out, size_t out_cap,
                  pv2_api_err *err);

/* --------------------------------------------------------------- cursor */
int  pv2_cursor_encode(const char *server_epoch, long seq, char *out, size_t n);
int  pv2_cursor_decode(const char *cursor, char *epoch, size_t en, long *seq);
/* 0 = usable, 1 = expired (epoch or retention), -1 = malformed */
int  pv2_cursor_check(pv2_ctx *c, const char *cursor, long *out_seq, char *why, size_t whyn);

/* ------------------------------------------------------------- client side */
/* Packs ops into request bodies of at most PV2_MAX_REQ_JSON bytes with at most
 * PV2_PACK_MAX_OPS ops per pack. Returns the number of ops packed (0 = the
 * single op does not fit). Used to verify 分包 (import packing). */
int pv2_pack_push(const pv2_op *ops, int n_ops, const char *client_id,
                  char *out, size_t out_cap, size_t *out_len);

/* Client-side import pre-flight (CHK-C06 / capacity-rules 2.6-1): the importer
 * measures the parsed batch (items, UTF-8 bytes incl. envelope) BEFORE packing.
 * Returns 0 = proceed to pv2_pack_push, -1 = items > PV2_IMPORT_MAX_ITEMS,
 * -2 = bytes > PV2_IMPORT_MAX_BYTES (both are 422 import_too_large,
 * retryable=false: a planning error, re-batch instead of retrying). */
int pv2_import_check(size_t items, size_t bytes);

/* --------------------------------------------------------------- auth API */
int pv2_auth_pair(pv2_ctx *c, const char *pin, const char *client_id,
                  const char *client_name, char *token_out, size_t tok_n,
                  pv2_api_err *err);
int pv2_auth_check(pv2_ctx *c, const char *uri, const char *auth_header,
                   char *client_id, size_t cid_n, pv2_api_err *err);
int pv2_auth_revoke(pv2_ctx *c, const char *token, pv2_api_err *err);
int pv2_auth_token_count(pv2_ctx *c);

/* ------------------------------------------------------------- helpers */
long pv2_rss_kb(void);
long long pv2_now_ms(void);
long long pv2_wall_seconds(void);
/* T04B (F05): the only randomness source is the OS CSPRNG — no time/pid/
 * xorshift fallback. A short read or unavailable source is a hard failure, so
 * callers must refuse to mint secrets (security-v2.md §4.2). */
int pv2_random_bytes(unsigned char *out, size_t n);
/* hex helper built on pv2_random_bytes; returns 0 = ok, -1 = random failure
 * (nothing written beyond NUL) */
int pv2_random_hex(char *out, size_t nbytes);
/* Test-only injection hook (T04B): install a source for failure/short-read/
 * deterministic tests. NULL restores the production path. Production code
 * must never install a seed-fixed source. */
typedef int (*pv2_random_fn)(unsigned char *out, size_t n);
void pv2_random_set_inject(pv2_random_fn fn);
int  pv2_const_time_eq(const char *a, const char *b);
const char *pv2_clock_iso(long long epoch_sec, char *buf, size_t n);
const char *pv2_date_of(long long epoch_sec, char *buf, size_t n);
int  pv2_uri_contains_token(const char *uri, const char *token);

#ifdef __cplusplus
}
#endif
#endif /* PANEL_V2_H */
