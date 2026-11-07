/* panel_v2_planlist.c - N2: materialize the phone's daily plan list for the
 * board words page (contract in panel_v2.h).  Replaces the M5-A date-hash
 * pick: the board shows the phone's list, never invents one.
 *
 * Read-only over the pv2 store.  Two walks: (1) newest live date <= today,
 * (2) collect that date's rows ordered by seq.  Tombstones are skipped during
 * the walk (F2-D1: p_list is a physical cursor, count() is live rows).
 *
 * Threading: the collection buffer is static because the pv2 layer serves one
 * request at a time (single-threaded serve loop, TRAE-M2 report); the harness
 * is likewise single-threaded.  Keep it that way or make the buffer dynamic.
 */
#include "panel_v2.h"

#include <cJSON.h>
#include <stdio.h>
#include <string.h>

#define PV2_PLAN_WALK_MAX 64                 /* p_list page size           */
#define PV2_PLAN_COLLECT_MAX 256            /* rows per day hard cap      */
#define PV2_PLAN_PAYLOAD_CAP (PV2_OP_PAYLOAD_BYTES + 1)

/* Parse one daily_plan payload into (word_id, state, seq, goal, date).
 * Returns 1 on success; out pointers may be NULL to skip a field. */
static int plan_parse(const char *payload, char *word_id, size_t wn,
                      char *state, size_t sn, long *seq, int *goal, char *date, size_t dn)
{
    cJSON *o = cJSON_Parse(payload);
    cJSON *jw, *js, *jq, *jg, *jd;
    const char *w, *s, *d;
    if (!o) return 0;
    jw = cJSON_GetObjectItemCaseSensitive(o, "word_id");
    js = cJSON_GetObjectItemCaseSensitive(o, "state");
    jq = cJSON_GetObjectItemCaseSensitive(o, "seq");
    jg = cJSON_GetObjectItemCaseSensitive(o, "goal");
    jd = cJSON_GetObjectItemCaseSensitive(o, "date");
    w = cJSON_GetStringValue(jw);
    s = cJSON_GetStringValue(js);
    d = cJSON_GetStringValue(jd);
    if (!w || !*w || !s || !*s || !d || !*d) { cJSON_Delete(o); return 0; }
    if (word_id) snprintf(word_id, wn, "%s", w);
    if (state)   snprintf(state, sn, "%s", s);
    if (date)    snprintf(date, dn, "%s", d);
    if (seq)     *seq = cJSON_IsNumber(jq) ? (long)jq->valuedouble : 0;
    if (goal)    *goal = cJSON_IsNumber(jg) ? (int)jg->valuedouble : 0;
    cJSON_Delete(o);
    return 1;
}

int pv2_planlist_pick(pv2_store *st, const char *today, pv2_plan_row *rows, int max,
                      char *date_used, size_t dn, int *goal)
{
    static pv2_plan_row collected[PV2_PLAN_COLLECT_MAX];
    pv2_entity_info infos[PV2_PLAN_WALK_MAX];
    char pbuf[PV2_PLAN_PAYLOAD_CAP];
    char best[16];
    char wid[48], stt[16], dt[16];
    long seq;
    int gl;
    long scanned;
    int n = 0, gmax = 0, i, copyn;

    if (date_used && dn) date_used[0] = 0;
    if (goal) *goal = 0;
    if (!st || !st->h || !st->p_list || !st->p_get_payload || !today || !*today)
        return 0;

    /* pass 1: newest live date <= today (lexicographic == chronological) */
    best[0] = 0;
    scanned = 0;
    for (;;) {
        int got = st->p_list(st->h, PV2_ENT_PLAN, scanned, infos, PV2_PLAN_WALK_MAX);
        if (got <= 0) break;
        for (i = 0; i < got; i++) {
            size_t plen = 0;
            long rev = 0;
            int del = 0;
            if (infos[i].deleted) continue;
            if (st->p_get_payload(st->h, PV2_ENT_PLAN, infos[i].id, pbuf,
                                  PV2_PLAN_PAYLOAD_CAP - 1, &plen, &rev, &del) != 1 || del)
                continue;
            pbuf[plen < PV2_PLAN_PAYLOAD_CAP - 1 ? plen : PV2_PLAN_PAYLOAD_CAP - 1] = 0;
            if (!plan_parse(pbuf, NULL, 0, NULL, 0, NULL, NULL, dt, sizeof dt)) continue;
            if (strcmp(dt, today) <= 0 && strcmp(dt, best) > 0)
                snprintf(best, sizeof best, "%s", dt);
        }
        scanned += got;
    }
    if (!best[0]) return 0;

    /* pass 2: collect that date's live rows (order of arrival kept for ties) */
    scanned = 0;
    for (;;) {
        int got = st->p_list(st->h, PV2_ENT_PLAN, scanned, infos, PV2_PLAN_WALK_MAX);
        if (got <= 0) break;
        for (i = 0; i < got; i++) {
            size_t plen = 0;
            long rev = 0;
            int del = 0;
            if (infos[i].deleted) continue;
            if (st->p_get_payload(st->h, PV2_ENT_PLAN, infos[i].id, pbuf,
                                  PV2_PLAN_PAYLOAD_CAP - 1, &plen, &rev, &del) != 1 || del)
                continue;
            pbuf[plen < PV2_PLAN_PAYLOAD_CAP - 1 ? plen : PV2_PLAN_PAYLOAD_CAP - 1] = 0;
            if (!plan_parse(pbuf, wid, sizeof wid, stt, sizeof stt, &seq, &gl,
                            dt, sizeof dt)) continue;
            if (strcmp(dt, best) != 0) continue;
            if (n < PV2_PLAN_COLLECT_MAX) {
                snprintf(collected[n].word_id, sizeof collected[n].word_id, "%s", wid);
                snprintf(collected[n].state, sizeof collected[n].state, "%s", stt);
                collected[n].seq = seq;
            }
            n++;
            if (gl > gmax) gmax = gl;
        }
        scanned += got;
    }

    /* stable insertion sort by seq (kept small: daily list <= phone goal cap) */
    {
        int a, b;
        for (a = 1; a < n && a < PV2_PLAN_COLLECT_MAX; a++) {
            pv2_plan_row key = collected[a];
            for (b = a - 1; b >= 0 && collected[b].seq > key.seq; b--)
                collected[b + 1] = collected[b];
            collected[b + 1] = key;
        }
    }

    copyn = n < PV2_PLAN_COLLECT_MAX ? n : PV2_PLAN_COLLECT_MAX;
    if (rows && max > 0) {
        int c = copyn < max ? copyn : max;
        for (i = 0; i < c; i++) rows[i] = collected[i];
    }
    if (date_used && dn) snprintf(date_used, dn, "%s", best);
    if (goal) *goal = gmax;
    return n;
}
