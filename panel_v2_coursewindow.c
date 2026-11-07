/* panel_v2_coursewindow.c - N3: derive the course timetable window state from
 * the pv2 store (contract in panel_v2.h).
 *
 * The phone pushes course_occurrence rows (payload carries the window
 * generation) plus ONE schedule_window row (id "schedule-window") declaring
 * the target generation and its exact row count.  The generation is ACTIVE
 * only when the live row count for that generation equals the declared count;
 * this is derived by recount on every read - no stored activation flag - so
 * restarts and interrupted syncs converge deterministically (sec.4.2: switch
 * to the new generation only after the pack completes; keep the old
 * generation rows displayable while syncing).
 *
 * Read-only over the pv2 store; tombstones skipped during the physical walk
 * (F2-D1: p_list is a physical cursor, count() is live rows).
 *
 * Threading: static buffers are safe under the single-threaded serve loop
 * (same precondition as panel_v2_planlist.c); the harness is likewise
 * single-threaded.
 */
#include "panel_v2.h"

#include <cJSON.h>
#include <stdio.h>
#include <string.h>

#define PV2_WINDOW_WALK_MAX 64              /* p_list page size            */
#define PV2_WINDOW_ID "schedule-window"
#define PV2_WINDOW_PAYLOAD_CAP (PV2_OP_PAYLOAD_BYTES + 1)

/* Parse the schedule_window payload: {"generation","table","from","to","count"}.
 * generation and a positive count are required; the rest are optional. */
static int window_parse(const char *payload, pv2_window_info *out)
{
    cJSON *o = cJSON_Parse(payload);
    cJSON *jg, *jc, *jt, *jf, *jto;
    const char *g, *t, *f, *to;
    if (!o) return 0;
    jg = cJSON_GetObjectItemCaseSensitive(o, "generation");
    jc = cJSON_GetObjectItemCaseSensitive(o, "count");
    jt = cJSON_GetObjectItemCaseSensitive(o, "table");
    jf = cJSON_GetObjectItemCaseSensitive(o, "from");
    jto = cJSON_GetObjectItemCaseSensitive(o, "to");
    g = cJSON_GetStringValue(jg);
    if (!g || !*g || !cJSON_IsNumber(jc) || jc->valuedouble <= 0) {
        cJSON_Delete(o);
        return 0;
    }
    snprintf(out->generation, sizeof out->generation, "%s", g);
    out->target = (int)jc->valuedouble;
    t = cJSON_GetStringValue(jt);
    f = cJSON_GetStringValue(jf);
    to = cJSON_GetStringValue(jto);
    snprintf(out->table, sizeof out->table, "%s", t ? t : "");
    snprintf(out->from, sizeof out->from, "%s", f ? f : "");
    snprintf(out->to, sizeof out->to, "%s", to ? to : "");
    cJSON_Delete(o);
    return 1;
}

/* payload "generation" string, or 0 when absent/malformed */
static int course_gen(const char *payload, char *gen, size_t gn)
{
    cJSON *o = cJSON_Parse(payload);
    cJSON *jg = o ? cJSON_GetObjectItemCaseSensitive(o, "generation") : NULL;
    const char *g = cJSON_GetStringValue(jg);
    if (!g || !*g) { cJSON_Delete(o); return 0; }
    snprintf(gen, gn, "%s", g);
    cJSON_Delete(o);
    return 1;
}

int pv2_window_state(pv2_store *st, pv2_window_info *out)
{
    static char pbuf[PV2_WINDOW_PAYLOAD_CAP];
    pv2_entity_info infos[PV2_WINDOW_WALK_MAX];
    long scanned;
    int found = 0;

    if (out) memset(out, 0, sizeof *out);
    if (!st || !st->h || !st->p_list || !st->p_get_payload || !out)
        return -1;

    /* pass 1: the live schedule_window row */
    scanned = 0;
    for (;;) {
        int got = st->p_list(st->h, PV2_ENT_WINDOW, scanned, infos, PV2_WINDOW_WALK_MAX);
        if (got <= 0) break;
        int i;
        for (i = 0; i < got; i++) {
            size_t plen = 0;
            long rev = 0;
            int del = 0;
            if (infos[i].deleted) continue;
            if (strcmp(infos[i].id, PV2_WINDOW_ID) != 0) continue;
            if (st->p_get_payload(st->h, PV2_ENT_WINDOW, infos[i].id, pbuf,
                                  PV2_WINDOW_PAYLOAD_CAP - 1, &plen, &rev, &del) != 1 || del)
                continue;
            pbuf[plen < PV2_WINDOW_PAYLOAD_CAP - 1 ? plen : PV2_WINDOW_PAYLOAD_CAP - 1] = 0;
            if (!window_parse(pbuf, out)) continue;
            found = 1;
        }
        scanned += got;
    }
    out->have_window = found;
    if (!found) return 0;

    /* pass 2: count live course_occurrence rows of the target generation */
    scanned = 0;
    for (;;) {
        int got = st->p_list(st->h, PV2_ENT_COURSE, scanned, infos, PV2_WINDOW_WALK_MAX);
        if (got <= 0) break;
        int i;
        for (i = 0; i < got; i++) {
            size_t plen = 0;
            long rev = 0;
            int del = 0;
            char gen[24];
            if (infos[i].deleted) continue;
            if (st->p_get_payload(st->h, PV2_ENT_COURSE, infos[i].id, pbuf,
                                  PV2_WINDOW_PAYLOAD_CAP - 1, &plen, &rev, &del) != 1 || del)
                continue;
            pbuf[plen < PV2_WINDOW_PAYLOAD_CAP - 1 ? plen : PV2_WINDOW_PAYLOAD_CAP - 1] = 0;
            if (!course_gen(pbuf, gen, sizeof gen)) continue;
            if (strcmp(gen, out->generation) == 0) out->have++;
        }
        scanned += got;
    }
    out->active = (out->target > 0 && out->have == out->target);
    return 1;
}

