/* panel_v2_json.c - M2 bounded JSON writer + route table (sec.5).
 *
 * The writer never allocates and never writes past the caller's buffer: any
 * overflow sets w->overflow and freezes the output, so a bounded buffer can be
 * used to *measure* the encoded size of a record before committing it to the
 * response. UTF-8 input is validated; malformed bytes become '?' so the
 * emitted document is always well-formed UTF-8 JSON.
 */
#include "panel_v2.h"

#include <stdio.h>
#include <string.h>

static const char *HEXD = "0123456789abcdef";

static void jw_need(pv2_jw *w, size_t n)
{
    if (w->overflow) return;
    if (w->cap == 0 || w->len + n + 1 > w->cap) { w->overflow = 1; return; }
}

static void jw_put(pv2_jw *w, const char *s, size_t n)
{
    jw_need(w, n);
    if (w->overflow) return;
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = '\0';
}

void pv2_jw_init(pv2_jw *w, char *buf, size_t cap)
{
    if (!w) return;
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->overflow = 0;
    if (buf && cap) buf[0] = '\0';
}

void pv2_jw_raw(pv2_jw *w, const char *s)
{
    if (!w || !s) return;
    jw_put(w, s, strlen(s));
}

void pv2_jw_ch(pv2_jw *w, char c)
{
    if (!w) return;
    jw_need(w, 1);
    if (w->overflow) return;
    w->buf[w->len++] = c;
    w->buf[w->len] = '\0';
}

/* UTF-8 sequence length at p (avail bytes available); 0 = malformed */
static size_t u8len(const unsigned char *p, size_t avail)
{
    unsigned char c = p[0];
    size_t n, i;
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0)      n = 2;
    else if ((c & 0xF0) == 0xE0) n = 3;
    else if ((c & 0xF8) == 0xF0) n = 4;
    else return 0;
    if (n > avail) return 0;
    for (i = 1; i < n; i++) if ((p[i] & 0xC0) != 0x80) return 0;
    if (n == 2 && c < 0xC2) return 0;                       /* overlong */
    if (n == 3) {
        if (c == 0xE0 && p[1] < 0xA0) return 0;             /* overlong */
        if (c == 0xED && p[1] > 0x9F) return 0;             /* surrogate */
    }
    if (n == 4) {
        if (c > 0xF4) return 0;
        if (c == 0xF0 && p[1] < 0x90) return 0;
        if (c == 0xF4 && p[1] > 0x8F) return 0;
    }
    return n;
}

static void jw_u_esc(pv2_jw *w, unsigned char c)
{
    char b[8];
    b[0] = '\\'; b[1] = 'u'; b[2] = '0'; b[3] = '0';
    b[4] = HEXD[(c >> 4) & 0xF];
    b[5] = HEXD[c & 0xF];
    b[6] = '\0';
    jw_put(w, b, 6);
}

/* quoted, escaped JSON string */
static void jw_quoted(pv2_jw *w, const char *s, size_t n)
{
    size_t i = 0;
    pv2_jw_ch(w, '"');
    while (i < n && !w->overflow) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { pv2_jw_ch(w, '\\'); pv2_jw_ch(w, (char)c); i++; }
        else if (c == '\n') { jw_put(w, "\\n", 2); i++; }
        else if (c == '\r') { jw_put(w, "\\r", 2); i++; }
        else if (c == '\t') { jw_put(w, "\\t", 2); i++; }
        else if (c == '\b') { jw_put(w, "\\b", 2); i++; }
        else if (c == '\f') { jw_put(w, "\\f", 2); i++; }
        else if (c < 0x20 || c == 0x7f) { jw_u_esc(w, c); i++; }
        else if (c < 0x80) { pv2_jw_ch(w, (char)c); i++; }
        else {
            size_t l = u8len((const unsigned char *)s + i, n - i);
            if (l == 0) { pv2_jw_ch(w, '?'); i++; }
            else { jw_put(w, s + i, l); i += l; }
        }
    }
    pv2_jw_ch(w, '"');
}

void pv2_jw_str(pv2_jw *w, const char *s)
{
    if (!w) return;
    if (!s) { pv2_jw_raw(w, "null"); return; }
    jw_quoted(w, s, strlen(s));
}

void pv2_jw_i64(pv2_jw *w, long long v)
{
    char b[32];
    snprintf(b, sizeof b, "%lld", v);
    pv2_jw_raw(w, b);
}

void pv2_jw_kv_str(pv2_jw *w, const char *k, const char *v)
{
    if (!w) return;
    pv2_jw_str(w, k);
    pv2_jw_ch(w, ':');
    pv2_jw_str(w, v);
}

void pv2_jw_kv_i64(pv2_jw *w, const char *k, long long v)
{
    if (!w) return;
    pv2_jw_str(w, k);
    pv2_jw_ch(w, ':');
    pv2_jw_i64(w, v);
}

void pv2_jw_kv_bool(pv2_jw *w, const char *k, int v)
{
    if (!w) return;
    pv2_jw_str(w, k);
    pv2_jw_ch(w, ':');
    pv2_jw_raw(w, v ? "true" : "false");
}

void pv2_jw_json_or_null(pv2_jw *w, const char *maybe_json)
{
    if (!w) return;
    if (!maybe_json || !*maybe_json) { pv2_jw_raw(w, "null"); return; }
    pv2_jw_raw(w, maybe_json);
}

/* ------------------------------------------------------------ route table */
const char *pv2_route_path(pv2_route r)
{
    switch (r) {
    case PV2_ROUTE_CAPABILITIES:   return "/api/v2/capabilities";
    case PV2_ROUTE_PAIR:           return "/api/v2/pair";
    case PV2_ROUTE_SYNC_STATUS:    return "/api/v2/sync/status";
    case PV2_ROUTE_SYNC_PUSH:      return "/api/v2/sync/push";
    case PV2_ROUTE_SYNC_PULL:      return "/api/v2/sync/pull";
    case PV2_ROUTE_WORDS:          return "/api/v2/words";
    case PV2_ROUTE_DAILY_PLAN:     return "/api/v2/daily-plan";
    case PV2_ROUTE_BRIEFINGS:      return "/api/v2/briefings";
    case PV2_ROUTE_AI_JOB_CREATE:  return "/api/v2/ai/jobs";
    case PV2_ROUTE_AI_JOB_GET:     return "/api/v2/ai/jobs/{job_id}";
    case PV2_ROUTE_AI_JOB_CANCEL:  return "/api/v2/ai/jobs/{job_id}/cancel";
    case PV2_ROUTE_SETTINGS_GET:   return "/api/v2/settings";
    case PV2_ROUTE_SETTINGS_POST:  return "/api/v2/settings";
    case PV2_ROUTE_TOKEN_REVOKE:   return "/api/v2/auth/token/revoke";
    default:                       return "";
    }
}
