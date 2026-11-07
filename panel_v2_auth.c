/* panel_v2_auth.c - M2 auth (sec.7).
 *
 *   * pairing with the control PIN (constant-time compare, rate limited)
 *   * revocable random bearer tokens (32 random bytes -> 64 hex chars)
 *   * tokens are only accepted from the Authorization header; a token that
 *     shows up anywhere in the request target is rejected outright so it can
 *     never land in a web server log or a browser history
 *   * the token table lives in the aux KV file at <dir>/auth/pv2aux.kv which
 *     is written 0600 (see pv2_kv_flush) outside the web root
 */
#include "panel_v2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PV2_PAIR_MAX_ATTEMPTS 5
#define PV2_PAIR_WINDOW_SEC   60

static void set_err(pv2_api_err *err, int http, const char *code, const char *msg)
{
    if (!err) return;
    err->http = http;
    err->code = code;
    snprintf(err->message, sizeof err->message, "%s", msg ? msg : "");
}

static void ok_err(pv2_api_err *err)
{
    set_err(err, 200, "", "");
}

/* token record: client_id|client_name|issued_at|last_seen */
static int token_load(pv2_ctx *c, const char *token, char *cid, size_t cidn,
                      char *name, size_t namen, long long *issued, long long *seen)
{
    char key[96];
    const char *v;
    char buf[256];
    char *p1, *p2, *p3;
    if (!c || !c->aux || !token || !*token) return 0;
    if (strlen(token) != 64) return 0;
    snprintf(key, sizeof key, "tok:%s", token);
    v = pv2_kv_get(c->aux, key);
    if (!v) return 0;
    snprintf(buf, sizeof buf, "%s", v);
    p1 = strchr(buf, '|');
    if (!p1) return 0;
    *p1++ = '\0';
    p2 = strchr(p1, '|');
    if (!p2) return 0;
    *p2++ = '\0';
    p3 = strchr(p2, '|');
    if (!p3) return 0;
    *p3++ = '\0';
    if (cid && cidn) snprintf(cid, cidn, "%s", buf);
    if (name && namen) snprintf(name, namen, "%s", p1);
    if (issued) *issued = strtoll(p2, NULL, 10);
    if (seen) *seen = strtoll(p3, NULL, 10);
    return 1;
}

/* rate limit shared by successful and failed pairing attempts.
 * T04C (F06): the counter is security state — if it cannot be persisted the
 * attempt is refused (500) instead of silently under-counting. Returns
 * 1 = limit hit, 0 = ok, -1 = counter write failed (err set). */
static int pair_rate_hit(pv2_ctx *c, long long now, pv2_api_err *err)
{
    const char *v = pv2_kv_get(c->aux, "rl:pair");
    long long win = 0;
    int count = 0;
    if (v) sscanf(v, "%lld|%d", &win, &count);
    if (now - win >= PV2_PAIR_WINDOW_SEC) { win = now; count = 0; }
    count++;
    {
        char buf[64];
        snprintf(buf, sizeof buf, "%lld|%d", win, count);
        if (pv2_kv_set(c->aux, "rl:pair", buf) != 0) {
            set_err(err, 500, PV2_CODE_STORAGE,
                    "pairing limiter could not be persisted");
            return -1;
        }
    }
    return count > PV2_PAIR_MAX_ATTEMPTS;
}

int pv2_auth_pair(pv2_ctx *c, const char *pin, const char *client_id,
                  const char *client_name, char *token_out, size_t tok_n,
                  pv2_api_err *err)
{
    char token[80];
    char key[96];
    char rec[256];
    char cid[64];
    long long now;
    size_t i;

    if (!c || !c->aux) { set_err(err, 500, PV2_CODE_STORAGE, "no context"); return -1; }
    now = pv2_wall_seconds();

    /* M9A: auth_enabled=0 → pairing open（任意非空 PIN 皆发 token），跳过
     * 空-PIN 拒绝与 PIN 比对；rate limit 与 client_id 校验保持不变。 */
    if (c->auth_enabled && !c->pin[0]) {
        set_err(err, 403, PV2_CODE_FORBIDDEN, "pairing disabled: no control pin configured");
        return -1;
    }
    if (!client_id || !*client_id || strlen(client_id) > 31) {
        set_err(err, 400, PV2_CODE_BAD_REQUEST, "client_id required (1..31 chars)");
        return -1;
    }
    for (i = 0; client_id[i]; i++) {
        char ch = client_id[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.')) {
            set_err(err, 400, PV2_CODE_BAD_REQUEST, "client_id must be ascii [A-Za-z0-9._-]");
            return -1;
        }
    }
    {
        int rl = pair_rate_hit(c, now, err);
        if (rl < 0) return -1;
        if (rl > 0) {
            set_err(err, 429, PV2_CODE_RATE_LIMITED, "too many pairing attempts, retry in 60s");
            return -1;
        }
    }
    if (c->auth_enabled && (!pin || !pv2_const_time_eq(pin, c->pin))) {
        set_err(err, 401, PV2_CODE_UNAUTHORIZED, "bad control pin");
        return -1;
    }

    /* T04B (F05/SEC-07): no OS randomness -> no token, no side effects. */
    if (pv2_random_hex(token, 32) != 0) {      /* 64 hex chars */
        set_err(err, 500, PV2_CODE_STORAGE,
                "secure random source unavailable; token not issued");
        return -1;
    }
    snprintf(cid, sizeof cid, "%s", client_id);
    snprintf(rec, sizeof rec, "%s|%s|%lld|%lld", cid, client_name ? client_name : "", now, now);
    snprintf(key, sizeof key, "tok:%s", token);
    if (pv2_kv_set(c->aux, key, rec) != 0) {
        set_err(err, 500, PV2_CODE_STORAGE, "token store write failed");
        return -1;
    }
    if (token_out && tok_n) snprintf(token_out, tok_n, "%s", token);
    ok_err(err);
    return 0;
}

int pv2_auth_check(pv2_ctx *c, const char *uri, const char *auth_header,
                   char *client_id, size_t cid_n, pv2_api_err *err)
{
    const char *h;
    char token[96];
    char cid[64];
    char cname[64];
    char key[96];
    char rec[256];
    long long issued = 0, seen = 0;
    char *keys[128];
    int n, i;

    if (!c || !c->aux) { set_err(err, 500, PV2_CODE_STORAGE, "no context"); return -1; }

    /* M9A: auth_enabled=0 → 请求免 token 放行（显式配置才生效）。 */
    if (!c->auth_enabled) {
        if (client_id && cid_n) snprintf(client_id, cid_n, "%s", "anonymous");
        ok_err(err);
        return 0;
    }

    /* 1. token must never travel in the request target */
    if (uri && (strstr(uri, "token=") || strstr(uri, "access_token=") || strstr(uri, "tok="))) {
        set_err(err, 400, PV2_CODE_TOKEN_IN_URL,
                "token must be sent in the Authorization header, not in the url");
        return -1;
    }
    if (uri) {
        n = pv2_kv_list(c->aux, "tok:", keys, 128);
        for (i = 0; i < n; i++) {
            const char *t = keys[i] + 4;
            if (pv2_uri_contains_token(uri, t)) {
                set_err(err, 400, PV2_CODE_TOKEN_IN_URL,
                        "token must be sent in the Authorization header, not in the url");
                return -1;
            }
        }
    }

    /* 2. bearer token from the header only */
    h = auth_header;
    if (!h) { set_err(err, 401, PV2_CODE_UNAUTHORIZED, "authorization header required"); return -1; }
    while (*h == ' ') h++;
    if (strncmp(h, "Bearer ", 7) != 0 && strncmp(h, "bearer ", 7) != 0) {
        set_err(err, 401, PV2_CODE_UNAUTHORIZED, "expected 'Authorization: Bearer <token>'");
        return -1;
    }
    h += 7;
    while (*h == ' ') h++;
    snprintf(token, sizeof token, "%s", h);
    {
        size_t tl = strlen(token);
        while (tl > 0 && (token[tl - 1] == ' ' || token[tl - 1] == '\r' || token[tl - 1] == '\n'))
            token[--tl] = '\0';
    }
    if (!token_load(c, token, cid, sizeof cid, cname, sizeof cname, &issued, &seen)) {
        set_err(err, 401, PV2_CODE_UNAUTHORIZED, "unknown or revoked token");
        return -1;
    }
    /* refresh last_seen (issued_at and client_name preserved) — throttled to
     * one rewrite per token per 60 s (T04C: ordinary reads must not rewrite
     * the whole token library every time). last_seen is advisory metadata:
     * a failed refresh never affects the auth decision, so the rc is
     * deliberately not surfaced here; security state (token add/del) is
     * always error-checked. */
    if ((long long)pv2_wall_seconds() - seen >= 60) {
        snprintf(rec, sizeof rec, "%s|%s|%lld|%lld", cid, cname, issued, pv2_wall_seconds());
        snprintf(key, sizeof key, "tok:%s", token);
        (void)pv2_kv_set(c->aux, key, rec);
    }
    if (client_id && cid_n) snprintf(client_id, cid_n, "%s", cid);
    ok_err(err);
    return 0;
}

/* T04C: returns 1 = revoked and persisted, 0 = unknown token, -1 = the
 * deletion could not be persisted (err set to 500 storage_failure) — the
 * caller must NOT report success. */
int pv2_auth_revoke(pv2_ctx *c, const char *token, pv2_api_err *err)
{
    char key[96];
    int rc;
    if (!c || !c->aux) { set_err(err, 500, PV2_CODE_STORAGE, "no context"); return -1; }
    if (!token || strlen(token) != 64) return 0;
    snprintf(key, sizeof key, "tok:%s", token);
    rc = pv2_kv_del(c->aux, key);
    if (rc < 0)
        set_err(err, 500, PV2_CODE_STORAGE, "revoke could not be persisted");
    else
        ok_err(err);
    return rc;
}

int pv2_auth_token_count(pv2_ctx *c)
{
    if (!c || !c->aux) return 0;
    return pv2_kv_count_prefix(c->aux, "tok:");
}
