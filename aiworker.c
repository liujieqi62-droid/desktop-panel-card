/* aiworker.c - TRAE-M6A: 板端独立 AI worker 进程。
 *
 * 独立于 panel-service 的单线程进程（Marves L117），消费文件队列并调用
 * DeepSeek chat/completions；service/LVGL 永不等待 API。
 *
 * 隔离与安全（完成判据）：
 *   - 绝不打开 pv2 层任何文件（store/journal/aux KV 均无跨进程锁）；
 *     队列 = <v2dir>/ai/queue/<job_id>.job.json，一任务一文件，tmp+rename 原子写。
 *   - 密钥独立文件 <v2dir>/auth/ai-deepseek.key（0600 单行）；每个任务执行时读取，
 *     只打 "key loaded (len=N)" 日志；Authorization 仅在请求缓冲区内构造，绝不落日志。
 *   - 业务快照 = 任务文件 payload 字段，原样转发；本文件不读 sqlite/journal/业务头。
 *   - TLS fail-closed：VERIFY_REQUIRED + set_hostname；证书/验证失败 → retryable
 *     tls_error（attempts 上限兜底）。请求构造与解析逐字复刻 M4A DeepSeek.java。
 *   - 无 shell 拼接：连接与密钥全部走库接口传参。
 *
 * 队列状态机：queued(next_retry_ms 到期才取) → running → succeeded|failed；
 *   retryable 失败 → attempts+1、next_retry_ms=now+1s/2s/4s ± ≤1s 抖动、回 queued；
 *   attempts ≥ 3 → failed（error_code 记最后一次原因）。并发 1：单进程顺序处理，
 *   扫目录按文件名排序。running 状态在扫描时视为上次崩溃残留 → 重置回 queued。
 *
 * 构建：见 scripts/build.ps1 target aiworker（ARM 静态链接 + 桌面 x64 验证版）。
 * 桌面降级：-DAIW_NO_TLS 时仅支持 http:// base（队列/重试/解析矩阵照常验证，
 * TLS 真值以 T06B ARM 实测为准）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>

#include "cJSON.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <poll.h>
#endif

#ifndef AIW_NO_TLS
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/error.h>
#include <psa/crypto.h>
#endif

/* ------------------------------------------------------------------ 常量 */
#define AIW_DEFAULT_BASE          "https://api.deepseek.com"
#define AIW_MODEL_DEFAULT         "deepseek-flash"
#define AIW_MAX_FIELD_CHARS       500      /* DeepSeek.java MAX_FIELD_CHARS */
#define AIW_DEFAULT_CONNECT_TIMEOUT_S 10   /* 连接超时（--connect-timeout-s 可覆盖） */
#define AIW_DEFAULT_DEADLINE_S        90   /* 单任务总体 deadline（--deadline-s 可覆盖） */
#define AIW_MAX_ATTEMPTS              3    /* 重试上限：attempts≥3 → failed */
#define AIW_DEFAULT_SCAN_MS        2000    /* loop 模式扫描间隔 */
#define AIW_MAX_RESPONSE_BYTES     (64*1024)  /* 响应上限 */
#define AIW_MAX_JOB_BYTES          (256*1024) /* 任务文件读取上限 */
#define AIW_KEY_LOG_INTERVAL_MS    30000   /* 密钥缺失限频日志 */

#ifndef O_BINARY
#define O_BINARY 0
#endif

/* ------------------------------------------------------------------ 平台垫片 */
#ifdef _WIN32
typedef SOCKET aiw_sock_t;
#define AIW_SOCK_INVALID INVALID_SOCKET
static int aiw_sock_close(aiw_sock_t s) { closesocket(s); return 0; }
static int aiw_sock_nonblock(aiw_sock_t s, int on)
{
    u_long m = (u_long)on;
    return ioctlsocket(s, (long)FIONBIO, &m) == 0 ? 0 : -1;
}
static int aiw_sock_err(void) { return (int)WSAGetLastError(); }
static int aiw_poll(struct pollfd *fds, size_t n, int timeout_ms)
{
    return WSAPoll(fds, (ULONG)n, timeout_ms);
}
static long long aiw_wall_ms(void)
{
    FILETIME ft;
    ULONGLONG q;
    GetSystemTimeAsFileTime(&ft);
    q = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    /* 1601-01-01 → 1970-01-01 */
    return (long long)(q / 10000ULL) - 11644473600000LL;
}
static int aiw_getpid(void) { return (int)GetCurrentProcessId(); }
static void aiw_sleep_ms(int ms) { Sleep((DWORD)ms); }
static int aiw_atomic_replace(const char *tmp, const char *path)
{
    /* zig mingw rename() 严格 POSIX：目标已存在即 EEXIST，不用它。 */
    wchar_t wtmp[640], wpath[640];
    if (MultiByteToWideChar(CP_UTF8, 0, tmp, -1, wtmp, 640) == 0) return -1;
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 640) == 0) return -1;
    if (!MoveFileExW(wtmp, wpath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return -1;
    return 0;
}
#else
typedef int aiw_sock_t;
#define AIW_SOCK_INVALID (-1)
static int aiw_sock_close(aiw_sock_t s) { close(s); return 0; }
static int aiw_sock_nonblock(aiw_sock_t s, int on)
{
    int fl = fcntl(s, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(s, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK)) == 0 ? 0 : -1;
}
static int aiw_sock_err(void) { return errno; }
static int aiw_poll(struct pollfd *fds, size_t n, int timeout_ms)
{
    return poll(fds, n, timeout_ms);
}
static long long aiw_wall_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0)
        return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
    return (long long)time(NULL) * 1000LL;
}
static int aiw_getpid(void) { return (int)getpid(); }
static void aiw_sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
static int aiw_atomic_replace(const char *tmp, const char *path)
{
    return rename(tmp, path);
}
#endif

/* ------------------------------------------------------------------ 日志 */
static long long g_log_now;                 /* 每轮扫描刷新，减少时钟调用 */
static void aiw_log(const char *fmt, ...)
{
    char stamp[32];
    time_t t = (time_t)(g_log_now / 1000);
    struct tm tmv;
    va_list ap;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(stamp, sizeof stamp, "%H:%M:%S", &tmv);
    printf("[%s.%03d aiworker %d] ", stamp, (int)(g_log_now % 1000), aiw_getpid());
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

/* 简易抖动源（仅用于退避抖动，非安全用途）。 */
static uint32_t g_jitter_state;
static void aiw_jitter_seed(void)
{
    g_jitter_state = (uint32_t)time(NULL) ^ (uint32_t)aiw_getpid() ^ 0xA1B2C3D4u;
    if (g_jitter_state == 0) g_jitter_state = 0x9E3779B9u;
}
static uint32_t aiw_jitter_next(void)
{
    uint32_t x = g_jitter_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g_jitter_state = x ? x : 0x9E3779B9u;
    return x;
}

/* ------------------------------------------------------------------ 字符串工具 */
static char *aiw_strdup_n(const char *s, size_t n)
{
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static void aiw_trim_ascii(const char **begin, size_t *len)
{
    while (*len > 0 && isspace((unsigned char)**begin)) { (*begin)++; (*len)--; }
    while (*len > 0 && isspace((unsigned char)(*begin)[*len - 1])) (*len)--; 
}

static const char *aiw_trim_ptr(const char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

/* 与 Java String.length() 对齐的 UTF-16 单元数（BMP=1，增补平面=2）。 */
static size_t aiw_utf16_len(const char *s, size_t len)
{
    size_t units = 0, i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { i += 1; units += 1; }
        else if ((c & 0xE0) == 0xC0) { i += 2; units += 1; }
        else if ((c & 0xF0) == 0xE0) { i += 3; units += 1; }
        else if ((c & 0xF8) == 0xF0) { i += 4; units += 2; }
        else { i += 1; units += 1; }        /* 非法字节按 1 记，避免死循环 */
    }
    return units;
}

/* 有界大小写不敏感查找（HTTP 头解析用，替代 GNU memmem/strncasecmp 依赖）。 */
static const char *aiw_casestr_n(const char *hay, size_t haylen, const char *needle)
{
    size_t n = strlen(needle);
    if (haylen < n) return NULL;
    for (size_t i = 0; i + n <= haylen; i++) {
        size_t j = 0;
        while (j < n &&
               tolower((unsigned char)hay[i + j]) == tolower((unsigned char)needle[j]))
            j++;
        if (j == n) return hay + i;
    }
    return NULL;
}

static void *aiw_memmem(const void *hay, size_t haylen, const void *needle, size_t nlen)
{
    const char *h = (const char *)hay;
    if (nlen == 0 || haylen < nlen) return NULL;
    for (size_t i = 0; i + nlen <= haylen; i++)
        if (memcmp(h + i, needle, nlen) == 0) return (void *)(h + i);
    return NULL;
}

/* ------------------------------------------------------------------ URL 解析 */
typedef struct {
    int is_https;
    char host[256];
    char port[16];
    char path[512];     /* 含前导 '/'，可为 "/" */
} aiw_url;

static int aiw_url_parse(const char *text, aiw_url *out)
{
    const char *rest, *slash, *p;
    size_t n;
    memset(out, 0, sizeof *out);
    if (strncmp(text, "https://", 8) == 0) { out->is_https = 1; rest = text + 8; }
    else if (strncmp(text, "http://", 7) == 0) { out->is_https = 0; rest = text + 7; }
    else return -1;
    slash = strchr(rest, '/');
    n = slash ? (size_t)(slash - rest) : strlen(rest);
    if (n == 0) return -1;
    /* host[:port]；IPv6 字面量仅按方括号形式支持 */
    if (rest[0] == '[') {
        const char *rb = memchr(rest, ']', n);
        if (!rb) return -1;
        size_t hl = (size_t)(rb - rest) - 1;      /* 去掉 '[' 与 ']' */
        if (hl == 0 || hl >= sizeof out->host) return -1;
        memcpy(out->host, rest + 1, hl);
        if (rb + 1 < rest + n && rb[1] == ':') {
            size_t pl = rest + n - (rb + 2);
            if (pl == 0 || pl >= sizeof out->port) return -1;
            memcpy(out->port, rb + 2, pl);
        } else {
            snprintf(out->port, sizeof out->port, "%d", out->is_https ? 443 : 80);
        }
    } else {
        const char *colon = memchr(rest, ':', n);
        p = colon ? colon : rest + n;
        if (p == rest || (size_t)(p - rest) >= sizeof out->host) return -1;
        memcpy(out->host, rest, (size_t)(p - rest));
        if (colon) {
            size_t pl = rest + n - (colon + 1);
            if (pl == 0 || pl >= sizeof out->port) return -1;
            memcpy(out->port, colon + 1, pl);
        } else {
            snprintf(out->port, sizeof out->port, "%d", out->is_https ? 443 : 80);
        }
    }
    if (slash) {
        size_t pl = strlen(slash);
        if (pl >= sizeof out->path) return -1;
        memcpy(out->path, slash, pl + 1);
    } else {
        snprintf(out->path, sizeof out->path, "/");
    }
    return 0;
}

/* ------------------------------------------------------------------ 密钥 */
/* 读 <v2dir>/auth/ai-deepseek.key（0600 单行）；只返回内容与长度，绝不打印。 */
static int aiw_load_key(const char *path, char *buf, size_t bufsz, size_t *out_len)
{
    int fd = open(path, O_RDONLY | O_BINARY);
    ssize_t got;
    size_t len;
    const char *b;
    if (fd < 0) return -1;
    got = read(fd, buf, bufsz - 1);
    close(fd);
    if (got <= 0) return -1;
    buf[got] = '\0';
    len = (size_t)got;
    b = buf;
    aiw_trim_ascii(&b, &len);               /* 去掉行尾 \r\n 与首尾空白 */
    if (b != buf) memmove(buf, b, len);
    buf[len] = '\0';
    *out_len = len;
    return len == 0 ? -1 : 0;
}

/* ------------------------------------------------------------------ 任务文件读写 */
static char *aiw_read_file(const char *path, size_t *out_len, size_t cap)
{
    int fd = open(path, O_RDONLY | O_BINARY);
    char *buf;
    ssize_t got;
    size_t total = 0;
    if (fd < 0) return NULL;
    buf = (char *)malloc(cap + 1);
    if (!buf) { close(fd); return NULL; }
    while (total < cap) {
        got = read(fd, buf + total, cap - total);
        if (got < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (got == 0) break;
        total += (size_t)got;
    }
    close(fd);
    if (total == 0) { free(buf); return NULL; }
    buf[total] = '\0';
    *out_len = total;
    return buf;
}

/* 原子写：tmp + rename（Windows 走 MoveFileExW REPLACE_EXISTING）。 */
static int aiw_write_file_atomic(const char *path, const char *data, size_t len)
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
    if (aiw_atomic_replace(tmp, path) != 0) return -1;
    return 0;
}

/* ------------------------------------------------------------------ 队列扫描 */
/* 列出 <dir> 下 *.job.json，按文件名排序；返回 malloc 的名字数组。 */
static int aiw_name_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int aiw_list_jobs(const char *dir, char ***out_names, size_t *out_n)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char **names = NULL;
    size_t n = 0, cap = 0;
    *out_names = NULL;
    *out_n = 0;
    if (!d) return -1;                       /* 目录不存在：本轮无任务可扫 */
    while ((e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);
        if (l < 10 || strcmp(e->d_name + l - 9, ".job.json") != 0) continue;
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 16;
            char **nn = (char **)realloc(names, nc * sizeof *nn);
            if (!nn) break;
            names = nn; cap = nc;
        }
        names[n] = aiw_strdup_n(e->d_name, l);
        if (names[n]) n++;
    }
    closedir(d);
    if (n) qsort(names, n, sizeof *names, aiw_name_cmp);
    *out_names = names;
    *out_n = n;
    return 0;
}

/* ------------------------------------------------------------------ cJSON 辅助 */
static const char *aiw_jstr(cJSON *o, const char *key)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(it) ? it->valuestring : NULL;
}

static void aiw_jset_str(cJSON *o, const char *key, const char *val)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    if (it) cJSON_SetValuestring(it, val);
    else cJSON_AddStringToObject(o, key, val);
}

static void aiw_jdel(cJSON *o, const char *key)
{
    cJSON_DeleteItemFromObjectCaseSensitive(o, key);
}

/* ------------------------------------------------------------------ 请求构造（复刻 M4A DeepSeek.java，UTF-8 逐字） */
/* note 请求：只含单词文本；绝不携带 PIN/主机地址/学习历史。 */
static char *aiw_note_request(const char *model, const char *word)
{
    char sys[768];
    char *user;
    size_t ulen;
    cJSON *req, *msgs, *m, *rf;
    char *out;
    snprintf(sys, sizeof sys,
        "你是词典助手。只输出一个 JSON 对象，不要输出任何其他文字。"
        "字段：\"pos\" 词性；\"phonetic\" 音标，没有就给空串；\"meaning\" 中文释义（"
        "%d 字以内）；\"example\" 一条英文例句及中文翻译；\"memo\" 一条记忆提示（"
        "%d 字以内）。", AIW_MAX_FIELD_CHARS, AIW_MAX_FIELD_CHARS);
    ulen = strlen(word) + 64;
    user = (char *)malloc(ulen);
    if (!user) return NULL;
    snprintf(user, ulen, "单词：%s", word);
    req = cJSON_CreateObject();
    if (!req) { free(user); return NULL; }
    cJSON_AddStringToObject(req, "model", model);
    msgs = cJSON_AddArrayToObject(req, "messages");
    m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "system");
    cJSON_AddStringToObject(m, "content", sys);
    cJSON_AddItemToArray(msgs, m);
    m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON_AddStringToObject(m, "content", user);
    cJSON_AddItemToArray(msgs, m);
    free(user);
    rf = cJSON_CreateObject();
    cJSON_AddStringToObject(rf, "type", "json_object");
    cJSON_AddItemToObject(req, "response_format", rf);
    cJSON_AddNumberToObject(req, "temperature", 0.3);
    cJSON_AddBoolToObject(req, "stream", 0);
    out = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return out;
}

/* briefing 请求：facts 由提交方组装，worker 原样转发，绝不添加数据。 */
static char *aiw_briefing_request(const char *model, const char *facts_json)
{
    char sys[896];
    char *user;
    size_t ulen;
    cJSON *req, *msgs, *m, *rf;
    char *out;
    snprintf(sys, sizeof sys,
        "你是每日简报助手。只输出一个 JSON 对象，不要输出任何其他文字。"
        "字段：\"plan\" 今日学习安排（必填，%d 字以内）；"
        "\"review\" 复习提醒；\"weather\" 天气一句话——若事实里天气为空就写\"暂无\"，"
        "绝不要编造温度或预报；\"tip\" 一条简短学习建议。每个字段 %d 字以内，语气自然简洁。",
        AIW_MAX_FIELD_CHARS, AIW_MAX_FIELD_CHARS);
    ulen = strlen(facts_json) + 64;
    user = (char *)malloc(ulen);
    if (!user) return NULL;
    snprintf(user, ulen, "今日事实：%s", facts_json);
    req = cJSON_CreateObject();
    if (!req) { free(user); return NULL; }
    cJSON_AddStringToObject(req, "model", model);
    msgs = cJSON_AddArrayToObject(req, "messages");
    m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "system");
    cJSON_AddStringToObject(m, "content", sys);
    cJSON_AddItemToArray(msgs, m);
    m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON_AddStringToObject(m, "content", user);
    cJSON_AddItemToArray(msgs, m);
    free(user);
    rf = cJSON_CreateObject();
    cJSON_AddStringToObject(rf, "type", "json_object");
    cJSON_AddItemToObject(req, "response_format", rf);
    cJSON_AddNumberToObject(req, "temperature", 0.3);
    cJSON_AddBoolToObject(req, "stream", 0);
    out = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return out;
}

/* HTTP 请求缓冲区构造：Bearer 凭据只出现在这里，本函数内禁止任何日志输出。 */
static int aiw_build_http_request(char *buf, size_t bufsz, const char *host_hdr,
                                  const char *path, const char *key,
                                  const char *body, size_t *out_len)
{
    int n = snprintf(buf, bufsz,
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Authorization: Bearer %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        path, host_hdr, key, (int)strlen(body), body);
    if (n < 0 || (size_t)n >= bufsz) return -1;
    *out_len = (size_t)n;
    return 0;
}

/* ------------------------------------------------------------------ deadline 工具 */
typedef struct { long long deadline_ms; } aiw_deadline;

static int aiw_deadline_left(aiw_deadline *dl)
{
    long long left = dl->deadline_ms - aiw_wall_ms();
    return left > 0 ? (int)left : 0;
}

/* poll 单 fd 等待 events，返回 1 就绪 / 0 超时 / -1 错误；全程扣减 deadline。 */
static int aiw_wait_fd(aiw_sock_t s, short events, aiw_deadline *dl)
{
    for (;;) {
        int left = aiw_deadline_left(dl);
        struct pollfd pfd;
        int r;
        if (left <= 0) return 0;
        pfd.fd = s;
        pfd.events = events;
        pfd.revents = 0;
        r = aiw_poll(&pfd, 1, left);
        if (r < 0) {
#ifdef _WIN32
            if (aiw_sock_err() == WSAEINTR) continue;
#else
            if (errno == EINTR) continue;
#endif
            return -1;
        }
        if (r == 0) return 0;
        return (pfd.revents & events) ? 1 : -1;
    }
}

/* ------------------------------------------------------------------ TCP 连接（非阻塞 connect + poll，超时可配） */
static aiw_sock_t aiw_tcp_connect(const char *host, const char *port, int timeout_ms)
{
    struct addrinfo hints, *res = NULL, *rp;
    aiw_sock_t sock = AIW_SOCK_INVALID;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) return AIW_SOCK_INVALID;
    for (rp = res; rp; rp = rp->ai_next) {
        aiw_sock_t s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        int err = 0;
        struct pollfd pfd;
        int soerr = 0;
        socklen_t slen = sizeof soerr;
        if (s == AIW_SOCK_INVALID) continue;
        if (aiw_sock_nonblock(s, 1) != 0) { aiw_sock_close(s); continue; }
        if (connect(s, rp->ai_addr, rp->ai_addrlen) == 0) { sock = s; break; }
        err = aiw_sock_err();
#ifdef _WIN32
        if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) { aiw_sock_close(s); continue; }
#else
        if (err != EINPROGRESS) { aiw_sock_close(s); continue; }
#endif
        pfd.fd = s;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        if (aiw_poll(&pfd, 1, timeout_ms) <= 0) { aiw_sock_close(s); continue; }
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soerr, &slen) != 0 || soerr != 0) {
            aiw_sock_close(s);
            continue;
        }
        sock = s;
        break;
    }
    freeaddrinfo(res);
    return sock;
}

#ifndef AIW_NO_TLS
/* ------------------------------------------------------------------ TLS 会话（fail-closed） */
typedef struct {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt cacert;
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;
    mbedtls_net_context net;
    aiw_sock_t sock;
    char handshake_err[192];
    char last_rc[160];                       /* 最近一次 mbedTLS 错误描述（诊断用） */
} aiw_tls;

static void aiw_tls_free(aiw_tls *t)
{
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_x509_crt_free(&t->cacert);
    mbedtls_ctr_drbg_free(&t->drbg);
    mbedtls_entropy_free(&t->entropy);
    if (t->sock != AIW_SOCK_INVALID) { aiw_sock_close(t->sock); t->sock = AIW_SOCK_INVALID; }
}

/* 连接 + 握手：证书/主机名验证失败 → 返回 tls_error（retryable，attempts 兜底）。 */
static int aiw_tls_connect(aiw_tls *t, const aiw_url *url, const char *ca_path,
                           int connect_timeout_s, aiw_deadline *dl, const char **err)
{
    const char *pers = "panel-aiworker";
    int rc, left;
    memset(t, 0, sizeof *t);
    t->sock = AIW_SOCK_INVALID;
    mbedtls_net_init(&t->net);
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_x509_crt_init(&t->cacert);
    mbedtls_ctr_drbg_init(&t->drbg);
    mbedtls_entropy_init(&t->entropy);
    if (psa_crypto_init() != PSA_SUCCESS) { *err = "psa init failed"; goto fail; }
    rc = mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->entropy,
                               (const unsigned char *)pers, strlen(pers));
    if (rc != 0) { *err = "drbg seed failed"; goto fail; }
    rc = mbedtls_x509_crt_parse_file(&t->cacert, ca_path);
    if (rc != 0) { *err = "ca parse failed"; goto fail; }
    rc = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) { *err = "conf defaults failed"; goto fail; }
    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->drbg);
    mbedtls_ssl_conf_ca_chain(&t->conf, &t->cacert, NULL);
    mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    /* 钉在 TLS 1.2（checkpoint 冻结设计）。实测：3.6 默认开启 TLS 1.3，握手后
     * 服务端 NewSessionTicket 会让 ssl_read 返回实验性错误码而非数据，必须显式钉死。 */
    mbedtls_ssl_conf_max_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    rc = mbedtls_ssl_setup(&t->ssl, &t->conf);
    if (rc != 0) { *err = "ssl setup failed"; goto fail; }
    rc = mbedtls_ssl_set_hostname(&t->ssl, url->host);
    if (rc != 0) { *err = "set hostname failed"; goto fail; }
    left = aiw_deadline_left(dl);
    if (connect_timeout_s > 0 && connect_timeout_s * 1000 < left)
        left = connect_timeout_s * 1000;
    if (left <= 0) { *err = "connect timeout"; goto fail; }
    t->sock = aiw_tcp_connect(url->host, url->port, left);
    if (t->sock == AIW_SOCK_INVALID) { *err = "connect timeout"; goto fail; }
    t->net.fd = (int)t->sock;
    mbedtls_ssl_set_bio(&t->ssl, &t->net, mbedtls_net_send, mbedtls_net_recv, NULL);
    while ((rc = mbedtls_ssl_handshake(&t->ssl)) != 0) {
        if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
            if (aiw_wait_fd(t->sock, POLLIN, dl) <= 0) { *err = "handshake timeout"; goto fail; }
        } else if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (aiw_wait_fd(t->sock, POLLOUT, dl) <= 0) { *err = "handshake timeout"; goto fail; }
        } else {
            char ebuf[160];
            mbedtls_strerror(rc, ebuf, sizeof ebuf);
            snprintf(t->handshake_err, sizeof t->handshake_err, "handshake: %s", ebuf);
            *err = t->handshake_err;
            goto fail;
        }
    }
    return 0;
fail:
    return -1;
}

/* mbedTLS 读写循环（WANT_* → poll 剩余 deadline；超时 → 取消本地等待）。 */
static int aiw_tls_send_all(aiw_tls *t, const unsigned char *buf, size_t len,
                            aiw_deadline *dl)
{
    size_t off = 0;
    while (off < len) {
        int rc = mbedtls_ssl_write(&t->ssl, buf + off, len - off);
        if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (aiw_wait_fd(t->sock, POLLOUT, dl) <= 0) return -1;
        } else if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
            if (aiw_wait_fd(t->sock, POLLIN, dl) <= 0) return -1;
        } else if (rc <= 0) {
            return -1;
        } else {
            off += (size_t)rc;
        }
    }
    return 0;
}

/* 返回读到的字节数；0 = 对端关闭；-1 = 错误/超时（详情记 t->last_rc）。 */
static int aiw_tls_read_some(aiw_tls *t, unsigned char *buf, size_t bufsz,
                             aiw_deadline *dl)
{
    for (;;) {
        int rc = mbedtls_ssl_read(&t->ssl, buf, bufsz);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
            if (aiw_wait_fd(t->sock, POLLIN, dl) <= 0) {
                snprintf(t->last_rc, sizeof t->last_rc, "poll POLLIN timeout/error");
                return -1;
            }
        } else if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (aiw_wait_fd(t->sock, POLLOUT, dl) <= 0) {
                snprintf(t->last_rc, sizeof t->last_rc, "poll POLLOUT timeout/error");
                return -1;
            }
        } else if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == 0) {
            return 0;
        } else if (rc < 0) {
            mbedtls_strerror(rc, t->last_rc, sizeof t->last_rc);
            return -1;
        } else {
            return rc;
        }
    }
}
#endif /* AIW_NO_TLS */

/* ------------------------------------------------------------------ HTTP 明文通道（仅 AIW_NO_TLS 桌面降级构建） */
#ifdef AIW_NO_TLS
typedef struct {
    aiw_sock_t sock;
} aiw_plain;

static int aiw_plain_connect(aiw_plain *p, const aiw_url *url, int connect_timeout_s,
                             aiw_deadline *dl)
{
    int left = aiw_deadline_left(dl);
    if (left <= 0) return -1;
    if (connect_timeout_s > 0 && connect_timeout_s * 1000 < left)
        left = connect_timeout_s * 1000;
    p->sock = aiw_tcp_connect(url->host, url->port, left);
    return p->sock == AIW_SOCK_INVALID ? -1 : 0;
}

static int aiw_plain_send_all(aiw_plain *p, const unsigned char *buf, size_t len,
                              aiw_deadline *dl)
{
    size_t off = 0;
    while (off < len) {
        ssize_t put;
#ifdef _WIN32
        put = send(p->sock, (const char *)buf + off, (int)(len - off), 0);
#else
        put = send(p->sock, buf + off, len - off, 0);
#endif
        if (put < 0) {
#ifdef _WIN32
            if (aiw_sock_err() == WSAEWOULDBLOCK) {
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
#endif
                if (aiw_wait_fd(p->sock, POLLOUT, dl) <= 0) return -1;
                continue;
            }
            return -1;
        }
        off += (size_t)put;
    }
    return 0;
}

static int aiw_plain_read_some(aiw_plain *p, unsigned char *buf, size_t bufsz,
                               aiw_deadline *dl)
{
    for (;;) {
        ssize_t got;
#ifdef _WIN32
        got = recv(p->sock, (char *)buf, (int)bufsz, 0);
#else
        got = recv(p->sock, buf, bufsz, 0);
#endif
        if (got < 0) {
#ifdef _WIN32
            if (aiw_sock_err() == WSAEWOULDBLOCK) {
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
#endif
                if (aiw_wait_fd(p->sock, POLLIN, dl) <= 0) return -1;
                continue;
            }
            return -1;
        }
        return (int)got;                     /* 0 = 对端关闭 */
    }
}
#endif /* AIW_NO_TLS */

/* ------------------------------------------------------------------ HTTP POST */
typedef struct {
    int status;
    char *body;                              /* malloc，调用方 free */
    size_t body_len;
} aiw_http_resp;

/* 最小 chunked 解码：就地把 <hex len>\r\n 片段拼接，返回解码长度。 */
static size_t aiw_chunked_decode(char *buf, size_t len)
{
    char *out = buf;
    size_t pos = 0;
    while (pos < len) {
        char *eol = (char *)memchr(buf + pos, '\n', len - pos);
        unsigned long long clen;
        if (!eol) break;
        clen = strtoull(buf + pos, NULL, 16);
        pos = (size_t)(eol - buf) + 1;
        if (clen == 0 || pos + clen > len) break;
        memmove(out, buf + pos, clen);
        out += clen;
        pos += clen + 2;                     /* 跳过片段后的 \r\n */
    }
    return (size_t)(out - buf);
}

/* 执行一次 POST；resp->body 由调用方 free。通道失败返回 -1 且 *err 置短描述
 * （无敏感信息）。超时分类：所有等待都受总体 deadline 约束。 */
static int aiw_http_post(const aiw_url *url, const char *key, const char *body,
                         const char *ca_path, int connect_timeout_s,
                         aiw_deadline *dl, aiw_http_resp *resp, const char **err)
{
    char *reqbuf;
    size_t reqlen, reqcap;
    char *rbuf;
    size_t rlen = 0, rcap = AIW_MAX_RESPONSE_BYTES + 4096;
    int done = 0, hdr_end = -1, hdrs_parsed = 0, chunked = 0;
    size_t want_total = 0;
    int default_port = url->is_https ? 443 : 80;
    int port_num = atoi(url->port);
    char host_hdr[560];
    if (port_num != default_port)
        snprintf(host_hdr, sizeof host_hdr, "%s:%s", url->host, url->port);
    else
        snprintf(host_hdr, sizeof host_hdr, "%s", url->host);
    reqcap = strlen(body) + 2048;
    reqbuf = (char *)malloc(reqcap);
    if (!reqbuf) { *err = "oom"; return -1; }
    if (aiw_build_http_request(reqbuf, reqcap, host_hdr, url->path, key, body,
                               &reqlen) != 0) {
        free(reqbuf);
        *err = "request build failed";
        return -1;
    }
    rbuf = (char *)malloc(rcap);
    if (!rbuf) { free(reqbuf); *err = "oom"; return -1; }
    resp->body = NULL;
    resp->body_len = 0;
    resp->status = 0;
#ifdef AIW_NO_TLS
    {
        aiw_plain chan;
#define AIW_PLAIN_FAIL(e) do { *err = (e); aiw_sock_close(chan.sock); goto out_err; } while (0)
        chan.sock = AIW_SOCK_INVALID;
        if (url->is_https) AIW_PLAIN_FAIL("https unavailable in NO_TLS build");
        if (aiw_plain_connect(&chan, url, connect_timeout_s, dl) != 0)
            AIW_PLAIN_FAIL("connect timeout");
        if (aiw_plain_send_all(&chan, (unsigned char *)reqbuf, reqlen, dl) != 0)
            AIW_PLAIN_FAIL("send timeout");
        while (!done) {
            int got;
            if (rlen >= rcap) AIW_PLAIN_FAIL("response too large");
            got = aiw_plain_read_some(&chan, (unsigned char *)rbuf + rlen, rcap - rlen, dl);
            if (got < 0) AIW_PLAIN_FAIL("read timeout");
            if (got == 0) { done = 1; break; }
            rlen += (size_t)got;
            if (hdr_end < 0) {
                char *p = (char *)aiw_memmem(rbuf, rlen, "\r\n\r\n", 4);
                if (p) hdr_end = (int)(p - rbuf) + 4;
            }
            if (hdr_end >= 0 && !hdrs_parsed) {
                const char *cl, *te;
                hdrs_parsed = 1;
                if (rlen > 12 && strncmp(rbuf, "HTTP/", 5) == 0)
                    resp->status = atoi(rbuf + 9);
                cl = aiw_casestr_n(rbuf, (size_t)hdr_end, "content-length:");
                if (cl) {
                    long v = atol(cl + 15);
                    if (v >= 0) want_total = (size_t)hdr_end + (size_t)v;
                }
                te = aiw_casestr_n(rbuf, (size_t)hdr_end, "transfer-encoding:");
                if (te && aiw_casestr_n(te, (size_t)hdr_end - (size_t)(te - rbuf), "chunked"))
                    chunked = 1;
            }
            if (hdrs_parsed) {
                if (want_total && rlen >= want_total) { done = 1; break; }
                if (chunked && rlen >= 5 &&
                    strcmp(rbuf + rlen - 5, "0\r\n\r\n") == 0) { done = 1; break; }
            }
        }
        aiw_sock_close(chan.sock);
#undef AIW_PLAIN_FAIL
    }
#else
    {
        aiw_tls chan;
#define AIW_TLS_FAIL(e) do { if (e && !*err) *err = (e); aiw_tls_free(&chan); goto out_err; } while (0)
        if (aiw_tls_connect(&chan, url, ca_path, connect_timeout_s, dl, err) != 0)
            AIW_TLS_FAIL("tls connect failed");
        if (aiw_tls_send_all(&chan, (unsigned char *)reqbuf, reqlen, dl) != 0)
            AIW_TLS_FAIL("send timeout");
        while (!done) {
            int got;
            if (rlen >= rcap) AIW_TLS_FAIL("response too large");
            got = aiw_tls_read_some(&chan, (unsigned char *)rbuf + rlen, rcap - rlen, dl);
            if (got < 0) {
                snprintf(chan.handshake_err, sizeof chan.handshake_err,
                         "read: %s", chan.last_rc);
                AIW_TLS_FAIL(chan.handshake_err);
            }
            if (got == 0) { done = 1; break; }
            rlen += (size_t)got;
            if (hdr_end < 0) {
                char *p = (char *)aiw_memmem(rbuf, rlen, "\r\n\r\n", 4);
                if (p) hdr_end = (int)(p - rbuf) + 4;
            }
            if (hdr_end >= 0 && !hdrs_parsed) {
                const char *cl, *te;
                hdrs_parsed = 1;
                if (rlen > 12 && strncmp(rbuf, "HTTP/", 5) == 0)
                    resp->status = atoi(rbuf + 9);
                cl = aiw_casestr_n(rbuf, (size_t)hdr_end, "content-length:");
                if (cl) {
                    long v = atol(cl + 15);
                    if (v >= 0) want_total = (size_t)hdr_end + (size_t)v;
                }
                te = aiw_casestr_n(rbuf, (size_t)hdr_end, "transfer-encoding:");
                if (te && aiw_casestr_n(te, (size_t)hdr_end - (size_t)(te - rbuf), "chunked"))
                    chunked = 1;
            }
            if (hdrs_parsed) {
                if (want_total && rlen >= want_total) { done = 1; break; }
                if (chunked && rlen >= 5 &&
                    strcmp(rbuf + rlen - 5, "0\r\n\r\n") == 0) { done = 1; break; }
            }
        }
        aiw_tls_free(&chan);
#undef AIW_TLS_FAIL
    }
#endif
    free(reqbuf);

    if (!resp->status) { *err = "no status line"; free(rbuf); return -1; }
    if (hdr_end < 0) { *err = "no header terminator"; free(rbuf); return -1; }
    if (chunked)
        rlen = (size_t)hdr_end + aiw_chunked_decode(rbuf + hdr_end, rlen - (size_t)hdr_end);
    if (want_total && rlen > want_total) rlen = want_total;
    resp->body_len = rlen - (size_t)hdr_end;
    resp->body = (char *)malloc(resp->body_len + 1);
    if (!resp->body) { free(rbuf); *err = "oom"; return -1; }
    memcpy(resp->body, rbuf + hdr_end, resp->body_len);
    resp->body[resp->body_len] = '\0';
    free(rbuf);
    return 0;
out_err:
    /* 通道句柄在各失败分支内已自行关闭（AIW_PLAIN_FAIL / AIW_TLS_FAIL）。 */
    free(reqbuf);
    free(rbuf);
    return -1;
}

/* ------------------------------------------------------------------ 响应解析（逐字复刻 M4A 解析口径） */
typedef enum {
    AIW_PARSE_OK = 0,
    AIW_PARSE_EMPTY_CONTENT,
    AIW_PARSE_INVALID_JSON,
    AIW_PARSE_MISSING_FIELD,
    AIW_PARSE_FIELD_TOO_LONG
} aiw_parse_kind;

static const char *aiw_parse_name(aiw_parse_kind k)
{
    switch (k) {
    case AIW_PARSE_EMPTY_CONTENT:  return "EMPTY_CONTENT";
    case AIW_PARSE_INVALID_JSON:   return "INVALID_JSON";
    case AIW_PARSE_MISSING_FIELD:  return "MISSING_FIELD";
    case AIW_PARSE_FIELD_TOO_LONG: return "FIELD_TOO_LONG";
    default:                       return "";
    }
}

/* 剥 ```json ... ``` 围栏（DeepSeek.java stripFence 逐字语义）。 */
static char *aiw_strip_fence(const char *s)
{
    const char *t = s;
    size_t len = strlen(t);
    const char *first_nl, *inner, *end;
    size_t inner_len;
    aiw_trim_ascii(&t, &len);
    if (len < 3 || strncmp(t, "```", 3) != 0)
        return aiw_strdup_n(t, len);
    first_nl = (const char *)memchr(t, '\n', len);
    if (!first_nl) return aiw_strdup_n(t, len);
    inner = first_nl + 1;
    inner_len = len - (size_t)(inner - t);
    end = NULL;                              /* lastIndexOf("```") */
    if (inner_len >= 3) {
        size_t i = inner_len - 3;
        for (;;) {
            if (strncmp(inner + i, "```", 3) == 0) { end = inner + i; break; }
            if (i == 0) break;
            i--;
        }
    }
    {
        const char *b = inner;
        size_t bl = end ? (size_t)(end - inner) : inner_len;
        aiw_trim_ascii(&b, &bl);
        return aiw_strdup_n(b, bl);
    }
}

/* content 提取：失败返回 kind（Java 口径：外层非法 → INVALID_JSON，空 → EMPTY_CONTENT）。 */
static aiw_parse_kind aiw_extract_content(const char *resp_body, char **out_content)
{
    cJSON *root = cJSON_Parse(resp_body);
    cJSON *choices, *first, *message;
    const char *content;
    *out_content = NULL;
    if (!root) return AIW_PARSE_INVALID_JSON;
    choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        cJSON_Delete(root);
        return AIW_PARSE_EMPTY_CONTENT;
    }
    first = cJSON_GetArrayItem(choices, 0);
    message = cJSON_GetObjectItemCaseSensitive(first, "message");
    content = (cJSON_IsObject(message) && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(message, "content")))
              ? cJSON_GetObjectItemCaseSensitive(message, "content")->valuestring : NULL;
    if (!content) { cJSON_Delete(root); return AIW_PARSE_EMPTY_CONTENT; }
    {
        const char *b = content;
        size_t bl = strlen(b);
        aiw_trim_ascii(&b, &bl);
        if (bl == 0) { cJSON_Delete(root); return AIW_PARSE_EMPTY_CONTENT; }
    }
    *out_content = aiw_strip_fence(content);
    cJSON_Delete(root);
    return *out_content ? AIW_PARSE_OK : AIW_PARSE_INVALID_JSON;
}

/* usage 计量：缺失按 0（Java parseUsage 口径）。 */
static void aiw_parse_usage(const char *resp_body, cJSON *job)
{
    cJSON *root = cJSON_Parse(resp_body);
    cJSON *usage = NULL;
    double pt = 0, ct = 0;
    if (root) {
        usage = cJSON_GetObjectItemCaseSensitive(root, "usage");
        if (cJSON_IsObject(usage)) {
            cJSON *v;
            if ((v = cJSON_GetObjectItemCaseSensitive(usage, "prompt_tokens")) && cJSON_IsNumber(v)) pt = v->valuedouble;
            if ((v = cJSON_GetObjectItemCaseSensitive(usage, "completion_tokens")) && cJSON_IsNumber(v)) ct = v->valuedouble;
        }
        cJSON_Delete(root);
    }
    {
        cJSON *u = cJSON_CreateObject();
        cJSON_AddNumberToObject(u, "prompt_tokens", pt);
        cJSON_AddNumberToObject(u, "completion_tokens", ct);
        cJSON_AddItemToObject(job, "usage", u);
    }
}

/* note/briefing 字段校验：全字段 ≤500（UTF-16 口径），必填项缺失 → MISSING_FIELD。 */
static aiw_parse_kind aiw_check_fields(cJSON *o, const char **fields, int nfields,
                                       const char *required, cJSON *result,
                                       const char **detail)
{
    int i;
    for (i = 0; i < nfields; i++) {
        const char *v = aiw_jstr(o, fields[i]);
        size_t un = v ? aiw_utf16_len(v, strlen(v)) : 0;
        if (un > AIW_MAX_FIELD_CHARS) {
            static char d[64];
            snprintf(d, sizeof d, "%s 超过 %d 字", fields[i], AIW_MAX_FIELD_CHARS);
            *detail = d;
            return AIW_PARSE_FIELD_TOO_LONG;
        }
    }
    if (!aiw_jstr(o, required) || !*aiw_trim_ptr(aiw_jstr(o, required))) {
        *detail = required;
        return AIW_PARSE_MISSING_FIELD;
    }
    for (i = 0; i < nfields; i++)
        cJSON_AddStringToObject(result, fields[i],
                                aiw_jstr(o, fields[i]) ? aiw_jstr(o, fields[i]) : "");
    return AIW_PARSE_OK;
}

/* note 解析：meaning 必填。 */
static aiw_parse_kind aiw_parse_note(const char *payload, cJSON *result, const char **detail)
{
    cJSON *o = cJSON_Parse(payload);
    static const char *fields[] = { "pos", "phonetic", "meaning", "example", "memo" };
    aiw_parse_kind k;
    if (!o) { *detail = "注释不是合法 JSON"; return AIW_PARSE_INVALID_JSON; }
    k = aiw_check_fields(o, fields, 5, "meaning", result, detail);
    cJSON_Delete(o);
    return k;
}

/* briefing 解析：plan 必填；weather 空缺语义由提示词约束（"暂无"）。 */
static aiw_parse_kind aiw_parse_briefing(const char *payload, cJSON *result, const char **detail)
{
    cJSON *o = cJSON_Parse(payload);
    static const char *fields[] = { "plan", "review", "weather", "tip" };
    aiw_parse_kind k;
    if (!o) { *detail = "简报不是合法 JSON"; return AIW_PARSE_INVALID_JSON; }
    k = aiw_check_fields(o, fields, 4, "plan", result, detail);
    cJSON_Delete(o);
    return k;
}

/* ------------------------------------------------------------------ 任务处理 */
typedef struct {
    const char *v2dir;
    char queue_dir[1024];
    char key_path[1024];
    char ca_path[1024];
    const char *base;
    int once;
    int connect_timeout_s;
    int deadline_s;
    int scan_ms;
    long long last_key_log_ms;
} aiw_opts;

/* 通道/HTTP 错误 → error_code 分类（短码，无敏感信息）。 */
static const char *aiw_classify_transport(const char *err)
{
    if (!err) return "network";
    if (strncmp(err, "handshake", 9) == 0 ||
        strncmp(err, "ca ", 3) == 0 ||
        strncmp(err, "psa ", 4) == 0 ||
        strncmp(err, "ssl ", 4) == 0 ||
        strncmp(err, "set hostname", 12) == 0)
        return "tls_error";
    if (strstr(err, "timeout") || strstr(err, "deadline")) return "timeout";
    if (strncmp(err, "response too large", 18) == 0) return "response_too_large";
    return "network";
}

/* HTTP 状态 → 终态/可重试分类。返回 1 = retryable，0 = 终态（code 已填）。 */
static int aiw_classify_http(int status, const char **code)
{
    static char buf[16];
    if (status == 401) { *code = "unauthorized"; return 0; }
    if (status == 402) { *code = "payment_required"; return 0; }
    if (status == 400) { *code = "bad_request"; return 0; }
    if (status == 422) { *code = "schema_invalid"; return 0; }
    if (status == 429) { *code = "http_429"; return 1; }
    if (status >= 500 && status <= 599) { *code = "http_5xx"; return 1; }
    snprintf(buf, sizeof buf, "http_%d", status);
    *code = buf;
    return 1;                                 /* 其余状态按网络类退避（attempts 兜底） */
}

/* 终态/重试统一落盘；delay = 1s/2s/4s ± ≤1s 抖动。 */
static int aiw_finish_job(const char *path, cJSON *job, const char *status,
                          const char *code, const char *detail, int retryable)
{
    long long now = aiw_wall_ms();
    if (code && *code) aiw_jset_str(job, "error_code", code);
    else aiw_jdel(job, "error_code");
    if (detail && *detail) aiw_jset_str(job, "error_detail", detail);
    else aiw_jdel(job, "error_detail");
    if (retryable) {
        cJSON *at = cJSON_GetObjectItemCaseSensitive(job, "attempts");
        int attempts = (at && cJSON_IsNumber(at)) ? (int)at->valuedouble : 0;
        int shift = attempts - 1;             /* 1s/2s/4s */
        long long delay = (shift >= 0 && shift <= 2) ? (1000LL << shift) : 4000LL;
        cJSON *nr = cJSON_GetObjectItemCaseSensitive(job, "next_retry_ms");
        delay += (long long)(aiw_jitter_next() % 1000);   /* ≤1s 抖动 */
        if (nr && cJSON_IsNumber(nr)) cJSON_SetNumberValue(nr, (double)(now + delay));
        else cJSON_AddNumberToObject(job, "next_retry_ms", (double)(now + delay));
        aiw_jset_str(job, "status", "queued");
    } else {
        cJSON *f = cJSON_GetObjectItemCaseSensitive(job, "finished_ms");
        aiw_jset_str(job, "status", status);
        if (f && cJSON_IsNumber(f)) cJSON_SetNumberValue(f, (double)now);
        else cJSON_AddNumberToObject(job, "finished_ms", (double)now);
    }
    {
        char *txt = cJSON_PrintUnformatted(job);
        int rc;
        if (!txt) return -1;
        rc = aiw_write_file_atomic(path, txt, strlen(txt));
        free(txt);
        return rc;
    }
}

/* 处理单个任务文件；返回 0 = 已推进（终态或重排队），-1 = 文件层面跳过。 */
static int aiw_process_job(const char *dir, const char *name, aiw_opts *o)
{
    char path[1200];
    char *raw;
    size_t rawlen;
    cJSON *job = NULL;
    const char *status, *type, *executor, *model;
    cJSON *payload, *attempts_j, *nr_j;
    int attempts;
    long long now;
    const char *err = NULL;
    aiw_http_resp resp;
    char key[512];
    size_t key_len = 0;
    char *body = NULL;
    const char *code = NULL, *detail = NULL;
    aiw_deadline dl;
    int retryable;

    if (snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) return -1;
    raw = aiw_read_file(path, &rawlen, AIW_MAX_JOB_BYTES);
    if (!raw) return -1;                      /* 读不到：跳过 */
    job = cJSON_Parse(raw);
    free(raw);
    if (!job) {
        /* 无法解析的任务：绝不重写（无法保结构），限频日志后跳过。 */
        if (g_log_now - o->last_key_log_ms > AIW_KEY_LOG_INTERVAL_MS) {
            aiw_log("skip invalid job file: %s", name);
            o->last_key_log_ms = g_log_now;
        }
        return -1;
    }

    status = aiw_jstr(job, "status");
    type = aiw_jstr(job, "type");
    executor = aiw_jstr(job, "executor");
    payload = cJSON_GetObjectItemCaseSensitive(job, "payload");
    attempts_j = cJSON_GetObjectItemCaseSensitive(job, "attempts");
    attempts = (attempts_j && cJSON_IsNumber(attempts_j)) ? (int)attempts_j->valuedouble : 0;

    if (executor && strcmp(executor, "board") != 0) goto skip;   /* 非本端执行者 */
    if (!status) goto skip;
    if (strcmp(status, "running") == 0) {
        /* 单 worker 设计下 running 只可能是上次崩溃残留 → 重置回 queued。
         * 不经 finish_job：reset 不该带 finished_ms。 */
        char *txt;
        aiw_log("stale running job reset to queued: %s", name);
        aiw_jset_str(job, "status", "queued");
        status = "queued";
        txt = cJSON_PrintUnformatted(job);
        if (!txt || aiw_write_file_atomic(path, txt, strlen(txt)) != 0) {
            free(txt);
            aiw_log("write failed: %s", name);
            goto skip;
        }
        free(txt);
    }
    if (strcmp(status, "queued") != 0) goto skip;                /* succeeded/failed 终态 */
    if (attempts >= AIW_MAX_ATTEMPTS) {
        aiw_log("attempts cap on queued job, fail: %s", name);
        aiw_finish_job(path, job, "failed", "attempts_cap", NULL, 0);
        cJSON_Delete(job);
        return 0;
    }
    nr_j = cJSON_GetObjectItemCaseSensitive(job, "next_retry_ms");
    now = aiw_wall_ms();
    if (nr_j && cJSON_IsNumber(nr_j) && (long long)nr_j->valuedouble > now) goto skip;

    if (!type || (strcmp(type, "note") != 0 && strcmp(type, "briefing") != 0)) {
        aiw_finish_job(path, job, "failed", "bad_payload", "type must be note|briefing", 0);
        aiw_log("job %s failed bad_payload (type)", name);
        cJSON_Delete(job);
        return 0;
    }
    if (!cJSON_IsObject(payload)) {
        aiw_finish_job(path, job, "failed", "bad_payload", "payload object missing", 0);
        aiw_log("job %s failed bad_payload (payload)", name);
        cJSON_Delete(job);
        return 0;
    }

    /* 密钥每任务执行时读取；缺失 → 本任务跳过（留 queued 不烧掉），限频日志。 */
    if (aiw_load_key(o->key_path, key, sizeof key, &key_len) != 0) {
        if (g_log_now - o->last_key_log_ms > AIW_KEY_LOG_INTERVAL_MS) {
            aiw_log("key not available, skip (job stays queued): %s", name);
            o->last_key_log_ms = g_log_now;
        }
        goto skip;
    }
    aiw_log("key loaded (len=%d), start job %s type=%s", (int)key_len, name, type);

    /* 请求体构造：note 只含单词文本；briefing 只转发 facts（提交方组装）。 */
    if (strcmp(type, "note") == 0) {
        const char *word = aiw_jstr(payload, "word");
        if (!word || !*word) {
            aiw_finish_job(path, job, "failed", "bad_payload", "payload.word missing", 0);
            aiw_log("job %s failed bad_payload (word)", name);
            cJSON_Delete(job);
            return 0;
        }
        model = aiw_jstr(job, "model");
        body = aiw_note_request(model && *model ? model : AIW_MODEL_DEFAULT, word);
    } else {
        cJSON *facts = cJSON_GetObjectItemCaseSensitive(payload, "facts");
        char *facts_json = NULL;
        if (cJSON_IsString(facts)) {
            facts_json = aiw_strdup_n(facts->valuestring, strlen(facts->valuestring));
        } else if (cJSON_IsObject(facts) || cJSON_IsArray(facts)) {
            facts_json = cJSON_PrintUnformatted(facts);
        }
        if (!facts_json) {
            aiw_finish_job(path, job, "failed", "bad_payload", "payload.facts missing", 0);
            aiw_log("job %s failed bad_payload (facts)", name);
            cJSON_Delete(job);
            return 0;
        }
        model = aiw_jstr(job, "model");
        body = aiw_briefing_request(model && *model ? model : AIW_MODEL_DEFAULT, facts_json);
        free(facts_json);
    }
    if (!body) {
        aiw_log("oom building request: %s", name);
        goto skip;
    }

    /* base URL 启动时已校验过，此处解析失败按配置终态处理（不烧 attempts）。 */
    aiw_url url;
    if (aiw_url_parse(o->base, &url) != 0) {
        aiw_finish_job(path, job, "failed", "bad_request", "invalid --base url", 0);
        aiw_log("job %s failed bad_request (base url)", name);
        cJSON_Delete(job);
        return 0;
    }

    /* running 落盘（崩溃后可恢复为 queued），attempts 随执行次数递增。 */
    attempts += 1;
    aiw_jset_str(job, "status", "running");
    {
        cJSON *a = cJSON_GetObjectItemCaseSensitive(job, "attempts");
        if (a && cJSON_IsNumber(a)) cJSON_SetNumberValue(a, attempts);
        else cJSON_AddNumberToObject(job, "attempts", attempts);
    }
    {
        char *txt = cJSON_PrintUnformatted(job);
        if (!txt || aiw_write_file_atomic(path, txt, strlen(txt)) != 0) {
            free(txt);
            aiw_log("write running failed: %s", name);
            goto skip;
        }
        free(txt);
    }

    /* 调用 DeepSeek（连接 10s / 总体 90s 默认，均可 CLI 覆盖）。 */
    dl.deadline_ms = aiw_wall_ms() + (long long)o->deadline_s * 1000;
    memset(&resp, 0, sizeof resp);
    {
        /* err 可能指向 aiw_http_post 已结束的栈帧（chan 缓冲），先拷贝。 */
        char errbuf[256];
        errbuf[0] = '\0';
        if (aiw_http_post(&url, key, body, o->ca_path, o->connect_timeout_s,
                          &dl, &resp, &err) != 0) {
            if (err && *err) {
                snprintf(errbuf, sizeof errbuf, "%s", err);
                err = errbuf;
            }
            free(body);
            body = NULL;
            /* 网络/TLS/超时全部按 retryable 分类（attempts 兜底） */
            code = aiw_classify_transport(errbuf[0] ? errbuf : NULL);
            if (attempts >= AIW_MAX_ATTEMPTS) {
                aiw_finish_job(path, job, "failed", code, err, 0);
                aiw_log("job %s failed (%s) attempts=%d", name, code, attempts);
            } else {
                aiw_finish_job(path, job, "queued", code, err, 1);
                aiw_log("job %s retryable (%s) attempts=%d", name, code, attempts);
            }
            cJSON_Delete(job);
            return 0;
        }
    }
    free(body);
    body = NULL;

    /* HTTP 分类：2xx 解析；401/402/400/422 终态；429/5xx/其他 退避重试。 */
    if (resp.status < 200 || resp.status > 299) {
        retryable = aiw_classify_http(resp.status, &code);
        if (retryable) {
            if (attempts >= AIW_MAX_ATTEMPTS) {
                aiw_finish_job(path, job, "failed", code, NULL, 0);
                aiw_log("job %s failed (%s) http=%d attempts=%d", name, code, resp.status, attempts);
            } else {
                aiw_finish_job(path, job, "queued", code, NULL, 1);
                aiw_log("job %s retryable (%s) http=%d attempts=%d", name, code, resp.status, attempts);
            }
        } else {
            aiw_finish_job(path, job, "failed", code, NULL, 0);
            aiw_log("job %s failed (%s) http=%d", name, code, resp.status);
        }
        free(resp.body);
        cJSON_Delete(job);
        return 0;
    }

    /* 2xx：解析四类失败 → failed 不可重试（与手机侧口径一致，EMPTY_CONTENT 同）。 */
    {
        char *content = NULL;
        aiw_parse_kind pk = aiw_extract_content(resp.body, &content);
        if (pk != AIW_PARSE_OK) {
            aiw_finish_job(path, job, "failed", aiw_parse_name(pk), NULL, 0);
            aiw_log("job %s failed (%s)", name, aiw_parse_name(pk));
            free(resp.body);
            cJSON_Delete(job);
            return 0;
        }
        {
            cJSON *result = cJSON_CreateObject();
            aiw_parse_kind fk = strcmp(type, "note") == 0
                                ? aiw_parse_note(content, result, &detail)
                                : aiw_parse_briefing(content, result, &detail);
            free(content);
            if (fk != AIW_PARSE_OK) {
                cJSON_Delete(result);
                aiw_finish_job(path, job, "failed", aiw_parse_name(fk), detail, 0);
                aiw_log("job %s failed (%s)", name, aiw_parse_name(fk));
                free(resp.body);
                cJSON_Delete(job);
                return 0;
            }
            cJSON_AddItemToObject(job, "result", result);
            aiw_parse_usage(resp.body, job);
            aiw_jdel(job, "error_code");
            aiw_jdel(job, "error_detail");
            aiw_finish_job(path, job, "succeeded", NULL, NULL, 0);
            aiw_log("job %s succeeded", name);
        }
    }
    free(resp.body);
    cJSON_Delete(job);
    return 0;

skip:
    cJSON_Delete(job);
    return -1;
}

/* ------------------------------------------------------------------ CLI */
static void aiw_usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s <v2dir> --ca <pem> [--base URL] [--once] "
        "[--connect-timeout-s N] [--deadline-s N] [--scan-ms N]\n"
        "  <v2dir>            pv2 数据目录（队列在 <v2dir>/ai/queue）\n"
        "  --ca <pem>         CA 证书 PEM（fail-closed 必选）\n"
        "  --base URL         DeepSeek base（默认 %s）\n"
        "  --once             处理完当前可执行任务即退出\n"
        "  --connect-timeout-s N   连接超时（默认 %d）\n"
        "  --deadline-s N          单任务总体 deadline（默认 %d）\n"
        "  --scan-ms N             loop 扫描间隔（默认 %d）\n",
        prog, AIW_DEFAULT_BASE, AIW_DEFAULT_CONNECT_TIMEOUT_S,
        AIW_DEFAULT_DEADLINE_S, AIW_DEFAULT_SCAN_MS);
}

int main(int argc, char **argv)
{
    aiw_opts o;
    int i;
    const char *ca = NULL;
    static char basebuf[768];
    memset(&o, 0, sizeof o);
    o.base = AIW_DEFAULT_BASE;
    o.connect_timeout_s = AIW_DEFAULT_CONNECT_TIMEOUT_S;
    o.deadline_s = AIW_DEFAULT_DEADLINE_S;
    o.scan_ms = AIW_DEFAULT_SCAN_MS;
    if (argc < 3 || argv[1][0] == '-') { aiw_usage(argv[0]); return 2; }
    o.v2dir = argv[1];
    i = 2;                                    /* v2dir 之后逐项解析选项 */
    while (i < argc) {
        const char *a = argv[i];
        if (strcmp(a, "--ca") == 0 && i + 1 < argc) { ca = argv[++i]; }
        else if (strcmp(a, "--base") == 0 && i + 1 < argc) { o.base = argv[++i]; }
        else if (strcmp(a, "--once") == 0) { o.once = 1; }
        else if (strcmp(a, "--connect-timeout-s") == 0 && i + 1 < argc) {
            o.connect_timeout_s = atoi(argv[++i]);
        }
        else if (strcmp(a, "--deadline-s") == 0 && i + 1 < argc) {
            o.deadline_s = atoi(argv[++i]);
        }
        else if (strcmp(a, "--scan-ms") == 0 && i + 1 < argc) {
            o.scan_ms = atoi(argv[++i]);
        }
        else { aiw_usage(argv[0]); return 2; }
        i++;
    }
    if (!ca || !ca[0]) {
        fprintf(stderr, "error: --ca <pem> is required (fail-closed)\n");
        aiw_usage(argv[0]);
        return 2;
    }
    if (strlen(ca) >= sizeof o.ca_path) { fprintf(stderr, "error: --ca path too long\n"); return 2; }
    snprintf(o.ca_path, sizeof o.ca_path, "%s", ca);
    if (snprintf(o.queue_dir, sizeof o.queue_dir, "%s/ai/queue", o.v2dir) >= (int)sizeof o.queue_dir ||
        snprintf(o.key_path, sizeof o.key_path, "%s/auth/ai-deepseek.key", o.v2dir) >= (int)sizeof o.key_path) {
        fprintf(stderr, "error: v2dir path too long\n");
        return 2;
    }
    if (strlen(o.base) >= sizeof basebuf) { fprintf(stderr, "error: --base too long\n"); return 2; }
    memcpy(basebuf, o.base, strlen(o.base) + 1);
    o.base = basebuf;
    {
        aiw_url probe;
        if (aiw_url_parse(o.base, &probe) != 0) {
            fprintf(stderr, "error: invalid --base url\n");
            return 2;
        }
#ifdef AIW_NO_TLS
        if (probe.is_https) {
            fprintf(stderr, "error: https unavailable in NO_TLS build, use http:// base\n");
            return 2;
        }
#endif
    }
#ifdef _WIN32
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            fprintf(stderr, "error: WSAStartup failed\n");
            return 2;
        }
    }
#endif
    aiw_jitter_seed();
    for (;;) {
        char **names = NULL;
        size_t n = 0;
        g_log_now = aiw_wall_ms();
        if (aiw_list_jobs(o.queue_dir, &names, &n) != 0) {
            /* 队列目录不存在：提交方尚未创建，安静等待 */
        } else {
            for (size_t k = 0; k < n; k++) {
                g_log_now = aiw_wall_ms();
                aiw_process_job(o.queue_dir, names[k], &o);
                free(names[k]);
            }
            free(names);
        }
        if (o.once) break;
        aiw_sleep_ms(o.scan_ms > 0 ? o.scan_ms : AIW_DEFAULT_SCAN_MS);
    }
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
