/* panel_v2_api.c - M2: /api/v2 dispatch, capacity rules, cursor and sync
 * state machine (sec.4 + sec.5).
 *
 * Capacity rules enforced here (never delegated to the transport):
 *   R1  every op record is encoded and measured before it is accepted; a
 *       record whose V2 encoding exceeds PV2_MAX_REQ_JSON (4096 B) is refused
 *       with 413 payload_too_large, so no accepted record can ever be larger;
 *   R2  a response is assembled prologue -> items -> epilogue with an explicit
 *       byte budget (PV2_PAGE_BYTE_BUDGET); when the next item does not fit the
 *       page is truncated early, has_more/next_cursor are set, so the body stays
 *       below PV2_MAX_RESP_JSON (32 KiB);
 *   R3  at most PV2_PAGE_ITEMS (20) items per page;
 *   R4  a briefing paragraph record is capped at PV2_BRIEF_PARA_BYTES (2048 B);
 *   R5  at most PV2_PACK_MAX_OPS (10) ops per push pack; pv2_pack_push() builds
 *       conformant packs and the dispatcher rejects larger packs.
 *
 * Sync state machine (sec.4):
 *   handshake  GET  /api/v2/capabilities          -> limits, server_epoch
 *   push       POST /api/v2/sync/push             -> per-op txn receipt
 *   pull       GET  /api/v2/sync/pull?cursor=...  -> delta + next cursor
 *   commit     client stores next_cursor
 *   op_id idempotency (duplicate receipt), 409 version_conflict with both
 *   copies kept, tombstones with >=30 d retention, expired cursor -> 409
 *   cursor_expired + full_resync flag.
 */
#include "panel_v2.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* --------------------------------------------------------------- helpers */

static void set_err(pv2_api_err *err, int http, const char *code, const char *msg)
{
    if (!err) return;
    err->http = http;
    err->code = code;
    snprintf(err->message, sizeof err->message, "%s", msg ? msg : "");
}

/* D2 (frozen in T02B): retryable means "can be retried after splitting or
 * backoff as a NEW request, never verbatim"; resync_required marks 409
 * cursor_expired. Mapped from the stable code so every error body stays
 * consistent regardless of the call site. */
static void err_flags(const char *code, int *retryable, int *resync_required)
{
    *retryable = 0;
    *resync_required = 0;
    if (!code) return;
    if (strcmp(code, PV2_CODE_TOO_LARGE) == 0 ||
        strcmp(code, PV2_CODE_QUEUE_FULL) == 0 ||
        strcmp(code, PV2_CODE_STORAGE) == 0)
        *retryable = 1;
    else if (strcmp(code, PV2_CODE_CURSOR_EXPIRED) == 0)
        *resync_required = 1;
}

/* Build a single-member detail object {"<key>":"<value>"} (D2: detail is a
 * JSON object, not a string). Fits small stack buffers via pv2_jw escaping. */
static void detail_str_kv(char *out, size_t n, const char *key, const char *val)
{
    pv2_jw t;
    if (!out || n < 8) return;
    pv2_jw_init(&t, out, n);
    pv2_jw_raw(&t, "{\"");
    pv2_jw_raw(&t, key);
    pv2_jw_raw(&t, "\":");
    pv2_jw_str(&t, val ? val : "");
    pv2_jw_raw(&t, "}");
}

/* Unified error body per openapi Error (D2): code/message/http_status plus a
 * detail OBJECT, retryable and resync_required. `detail_obj` is a pre-encoded
 * JSON object fragment (or NULL/"" for {}). http_status always matches the
 * status returned in err->http. */
static long fail(pv2_jw *w, pv2_api_err *err, int http, const char *code,
                 const char *msg, const char *detail_obj)
{
    int retryable, resync_required;
    pv2_jw_init(w, w->buf, w->cap);
    pv2_jw_raw(w, "{\"ok\":false,\"code\":");
    pv2_jw_str(w, code);
    pv2_jw_raw(w, ",\"message\":");
    pv2_jw_str(w, msg);
    pv2_jw_raw(w, ",\"http_status\":");
    pv2_jw_i64(w, http);
    pv2_jw_raw(w, ",\"detail\":");
    pv2_jw_raw(w, (detail_obj && *detail_obj) ? detail_obj : "{}");
    err_flags(code, &retryable, &resync_required);
    pv2_jw_raw(w, ",\"retryable\":");
    pv2_jw_raw(w, retryable ? "true" : "false");
    pv2_jw_raw(w, ",\"resync_required\":");
    pv2_jw_raw(w, resync_required ? "true" : "false");
    pv2_jw_raw(w, "}");
    set_err(err, http, code, msg);
    return (long)w->len;
}

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/* json_get_str - minimal, ASCII, fixture-oriented scan for `"key":"value"` at
 * any depth. The real HTTP layer will hand pre-parsed fields (pv2_request);
 * this keeps the M2 harness and the OpenAPI fixtures self-contained. */
static int json_get_str(const char *body, size_t blen, const char *key,
                        char *out, size_t outn)
{
    char pat[80];
    size_t kl = strlen(key), i, o = 0;
    if (!body) return 0;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    for (i = 0; i + kl + 2 <= blen; i++) {
        if (strncmp(body + i, pat, kl + 2) != 0) continue;
        i += kl + 2;
        while (i < blen && is_ws(body[i])) i++;
        if (i >= blen || body[i] != ':') continue;
        i++;
        while (i < blen && is_ws(body[i])) i++;
        if (i >= blen) return 0;
        if (body[i] == '"') {
            i++;
            while (i < blen && body[i] != '"') {
                char c = body[i];
                if (c == '\\' && i + 1 < blen) {
                    char e = body[i + 1];
                    c = (e == 'n') ? '\n' : (e == 'r') ? '\r' : (e == 't') ? '\t' : e;
                    i += 2;
                } else {
                    i++;
                }
                if (o + 1 < outn) out[o++] = c;
            }
            if (outn) out[o < outn ? o : outn - 1] = '\0';
            return 1;
        }
        return 0;
    }
    return 0;
}

static int body_has_secret_key(const char *body, size_t blen, char *hit, size_t hitn)
{
    static const char *bad[] = { "\"pin\"", "\"token\"", "\"password\"", "\"passwd\"",
                                 "\"secret\"", "\"api_key\"", "\"apikey\"", "\"credential\"",
                                 "\"control_pin\"", "\"bearer\"" };
    size_t i;
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        if (body && strstr(body, bad[i])) {
            if (hit && hitn) snprintf(hit, hitn, "%s", bad[i]);
            return 1;
        }
    }
    (void)blen;
    return 0;
}

/* ------------------------------------------------------------- cursors */
int pv2_cursor_encode(const char *server_epoch, long seq, char *out, size_t n)
{
    const char *ep = (server_epoch && *server_epoch) ? server_epoch : "e0";
    if (!out || n < 8) return -1;
    return snprintf(out, n, "c1.%s.%ld", ep, seq) > 0 ? 0 : -1;
}

int pv2_cursor_decode(const char *cursor, char *epoch, size_t en, long *seq)
{
    const char *p, *d;
    size_t i = 0;
    if (!cursor || strncmp(cursor, "c1.", 3) != 0) return -1;
    p = cursor + 3;
    d = strchr(p, '.');
    if (!d) return -1;
    if (d == p) return -1;
    while (p + i < d) {
        char c = p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return -1;
        if (epoch && i + 1 < en) epoch[i] = c;
        i++;
    }
    if (epoch && en) epoch[i < en ? i : en - 1] = '\0';
    d++;
    if (!*d) return -1;
    {
        const char *q;
        long v = 0;
        for (q = d; *q; q++) {
            if (!isdigit((unsigned char)*q)) return -1;
            if (v > 100000000L) return -1;
            v = v * 10 + (*q - '0');
        }
        if (seq) *seq = v;
    }
    return 0;
}

int pv2_cursor_check(pv2_ctx *c, const char *cursor, long *out_seq, char *why, size_t whyn)
{
    char ep[64];
    long s = 0;
    long last_seq, min_seq;
    if (why && whyn) why[0] = '\0';
    if (!cursor || !*cursor) {                       /* first sync: full pull */
        if (out_seq) *out_seq = 0;
        return 0;
    }
    if (pv2_cursor_decode(cursor, ep, sizeof ep, &s) != 0) {
        if (why && whyn) snprintf(why, whyn, "malformed cursor");
        return -1;
    }
    if (c->server_epoch[0] && strcmp(ep, c->server_epoch) != 0) {
        if (why && whyn) snprintf(why, whyn, "server epoch changed (%s != %s)",
                                  ep, c->server_epoch);
        return 1;
    }
    last_seq = pv2_store_last_seq(&c->store);
    min_seq  = pv2_store_min_seq(&c->store);
    if (s > last_seq) {
        if (why && whyn) snprintf(why, whyn, "cursor ahead of server (%ld > %ld)", s, last_seq);
        return 1;
    }
    if (min_seq > 0 && s + 1 < min_seq) {
        if (why && whyn) snprintf(why, whyn,
                                  "changes for seq %ld..%ld already collected", s + 1, min_seq - 1);
        return 1;
    }
    if (out_seq) *out_seq = s;
    return 0;
}

/* ------------------------------------------------------------ op packing */
static void enc_op(pv2_jw *w, const pv2_op *op)
{
    pv2_jw_raw(w, "{\"op_id\":");
    pv2_jw_str(w, op->op_id);
    pv2_jw_raw(w, ",\"entity\":");
    pv2_jw_str(w, pv2_entity_name(op->entity));
    pv2_jw_raw(w, ",\"id\":");
    pv2_jw_str(w, op->entity_id);
    pv2_jw_raw(w, ",\"base_revision\":");
    pv2_jw_i64(w, (long long)op->base_revision);
    pv2_jw_raw(w, ",\"deleted\":");
    pv2_jw_raw(w, op->deleted ? "true" : "false");
    pv2_jw_raw(w, ",\"retry_count\":");
    pv2_jw_i64(w, (long long)op->retry_count);
    pv2_jw_raw(w, ",\"payload\":");
    pv2_jw_json_or_null(w, op->payload);
    pv2_jw_raw(w, "}");
}

/* Encode one op as a standalone record; returns 0 when it fits in max (the
 * encoded document excludes the terminating NUL). */
static int enc_op_measured(const pv2_op *op, size_t max, char *scratch, size_t scratch_cap,
                           size_t *out_len)
{
    pv2_jw m;
    pv2_jw_init(&m, scratch, scratch_cap < max + 1 ? scratch_cap : max + 1);
    enc_op(&m, op);
    if (m.overflow) return -1;
    if (out_len) *out_len = m.len;
    return 0;
}

int pv2_pack_push(const pv2_op *ops, int n_ops, const char *client_id,
                  char *out, size_t out_cap, size_t *out_len)
{
    pv2_jw w;
    char scratch[PV2_MAX_REQ_JSON + 1];
    size_t budget = out_cap < PV2_MAX_REQ_JSON ? out_cap : PV2_MAX_REQ_JSON;
    int i, packed = 0;

    if (!out || budget < 64) return 0;
    pv2_jw_init(&w, out, budget);
    pv2_jw_raw(&w, "{\"client_id\":");
    pv2_jw_str(&w, client_id ? client_id : "");
    pv2_jw_raw(&w, ",\"ops\":[");
    for (i = 0; i < n_ops && packed < (int)PV2_PACK_MAX_OPS; i++) {
        size_t need;
        if (enc_op_measured(&ops[i], PV2_MAX_REQ_JSON, scratch, sizeof scratch, &need) != 0)
            continue;                                  /* single op too big: skip */
        if (w.len + need + 3 > budget - 1) break;      /* would not fit the pack */
        if (packed) pv2_jw_ch(&w, ',');
        pv2_jw_raw(&w, scratch);
        packed++;
    }
    if (w.overflow) return 0;
    pv2_jw_raw(&w, "]}");
    if (w.overflow) return 0;
    if (out_len) *out_len = w.len;
    return packed;
}

/* CHK-C06: client-side import pre-flight. The importer measures the parsed
 * batch (2.6-1: UTF-8 bytes incl. envelope) BEFORE packing; an oversized
 * batch is a planning error (422 import_too_large, retryable=false), not a
 * transient failure — re-batch instead of retrying. */
int pv2_import_check(size_t items, size_t bytes)
{
    if (items > (size_t)PV2_IMPORT_MAX_ITEMS) return -1;
    if (bytes > (size_t)PV2_IMPORT_MAX_BYTES) return -2;
    return 0;
}

/* --------------------------------------------------------- page assembly */
/* Append items while they fit the byte budget and the 20-item page. */
#define PAGE_BUDGET PV2_PAGE_BYTE_BUDGET

static void epilogue_page(pv2_jw *w, int count, int has_more, int trunc_bytes,
                          const char *next_cursor)
{
    pv2_jw_raw(w, "],\"count\":");
    pv2_jw_i64(w, count);
    pv2_jw_raw(w, ",\"has_more\":");
    pv2_jw_raw(w, has_more ? "true" : "false");
    /* D1 (frozen): truncated_by_bytes is true ONLY on the byte-budget branch;
     * short pages from count<limit or filtered/missing records stay false. */
    pv2_jw_raw(w, ",\"truncated_by_bytes\":");
    pv2_jw_raw(w, trunc_bytes ? "true" : "false");
    pv2_jw_raw(w, ",\"next_cursor\":");
    pv2_jw_str(w, next_cursor);
}

/* ------------------------------------------------------------ dispatch */

static long r_capabilities(pv2_ctx *c, pv2_jw *w)
{
    pv2_jw_raw(w, "{\"ok\":true,\"schema\":\"" PV2_SCHEMA_VERSION "\",\"board_id\":");
    pv2_jw_str(w, c->board_id);
    pv2_jw_raw(w, ",\"library_id\":");
    pv2_jw_str(w, c->library_id);
    pv2_jw_raw(w, ",\"server_epoch\":");
    pv2_jw_str(w, c->server_epoch);
    pv2_jw_raw(w, ",\"store\":");
    pv2_jw_str(w, pv2_store_kind(&c->store));
    pv2_jw_raw(w, ",\"sync_mode\":");
    pv2_jw_i64(w, c->sync_mode);
    pv2_jw_raw(w, ",\"torn_tail\":");
    pv2_jw_raw(w, pv2_store_torn_tail(&c->store) ? "true" : "false");
    pv2_jw_raw(w, ",\"limits\":{");
    {
        /* D3 (frozen): descriptive capability names per openapi Limits;
         * internal short names / budgets (28672, op_payload, tombstone days)
         * are NOT published. */
        char lb[256];
        snprintf(lb, sizeof lb,
                 "\"request_json_bytes_max\":%u,\"response_bytes_max\":%u,"
                 "\"page_items_max\":%u,\"briefing_paragraph_bytes_max\":%u,"
                 "\"import_packet_items_max\":%u,\"library_words_max\":%u,"
                 "\"import_items_max\":%u,\"import_bytes_max\":%u,\"ai_queue_max\":%u}",
                 (unsigned)PV2_MAX_REQ_JSON, (unsigned)PV2_MAX_RESP_JSON,
                 (unsigned)PV2_PAGE_ITEMS, (unsigned)PV2_BRIEF_PARA_BYTES,
                 (unsigned)PV2_PACK_MAX_OPS, (unsigned)PV2_LIBRARY_MAX_WORDS,
                 (unsigned)PV2_IMPORT_MAX_ITEMS, (unsigned)PV2_IMPORT_MAX_BYTES,
                 (unsigned)PV2_AI_QUEUE_MAX);
        pv2_jw_raw(w, lb);
    }
    pv2_jw_raw(w, ",\"entities\":[");
    {
        int e;
        for (e = PV2_ENT_WORD; e <= PV2_ENT_MAX; e++) {   /* N3: bound widened */
            if (e > PV2_ENT_WORD) pv2_jw_ch(w, ',');
            pv2_jw_str(w, pv2_entity_name(e));
        }
    }
    pv2_jw_raw(w, "],\"auth\":{\"scheme\":\"Bearer\",\"pairing\":\"control_pin\","
                  "\"token\":\"revocable\",\"token_in_url\":false}");
    pv2_jw_raw(w, ",\"paired_clients\":");
    pv2_jw_i64(w, pv2_auth_token_count(c));
    pv2_jw_raw(w, "}");
    return (long)w->len;
}

static long r_pair(pv2_ctx *c, const pv2_request *req, pv2_jw *w, pv2_api_err *err)
{
    char pin[32] = "", cid[64] = "", cname[96] = "";
    char token[128];
    if (!req->body || req->body_len == 0)
        return fail(w, err, 400, PV2_CODE_BAD_REQUEST, "pair body required", NULL);
    json_get_str(req->body, req->body_len, "pin", pin, sizeof pin);
    json_get_str(req->body, req->body_len, "client_id", cid, sizeof cid);
    json_get_str(req->body, req->body_len, "client_name", cname, sizeof cname);
    if (!pin[0])
        return fail(w, err, 400, PV2_CODE_BAD_REQUEST, "pin required", NULL);
    if (pv2_auth_pair(c, pin, cid, cname, token, sizeof token, err) != 0) {
        char msg[192];
        snprintf(msg, sizeof msg, "%s", err ? err->message : "pairing failed");
        return fail(w, err, err ? err->http : 401, err ? err->code : PV2_CODE_UNAUTHORIZED, msg, NULL);
    }
    pv2_jw_raw(w, "{\"ok\":true,\"token\":");
    pv2_jw_str(w, token);
    pv2_jw_raw(w, ",\"client_id\":");
    pv2_jw_str(w, cid);
    pv2_jw_raw(w, ",\"server_epoch\":");
    pv2_jw_str(w, c->server_epoch);
    pv2_jw_raw(w, ",\"ttl\":\"revocable\"}");
    set_err(err, 200, "", "");
    return (long)w->len;
}

static long r_sync_status(pv2_ctx *c, pv2_jw *w)
{
    char cur[64];
    int e;
    long live = 0, total = 0;
    long last_seq = pv2_store_last_seq(&c->store);
    pv2_cursor_encode(c->server_epoch, last_seq, cur, sizeof cur);
    pv2_jw_raw(w, "{\"ok\":true,\"server_epoch\":");
    pv2_jw_str(w, c->server_epoch);
    pv2_jw_raw(w, ",\"board_id\":");
    pv2_jw_str(w, c->board_id);
    pv2_jw_raw(w, ",\"last_seq\":");
    pv2_jw_i64(w, last_seq);
    pv2_jw_raw(w, ",\"min_seq\":");
    pv2_jw_i64(w, pv2_store_min_seq(&c->store));
    pv2_jw_raw(w, ",\"retention_floor\":");
    pv2_jw_i64(w, (c->store.p_retention_floor)
                      ? c->store.p_retention_floor(c->store.h) : 0);
    pv2_jw_raw(w, ",\"torn_tail\":");
    pv2_jw_raw(w, pv2_store_torn_tail(&c->store) ? "true" : "false");
    pv2_jw_raw(w, ",\"integrity\":");
    {
        char msg[256] = "";
        pv2_store_integrity(&c->store, msg, sizeof msg);
        pv2_jw_str(w, msg);
    }
    pv2_jw_raw(w, ",\"conflicts\":");
    pv2_jw_i64(w, c->store.p_conflict_count ? c->store.p_conflict_count(c->store.h) : 0);
    pv2_jw_raw(w, ",\"cursor\":");
    pv2_jw_str(w, cur);
    pv2_jw_raw(w, ",\"counts\":{");
    for (e = PV2_ENT_WORD; e <= PV2_ENT_MAX; e++) {   /* N3: bound widened */
        long l = pv2_store_count(&c->store, e, 0);
        long t = pv2_store_count(&c->store, e, 1);
        live += l; total += t;
        if (e > PV2_ENT_WORD) pv2_jw_ch(w, ',');
        pv2_jw_str(w, pv2_entity_name(e));
        pv2_jw_ch(w, ':');
        pv2_jw_i64(w, l);
    }
    pv2_jw_raw(w, "},\"live\":");
    pv2_jw_i64(w, live);
    pv2_jw_raw(w, ",\"with_tombstones\":");
    pv2_jw_i64(w, total);
    pv2_jw_raw(w, ",\"tombstones\":");
    pv2_jw_i64(w, total - live);
    pv2_jw_raw(w, "}");
    return (long)w->len;
}

static long r_sync_push(pv2_ctx *c, const pv2_request *req, pv2_jw *w, pv2_api_err *err)
{
    int i, applied = 0, dup = 0, conflict = 0, rejected = 0, storagefail = 0;
    int worst_http = 200;
    const char *worst_code = "";
    char scratch[PV2_MAX_REQ_JSON + 1];
    char cid[64];

    snprintf(cid, sizeof cid, "%s", req->client_id ? req->client_id : "");
    if (!cid[0] && req->body)
        json_get_str(req->body, req->body_len, "client_id", cid, sizeof cid);
    if (!cid[0])
        return fail(w, err, 400, PV2_CODE_BAD_REQUEST, "client_id required", NULL);
    if (!req->ops || req->n_ops <= 0)
        return fail(w, err, 400, PV2_CODE_BAD_REQUEST, "ops[] required (pre-parsed)", NULL);
    if (req->n_ops > (int)PV2_PACK_MAX_OPS) {
        char d[96];
        snprintf(d, sizeof d, "{\"ops\":%d,\"ops_max\":%u}", req->n_ops,
                 (unsigned)PV2_PACK_MAX_OPS);
        return fail(w, err, 400, PV2_CODE_BAD_REQUEST, "too many ops in one pack", d);
    }

    pv2_jw_raw(w, "{\"ok\":");
    pv2_jw_raw(w, "true,\"server_epoch\":");
    pv2_jw_str(w, c->server_epoch);
    pv2_jw_raw(w, ",\"client_id\":");
    pv2_jw_str(w, cid);
    pv2_jw_raw(w, ",\"receipts\":[");
    for (i = 0; i < req->n_ops; i++) {
        const pv2_op *op = &req->ops[i];
        pv2_receipt rc;
        int rc_status;
        memset(&rc, 0, sizeof rc);
        if (i) pv2_jw_ch(w, ',');
        /* R1: measure the record before accepting it */
        if (enc_op_measured(op, PV2_MAX_REQ_JSON, scratch, sizeof scratch, NULL) != 0) {
            rejected++;
            worst_http = 413;
            worst_code = PV2_CODE_TOO_LARGE;
            pv2_jw_raw(w, "{\"op_id\":");
            pv2_jw_str(w, op->op_id);
            pv2_jw_raw(w, ",\"status\":\"rejected\",\"code\":\"" PV2_CODE_TOO_LARGE
                          "\",\"message\":\"op record exceeds 4096 B after V2 encoding\"}");
            continue;
        }
        if (!op->op_id[0] || !op->entity_id[0] ||
            strcmp(pv2_entity_name(op->entity), "unknown") == 0) {
            rejected++;
            if (worst_http == 200) { worst_http = 422; worst_code = PV2_CODE_UNPROCESSABLE; }
            pv2_jw_raw(w, "{\"op_id\":");
            pv2_jw_str(w, op->op_id);
            pv2_jw_raw(w, ",\"status\":\"rejected\",\"code\":\"" PV2_CODE_UNPROCESSABLE
                          "\",\"message\":\"op_id, entity and id are required\"}");
            continue;
        }
        if (op->payload && op->payload_len > PV2_OP_PAYLOAD_BYTES) {
            rejected++;
            worst_http = 413;
            worst_code = PV2_CODE_TOO_LARGE;
            pv2_jw_raw(w, "{\"op_id\":");
            pv2_jw_str(w, op->op_id);
            pv2_jw_raw(w, ",\"status\":\"rejected\",\"code\":\"" PV2_CODE_TOO_LARGE
                          "\",\"message\":\"op payload exceeds 3072 B\"}");
            continue;
        }
        if (op->entity == PV2_ENT_BRIEFING && op->payload) {
            size_t l;
            if (enc_op_measured(op, PV2_BRIEF_PARA_BYTES, scratch, sizeof scratch, &l) != 0) {
                rejected++;
                worst_http = 413;
                worst_code = PV2_CODE_TOO_LARGE;
                pv2_jw_raw(w, "{\"op_id\":");
                pv2_jw_str(w, op->op_id);
                pv2_jw_raw(w, ",\"status\":\"rejected\",\"code\":\"" PV2_CODE_TOO_LARGE
                              "\",\"message\":\"briefing paragraph record exceeds 2048 B; split it\"}");
                continue;
            }
        }
        /* CHK-C08: first-release library cap. A NEW live word beyond
         * PV2_LIBRARY_MAX_WORDS is refused and never stored; updates to
         * existing ids (duplicate/conflict) still apply. */
        if (op->entity == PV2_ENT_WORD && !op->deleted) {
            char probe[PV2_MAX_REQ_JSON];
            size_t xlen = 0;
            long xrev = 0;
            int xdel = 0;
            int exists = c->store.p_get_payload &&
                         c->store.p_get_payload(c->store.h, PV2_ENT_WORD, op->entity_id,
                                                probe, sizeof probe, &xlen, &xrev, &xdel) == 1;
            if (!exists &&
                pv2_store_count(&c->store, PV2_ENT_WORD, 0) >= (long)PV2_LIBRARY_MAX_WORDS) {
                rejected++;
                worst_http = 413;
                worst_code = PV2_CODE_TOO_LARGE;
                pv2_jw_raw(w, "{\"op_id\":");
                pv2_jw_str(w, op->op_id);
                pv2_jw_raw(w, ",\"status\":\"rejected\",\"code\":\"library_full\","
                              "\"message\":\"word library full; delete words before adding more\"}");
                continue;
            }
        }

        rc_status = pv2_store_apply(&c->store, op, cid, &rc);
        snprintf(rc.server_epoch, sizeof rc.server_epoch, "%s", c->server_epoch);
        if (rc_status < 0) {
            storagefail++;
            worst_http = 500;
            worst_code = PV2_CODE_STORAGE;
            pv2_jw_raw(w, "{\"op_id\":");
            pv2_jw_str(w, op->op_id);
            pv2_jw_raw(w, ",\"status\":\"storage_fail\",\"code\":\"" PV2_CODE_STORAGE
                          "\",\"message\":\"transaction failed\"}");
            continue;
        }
        switch (rc.status) {
        case PV2_ST_APPLIED:   applied++;   break;
        case PV2_ST_DUPLICATE: dup++;       break;
        case PV2_ST_CONFLICT:  conflict++;  if (worst_http == 200) { worst_http = 409; worst_code = PV2_CODE_CONFLICT; } break;
        default:               rejected++;  if (worst_http == 200) { worst_http = 422; worst_code = PV2_CODE_UNPROCESSABLE; } break;
        }
        pv2_jw_raw(w, "{\"op_id\":");
        pv2_jw_str(w, rc.op_id);
        pv2_jw_raw(w, ",\"status\":");
        pv2_jw_str(w, pv2_op_status_name(rc.status));
        pv2_jw_raw(w, ",\"revision\":");
        pv2_jw_i64(w, rc.revision);
        pv2_jw_raw(w, ",\"seq\":");
        pv2_jw_i64(w, rc.seq);
        if (rc.code[0]) {
            pv2_jw_raw(w, ",\"code\":");
            pv2_jw_str(w, rc.code);
        }
        if (rc.message[0]) {
            pv2_jw_raw(w, ",\"message\":");
            pv2_jw_str(w, rc.message);
        }
        if (rc.status == PV2_ST_CONFLICT && rc.conflict_copy_id[0]) {
            pv2_jw_raw(w, ",\"conflict_copy_id\":");
            pv2_jw_str(w, rc.conflict_copy_id);
            pv2_jw_raw(w, ",\"conflict_strategy\":\"both_copies_kept\"");
        }
        pv2_jw_ch(w, '}');
    }
    pv2_jw_raw(w, "],\"applied\":");
    pv2_jw_i64(w, applied);
    pv2_jw_raw(w, ",\"duplicate\":");
    pv2_jw_i64(w, dup);
    pv2_jw_raw(w, ",\"conflict\":");
    pv2_jw_i64(w, conflict);
    pv2_jw_raw(w, ",\"rejected\":");
    pv2_jw_i64(w, rejected);
    pv2_jw_raw(w, ",\"storage_fail\":");
    pv2_jw_i64(w, storagefail);
    pv2_jw_raw(w, ",\"last_seq\":");
    pv2_jw_i64(w, pv2_store_last_seq(&c->store));
    pv2_jw_raw(w, "}");

    if (worst_http != 200) {
        if (storagefail) set_err(err, 500, PV2_CODE_STORAGE, "one or more transactions failed");
        else if (rejected) set_err(err, worst_http == 413 ? 413 : 422, worst_code,
                                   "one or more ops rejected");
        else set_err(err, 409, PV2_CODE_CONFLICT,
                     "one or more ops conflicted; both copies kept");
    } else {
        set_err(err, 200, "", "");
    }
    return (long)w->len;
}

static long r_sync_pull(pv2_ctx *c, const pv2_request *req, pv2_jw *w, pv2_api_err *err)
{
    char why[192] = "";
    long from = 0, last_seq, min_seq;
    long cursor_seq = 0;
    int check, n = 0, has_more = 0, gap = 0, trunc_bytes = 0;
    size_t limit = (req->limit > 0) ? (size_t)req->limit : PV2_PAGE_ITEMS;
    char scratch[PV2_MAX_REQ_JSON + 2];
    char cur[64];
    long i;

    check = pv2_cursor_check(c, req->cursor, &from, why, sizeof why);
    if (check < 0) {
        char d[224];
        detail_str_kv(d, sizeof d, "cursor", req->cursor);
        return fail(w, err, 400, PV2_CODE_BAD_REQUEST, "malformed cursor", d);
    }
    if (check > 0) {
        char d[224];
        detail_str_kv(d, sizeof d, "reason", why);
        return fail(w, err, 409, PV2_CODE_CURSOR_EXPIRED,
                    "cursor expired: full re-sync required", d);
    }

    last_seq = pv2_store_last_seq(&c->store);
    min_seq  = pv2_store_min_seq(&c->store);
    if ((!req->cursor || !*req->cursor) && min_seq > 1) {
        /* first sync: start at the retention floor, everything older is gone */
        from = min_seq - 1;
    }

    pv2_jw_raw(w, "{\"ok\":true,\"server_epoch\":");
    pv2_jw_str(w, c->server_epoch);
    pv2_jw_raw(w, ",\"from_seq\":");
    pv2_jw_i64(w, from);
    pv2_jw_raw(w, ",\"last_seq\":");
    pv2_jw_i64(w, last_seq);
    pv2_jw_raw(w, ",\"min_seq\":");
    pv2_jw_i64(w, min_seq);
    pv2_jw_raw(w, ",\"full_resync\":false,\"gap\":false,\"changes\":[");
    {
        int first = 1;
        for (i = from + 1; i <= last_seq; i++) {
            int entity = 0, deleted = 0;
            long rev = 0;
            char id[48] = "";
            char payload[PV2_MAX_REQ_JSON];
            size_t plen = 0;
            pv2_jw m;
            if (pv2_store_read_change(&c->store, i, &entity, id, sizeof id, &rev, &deleted) != 1) {
                gap = 1;                 /* collected/absent change: cannot continue */
                has_more = 1;
                break;
            }
            memset(payload, 0, sizeof payload);
            if (c->store.p_get_payload &&
                c->store.p_get_payload(c->store.h, entity, id, payload, sizeof payload,
                                       &plen, &rev, &deleted) != 1) {
                plen = 0;
            }
            pv2_jw_init(&m, scratch, sizeof scratch);
            pv2_jw_raw(&m, "{\"seq\":");
            pv2_jw_i64(&m, i);
            pv2_jw_raw(&m, ",\"entity\":");
            pv2_jw_str(&m, pv2_entity_name(entity));
            pv2_jw_raw(&m, ",\"id\":");
            pv2_jw_str(&m, id);
            pv2_jw_raw(&m, ",\"revision\":");
            pv2_jw_i64(&m, rev);
            pv2_jw_raw(&m, ",\"deleted\":");
            pv2_jw_raw(&m, deleted ? "true" : "false");
            pv2_jw_raw(&m, ",\"payload\":");
            if (deleted || plen == 0) pv2_jw_raw(&m, "null");
            else pv2_jw_json_or_null(&m, payload);
            pv2_jw_raw(&m, "}");
            if (m.overflow) { gap = 1; has_more = 1; break; }
            /* D1: item cap and byte budget are distinct stop reasons — only
             * the byte-budget hit may set truncated_by_bytes. */
            if ((size_t)n >= limit) { has_more = 1; break; }
            if (w->len + m.len + 8 > PAGE_BUDGET) {
                has_more = 1;            /* R2/R3: truncate the page early */
                trunc_bytes = 1;
                break;
            }
            if (!first) pv2_jw_ch(w, ',');
            pv2_jw_raw(w, m.buf);
            first = 0;
            cursor_seq = i;
            n++;
        }
    }
    pv2_jw_raw(w, "],\"count\":");
    pv2_jw_i64(w, n);
    pv2_jw_raw(w, ",\"has_more\":");
    pv2_jw_raw(w, has_more ? "true" : "false");
    pv2_jw_raw(w, ",\"gap\":");
    pv2_jw_raw(w, gap ? "true" : "false");
    pv2_jw_raw(w, ",\"truncated_by_bytes\":");
    pv2_jw_raw(w, trunc_bytes ? "true" : "false");
    pv2_cursor_encode(c->server_epoch, cursor_seq ? cursor_seq : from, cur, sizeof cur);
    pv2_jw_raw(w, ",\"next_cursor\":");
    pv2_jw_str(w, cur);
    pv2_jw_raw(w, "}");
    set_err(err, 200, "", "");
    return (long)w->len;
}

/* list page over an entity (words / daily-plan).
 * Live rows only; tombstones stay in the pull stream. The page stops at 20
 * items or when the next record would cross the byte budget (R2/R3). */
static long r_list_entity(pv2_ctx *c, int entity, const pv2_request *req, pv2_jw *w,
                          pv2_api_err *err, const char *extra_kv, const char *extra_val)
{
    pv2_entity_info infos[64];
    char scratch[PV2_MAX_REQ_JSON + 2];
    char cur[64];
    long scanned = 0;
    int n = 0, has_more = 0, trunc_bytes = 0, first = 1;
    size_t limit = (req->limit > 0) ? (size_t)req->limit : PV2_PAGE_ITEMS;
    long total;

    if (!c->store.p_list || !c->store.p_get_payload)
        return fail(w, err, 500, PV2_CODE_STORAGE, "store has no list support", NULL);
    total = pv2_store_count(&c->store, entity, 0);      /* live rows */

    if (req->cursor && *req->cursor) {
        char ep[64];
        long o = 0;
        if (pv2_cursor_decode(req->cursor, ep, sizeof ep, &o) != 0) {
            char d[224];
            detail_str_kv(d, sizeof d, "cursor", req->cursor);
            return fail(w, err, 400, PV2_CODE_BAD_REQUEST, "malformed cursor", d);
        }
        if (c->server_epoch[0] && strcmp(ep, c->server_epoch) != 0) {
            char d[224];
            detail_str_kv(d, sizeof d, "cursor", req->cursor);
            return fail(w, err, 409, PV2_CODE_CURSOR_EXPIRED,
                        "epoch changed: restart paging", d);
        }
        scanned = o;
    }
    if (limit > PV2_PAGE_ITEMS) limit = PV2_PAGE_ITEMS;

    pv2_jw_raw(w, "{\"ok\":true,\"server_epoch\":");
    pv2_jw_str(w, c->server_epoch);
    pv2_jw_raw(w, ",\"entity\":");
    pv2_jw_str(w, pv2_entity_name(entity));
    /* WordPage required member (schema): the paged library id */
    pv2_jw_raw(w, ",\"library_id\":");
    pv2_jw_str(w, c->library_id);
    if (extra_kv && extra_val) {
        pv2_jw_ch(w, ',');
        pv2_jw_str(w, extra_kv);
        pv2_jw_ch(w, ':');
        pv2_jw_str(w, extra_val);
    }
    pv2_jw_raw(w, ",\"total\":");
    pv2_jw_i64(w, total);
    pv2_jw_raw(w, ",\"items\":[");
    while (n < (int)limit) {
        int got = c->store.p_list(c->store.h, entity, scanned, infos,
                                  (int)(sizeof infos / sizeof infos[0]));
        int k, stop = 0;
        if (got <= 0) break;
        for (k = 0; k < got; k++) {
            char payload[PV2_MAX_REQ_JSON];
            size_t plen = 0;
            pv2_jw m;
            long rev = 0;
            int del = 0;
            if (n >= (int)limit) { has_more = 1; stop = 1; break; }   /* item cap, not bytes */
            if (infos[k].deleted) continue;
            memset(payload, 0, sizeof payload);
            if (c->store.p_get_payload(c->store.h, entity, infos[k].id, payload,
                                       sizeof payload, &plen, &rev, &del) != 1) continue;
            pv2_jw_init(&m, scratch, sizeof scratch);
            pv2_jw_raw(&m, "{\"id\":");
            pv2_jw_str(&m, infos[k].id);
            pv2_jw_raw(&m, ",\"revision\":");
            pv2_jw_i64(&m, infos[k].revision);
            pv2_jw_raw(&m, ",\"updated_at\":");
            pv2_jw_i64(&m, infos[k].updated_at);
            pv2_jw_raw(&m, ",\"payload\":");
            pv2_jw_json_or_null(&m, plen ? payload : NULL);
            pv2_jw_raw(&m, "}");
            if (m.overflow) {                       /* record taller than scratch: */
                has_more = 1; trunc_bytes = 1; stop = 1; break;   /* byte-capacity stop (D1) */
            }
            if (w->len + m.len + 8 > PAGE_BUDGET) { has_more = 1; trunc_bytes = 1; stop = 1; break; }
            if (!first) pv2_jw_ch(w, ',');
            pv2_jw_raw(w, m.buf);
            first = 0;
            n++;
        }
        scanned += got;
        if (stop) break;
    }
    if (!has_more) {
        pv2_entity_info probe;
        if (n < (int)limit && c->store.p_list(c->store.h, entity, scanned, &probe, 1) > 0)
            has_more = 1;
    }
    pv2_cursor_encode(c->server_epoch, scanned, cur, sizeof cur);
    epilogue_page(w, n, has_more, trunc_bytes, cur);
    pv2_jw_raw(w, "}");
    set_err(err, 200, "", "");
    return (long)w->len;
}

/* D4-frozen briefing page (GET /briefings). Paragraphs are one-record-per-
 * paragraph rows of a day (ids carry the date, e.g. br-2026-09-15-p1);
 * part is a 0-based paragraph offset, parts_total the day's paragraph count
 * (not a shard count), next_part = part + len(paragraphs) or null. The
 * optional embedded briefing object is NOT emitted: the prototype store keeps
 * only paragraph records, there is no aggregate to embed (schema-legal).
 * content_version lives in the (omitted) aggregate; per capacity-rules 2.6-5
 * clients restart paging from part=0 whenever the day is rewritten. */
static long r_briefing_page(pv2_ctx *c, const pv2_request *req, pv2_jw *w, pv2_api_err *err)
{
    pv2_entity_info infos[64];
    char scratch[PV2_MAX_REQ_JSON + 2];
    char para[PV2_BRIEF_PARA_BYTES + 1];
    const char *date = (req->date && *req->date) ? req->date : NULL;
    long part = req->part, limit = (req->limit > 0) ? req->limit : 2;
    long parts_total = 0, emitted = 0, matched = 0, scanned = 0;
    int trunc_bytes = 0;

    if (!date)
        return fail(w, err, 400, PV2_CODE_BAD_REQUEST,
                    "date required (?date=YYYY-MM-DD)", NULL);
    if (part < 0) {
        char d[64];
        snprintf(d, sizeof d, "{\"part\":%ld}", part);
        return fail(w, err, 400, PV2_CODE_BAD_REQUEST, "part must be >= 0", d);
    }
    if (limit > 3) limit = 3;                    /* schema maximum, silent clamp */

    if (!c->store.p_list || !c->store.p_get_payload)
        return fail(w, err, 500, PV2_CODE_STORAGE, "store has no list support", NULL);

    /* pass 1: parts_total = number of live paragraph records for this date */
    for (;;) {
        int got = c->store.p_list(c->store.h, PV2_ENT_BRIEFING, scanned, infos,
                                  (int)(sizeof infos / sizeof infos[0]));
        int k;
        if (got <= 0) break;
        for (k = 0; k < got; k++)
            if (!infos[k].deleted && strstr(infos[k].id, date)) parts_total++;
        scanned += got;
    }
    if (parts_total == 0) {
        char d[224];
        detail_str_kv(d, sizeof d, "date", date);
        return fail(w, err, 404, PV2_CODE_NOT_FOUND, "no briefing for date", d);
    }

    pv2_jw_raw(w, "{\"ok\":true,\"date\":");
    pv2_jw_str(w, date);
    pv2_jw_raw(w, ",\"part\":");
    pv2_jw_i64(w, part);
    pv2_jw_raw(w, ",\"parts_total\":");
    pv2_jw_i64(w, parts_total);
    pv2_jw_raw(w, ",\"paragraphs\":[");

    /* pass 2: skip the first `part` matches, emit up to `limit` paragraphs
     * while they fit the page byte budget (D1 branch, defense in depth —
     * legal R4-bounded paragraphs can never reach it with limit<=3). */
    scanned = 0;
    for (;;) {
        int got = c->store.p_list(c->store.h, PV2_ENT_BRIEFING, scanned, infos,
                                  (int)(sizeof infos / sizeof infos[0]));
        int k, stop = 0;
        if (got <= 0) break;
        for (k = 0; k < got; k++) {
            char payload[PV2_MAX_REQ_JSON];
            size_t plen = 0;
            long rev = 0;
            int del = 0;
            pv2_jw m;
            if (infos[k].deleted || !strstr(infos[k].id, date)) continue;
            if (matched < part) { matched++; continue; }
            if (emitted >= limit) break;         /* more matches exist below */
            memset(payload, 0, sizeof payload);
            if (c->store.p_get_payload(c->store.h, PV2_ENT_BRIEFING, infos[k].id,
                                       payload, sizeof payload, &plen, &rev, &del) != 1)
                continue;
            if (!json_get_str(payload, plen, "p", para, sizeof para)) continue;
            pv2_jw_init(&m, scratch, sizeof scratch);
            pv2_jw_str(&m, para);                /* re-encoded JSON string */
            if (m.overflow) { trunc_bytes = 1; stop = 1; break; }
            if (w->len + m.len + 8 > PAGE_BUDGET) { trunc_bytes = 1; stop = 1; break; }
            if (emitted) pv2_jw_ch(w, ',');
            pv2_jw_raw(w, m.buf);
            emitted++;
        }
        scanned += got;
        if (stop) break;
        if (emitted >= limit) break;
    }

    pv2_jw_raw(w, "],\"has_more\":");
    pv2_jw_raw(w, (part + emitted < parts_total) ? "true" : "false");
    pv2_jw_raw(w, ",\"next_part\":");
    if (part + emitted < parts_total) pv2_jw_i64(w, part + emitted);
    else pv2_jw_raw(w, "null");
    pv2_jw_raw(w, ",\"truncated_by_bytes\":");
    pv2_jw_raw(w, trunc_bytes ? "true" : "false");
    pv2_jw_raw(w, "}");
    set_err(err, 200, "", "");
    return (long)w->len;
}

static long r_settings_get(pv2_ctx *c, pv2_jw *w)
{
    char *keys[128];
    int n = pv2_kv_list(c->aux, "set:", keys, 128), i;
    pv2_jw_raw(w, "{\"ok\":true,\"count\":");
    pv2_jw_i64(w, n);
    pv2_jw_raw(w, ",\"settings\":{");
    for (i = 0; i < n; i++) {
        const char *v = pv2_kv_get(c->aux, keys[i]);
        if (i) pv2_jw_ch(w, ',');
        pv2_jw_str(w, keys[i] + 4);
        pv2_jw_ch(w, ':');
        pv2_jw_str(w, v ? v : "");
    }
    pv2_jw_raw(w, "}}");
    return (long)w->len;
}

static long r_settings_post(pv2_ctx *c, const pv2_request *req, pv2_jw *w, pv2_api_err *err)
{
    char hit[32];
    const char *p;
    size_t blen;
    int count = 0, kvbad = 0;
    if (!req->body) return fail(w, err, 400, PV2_CODE_BAD_REQUEST, "body required", NULL);
    blen = req->body_len;
    if (body_has_secret_key(req->body, blen, hit, sizeof hit)) {
        char d[96];
        detail_str_kv(d, sizeof d, "key", hit);
        return fail(w, err, 400, PV2_CODE_SECRET_IN_BODY,
                    "secrets are not accepted through ordinary settings", d);
    }
    pv2_jw_raw(w, "{\"ok\":true,\"applied\":[");
    p = req->body;
    for (;;) {
        const char *k1 = strchr(p, '"');
        const char *k2, *v1, *v2;
        char key[65], val[513];
        size_t kl, vl;
        if (!k1 || !(k2 = strchr(k1 + 1, '"'))) break;
        kl = (size_t)(k2 - k1 - 1);
        if (kl == 0 || kl > 64) { p = k2 + 1; continue; }
        memcpy(key, k1 + 1, kl); key[kl] = '\0';
        v1 = strchr(k2, ':');
        if (!v1) break;
        v1++;
        while (*v1 && is_ws(*v1)) v1++;
        if (*v1 != '"') { p = v1; continue; }
        v2 = strchr(v1 + 1, '"');
        if (!v2) break;
        vl = (size_t)(v2 - v1 - 1);
        if (vl > 512) vl = 512;
        memcpy(val, v1 + 1, vl); val[vl] = '\0';
        if (count) pv2_jw_ch(w, ',');
        pv2_jw_str(w, key);
        {
            char kk[80];
            snprintf(kk, sizeof kk, "set:%s", key);
            if (pv2_kv_set(c->aux, kk, val) != 0) kvbad = 1;   /* T04C */
        }
        count++;
        p = v2 + 1;
    }
    if (kvbad)
        return fail(w, err, 500, PV2_CODE_STORAGE,
                    "setting could not be persisted", NULL);
    pv2_jw_raw(w, "],\"count\":");
    pv2_jw_i64(w, count);
    pv2_jw_raw(w, "}");
    set_err(err, 200, "", "");
    return (long)w->len;
}

static int job_split(const char *rec, char *state, size_t sn, char *kind, size_t kn, char *ref, size_t rn,
                     long long *created, char *task_key, size_t tn);

/* M6-B2: capture a balanced JSON object value for `key` in body (verbatim span,
 * no re-serialization). Handles strings/escapes/nesting. Returns 1 on hit. */
static int json_get_obj(const char *body, size_t blen, const char *key,
                        const char **out, size_t *outlen)
{
    size_t klen = strlen(key), i = 0;
    while (i + klen + 2 < blen) {
        const char *k = (const char *)memchr(body + i, '"', blen - i);
        const char *p, *start = NULL;
        int depth;
        size_t j;
        if (!k) return 0;
        j = 1;
        while (k + j < body + blen && k[j] != '"') {
            if (k[j] == '\\') j++;                 /* skip escaped char */
            j++;
        }
        if (k + j >= body + blen) return 0;
        if (j == klen + 1 && memcmp(k + 1, key, klen) == 0) {   /* exact "key" */
            p = k + j + 1;
            while (p < body + (long)blen && (*p == ' ' || *p == ':' || *p == '\t' ||
                                             *p == '\r' || *p == '\n'))
                p++;
            if (p >= body + blen || *p != '{') { i = (size_t)(k - body) + 1; continue; }
            start = p;
            depth = 0;
            for (; p < body + blen; p++) {
                if (*p == '"') {                    /* skip string literal */
                    p++;
                    while (p < body + blen && *p != '"') {
                        if (*p == '\\' && p + 1 < body + blen) p++;
                        p++;
                    }
                    continue;
                }
                if (*p == '{') depth++;
                else if (*p == '}' && --depth == 0) {
                    *out = start;
                    *outlen = (size_t)(p - start) + 1;
                    return 1;
                }
            }
            return 0;                               /* unbalanced: reject */
        }
        i = (size_t)(k - body) + 1;
    }
    return 0;
}

/* M6-B2: queue-dir shim (api layer previously had no filesystem writes). */
static void job_mkdir1(const char *path)
{
#ifdef _WIN32
    mkdir(path);                                   /* mingw CRT: 1 arg */
#else
    mkdir(path, 0700);
#endif
}

/* M6-B2: 原子替换（zig mingw rename() 目标存在即 EEXIST，走 MoveFileExW）。 */
#ifdef _WIN32
#include <windows.h>
static int job_atomic_replace(const char *tmp, const char *path)
{
    wchar_t wtmp[1024], wpath[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, tmp, -1, wtmp, 1024) == 0) return -1;
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024) == 0) return -1;
    return MoveFileExW(wtmp, wpath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
               ? 0 : -1;
}
#else
static int job_atomic_replace(const char *tmp, const char *path)
{
    return rename(tmp, path);
}
#endif

static long r_job_create(pv2_ctx *c, const pv2_request *req, pv2_jw *w, pv2_api_err *err)
{
    char kind[32] = "", ref[96] = "", jid[24];
    char key[64], rec[512];
    char task_key[128] = "", tkey_key[192], prev_jid[24];
    /* M6-B2: 可选执行载荷——payload 为 body 内原样平衡对象 span（不重序列化）；
     * executor 默认 "phone"（ai_tick 自提任务恒写 "board"，互不影响）。 */
    char input_hash[128] = "", executor[24] = "phone";
    const char *payload_span = NULL;
    size_t payload_len = 0;
    const char *prev_rec;
    char prev_state[32];
    if (!req->body) return fail(w, err, 400, PV2_CODE_BAD_REQUEST, "body required", NULL);
    json_get_str(req->body, req->body_len, "kind", kind, sizeof kind);
    json_get_str(req->body, req->body_len, "ref", ref, sizeof ref);
    json_get_str(req->body, req->body_len, "task_key", task_key, sizeof task_key);
    json_get_str(req->body, req->body_len, "input_hash", input_hash, sizeof input_hash);
    json_get_str(req->body, req->body_len, "executor", executor, sizeof executor);
    if (!executor[0]) snprintf(executor, sizeof executor, "phone");
    json_get_obj(req->body, req->body_len, "payload", &payload_span, &payload_len);
    if (!kind[0])
        return fail(w, err, 400, PV2_CODE_BAD_REQUEST, "kind required", NULL);
    /* M6-B1: 同 task_key 且任务未终态 → 幂等返回已有 job（Marves L113「板卡已
     * 接单的同 task_key 请求幂等返回已有任务」）；终态后的同 key 请求新建新
     * job（无双跑：旧任务已结束），映射指向新 job。 */
    if (task_key[0]) {
        snprintf(tkey_key, sizeof tkey_key, "tkey:%s", task_key);
        prev_rec = NULL;
        prev_jid[0] = '\0';
        {
            const char *mapped = pv2_kv_get(c->aux, tkey_key);
            if (mapped && *mapped) {
                char pk[64];
                snprintf(pk, sizeof pk, "job:%s", mapped);
                prev_rec = pv2_kv_get(c->aux, pk);
                if (prev_rec) snprintf(prev_jid, sizeof prev_jid, "%s", mapped);
            }
        }
        if (prev_jid[0] && prev_rec && job_split(prev_rec, prev_state, sizeof prev_state,
                                                 kind, sizeof kind, ref, sizeof ref, NULL,
                                                 NULL, 0)) {
            if (!strcmp(prev_state, "queued") || !strcmp(prev_state, "running")) {
                pv2_jw_raw(w, "{\"ok\":true,\"deduplicated\":true,\"job_id\":");
                pv2_jw_str(w, prev_jid);
                pv2_jw_raw(w, ",\"state\":");
                pv2_jw_str(w, prev_state);
                pv2_jw_raw(w, ",\"kind\":");
                pv2_jw_str(w, kind);
                pv2_jw_raw(w, ",\"ref\":");
                pv2_jw_str(w, ref);
                pv2_jw_raw(w, ",\"task_key\":");
                pv2_jw_str(w, task_key);
                pv2_jw_raw(w, ",\"queued_total\":");
                pv2_jw_i64(w, pv2_kv_count_prefix(c->aux, "job:"));
                pv2_jw_raw(w, "}");
                set_err(err, 200, "", "");
                return (long)w->len;
            }
        }
    }
    if (pv2_kv_count_prefix(c->aux, "job:") >= (int)PV2_AI_QUEUE_MAX)
        return fail(w, err, 429, PV2_CODE_QUEUE_FULL, "ai job queue is full", NULL);
    if (pv2_random_hex(jid, 8) != 0)           /* T04B: no randomness -> no job id */
        return fail(w, err, 500, PV2_CODE_STORAGE,
                    "secure random source unavailable", NULL);
    snprintf(key, sizeof key, "job:%s", jid);
    /* M6-B1: rec 扩为 6 字段 state|kind|ref|created|updated|<task_key 原文>；
     * task_key 可含 '|'，恒居末段。 */
    snprintf(rec, sizeof rec, "queued|%s|%s|%lld|%lld|%s", kind, ref,
             pv2_wall_seconds(), pv2_wall_seconds(), task_key);
    if (pv2_kv_set(c->aux, key, rec) != 0)     /* T04C: no fake "queued" */
        return fail(w, err, 500, PV2_CODE_STORAGE,
                    "job could not be persisted", NULL);
    if (task_key[0]) {
        snprintf(tkey_key, sizeof tkey_key, "tkey:%s", task_key);
        if (pv2_kv_set(c->aux, tkey_key, jid) != 0) {   /* T04C: no fake dedupe map */
            pv2_kv_del(c->aux, key);
            return fail(w, err, 500, PV2_CODE_STORAGE,
                        "task_key mapping could not be persisted", NULL);
        }
    }
    if (payload_span) {
        /* M6-B2 桥：把任务落成 aiworker 可见的队列文件 <dir>/ai/queue/<jid>.job.json
         * （此前 API 创建的任务只进 aux rec，worker 永远看不到）。schema 与 ai_tick
         * 提交的任务文件一致；手机提交不带 base_rev → worker 按缺省 0 做乐观并发，
         * conflict 走 store 双副本语义（L121/M4A）。文件失败回滚 rec+tkey（T04C：
         * 不假报 queued）。无 payload → legacy 仅记录，不写文件。 */
        char qdir[600], qparent[600], qpath[640], qtmp[660], head[768], tail[96];
        pv2_jw t;
        FILE *f;
        size_t hn, tn;
        int ok;
        snprintf(qdir, sizeof qdir, "%s/ai/queue", c->dir);
        snprintf(qparent, sizeof qparent, "%s/ai", c->dir);
        job_mkdir1(qparent);                       /* EEXIST 忽略 */
        job_mkdir1(qdir);
        snprintf(qpath, sizeof qpath, "%s/%s%s", qdir, jid, ".job.json");
        snprintf(qtmp, sizeof qtmp, "%s.tmp", qpath);
        pv2_jw_init(&t, head, sizeof head);
        pv2_jw_raw(&t, "{\"job_id\":");
        pv2_jw_str(&t, jid);
        pv2_jw_raw(&t, ",\"task_key\":");
        pv2_jw_str(&t, task_key);
        pv2_jw_raw(&t, ",\"type\":");
        pv2_jw_str(&t, kind);
        pv2_jw_raw(&t, ",\"executor\":");
        pv2_jw_str(&t, executor);
        pv2_jw_raw(&t, ",\"input_hash\":");
        pv2_jw_str(&t, input_hash);
        pv2_jw_raw(&t, ",\"payload\":");
        hn = t.len;
        tn = (size_t)snprintf(tail, sizeof tail,
                              ",\"status\":\"queued\",\"attempts\":0,"
                              "\"next_retry_ms\":0,\"created_ms\":%lld}",
                              (long long)time(NULL) * 1000LL);
        if (t.overflow || tn == 0 || tn >= sizeof tail)
            goto file_rollback;
        f = fopen(qtmp, "wb");                     /* 二进制写：无换行翻译 */
        if (!f) goto file_rollback;
        ok = fwrite(head, 1, hn, f) == hn &&
             (payload_len == 0 || fwrite(payload_span, 1, payload_len, f) == payload_len) &&
             fwrite(tail, 1, tn, f) == tn && fflush(f) == 0;
        if (fclose(f) != 0) ok = 0;
        if (!ok || job_atomic_replace(qtmp, qpath) != 0) {
            remove(qtmp);
            goto file_rollback;
        }
        goto file_done;
file_rollback:
        pv2_kv_del(c->aux, key);
        if (task_key[0]) pv2_kv_del(c->aux, tkey_key);
        return fail(w, err, 500, PV2_CODE_STORAGE,
                    "job file could not be written", NULL);
file_done:
        ;
    }
    pv2_jw_raw(w, "{\"ok\":true,\"job_id\":");
    pv2_jw_str(w, jid);
    pv2_jw_raw(w, ",\"state\":\"queued\",\"kind\":");
    pv2_jw_str(w, kind);
    pv2_jw_raw(w, ",\"ref\":");
    pv2_jw_str(w, ref);
    pv2_jw_raw(w, ",\"task_key\":");
    pv2_jw_str(w, task_key);
    pv2_jw_raw(w, ",\"queued_total\":");
    pv2_jw_i64(w, pv2_kv_count_prefix(c->aux, "job:"));
    pv2_jw_raw(w, "}");
    set_err(err, 200, "", "");
    return (long)w->len;
}

/* M6-B1: rec = state|kind|ref|created|updated|<task_key 原文>。task_key 可含
 * '|'，恒居末段：前 4 段按 '|' 切，第 5 段 updated 丢弃，第 6 段取余下整体。
 * 旧 5 字段记录（无 task_key）→ task_key 空串，兼容。 */
static int job_split(const char *rec, char *state, size_t sn, char *kind, size_t kn, char *ref, size_t rn,
                     long long *created, char *task_key, size_t tn)
{
    char buf[512];
    char *p1, *p2, *p3, *p4, *p5;
    snprintf(buf, sizeof buf, "%s", rec);
    p1 = strchr(buf, '|'); if (!p1) return 0; *p1++ = '\0';
    p2 = strchr(p1, '|');  if (!p2) return 0; *p2++ = '\0';
    p3 = strchr(p2, '|');  if (!p3) return 0; *p3++ = '\0';
    p4 = strchr(p3, '|');
    if (p4) {
        *p4++ = '\0';
        p5 = strchr(p4, '|');
        if (p5 && task_key) snprintf(task_key, tn, "%s", p5 + 1);
        else if (task_key) task_key[0] = '\0';
    } else if (task_key) {
        task_key[0] = '\0';
    }
    snprintf(state, sn, "%s", buf);
    snprintf(kind, kn, "%s", p1);
    snprintf(ref, rn, "%s", p2);
    if (created) *created = strtoll(p3, NULL, 10);
    return 1;
}

static long r_job_get(pv2_ctx *c, const pv2_request *req, pv2_jw *w, pv2_api_err *err)
{
    char key[64], state[32] = "", kind[32] = "", ref[96] = "", task_key[128] = "";
    const char *rec;
    long long created = 0;
    if (!req->job_id || !*req->job_id)
        return fail(w, err, 400, PV2_CODE_BAD_REQUEST, "job_id required", NULL);
    snprintf(key, sizeof key, "job:%s", req->job_id);
    rec = pv2_kv_get(c->aux, key);
    if (!rec) {
        char d[96];
        detail_str_kv(d, sizeof d, "job_id", req->job_id);
        return fail(w, err, 404, PV2_CODE_NOT_FOUND, "unknown job_id", d);
    }
    job_split(rec, state, sizeof state, kind, sizeof kind, ref, sizeof ref, &created,
              task_key, sizeof task_key);
    pv2_jw_raw(w, "{\"ok\":true,\"job_id\":");
    pv2_jw_str(w, req->job_id);
    pv2_jw_raw(w, ",\"state\":");
    pv2_jw_str(w, state);
    pv2_jw_raw(w, ",\"kind\":");
    pv2_jw_str(w, kind);
    pv2_jw_raw(w, ",\"ref\":");
    pv2_jw_str(w, ref);
    pv2_jw_raw(w, ",\"task_key\":");
    pv2_jw_str(w, task_key);
    pv2_jw_raw(w, ",\"created_at\":");
    pv2_jw_i64(w, created);
    pv2_jw_raw(w, "}");
    set_err(err, 200, "", "");
    return (long)w->len;
}

static long r_job_cancel(pv2_ctx *c, const pv2_request *req, pv2_jw *w, pv2_api_err *err)
{
    char key[64], state[32] = "", kind[32] = "", ref[96] = "", rec2[512], task_key[128] = "";
    const char *rec;
    long long created = 0;
    if (!req->job_id || !*req->job_id)
        return fail(w, err, 400, PV2_CODE_BAD_REQUEST, "job_id required", NULL);
    snprintf(key, sizeof key, "job:%s", req->job_id);
    rec = pv2_kv_get(c->aux, key);
    if (!rec) {
        char d[96];
        detail_str_kv(d, sizeof d, "job_id", req->job_id);
        return fail(w, err, 404, PV2_CODE_NOT_FOUND, "unknown job_id", d);
    }
    job_split(rec, state, sizeof state, kind, sizeof kind, ref, sizeof ref, &created,
              task_key, sizeof task_key);
    if (strcmp(state, "queued") != 0 && strcmp(state, "running") != 0) {
        char d[96];
        detail_str_kv(d, sizeof d, "state", state);
        return fail(w, err, 409, PV2_CODE_CONFLICT, "job already finished", d);
    }
    snprintf(rec2, sizeof rec2, "canceled|%s|%s|%lld|%lld|%s", kind, ref, created,
             pv2_wall_seconds(), task_key);
    if (pv2_kv_set(c->aux, key, rec2) != 0)    /* T04C: no fake "canceled" */
        return fail(w, err, 500, PV2_CODE_STORAGE,
                    "cancel could not be persisted", NULL);
    pv2_jw_raw(w, "{\"ok\":true,\"job_id\":");
    pv2_jw_str(w, req->job_id);
    pv2_jw_raw(w, ",\"state\":\"canceled\"}");
    set_err(err, 200, "", "");
    return (long)w->len;
}

/* T04C: POST /auth/token/revoke (openapi /auth/token/revoke). Body
 * {"token_id":"..."} — empty means "revoke my own token". The prototype has
 * no separate id space, so token_id IS the 64-hex token. A revoke that
 * cannot be persisted must never report success (SEC-05/06). */
static long r_token_revoke(pv2_ctx *c, const pv2_request *req, pv2_jw *w, pv2_api_err *err)
{
    char token_id[80] = "", tok[80] = "";
    const char *h;
    int rc;
    json_get_str(req->body, req->body_len, "token_id", token_id, sizeof token_id);
    if (!token_id[0]) {
        /* fall back to the caller's own bearer token */
        h = req->auth_header;
        if (h && (!strncmp(h, "Bearer ", 7) || !strncmp(h, "bearer ", 7)))
            snprintf(tok, sizeof tok, "%s", h + 7);
    } else {
        snprintf(tok, sizeof tok, "%s", token_id);
    }
    rc = pv2_auth_revoke(c, tok, err);
    if (rc < 0) {
        char msg[192];
        snprintf(msg, sizeof msg, "%s", err ? err->message : "revoke not persisted");
        return fail(w, err, 500, PV2_CODE_STORAGE, msg, NULL);
    }
    if (rc == 0) {
        char d[96];
        detail_str_kv(d, sizeof d, "token_id", token_id[0] ? token_id : "<self>");
        return fail(w, err, 404, PV2_CODE_NOT_FOUND, "unknown token_id", d);
    }
    pv2_jw_raw(w, "{\"ok\":true,\"revoked\":true}");
    set_err(err, 200, "", "");
    return (long)w->len;
}

long pv2_dispatch(pv2_ctx *c, const pv2_request *req, char *out, size_t out_cap,
                  pv2_api_err *err)
{
    pv2_jw w;
    char cid[64] = "";
    long rc;
    if (!c || !req || !out) return -1;
    pv2_jw_init(&w, out, out_cap);

    /* request-level size gate (CHK-C01 companion at the API layer): the HTTP
     * reader already refuses >4096 B; this re-check keeps the same 413 shape
     * when dispatch is fed a pre-parsed body. */
    if (req->body && req->body_len > (long)PV2_MAX_REQ_JSON) {
        char d[96];
        snprintf(d, sizeof d, "{\"bytes\":%ld,\"bytes_max\":%u}",
                 (long)req->body_len, (unsigned)PV2_MAX_REQ_JSON);
        return fail(&w, err, 413, PV2_CODE_TOO_LARGE,
                    "request body exceeds 4096 B limit", d);
    }

    /* auth: capabilities is public, pair carries the PIN in the body */
    if (req->route != PV2_ROUTE_CAPABILITIES && req->route != PV2_ROUTE_PAIR) {
        if (pv2_auth_check(c, req->uri, req->auth_header, cid, sizeof cid, err) != 0) {
            char msg[192];
            snprintf(msg, sizeof msg, "%s", err ? err->message : "unauthorized");
            return fail(&w, err, err ? err->http : 401,
                        err ? err->code : PV2_CODE_UNAUTHORIZED, msg, NULL);
        }
    } else {
        set_err(err, 200, "", "");
    }

    switch (req->route) {
    case PV2_ROUTE_CAPABILITIES: rc = r_capabilities(c, &w); break;
    case PV2_ROUTE_PAIR:         rc = r_pair(c, req, &w, err); break;
    case PV2_ROUTE_SYNC_STATUS:  rc = r_sync_status(c, &w); break;
    case PV2_ROUTE_SYNC_PUSH:    rc = r_sync_push(c, req, &w, err); break;
    case PV2_ROUTE_SYNC_PULL:    rc = r_sync_pull(c, req, &w, err); break;
    case PV2_ROUTE_WORDS:        rc = r_list_entity(c, PV2_ENT_WORD, req, &w, err, NULL, NULL); break;
    case PV2_ROUTE_DAILY_PLAN:   rc = r_list_entity(c, PV2_ENT_PLAN, req, &w, err, "date",
                                                    req->date ? req->date : ""); break;
    case PV2_ROUTE_BRIEFINGS:    rc = r_briefing_page(c, req, &w, err); break;
    case PV2_ROUTE_AI_JOB_CREATE: rc = r_job_create(c, req, &w, err); break;
    case PV2_ROUTE_AI_JOB_GET:   rc = r_job_get(c, req, &w, err); break;
    case PV2_ROUTE_AI_JOB_CANCEL: rc = r_job_cancel(c, req, &w, err); break;
    case PV2_ROUTE_SETTINGS_GET:  rc = r_settings_get(c, &w); break;
    case PV2_ROUTE_SETTINGS_POST: rc = r_settings_post(c, req, &w, err); break;
    case PV2_ROUTE_TOKEN_REVOKE:  rc = r_token_revoke(c, req, &w, err); break;
    default:
    {
        char d[320];
        detail_str_kv(d, sizeof d, "uri", req->uri);
        rc = fail(&w, err, 404, PV2_CODE_NOT_FOUND, "unknown route", d);
        break;
    }
    }
    if (rc < 0) return -1;
    if (w.overflow) return -1;
    if (w.len > PV2_MAX_RESP_JSON) return -1;   /* R2: never exceed 32 KiB */
    return (long)w.len;
}
