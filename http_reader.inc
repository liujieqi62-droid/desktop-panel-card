/* Bounded HTTP request reader, shared by the production service (service.c
 * via remote_state.inc) and the host unit test (tests/test_http_reader.c).
 * Included, not linked: the includer may override the transport/clock hooks
 * (PV2_RECV / PV2_NOW / PV2_STRNCASECMP) to script TCP segmentation without
 * real sockets.
 *
 * Waits for complete headers and a Content-Length body, rejects chunked and
 * ambiguous/oversized requests with a reason, and never hands truncated JSON
 * to a handler. On rejection returns -1 and sets *reason ("too_large" /
 * "bad_chunked" / "bad_headers" / "timeout" / "closed") plus *decl_len (the
 * declared Content-Length when seen) so the caller can answer 413/400 instead
 * of dropping the connection silently (T03B2 / ACT-01 / CHK-C01). TCP segments
 * arriving in pieces are normal and keep being assembled here. */

#ifndef PV2_RECV
#define PV2_RECV(fd, buf, len) recv((fd), (buf), (len), 0)
#endif
#ifndef PV2_NOW
#define PV2_NOW() monotonic_ms()
#endif
#ifdef _WIN32
#ifndef PV2_STRNCASECMP
#define PV2_STRNCASECMP _strnicmp
#endif
#else
#ifndef PV2_STRNCASECMP
#define PV2_STRNCASECMP strncasecmp
#endif
#endif

static int read_request(int fd, char *buf, size_t cap, const char **reason, long *decl_len) {
    size_t used = 0;
    int expected = -1;
    int64_t deadline = PV2_NOW() + 1200;
    if (reason) *reason = "closed";
    if (decl_len) *decl_len = -1;
    while (used < cap - 1 && PV2_NOW() < deadline) {
        long long n = PV2_RECV(fd, buf + used, cap - 1 - used);
        if (n <= 0) { if (reason) *reason = "closed"; return -1; }
        used += (size_t)n; buf[used] = 0;
        char *end = strstr(buf, "\r\n\r\n"); if (!end) continue;
        if (expected < 0) {
            int len = 0, seen = 0;
            for (char *p = strstr(buf, "\r\n") + 2; p < end;) {
                char *e = strstr(p, "\r\n");
                if (!e) { if (reason) *reason = "bad_headers"; return -1; }
                if (PV2_STRNCASECMP(p, "Transfer-Encoding:", 18) == 0) {
                    if (reason) *reason = "bad_chunked"; return -1;
                }
                if (PV2_STRNCASECMP(p, "Content-Length:", 15) == 0) {
                    if (seen++) { if (reason) *reason = "bad_headers"; return -1; }
                    char *stop;
                    long x = strtol(p + 15, &stop, 10);
                    while (stop < e && (*stop == ' ' || *stop == '\t')) stop++;
                    if (stop != e || x < 0) { if (reason) *reason = "bad_headers"; return -1; }
                    if (decl_len) *decl_len = x;
                    if (x > 4096) { if (reason) *reason = "too_large"; return -1; }
                    len = (int)x;
                }
                p = e + 2;
            }
            expected = (int)(end + 4 - buf) + len;
            if (expected >= (int)cap) { if (reason) *reason = "too_large"; return -1; }
        }
        if ((int)used >= expected) { buf[expected] = 0; return expected; }
    }
    if (reason) *reason = "timeout";
    return -1;
}
