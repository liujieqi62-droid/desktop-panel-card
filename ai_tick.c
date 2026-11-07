/* ai_tick.c - M6-B1: service 侧 AI 队列桥（提交 + 回传）。
 *
 * 独立编译单元：只依赖 panel_v2.h + cJSON，service（板端）与桌面宿主测试共用。
 * 线程模型：service 单线程 serve 循环节流调用 ai_tick()，无锁需求。
 *
 * 提交（Marves L113）：scheduler_owner=board 时板卡每天生成一份简报——
 *   task_key = "brief|<date>|board|<facts 指纹>|brief-v1"；ref 扫描保证每业务日
 *   至多一次提交（断线切换不双生成的板端半边，手机半边在 M6-B2）。
 *   facts 只含板端真实拥有的数据（日期/最近单词/天气占位），绝不编造（L155）。
 * 回传（Marves L139/L121）：worker 终态任务文件 → 结果按手机 push 同款段落 id
 *   （brief-<date>-<slot>）走 pv2_store_apply 写回——同一路径天然获得幂等回执、
 *   冲突双副本语义；failed 只同步状态，当日绝不重提（调用结果未知不盲目无限重试）。
 *
 * aux 键位（job rec 的 task_key 含 '|'，故标记一律走独立键）：
 *   job:<jid>        = state|kind|ref|created|updated|<task_key 原文>
 *   tkey:<task_key>  = jid                     （同 task_key 幂等映射）
 *   aidone:<jid>     = 1                       （终态任务已采集，恰一次）
 */
#include "panel_v2.h"

#include <cJSON.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif

/* queue 文件名后缀与读取上限（与 aiworker.c 同源常量） */
#define AI_JOB_SUFFIX      ".job.json"
#define AI_MAX_JOB_BYTES   (256 * 1024)
#define AI_PROMPT_VERSION  "brief-v1"
#define AI_CLIENT_ID       "board-aiworker"
#define AI_PARA_BYTES_MAX  1500        /* fitBriefPara 同款：2048B 记录含封套的内层安全值 */

/* --------------------------------------------------------------- 平台垫片 */
#ifdef _WIN32
#include <windows.h>
static int ai_atomic_replace(const char *tmp, const char *path)
{
    /* zig mingw rename() 目标已存在即 EEXIST，不用它（M5 教训）。 */
    wchar_t wtmp[1024], wpath[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, tmp, -1, wtmp, 1024) == 0) return -1;
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024) == 0) return -1;
    if (!MoveFileExW(wtmp, wpath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return -1;
    return 0;
}
#else
static int ai_atomic_replace(const char *tmp, const char *path)
{
    return rename(tmp, path);
}
#endif

/* --------------------------------------------------------------- 小工具 */
static char *ai_strdup_n(const char *s, size_t n)
{
    char *d = (char *)malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

/* FNV-1a 64：facts 指纹。非安全用途——只作任务去重标识，与手机端 SHA-256
 * 不同源（板端不链接摘要库）；跨端 key 合并对齐留 M7 联调（checkpoint 记录）。 */
static void ai_fnv64_hex(const char *data, size_t len, char *out /*17+*/)
{
    uint64_t h = 1469598103934665603ULL;
    size_t i;
    static const char hexd[] = "0123456789abcdef";
    for (i = 0; i < len; i++) {
        h ^= (unsigned char)data[i];
        h *= 1099511628211ULL;
    }
    for (i = 0; i < 16; i++)
        out[15 - i] = hexd[(h >> (i * 4)) & 0xF];
    out[16] = '\0';
}

/* 一次性读整个文件（上限 cap），返回 malloc 缓冲或 NULL。 */
static char *ai_read_file(const char *path, size_t *out_len, size_t cap)
{
    int fd = open(path, O_RDONLY | O_BINARY);
    char *buf;
    size_t total = 0;
    *out_len = 0;
    if (fd < 0) return NULL;
    buf = (char *)malloc(cap + 1);
    if (!buf) { close(fd); return NULL; }
    for (;;) {
        ssize_t got;
        if (total >= cap) { break; }
        got = read(fd, buf + total, cap - total);
        if (got < 0) {
            if (errno == EINTR) continue;
            free(buf);
            close(fd);
            return NULL;
        }
        if (got == 0) break;
        total += (size_t)got;
    }
    close(fd);
    buf[total] = '\0';
    *out_len = total;
    return buf;
}

/* 原子写：tmp + rename（Windows 走 MoveFileExW REPLACE_EXISTING）。 */
static int ai_write_atomic(const char *path, const char *data, size_t len)
{
    char tmp[1200];
    int fd;
    size_t off = 0;
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return -1;
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0600);
    if (fd < 0) return -1;
    while (off < len) {
        ssize_t put = write(fd, data + off, len - off);
        if (put < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        off += (size_t)put;
    }
#ifdef _WIN32
    _commit(fd);
#else
    fsync(fd);
#endif
    close(fd);
    return ai_atomic_replace(tmp, path);
}

/* 列出目录下 *.job.json，文件名排序；返回 malloc 名字数组（调用方 free）。 */
static int ai_name_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int ai_list_jobs(const char *dir, char ***out_names, size_t *out_n)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char **names = NULL;
    size_t n = 0, cap = 0;
    *out_names = NULL;
    *out_n = 0;
    if (!d) return -1;                       /* 目录不存在：本轮无事可做 */
    while ((e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);
        if (l < 10 || strcmp(e->d_name + l - 9, AI_JOB_SUFFIX) != 0) continue;
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 16;
            char **nn = (char **)realloc(names, nc * sizeof *nn);
            if (!nn) break;
            names = nn; cap = nc;
        }
        names[n] = ai_strdup_n(e->d_name, l);
        if (names[n]) n++;
    }
    closedir(d);
    if (n) qsort(names, n, sizeof *names, ai_name_cmp);
    *out_names = names;
    *out_n = n;
    return 0;
}

/* fitBriefPara 同款：≤1500B 的最长完整 UTF-8 前缀 + 「…」（绝不产生半个码点）。 */
static void ai_fit_para(const char *s, char *out, size_t cap)
{
    size_t bl = strlen(s);
    size_t fit;
    if (cap < 8) return;
    if (bl < AI_PARA_BYTES_MAX) {
        snprintf(out, cap, "%s", s);
        return;
    }
    fit = AI_PARA_BYTES_MAX;
    while (fit > 0 && ((unsigned char)s[fit] & 0xC0) == 0x80) fit--;   /* 回退完整码点 */
    memcpy(out, s, fit);
    memcpy(out + fit, "…", 4);            /* E2 80 A6 + NUL */
}

/* --------------------------------------------------------------- job rec */
/* rec = state|kind|ref|created|updated|<task_key 原文，可含 '>' 前所有字符 '|'>
 * 取字段：idx 0..4 用 '|' 切；idx 5 = 余下整体（task_key 可含 '|'）。 */
static int ai_rec_field(const char *rec, int idx, char *out, size_t n)
{
    char buf[768];
    char *p;
    int i;
    if (!rec) return 0;
    snprintf(buf, sizeof buf, "%s", rec);
    for (i = 0; i < idx && i < 5; i++) {
        p = strchr(buf, '|');
        if (!p) return 0;
        *p = '\0';
        memmove(buf, p + 1, strlen(p + 1) + 1);
    }
    if (idx < 5) {
        p = strchr(buf, '|');
        if (p) *p = '\0';
    }
    snprintf(out, n, "%s", buf);
    return 1;
}

static void ai_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("[ai-tick] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* --------------------------------------------------------------- 提交 */
/* 确保队列目录存在（<parent>=<v2dir>/ai 可能尚未创建；0700 与数据目录一致）。 */
static void ai_mkdir(const char *path)
{
#ifdef _WIN32
    mkdir(path);                              /* mingw CRT：1 参数 */
#else
    mkdir(path, 0700);
#endif
}

static void ai_ensure_queue_dir(const char *queue_dir)
{
    char parent[1024];
    char *slash;
    snprintf(parent, sizeof parent, "%s", queue_dir);
    slash = strrchr(parent, '/');
    if (slash && slash != parent) {
        *slash = '\0';
        ai_mkdir(parent);                     /* EEXIST 忽略 */
    }
    ai_mkdir(queue_dir);
}

/* 业务日期口径（TRAE-F1 缺陷修复）：「当日」必须与手机端一致走 CST-8 本地日历日，
 * 不能用 pv2_date_of（UTC）。UTC 口径在 CST 00:00–08:00 期间仍是前一天——
 * 防双生成扫描 ref=brief-<UTC 昨日> 看不到手机当天提交（双生成），且自提交会
 * 生成「昨天」日期的简报（M6B2 验证均在 CST 白天、UTC 同日，掩盖了该边界）。
 * TZ 字面量与 display.c/service.c 同约定 CST-8；只设一次。 */
static void ai_local_date(char *buf, size_t n)
{
    time_t t = time(NULL);
    struct tm tmv;
    struct tm *pt;
#ifdef _WIN32
    static int tz_done = 0;
    if (!tz_done) { _putenv("TZ=CST-8"); _tzset(); tz_done = 1; }
    pt = localtime(&t);                    /* 单线程约定（service serve 循环） */
#else
    static int tz_done = 0;
    struct tm tmb;
    if (!tz_done) { setenv("TZ", "CST-8", 1); tzset(); tz_done = 1; }
    localtime_r(&t, &tmb);
    pt = &tmb;
#endif
    if (pt)
        snprintf(buf, n, "%04d-%02d-%02d", pt->tm_year + 1900, pt->tm_mon + 1, pt->tm_mday);
    else
        snprintf(buf, n, "1970-01-01");    /* 不应发生：保守占位，绝不崩溃 */
}

/* 当日已提交判定：aux job: 前缀里 ref=="brief-<date>"（任何状态都算已提交）。 */
static int ai_submitted_today(pv2_ctx *c, const char *date)
{
    char *keys[128];
    int n = pv2_kv_list(c->aux, "job:", keys, 128), i;
    char ref[96], want[96];
    snprintf(want, sizeof want, "brief-%s", date);
    for (i = 0; i < n; i++) {
        const char *rec = pv2_kv_get(c->aux, keys[i]);
        if (rec && ai_rec_field(rec, 2, ref, sizeof ref) && !strcmp(ref, want))
            return 1;
    }
    return 0;
}

/* 今日简报是否已存在（4 个槽位 id 任一存在即算——含墓碑；删除属手机侧业务，
 * 板端不为「槽位被删」重新生成，避免与墓碑打架）。
 * 同时经 *out_max_rev 返回当前最大展示 revision（提交时作乐观并发基线）。 */
static int ai_briefing_exists_today(pv2_ctx *c, const char *date, long *out_max_rev)
{
    int slot;
    char id[40];
    long rev = 0;
    int del = 0;
    int exists = 0;
    if (out_max_rev) *out_max_rev = 0;
    for (slot = 0; slot < 4; slot++) {
        snprintf(id, sizeof id, "brief-%s-%d", date, slot);
        if (c->store.p_get_payload(c->store.h, PV2_ENT_BRIEFING, id, NULL, 0, NULL,
                                   &rev, &del) == 1) {
            exists = 1;
            if (rev > (out_max_rev ? *out_max_rev : 0) && out_max_rev)
                *out_max_rev = rev;
        }
    }
    return exists;
}

/* 板端真实事实子集：date + 最近 ≤5 词文本 + 天气占位（绝不编造，L155）。 */
static cJSON *ai_build_facts(pv2_ctx *c, const char *date)
{
    cJSON *facts = cJSON_CreateObject();
    cJSON *words;
    pv2_entity_info info;
    long n, off;
    int taken = 0;
    if (!facts) return NULL;
    cJSON_AddStringToObject(facts, "date", date);
    words = cJSON_AddArrayToObject(facts, "recent_words");
    cJSON_AddStringToObject(facts, "weather", "");
    cJSON_AddNumberToObject(facts, "weather_updated_at", 0);
    if (!words) { cJSON_Delete(facts); return NULL; }
    n = pv2_store_count(&c->store, PV2_ENT_WORD, 0);
    for (off = 0; off < n && taken < 5; off++) {
        char payload[1024];
        size_t plen = 0;
        long rev = 0;
        int del = 0;
        cJSON *o;
        const char *t;
        if (c->store.p_list(c->store.h, PV2_ENT_WORD, off, &info, 1) != 1) break;
        if (c->store.p_get_payload(c->store.h, PV2_ENT_WORD, info.id, payload,
                                   sizeof payload - 1, &plen, &rev, &del) != 1)
            continue;
        payload[plen] = 0;
        o = cJSON_Parse(payload);
        t = o ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "text")) : NULL;
        if (t && *t) {
            cJSON_AddItemToArray(words, cJSON_CreateString(t));
            taken++;
        }
        cJSON_Delete(o);
    }
    return facts;
}

/* owner=board 且当日无简报且当日未提交 → 提交一份 briefing 任务到队列。
 * 返回 0=正常（含无事可做），<0=本地异常（已记日志，serve 不中断）。 */
static int ai_submit_due(pv2_ctx *c, const char *queue_dir, long long now_ms)
{
    const char *owner;
    char date[16], want[96];
    char facts_json[2048], hash[17], task_key[192], jid[24];
    char key[320], rec[512], path[1024];
    char *txt = NULL;
    cJSON *facts, *payload, *job;
    long long now_s = pv2_wall_seconds();
    long base_rev = 0;
    int rc = 0;

    owner = pv2_kv_get(c->aux, "set:scheduler_owner");
    if (!owner || strcmp(owner, "board") != 0) return 0;      /* 手机负责：无事 */
    ai_local_date(date, sizeof date);                         /* TRAE-F1: CST-8 业务日 */

    base_rev = 0;
    if (ai_briefing_exists_today(c, date, &base_rev)) return 0;   /* 当日已有简报 */
    snprintf(want, sizeof want, "brief-%s", date);
    if (ai_submitted_today(c, date)) return 0;                /* 当日已提交过 */

    if (pv2_kv_count_prefix(c->aux, "job:") >= (int)PV2_AI_QUEUE_MAX) {
        ai_log("queue full, skip submit (%s)", date);          /* 不报错不重试，下轮再看 */
        return 0;
    }

    facts = ai_build_facts(c, date);
    if (!facts) { ai_log("oom building facts"); return -1; }
    txt = cJSON_PrintUnformatted(facts);
    if (!txt || strlen(txt) >= sizeof facts_json) {
        ai_log("facts too large, skip submit");
        free(txt); cJSON_Delete(facts);
        return 0;
    }
    memcpy(facts_json, txt, strlen(txt) + 1);
    free(txt);
    txt = NULL;

    ai_fnv64_hex(facts_json, strlen(facts_json), hash);
    snprintf(task_key, sizeof task_key, "brief|%s|board|%s|%s", date, hash, AI_PROMPT_VERSION);

    /* tkey 幂等双保险（ref 扫描之外）：同 task_key 已有映射就不重提 */
    snprintf(key, sizeof key, "tkey:%s", task_key);
    if (pv2_kv_get(c->aux, key)) { cJSON_Delete(facts); return 0; }

    if (pv2_random_hex(jid, 8) != 0) {                        /* T04B：无随机源不提交 */
        ai_log("no secure random, skip submit");
        cJSON_Delete(facts);
        return -1;
    }

    job = cJSON_CreateObject();
    if (!job) { cJSON_Delete(facts); return -1; }
    cJSON_AddStringToObject(job, "job_id", jid);
    cJSON_AddStringToObject(job, "task_key", task_key);
    cJSON_AddStringToObject(job, "type", "briefing");
    cJSON_AddStringToObject(job, "executor", "board");
    cJSON_AddStringToObject(job, "input_hash", hash);
    payload = cJSON_AddObjectToObject(job, "payload");
    cJSON_AddItemToObject(payload, "facts", facts);           /* worker 读 payload.facts */
    /* base_rev = 提交时简报最大 revision（通常 0）：回传按它做乐观并发——若手机
     * 在任务执行期间推了更新的段落，回写将得到 conflict，人工内容按 store 语义
     * 双副本保全，绝不覆盖（L121/M4A「AI 不得覆盖较新人工内容」）。 */
    cJSON_AddNumberToObject(job, "base_rev", base_rev);
    cJSON_AddStringToObject(job, "status", "queued");
    cJSON_AddNumberToObject(job, "attempts", 0);
    cJSON_AddNumberToObject(job, "next_retry_ms", 0);
    cJSON_AddNumberToObject(job, "created_ms", now_ms);
    txt = cJSON_PrintUnformatted(job);
    cJSON_Delete(job);
    if (!txt) return -1;

    /* 顺序：先 rec（提交标记）→ 文件 → tkey。rec 先行保证任何后续失败都不会
     * 重提（宁可当日丢弃，绝不双生成）；rec 后文件失败则回滚 rec 重试。 */
    snprintf(key, sizeof key, "job:%s", jid);
    snprintf(rec, sizeof rec, "queued|briefing|%s|%lld|%lld|%s",
             want, now_s, now_s, task_key);
    if (pv2_kv_set(c->aux, key, rec) != 0) {                  /* T04C：不假报已提交 */
        ai_log("job rec persist failed, skip submit");
        free(txt);
        return -1;
    }
    ai_ensure_queue_dir(queue_dir);
    snprintf(path, sizeof path, "%s/%s%s", queue_dir, jid, AI_JOB_SUFFIX);
    if (ai_write_atomic(path, txt, strlen(txt)) != 0) {
        ai_log("queue file write failed: %s", path);
        free(txt);
        pv2_kv_del(c->aux, key);                              /* 回滚：下轮可重提 */
        return -1;
    }
    free(txt);
    txt = NULL;
    snprintf(key, sizeof key, "tkey:%s", task_key);
    if (pv2_kv_set(c->aux, key, jid) != 0)                    /* 失败无害：ref 扫描兜底 */
        ai_log("tkey persist failed (ref scan covers dedupe): %s", jid);
    ai_log("submitted briefing job %s for %s", jid, date);
    return rc;
}

/* --------------------------------------------------------------- 回传 */
/* 终态任务状态同步进 rec（succeeded/failed/canceled；保留原 kind/ref/created）。 */
static void ai_sync_rec_state(pv2_ctx *c, const char *jid, const char *state)
{
    char key[80], rec[512], kind[32], ref[96], created_s[32], task_key[256];
    const char *old;
    long long created;
    snprintf(key, sizeof key, "job:%s", jid);
    old = pv2_kv_get(c->aux, key);
    if (!old) return;
    if (!ai_rec_field(old, 1, kind, sizeof kind) ||
        !ai_rec_field(old, 2, ref, sizeof ref) ||
        !ai_rec_field(old, 3, created_s, sizeof created_s))
        return;
    created = strtoll(created_s, NULL, 10);
    ai_rec_field(old, 5, task_key, sizeof task_key);
    snprintf(rec, sizeof rec, "%s|%s|%s|%lld|%lld|%s",
             state, kind, ref, created, pv2_wall_seconds(), task_key);
    if (pv2_kv_set(c->aux, key, rec) != 0)                    /* T04C */
        ai_log("rec state persist failed: %s", jid);
}

/* 成功简报结果 → 4 槽位段落 upsert（与手机 push 同路径同语义）。
 * op_id 确定性（ai-<jid>-s<slot>）→ 意外重采也能拿到 duplicate 回执。
 * 乐观并发：base 统一用提交时冻结的 base_rev——执行期间手机推了更新段落
 * （server rev 已前进）→ conflict，人工内容双副本保全，AI 结果绝不覆盖。 */
static void ai_writeback_briefing(pv2_ctx *c, const char *jid, const char *date,
                                  cJSON *result, long base_rev)
{
    static const char *fields[4] = { "plan", "review", "weather", "tip" };
    char para[AI_PARA_BYTES_MAX + 8];
    int slot;
    long new_rev = base_rev + 1;              /* 展示用 revision（手机口径：全槽一致） */
    int ok = 1;

    for (slot = 0; slot < 4 && ok; slot++) {
        cJSON *pl;
        pv2_op op;
        pv2_receipt receipt;
        char op_id[96], id[40];
        char *ptxt;
        size_t plen;
        const char *v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result, fields[slot]));
        const char *src = v ? v : "";
        int rc;

        ai_fit_para(src, para, sizeof para);
        pl = cJSON_CreateObject();
        if (!pl) { ok = 0; break; }
        cJSON_AddStringToObject(pl, "p", para);
        cJSON_AddNumberToObject(pl, "revision", new_rev);
        ptxt = cJSON_PrintUnformatted(pl);
        cJSON_Delete(pl);
        if (!ptxt) { ok = 0; break; }
        plen = strlen(ptxt);

        memset(&op, 0, sizeof op);
        snprintf(op_id, sizeof op_id, "ai-%s-s%d", jid, slot);
        snprintf(op.op_id, sizeof op.op_id, "%s", op_id);
        snprintf(id, sizeof id, "brief-%s-%d", date, slot);
        snprintf(op.entity_id, sizeof op.entity_id, "%s", id);
        op.entity = PV2_ENT_BRIEFING;
        op.base_revision = base_rev;          /* 乐观基线：不符即 conflict，不覆盖 */
        op.deleted = 0;
        op.retry_count = 0;
        op.payload = ptxt;
        op.payload_len = plen;

        memset(&receipt, 0, sizeof receipt);
        rc = pv2_store_apply(&c->store, &op, AI_CLIENT_ID, &receipt);
        free(ptxt);
        if (rc < 0 || receipt.status == PV2_ST_REJECTED ||
            receipt.status == PV2_ST_STORAGE_FAIL) {
            ai_log("writeback slot %d not applied (%s), will retry next tick",
                   slot, pv2_op_status_name(receipt.status));
            ok = 0;                            /* 不标 collected：下轮重试（存储类失败） */
        }
        /* applied/duplicate/conflict 均视为已处理：conflict=手机有更新版本，
         * 冲突副本按 store 语义留存，绝不覆盖人工内容（L121）。 */
    }
    if (ok) {
        char key[80];
        snprintf(key, sizeof key, "aidone:%s", jid);
        if (pv2_kv_set(c->aux, key, "1") != 0)
            ai_log("aidone persist failed: %s", jid);
        else
            ai_log("briefing result collected: %s (%s)", jid, date);
    }
}

/* 扫队列目录：worker 终态任务 → 状态同步 + briefing 结果回传（每任务恰一次）。 */
static int ai_collect_once(pv2_ctx *c, const char *queue_dir)
{
    char **names = NULL;
    size_t n = 0, i;
    if (ai_list_jobs(queue_dir, &names, &n) != 0) return 0;   /* 目录不存在：无事 */
    for (i = 0; i < n; i++) {
        char path[1024], jid[64];
        char *raw;
        size_t rawlen;
        cJSON *job, *result;
        const char *status;
        char key[96], rec_kind[32], rec_ref[96];
        const char *rec;
        size_t nl = strlen(names[i]);
        if (nl <= 9 || nl >= sizeof jid) { free(names[i]); continue; }
        memcpy(jid, names[i], nl - 9);        /* 去掉 .job.json */
        jid[nl - 9] = '\0';

        snprintf(key, sizeof key, "aidone:%s", jid);
        if (pv2_kv_get(c->aux, key)) { free(names[i]); continue; }   /* 已采集 */
        snprintf(key, sizeof key, "job:%s", jid);
        rec = pv2_kv_get(c->aux, key);
        if (!rec) { free(names[i]); continue; }               /* 非本桥登记的任务 */

        snprintf(path, sizeof path, "%s/%s", queue_dir, names[i]);
        raw = ai_read_file(path, &rawlen, AI_MAX_JOB_BYTES);
        if (!raw) { free(names[i]); continue; }
        job = cJSON_Parse(raw);
        free(raw);
        if (!job) { free(names[i]); continue; }               /* 无法解析：不动它 */

        status = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(job, "status"));
        ai_rec_field(rec, 1, rec_kind, sizeof rec_kind);
        ai_rec_field(rec, 2, rec_ref, sizeof rec_ref);

        if (status && (!strcmp(status, "succeeded") || !strcmp(status, "failed") ||
                       !strcmp(status, "canceled"))) {
            ai_sync_rec_state(c, jid, status);                /* GET /ai/jobs 不再撒谎 */
            if (!strcmp(status, "succeeded") && rec_kind[0] &&
                !strcmp(rec_kind, "briefing") && !strncmp(rec_ref, "brief-", 6) &&
                strlen(rec_ref) > 10) {
                result = cJSON_GetObjectItemCaseSensitive(job, "result");
                if (cJSON_IsObject(result)) {
                    /* base_rev = 提交时冻结的乐观基线（旧任务文件无此字段 → 0） */
                    cJSON *br_j = cJSON_GetObjectItemCaseSensitive(job, "base_rev");
                    long base_rev = (br_j && cJSON_IsNumber(br_j))
                                        ? (long)br_j->valuedouble : 0;
                    ai_writeback_briefing(c, jid, rec_ref + 6, result, base_rev);
                }
                else {
                    char k2[80];
                    ai_log("succeeded job without result object: %s", jid);
                    snprintf(k2, sizeof k2, "aidone:%s", jid);
                    pv2_kv_set(c->aux, k2, "1");              /* 无结果可写：标记防扫噪 */
                }
            } else if (strcmp(status, "succeeded") != 0) {
                char k2[80];
                snprintf(k2, sizeof k2, "aidone:%s", jid);
                pv2_kv_set(c->aux, k2, "1");                  /* failed/canceled：无回传 */
            }
            /* succeeded 但 kind 非 briefing：留待扫描（暂无回传路径，不假标） */
        }
        cJSON_Delete(job);
        free(names[i]);
    }
    free(names);
    return 0;
}

/* --------------------------------------------------------------- 对外入口 */
void ai_tick(pv2_ctx *c, const char *queue_dir)
{
    long long now_ms;
    if (!c || !c->aux || !c->store.h || !queue_dir || !*queue_dir) return;
    now_ms = (long long)time(NULL) * 1000LL;
    ai_collect_once(c, queue_dir);            /* 先回传：腾出队列视野 */
    ai_submit_due(c, queue_dir, now_ms);      /* 再提交：当日仍缺才补 */
}
