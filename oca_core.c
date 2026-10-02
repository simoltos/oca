/* OCA agent core implementation. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#include "oca_core.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <poll.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <cjson/cJSON.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

/* ==========================================================================
 * Stable error codes
 * ========================================================================== */

#define E_ROUTE     "route_unavailable"
#define E_LIMIT     "limit_exceeded"
#define E_PROMPT    "invalid_prompt"
#define E_QUEUE     "queue_full"
#define E_MODEL     "unknown_model"
#define E_STREAM    "stream_invalid"
#define E_ADAPTER   "adapter_failed"
#define E_COMPACT   "compaction_failed"
#define E_TOOLVAL   "tool_validation_failed"
#define E_EXEC      "executor_failed"
#define E_SESSION   "session_failed"
#define E_TRACE     "diagnostic_failed"
#define E_SAVE      "save_failed"
#define E_STATE     "invalid_state"
#define E_GOAL      "invalid_goal"
#define E_ROUTESET  "route_failed"

#define OCA_EDIT_FILE_MAX_BYTES (8 * 1024 * 1024)
#define OCA_TOOL_CALL_ID_MAX_BYTES 256
#define OCA_TOOL_NAME_MAX_BYTES 64
#define ACTION_RING_SIZE (OCA_ACTION_QUEUE_MAX + 1)
#define EVENT_RING_SIZE  (OCA_EVENT_QUEUE_MAX + 1)

/* ============================================================================
 * Small helpers
 * ============================================================================
 * Every error buffer is caller-owned and stays valid until the next call.
 * ========================================================================== */

static void err_set(char *err, size_t errsz, const char *fmt, ...) {
    va_list ap;
    if (!err || errsz == 0) return;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static char *xstrndup(const char *s, size_t n) {
    char *p = malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* A configured reasoning level name is one bounded printable token. */
static int reasoning_name_valid(const char *name, size_t len) {
    if (!name || len == 0 || len > OCA_REASONING_NAME_MAX) return 0;
    for (size_t i = 0; i < len; i++)
        if ((unsigned char)name[i] <= 0x20 || (unsigned char)name[i] >= 0x7f) return 0;
    return 1;
}

static long long now_unix(void) {
    return (long long)time(NULL);
}

/* Validates UTF-8. Rejects overlong forms, surrogates, and values above
 * U+10FFFF. */
static int utf8_valid(const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            i++;
        } else if ((c & 0xE0) == 0xC0) {
            if (n - i < 2 || (c & 0xFE) == 0xC0) return 0;
            if (((unsigned char)s[i + 1] & 0xC0) != 0x80) return 0;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (n - i < 3) return 0;
            if (((unsigned char)s[i + 1] & 0xC0) != 0x80) return 0;
            if (((unsigned char)s[i + 2] & 0xC0) != 0x80) return 0;
            unsigned cp = (unsigned)(c & 0x0F) << 12 | (unsigned)(s[i + 1] & 0x3F) << 6 |
                          (unsigned)(s[i + 2] & 0x3F);
            if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            if (n - i < 4) return 0;
            if ((c & 0xF8) > 0xF0) return 0;
            if (((unsigned char)s[i + 1] & 0xC0) != 0x80) return 0;
            if (((unsigned char)s[i + 2] & 0xC0) != 0x80) return 0;
            if (((unsigned char)s[i + 3] & 0xC0) != 0x80) return 0;
            unsigned cp = (unsigned)(c & 0x07) << 18 | (unsigned)(s[i + 1] & 0x3F) << 12 |
                          (unsigned)(s[i + 2] & 0x3F) << 6 | (unsigned)(s[i + 3] & 0x3F);
            if (cp < 0x10000 || cp > 0x10FFFF) return 0;
            i += 4;
        } else {
            return 0;
        }
    }
    return 1;
}

static int utf8_text_valid(const char *s, size_t n) {
    return !memchr(s, '\0', n) && utf8_valid(s, n);
}

static void utf8_sanitize_in_place(char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c > 0 && c < 0x80) {
            i++;
            continue;
        }
        size_t need = 0;
        if (c >= 0xC2 && c <= 0xDF) need = 2;
        else if (c >= 0xE0 && c <= 0xEF) need = 3;
        else if (c >= 0xF0 && c <= 0xF4) need = 4;
        int valid = need && i + need <= n;
        for (size_t k = 1; valid && k < need; k++)
            valid = ((unsigned char)s[i + k] & 0xC0) == 0x80;
        if (valid && need == 3) {
            unsigned char c1 = (unsigned char)s[i + 1];
            if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0)) valid = 0;
        }
        if (valid && need == 4) {
            unsigned char c1 = (unsigned char)s[i + 1];
            if ((c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 >= 0x90)) valid = 0;
        }
        if (!valid) {
            s[i++] = '?';
        } else {
            i += need;
        }
    }
}

static size_t utf8_chars(const char *s) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p & 0xC0) != 0x80) n++;
    }
    return n;
}

typedef struct {
    char *buf;
    size_t len, cap;
    int oom;
} sbuf;

static void sb_init(sbuf *b) {
    memset(b, 0, sizeof(*b));
}

static void sb_free(sbuf *b) {
    free(b->buf);
    memset(b, 0, sizeof(*b));
}

static int sb_need(sbuf *b, size_t extra) {
    if (b->oom) return -1;
    if (b->len == SIZE_MAX || extra > SIZE_MAX - b->len - 1) {
        b->oom = 1;
        return -1;
    }
    if (b->len + extra + 1 <= b->cap) return 0;
    size_t nc = b->cap ? b->cap : 256;
    while (nc < b->len + extra + 1) {
        if (nc > SIZE_MAX / 2) {
            nc = b->len + extra + 1;
            break;
        }
        nc *= 2;
    }
    char *nb = realloc(b->buf, nc);
    if (!nb) {
        b->oom = 1;
        return -1;
    }
    b->buf = nb;
    b->cap = nc;
    return 0;
}

static void sb_put(sbuf *b, const void *data, size_t n) {
    if (sb_need(b, n) != 0) return;
    memcpy(b->buf + b->len, data, n);
    b->len += n;
    b->buf[b->len] = 0;
}

static void sb_puts(sbuf *b, const char *s) {
    sb_put(b, s, strlen(s));
}

static void sb_printf(sbuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0 || sb_need(b, (size_t)need) != 0) return;
    va_start(ap, fmt);
    vsnprintf(b->buf + b->len, (size_t)need + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)need;
}

static char *sb_take(sbuf *b) {
    if (b->oom) {
        sb_free(b);
        return NULL;
    }
    if (!b->buf) {
        b->buf = malloc(1);
        if (b->buf) b->buf[0] = 0;
        else return NULL;
    }
    /* Detach the buffer and leave the sbuf empty so later sb_free is safe. */
    char *out = b->buf;
    b->buf = NULL;
    b->len = b->cap = 0;
    return out;
}

/* Appends one JSON-escaped string including quotes. */
static void sb_json_str(sbuf *b, const char *s) {
    sb_put(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            char tmp[2] = { '\\', (char)c };
            sb_put(b, tmp, 2);
        } else if (c == '\n') {
            sb_put(b, "\\n", 2);
        } else if (c == '\r') {
            sb_put(b, "\\r", 2);
        } else if (c == '\t') {
            sb_put(b, "\\t", 2);
        } else if (c < 0x20) {
            sb_printf(b, "\\u%04x", c);
        } else {
            sb_put(b, (const char *)p, 1);
        }
    }
    sb_put(b, "\"", 1);
}

static int mkdir_parents_0700(const char *path, char *err, size_t errsz) {
    char buf[4096];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(buf)) {
        err_set(err, errsz, "state path too long");
        return -1;
    }
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (buf[i] != '/') continue;
        buf[i] = 0;
        if (mkdir(buf, 0700) == 0) {
            if (chmod(buf, 0700) != 0) {
                err_set(err, errsz, "cannot protect directory %s: %s", buf, strerror(errno));
                return -1;
            }
        } else if (errno != EEXIST) {
            err_set(err, errsz, "cannot create directory %s: %s", buf, strerror(errno));
            return -1;
        }
        buf[i] = '/';
    }
    return 0;
}

static int write_file_atomic_0600(const char *path, const char *data, size_t len,
                                  char *err, size_t errsz) {
    char tmp[4400];
    if (mkdir_parents_0700(path, err, errsz) != 0) return -1;
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        err_set(err, errsz, "cannot create %s: %s", tmp, strerror(errno));
        return -1;
    }
    if (fchmod(fd, 0600) != 0) {
        err_set(err, errsz, "cannot protect %s: %s", tmp, strerror(errno));
        goto failed;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t written = write(fd, data + off, len - off);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            if (!written) errno = EIO;
            err_set(err, errsz, "cannot write %s: %s", tmp, strerror(errno));
            goto failed;
        }
        off += (size_t)written;
    }
    if (fsync(fd) != 0) {
        err_set(err, errsz, "cannot fsync %s: %s", tmp, strerror(errno));
        goto failed;
    }
    int closed = close(fd);
    fd = -1;
    if (closed != 0) {
        err_set(err, errsz, "cannot close %s: %s", tmp, strerror(errno));
        goto failed;
    }
    if (rename(tmp, path) != 0) {
        err_set(err, errsz, "cannot replace %s: %s", path, strerror(errno));
        goto failed;
    }
    return 0;
failed:
    if (fd >= 0) close(fd);
    unlink(tmp);
    return -1;
}

static char *read_whole_file(const char *path, size_t max_bytes, size_t *out_len,
                             char *err, size_t errsz) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        err_set(err, errsz, "cannot open %s: %s", path, strerror(errno));
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        err_set(err, errsz, "cannot stat %s: %s", path, strerror(errno));
        close(fd);
        return NULL;
    }
    if (!S_ISREG(st.st_mode)) {
        err_set(err, errsz, "%s is not a regular file", path);
        close(fd);
        return NULL;
    }
    if ((long long)st.st_size > (long long)max_bytes) {
        err_set(err, errsz, "%s exceeds the size limit", path);
        close(fd);
        return NULL;
    }
    char *buf = malloc(max_bytes + 1);
    if (!buf) {
        err_set(err, errsz, "out of memory");
        close(fd);
        return NULL;
    }
    size_t total = 0;
    while (total < max_bytes + 1) {
        ssize_t r = read(fd, buf + total, max_bytes + 1 - total);
        if (r < 0) {
            err_set(err, errsz, "cannot read %s: %s", path, strerror(errno));
            free(buf);
            close(fd);
            return NULL;
        }
        if (r == 0) break;
        total += (size_t)r;
    }
    close(fd);
    if (total > max_bytes) {
        err_set(err, errsz, "%s exceeds the size limit", path);
        free(buf);
        return NULL;
    }
    buf[total] = 0;
    *out_len = total;
    return buf;
}

/* Read optional project context. A missing optional file yields empty text. */
static char *context_file_read(const char *path, size_t maximum, int optional,
                               size_t *out_len, char *err, size_t errsz) {
    *out_len = 0;
    struct stat state;
    if (optional && stat(path, &state) && errno == ENOENT) {
        char *empty = xstrdup("");
        if (!empty) err_set(err, errsz, "out of memory");
        return empty;
    }
    return read_whole_file(path, maximum, out_len, err, errsz);
}

static char *path_join_alloc(const char *dir, const char *name) {
    if (name[0] == '/') return xstrdup(name);
    size_t dl = strlen(dir);
    while (dl > 1 && dir[dl - 1] == '/') dl--;
    size_t nb = dl + 1 + strlen(name) + 1;
    char *p = malloc(nb);
    if (!p) return NULL;
    snprintf(p, nb, "%.*s/%s", (int)dl, dir, name);
    return p;
}

static char *state_dir_alloc(const char *leaf) {
    const char *home = getenv("HOME");
    if (!home || !*home) return NULL;
    const char *fmt = leaf ? "%s/.oca/%s" : "%s/.oca";
    size_t nb = snprintf(NULL, 0, fmt, home, leaf ? leaf : "") + 1;
    char *p = malloc(nb);
    if (!p) return NULL;
    snprintf(p, nb, fmt, home, leaf ? leaf : "");
    return p;
}

/* ============================================================================
 * Tool JSON parsing
 * ============================================================================
 * The parser bounds depth, node count, and string size before allocation.
 * ========================================================================== */

typedef struct {
    const char *p, *end;
    int depth;
    size_t nodes;
    const char *why;
} json_parser;

static cJSON *json_new(json_parser *jp, int type) {
    if (jp->nodes >= 4096 || jp->depth > 16) {
        jp->why = "document too complex";
        return NULL;
    }
    cJSON *v = cJSON_CreateNull();
    if (!v) {
        jp->why = "out of memory";
        return NULL;
    }
    v->type = type;
    v->valueint = type == cJSON_True;
    jp->nodes++;
    return v;
}

static void json_skip_space(json_parser *jp) {
    while (jp->p < jp->end && (*jp->p == ' ' || *jp->p == '\t' || *jp->p == '\n' || *jp->p == '\r'))
        jp->p++;
}

static int json_expect(json_parser *jp, char c) {
    json_skip_space(jp);
    if (jp->p < jp->end && *jp->p == c) {
        jp->p++;
        return 0;
    }
    jp->why = "unexpected token";
    return -1;
}

static int json_string(json_parser *jp, char **out) {
    json_skip_space(jp);
    if (jp->p >= jp->end || *jp->p != '"') {
        jp->why = "expected string";
        return -1;
    }
    jp->p++;
    sbuf b;
    sb_init(&b);
    while (jp->p < jp->end && *jp->p != '"') {
        unsigned char c = (unsigned char)*jp->p;
        if (c < 0x20) {
            jp->why = "control character";
            goto fail;
        }
        if (c == '\\') {
            jp->p++;
            if (jp->p >= jp->end) {
                jp->why = "truncated escape";
                goto fail;
            }
            char e = *jp->p++;
            switch (e) {
            case '"':
                sb_put(&b, "\"", 1);
                break;
            case '\\':
                sb_put(&b, "\\", 1);
                break;
            case '/':
                sb_put(&b, "/", 1);
                break;
            case 'b':
                sb_put(&b, "\b", 1);
                break;
            case 'f':
                sb_put(&b, "\f", 1);
                break;
            case 'n':
                sb_put(&b, "\n", 1);
                break;
            case 'r':
                sb_put(&b, "\r", 1);
                break;
            case 't':
                sb_put(&b, "\t", 1);
                break;
            case 'u': {
                unsigned v = 0;
                for (int k = 0; k < 4; k++) {
                    if (jp->p >= jp->end) {
                        jp->why = "truncated escape";
                        goto fail;
                    }
                    char h = *jp->p++;
                    v <<= 4;
                    if (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
                    else {
                        jp->why = "bad escape";
                        goto fail;
                    }
                }
                if (v == 0 || (v >= 0xD800 && v <= 0xDFFF)) {
                    jp->why = "unsupported Unicode escape";
                    goto fail;
                }
                char u[4];
                int un;
                if (v < 0x80) {
                    u[0] = (char)v;
                    un = 1;
                } else if (v < 0x800) {
                    u[0] = (char)(0xC0 | (v >> 6));
                    u[1] = (char)(0x80 | (v & 0x3F));
                    un = 2;
                } else {
                    u[0] = (char)(0xE0 | (v >> 12));
                    u[1] = (char)(0x80 | ((v >> 6) & 0x3F));
                    u[2] = (char)(0x80 | (v & 0x3F));
                    un = 3;
                }
                sb_put(&b, u, (size_t)un);
                break;
            }
            default:
                jp->why = "bad escape";
                goto fail;
            }
        } else {
            sb_put(&b, jp->p, 1);
            jp->p++;
        }
        if (b.len > (1 << 20)) {
            jp->why = "string too long";
            goto fail;
        }
    }
    if (jp->p >= jp->end) {
        jp->why = "unterminated string";
        goto fail;
    }
    jp->p++;
    *out = sb_take(&b);
    if (!*out) {
        jp->why = "out of memory";
        return -1;
    }
    return 0;
fail:
    sb_free(&b);
    return -1;
}

static cJSON *json_value(json_parser *jp);

static cJSON *json_container(json_parser *jp, int kind) {
    if (json_expect(jp, kind == cJSON_Array ? '[' : '{') != 0) return NULL;
    cJSON *v = json_new(jp, kind);
    if (!v) return NULL;
    jp->depth++;
    json_skip_space(jp);
    if (jp->p < jp->end && *jp->p == (kind == cJSON_Array ? ']' : '}')) {
        jp->p++;
        jp->depth--;
        return v;
    }
    for (;;) {
        if (kind == cJSON_Array) {
            cJSON *item = json_value(jp);
            if (!item) goto fail;
            if (!cJSON_AddItemToArray(v, item)) {
                cJSON_Delete(item);
                jp->why = "out of memory";
                goto fail;
            }
        } else {
            char *key = NULL;
            if (json_string(jp, &key) != 0) {
                free(key);
                goto fail;
            }
            if (json_expect(jp, ':') != 0) {
                free(key);
                goto fail;
            }
            cJSON *val = json_value(jp);
            if (!val) {
                free(key);
                goto fail;
            }
            if (cJSON_GetObjectItemCaseSensitive(v, key)) {
                jp->why = "duplicate key";
                free(key);
                cJSON_Delete(val);
                goto fail;
            }
            int added = cJSON_AddItemToObject(v, key, val);
            free(key);
            if (!added) {
                cJSON_Delete(val);
                jp->why = "out of memory";
                goto fail;
            }
        }
        json_skip_space(jp);
        if (jp->p < jp->end && *jp->p == ',') {
            jp->p++;
            continue;
        }
        break;
    }
    if (json_expect(jp, kind == cJSON_Array ? ']' : '}') != 0) goto fail;
    jp->depth--;
    return v;
fail:
    jp->depth--;
    cJSON_Delete(v);
    return NULL;
}

static cJSON *json_value(json_parser *jp) {
    json_skip_space(jp);
    if (jp->p >= jp->end) {
        jp->why = "unexpected end";
        return NULL;
    }
    char c = *jp->p;
    if (c == '{') return json_container(jp, cJSON_Object);
    if (c == '[') return json_container(jp, cJSON_Array);
    if (c == '"') {
        cJSON *v = json_new(jp, cJSON_String);
        if (!v) return NULL;
        if (json_string(jp, &v->valuestring) != 0) {
            cJSON_Delete(v);
            return NULL;
        }
        return v;
    }
    if (c == 't' || c == 'f') {
        int want = c == 't';
        const char *word = want ? "true" : "false";
        size_t wl = want ? 4 : 5;
        if ((size_t)(jp->end - jp->p) < wl || memcmp(jp->p, word, wl) != 0) {
            jp->why = "bad literal";
            return NULL;
        }
        jp->p += wl;
        return json_new(jp, want ? cJSON_True : cJSON_False);
    }
    if (c == 'n') {
        if ((size_t)(jp->end - jp->p) < 4 || memcmp(jp->p, "null", 4) != 0) {
            jp->why = "bad literal";
            return NULL;
        }
        jp->p += 4;
        return json_new(jp, cJSON_NULL);
    }
    {
        char *endp = NULL;
        errno = 0;
        double d = strtod(jp->p, &endp);
        if (endp == jp->p || errno == ERANGE) {
            jp->why = "bad number";
            return NULL;
        }
        cJSON *v = json_new(jp, cJSON_Number);
        if (!v) return NULL;
        v->valuedouble = d;
        if (d >= INT_MAX) v->valueint = INT_MAX;
        else if (d <= INT_MIN) v->valueint = INT_MIN;
        else if (isfinite(d)) v->valueint = (int)d;
        jp->p = endp;
        return v;
    }
}

static cJSON *json_parse(const char *s, size_t len, const char **why) {
    if (!utf8_text_valid(s, len)) {
        if (why) *why = "invalid UTF-8";
        return NULL;
    }
    json_parser jp = { s, s + len, 0, 0, "syntax" };
    cJSON *v = json_value(&jp);
    if (!v) {
        if (why) *why = jp.why;
        return NULL;
    }
    json_skip_space(&jp);
    if (jp.p != jp.end) {
        if (why) *why = "trailing data";
        cJSON_Delete(v);
        return NULL;
    }
    return v;
}

/* The result borrows the object's string, or is NULL for a missing or non-string field. */
static const char *json_text(const cJSON *object, const char *key) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
}

static int json_integer(const cJSON *v, long *out) {
    if (!cJSON_IsNumber(v)) return 0;
    if (!isfinite(v->valuedouble) || v->valuedouble < (double)LONG_MIN ||
        v->valuedouble > (double)LONG_MAX || v->valuedouble != floor(v->valuedouble)) return 0;
    *out = (long)v->valuedouble;
    return 1;
}

/* ============================================================================
 * Skills
 * ========================================================================== */

typedef struct {
    char *name;
    char *description;
    char *path;
} skill_rec;

static int skill_name_valid(const char *name, size_t len) {
    if (len < 1 || len > OCA_SKILL_NAME_MAX) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) return 0;
        if (c == '-' && i > 0 && name[i - 1] == '-') return 0;
    }
    if (name[0] == '-' || name[len - 1] == '-') return 0;
    return 1;
}

/* Parses one SKILL.md buffer. Returns zero for a valid skill. */
static int skill_parse(const char *data, size_t len, const char *dirname,
                       char *name_out, size_t name_sz, char *desc_out, size_t desc_sz,
                       char *why, size_t whysz) {
    const char *fail_msg = NULL;
    const char *nl = memchr(data, '\n', len);
    if (!nl || (size_t)(nl - data) != 3 || memcmp(data, "---", 3) != 0) {
        fail_msg = "missing front matter";
        goto fail;
    }
    const char *p = nl + 1;
    const char *end = data + len;
    const char *name = NULL, *desc = NULL;
    size_t name_len = 0, desc_len = 0;
    int keys = 0;
    /* Parse front matter lines until the closing marker. */
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        size_t ll = eol ? (size_t)(eol - p) : (size_t)(end - p);
        if (ll == 3 && memcmp(p, "---", 3) == 0) {
            p = eol ? eol + 1 : end;
            break;
        }
        if (ll > 5 && memcmp(p, "name:", 5) == 0) {
            if (keys & 1) {
                fail_msg = "duplicate name";
                goto fail;
            }
            name = p + 5;
            while (name < p + ll && *name == ' ') name++;
            name_len = (size_t)(p + ll - name);
            keys |= 1;
        } else if (ll > 12 && memcmp(p, "description:", 12) == 0) {
            if (keys & 2) {
                fail_msg = "duplicate description";
                goto fail;
            }
            desc = p + 12;
            while (desc < p + ll && *desc == ' ') desc++;
            desc_len = (size_t)(p + ll - desc);
            keys |= 2;
        } else {
            fail_msg = "unknown front matter key";
            goto fail;
        }
        if (!eol) {
            fail_msg = "missing front matter close";
            goto fail;
        }
        p = eol + 1;
    }
    if (keys != 3) {
        fail_msg = "front matter must contain name and description";
        goto fail;
    }
    if (!name_len || !desc_len) {
        fail_msg = "empty name or description";
        goto fail;
    }
    if (!utf8_valid(name, name_len) || !utf8_valid(desc, desc_len)) {
        fail_msg = "invalid UTF-8";
        goto fail;
    }
    if (!skill_name_valid(name, name_len)) {
        fail_msg = "invalid skill name";
        goto fail;
    }
    if (name_len != strlen(dirname) || memcmp(name, dirname, name_len) != 0) {
        fail_msg = "name does not match the directory";
        goto fail;
    }
    if (desc_len > OCA_SKILL_DESC_MAX_BYTES) {
        fail_msg = "description exceeds the limit";
        goto fail;
    }
    /* The body is the remaining data and must not be empty. */
    size_t body_len = (size_t)(end - p);
    size_t trimmed = body_len;
    while (trimmed > 0 && (p[trimmed - 1] == '\n' || p[trimmed - 1] == ' ' ||
                          p[trimmed - 1] == '\t' || p[trimmed - 1] == '\r'))
        trimmed--;
    if (trimmed == 0) {
        fail_msg = "empty body";
        goto fail;
    }
    snprintf(name_out, name_sz, "%.*s", (int)name_len, name);
    snprintf(desc_out, desc_sz, "%.*s", (int)desc_len, desc);
    return 0;
fail:
    snprintf(why, whysz, "%s", fail_msg);
    return -1;
}

typedef struct {
    skill_rec *skills;
    size_t n;
    char **warns;
    size_t n_warns;
} skill_catalog;

static int skill_dir_parse(const char *skill_md_path, const char *dirname,
                           char *name, size_t name_sz, char *desc, size_t desc_sz,
                           char *why, size_t whysz) {
    size_t flen = 0;
    char why2[128];
    char *data = context_file_read(skill_md_path, OCA_SKILL_FILE_MAX_BYTES, 0, &flen, why2, sizeof(why2));
    if (!data) {
        snprintf(why, whysz, "cannot read SKILL.md: %s", why2);
        return -1;
    }
    if (!utf8_text_valid(data, flen)) {
        free(data);
        snprintf(why, whysz, "SKILL.md is not valid UTF-8");
        return -1;
    }
    int rc = skill_parse(data, flen, dirname, name, name_sz, desc, desc_sz, why, whysz);
    free(data);
    return rc;
}

static void catalog_free(skill_catalog *cat) {
    for (size_t i = 0; i < cat->n; i++) {
        free(cat->skills[i].name);
        free(cat->skills[i].description);
        free(cat->skills[i].path);
    }
    free(cat->skills);
    for (size_t i = 0; i < cat->n_warns; i++) free(cat->warns[i]);
    free(cat->warns);
    memset(cat, 0, sizeof(*cat));
}

static int catalog_discover(const char *dir, skill_catalog *cat) {
    memset(cat, 0, sizeof(*cat));
    DIR *d = opendir(dir);
    if (!d) {
        if (errno == ENOENT) return 0;
        char warning[256];
        snprintf(warning, sizeof(warning), "Skill discovery failed: %s", strerror(errno));
        cat->warns = calloc(1, sizeof(*cat->warns));
        if (cat->warns) {
            cat->warns[0] = xstrdup(warning);
            if (cat->warns[0]) cat->n_warns = 1;
        }
        return -1;
    }
    struct dirent *de;
    size_t cap = 0;
    while ((de = readdir(d)) != NULL) {
        const char *dn = de->d_name;
        if (strcmp(dn, ".") == 0 || strcmp(dn, "..") == 0) continue;
        char *md = path_join_alloc(dir, dn);
        if (!md) continue;
        char *sk = path_join_alloc(md, "SKILL.md");
        free(md);
        if (!sk) continue;
        struct stat st;
        if (stat(sk, &st) != 0 || !S_ISREG(st.st_mode)) {
            free(sk);
            continue; /* Ignore other entries directly below the root. */
        }
        char name[OCA_SKILL_NAME_MAX + 1], desc[OCA_SKILL_DESC_MAX_BYTES + 1], why[160];
        if (skill_dir_parse(sk, dn, name, sizeof(name), desc, sizeof(desc),
                            why, sizeof(why)) != 0) {
            size_t wl = strlen("Skill  ignored: ") + strlen(dn) + strlen(why) + 1;
            char *w = malloc(wl);
            if (!w) {
                free(sk);
                continue;
            }
            snprintf(w, wl, "Skill %s ignored: %s", dn, why);
            if (cat->n_warns < 64) {
                char **nw = realloc(cat->warns, (cat->n_warns + 1) * sizeof(*nw));
                if (nw) {
                    cat->warns = nw;
                    cat->warns[cat->n_warns++] = w;
                } else {
                    free(w);
                }
            } else {
                free(w);
            }
            free(sk);
            continue;
        }
        if (cat->n == cap) {
            size_t nc = cap ? cap * 2 : 8;
            skill_rec *ns = realloc(cat->skills, nc * sizeof(*ns));
            if (!ns) {
                free(sk);
                continue;
            }
            cat->skills = ns;
            cap = nc;
        }
        skill_rec *s = &cat->skills[cat->n];
        s->name = xstrdup(name);
        s->description = xstrdup(desc);
        s->path = sk;
        if (s->name && s->description && s->path) {
            cat->n++;
        } else {
            free(s->name);
            free(s->description);
            free(s->path);
        }
    }
    closedir(d);
    for (size_t i = 1; i < cat->n; i++) {
        skill_rec key = cat->skills[i];
        size_t j = i;
        while (j > 0 && strcmp(cat->skills[j - 1].name, key.name) > 0) {
            cat->skills[j] = cat->skills[j - 1];
            j--;
        }
        cat->skills[j] = key;
    }
    return 0;
}

/* ============================================================================
 * Messages and session framing
 * ========================================================================== */

enum { MS_COMPLETE = 0, MS_INTERRUPTED };

typedef struct {
    char *id;
    char *name;
    char *args;
} call_rec;

typedef struct {
    oca_msg_role role;
    int state;              /* assistant or tool result state */
    char *text;
    char *tool_context; /* Owned bounded view; requests borrow it until return. */
    char *reasoning;
    char *model_id;
    char *effort;
    char *continuation;
    call_rec *calls;
    size_t n_calls;
    char *call_id;
} msg_rec;

typedef struct {
    char id[33];
    long long created;
    long long last_used;
    char *title;
    char *workspace;
    char *agents;
    char *model;
    char *effort;
    int goal_state;
    char *goal;
    uint64_t goal_elapsed_ms;
    msg_rec *msgs;
    size_t n_msgs;
    long first_kept;
    long tokens_before;
    long tokens_after;
    char *summary;
} session_data;

static void msg_free_all(msg_rec *msgs, size_t n) {
    for (size_t i = 0; i < n; i++) {
        free(msgs[i].text);
        free(msgs[i].tool_context);
        free(msgs[i].reasoning);
        free(msgs[i].model_id);
        free(msgs[i].effort);
        free(msgs[i].continuation);
        free(msgs[i].call_id);
        for (size_t k = 0; msgs[i].calls && k < msgs[i].n_calls; k++) {
            free(msgs[i].calls[k].id);
            free(msgs[i].calls[k].name);
            free(msgs[i].calls[k].args);
        }
        free(msgs[i].calls);
    }
    free(msgs);
}

static void session_data_free(session_data *sd) {
    free(sd->title);
    free(sd->workspace);
    free(sd->agents);
    free(sd->model);
    free(sd->effort);
    free(sd->goal);
    free(sd->summary);
    msg_free_all(sd->msgs, sd->n_msgs);
    memset(sd, 0, sizeof(*sd));
}

static void session_data_init(session_data *sd) {
    memset(sd, 0, sizeof(*sd));
    sd->goal_state = OCA_GOAL_NONE;
    sd->first_kept = -1;
}

/* ---- framing writer ---- */

static void session_write_field(sbuf *b, const char *key, const char *data, size_t n) {
    if (!data) {
        data = "";
        n = 0;
    }
    if (key) sb_printf(b, "%s %zu\n", key, n);
    sb_put(b, data, n);
    sb_put(b, "\n", 1);
}

static void session_write_message(sbuf *b, const msg_rec *m) {
    switch (m->role) {
    case OCA_MSG_SYSTEM:
        session_write_field(b, "message system", m->text, strlen(m->text ? m->text : ""));
        break;
    case OCA_MSG_USER:
        session_write_field(b, "message user", m->text, strlen(m->text ? m->text : ""));
        break;
    case OCA_MSG_ASSISTANT:
        sb_printf(b, "message assistant %s %zu %zu %zu %zu %zu %zu %zu\n",
                  m->state == MS_INTERRUPTED ? "interrupted" : "complete",
                  strlen(m->model_id ? m->model_id : ""),
                  strlen(m->effort ? m->effort : ""),
                  (size_t)6,
                  strlen(m->text ? m->text : ""),
                  strlen(m->reasoning ? m->reasoning : ""),
                  strlen(m->continuation ? m->continuation : ""),
                  m->n_calls);
        session_write_field(b, NULL, m->model_id, m->model_id ? strlen(m->model_id) : 0);
        session_write_field(b, NULL, m->effort, m->effort ? strlen(m->effort) : 0);
        session_write_field(b, NULL, "normal", 6);
        session_write_field(b, NULL, m->text, m->text ? strlen(m->text) : 0);
        session_write_field(b, NULL, m->reasoning, m->reasoning ? strlen(m->reasoning) : 0);
        session_write_field(b, NULL, m->continuation, m->continuation ? strlen(m->continuation) : 0);
        for (size_t k = 0; k < m->n_calls; k++) {
            sb_printf(b, "call %zu %zu %zu\n",
                      strlen(m->calls[k].id), strlen(m->calls[k].name),
                      strlen(m->calls[k].args));
            session_write_field(b, NULL, m->calls[k].id, strlen(m->calls[k].id));
            session_write_field(b, NULL, m->calls[k].name, strlen(m->calls[k].name));
            session_write_field(b, NULL, m->calls[k].args, strlen(m->calls[k].args));
        }
        break;
    case OCA_MSG_TOOL:
        sb_printf(b, "message tool %s %zu %zu\n",
                  m->state == OCA_TOOL_INTERRUPTED ? "interrupted" :
                  m->state == OCA_TOOL_ERROR ? "error" : "complete",
                  strlen(m->call_id ? m->call_id : ""),
                  strlen(m->text ? m->text : ""));
        session_write_field(b, NULL, m->call_id, m->call_id ? strlen(m->call_id) : 0);
        session_write_field(b, NULL, m->text, m->text ? strlen(m->text) : 0);
        break;
    }
}

static int session_serialize(const session_data *sd, sbuf *b) {
    sb_puts(b, "oca-session 1\n");
    session_write_field(b, "id", sd->id, 32);
    sb_printf(b, "created %lld\n", sd->created);
    sb_printf(b, "last-used %lld\n", sd->last_used);
    session_write_field(b, "title", sd->title, sd->title ? strlen(sd->title) : 0);
    session_write_field(b, "workspace", sd->workspace, sd->workspace ? strlen(sd->workspace) : 0);
    session_write_field(b, "agents", sd->agents, sd->agents ? strlen(sd->agents) : 0);
    session_write_field(b, "model", sd->model, sd->model ? strlen(sd->model) : 0);
    session_write_field(b, "reasoning", sd->effort, sd->effort ? strlen(sd->effort) : 0);
    sb_puts(b, "mode normal\n");
    sb_printf(b, "goal %s %zu\n",
              sd->goal_state == OCA_GOAL_NONE ? "none" :
              sd->goal_state == OCA_GOAL_ACTIVE ? "active" :
              sd->goal_state == OCA_GOAL_PAUSED ? "paused" :
              sd->goal_state == OCA_GOAL_COMPLETED ? "completed" : "blocked",
              sd->goal ? strlen(sd->goal) : 0);
    if (sd->goal) sb_put(b, sd->goal, strlen(sd->goal));
    sb_put(b, "\n", 1);
    if (sd->goal_elapsed_ms) sb_printf(b, "goal-elapsed %llu\n", (unsigned long long)sd->goal_elapsed_ms);
    sb_printf(b, "messages %zu\n", sd->n_msgs);
    for (size_t i = 0; i < sd->n_msgs; i++) {
        session_write_message(b, &sd->msgs[i]);
        if (b->len > OCA_SNAPSHOT_MAX_BYTES) return -1;
    }
    if (sd->first_kept < 0) {
        sb_puts(b, "compaction none\n");
    } else {
        sb_printf(b, "compaction present %ld %ld %ld %zu\n",
                  sd->first_kept, sd->tokens_before, sd->tokens_after,
                  sd->summary ? strlen(sd->summary) : 0);
        if (sd->summary) sb_put(b, sd->summary, strlen(sd->summary));
        sb_put(b, "\n", 1);
    }
    sb_puts(b, "end\n");
    return b->oom || b->len > OCA_SNAPSHOT_MAX_BYTES ? -1 : 0;
}

/* ---- framing reader ---- */

typedef struct {
    const char *p, *end;
    const char *why;
} session_reader;

static int session_read_line(session_reader *r, const char **out, size_t *out_len) {
    const char *nl = memchr(r->p, '\n', (size_t)(r->end - r->p));
    if (!nl) {
        r->why = "truncated record";
        return -1;
    }
    *out = r->p;
    *out_len = (size_t)(nl - r->p);
    r->p = nl + 1;
    return 0;
}

static int session_read_payload(session_reader *r, size_t n, const char **out) {
    if ((size_t)(r->end - r->p) < n + 1) {
        r->why = "truncated payload";
        return -1;
    }
    *out = r->p;
    r->p += n;
    if (*r->p != '\n') {
        r->why = "missing payload terminator";
        return -1;
    }
    r->p++;
    return 0;
}

static int session_read_field(session_reader *r, const char *key, const char **out, size_t *out_len) {
    const char *line;
    size_t ll;
    if (session_read_line(r, &line, &ll) != 0) return -1;
    size_t kl = strlen(key);
    if (ll <= kl + 1 || memcmp(line, key, kl) != 0 || line[kl] != ' ') {
        r->why = "unexpected record";
        return -1;
    }
    char numbuf[32];
    size_t nl = ll - kl - 1;
    if (nl >= sizeof(numbuf)) {
        r->why = "bad length";
        return -1;
    }
    memcpy(numbuf, line + kl + 1, nl);
    numbuf[nl] = 0;
    char *endp = NULL;
    errno = 0;
    unsigned long long v = strtoull(numbuf, &endp, 10);
    if (*endp || errno == ERANGE) {
        r->why = "bad length";
        return -1;
    }
    if (v > OCA_SNAPSHOT_MAX_BYTES) {
        r->why = "payload too large";
        return -1;
    }
    if (session_read_payload(r, (size_t)v, out) != 0) return -1;
    *out_len = (size_t)v;
    return 0;
}

static int session_expect_record(session_reader *r, const char *key) {
    const char *line;
    size_t ll;
    if (session_read_line(r, &line, &ll) != 0) return -1;
    size_t kl = strlen(key);
    if (ll != kl || memcmp(line, key, kl) != 0) {
        r->why = "unexpected record";
        return -1;
    }
    return 0;
}

static int session_read_time(session_reader *r, const char *key, long long *out) {
    const char *line;
    size_t len;
    if (session_read_line(r, &line, &len) != 0) return -1;
    size_t key_len = strlen(key);
    if (len <= key_len + 1 || memcmp(line, key, key_len) != 0 ||
        line[key_len] != ' ') {
        r->why = "unexpected record";
        return -1;
    }
    char number[32];
    size_t number_len = len - key_len - 1;
    if (number_len >= sizeof(number)) {
        r->why = "invalid time value";
        return -1;
    }
    memcpy(number, line + key_len + 1, number_len);
    number[number_len] = '\0';
    char *end = NULL;
    errno = 0;
    long long value = strtoll(number, &end, 10);
    if (errno == ERANGE || !end || *end || value < 0) {
        r->why = "invalid time value";
        return -1;
    }
    *out = value;
    return 0;
}

static int hex_id_valid(const char *s, size_t n) {
    if (n != 32) return 0;
    for (size_t i = 0; i < n; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    }
    return 1;
}

/* Validates complete tool exchanges across the log. */
static int validate_log_exchanges(const msg_rec *msgs, size_t n, char *err, size_t errsz) {
    size_t i = 0;
    while (i < n) {
        if (msgs[i].role == OCA_MSG_ASSISTANT && msgs[i].n_calls > 0) {
            size_t k = msgs[i].n_calls;
            if (k >= n - i) {
                err_set(err, errsz, "missing tool results for a call group");
                return -1;
            }
            for (size_t j = 0; j < k; j++) {
                const msg_rec *res = &msgs[i + 1 + j];
                if (res->role != OCA_MSG_TOOL) {
                    err_set(err, errsz, "missing tool results for a call group");
                    return -1;
                }
                int found = 0;
                for (size_t q = 0; q < k; q++) {
                    if (msgs[i].calls[q].id && res->call_id &&
                        strcmp(msgs[i].calls[q].id, res->call_id) == 0) {
                        found++;
                    }
                }
                if (found != 1) {
                    err_set(err, errsz, "orphan or duplicate tool result");
                    return -1;
                }
            }
            /* Duplicate result ids inside one group. */
            for (size_t a = 0; a < k; a++) {
                for (size_t bq = a + 1; bq < k; bq++) {
                    if (msgs[i + 1 + a].call_id && msgs[i + 1 + bq].call_id &&
                        strcmp(msgs[i + 1 + a].call_id, msgs[i + 1 + bq].call_id) == 0) {
                        err_set(err, errsz, "duplicate tool result");
                        return -1;
                    }
                }
            }
            i += k + 1;
            continue;
        }
        if (msgs[i].role == OCA_MSG_TOOL) {
            err_set(err, errsz, "orphan tool result");
            return -1;
        }
        i++;
    }
    return 0;
}

/* On failure, the caller releases the partially constructed message. */
static int session_read_message(session_reader *r, msg_rec *m,
                                const msg_rec *previous, size_t previous_count) {
    const char *mline;
    size_t mll;
    if (session_read_line(r, &mline, &mll) != 0) return -1;
    if (mll >= 13 && memcmp(mline, "message tool ", 13) == 0) {
        /* message tool STATE CID_LEN TXT_LEN */
        const char *rest = mline + 13;
        int ts;
        size_t rl = mll - 13;
        if (rl >= 9 && memcmp(rest, "complete ", 9) == 0) {
            ts = OCA_TOOL_COMPLETE;
            rest += 9;
        } else if (rl >= 12 && memcmp(rest, "interrupted ", 12) == 0) {
            ts = OCA_TOOL_INTERRUPTED;
            rest += 12;
        } else if (rl >= 6 && memcmp(rest, "error ", 6) == 0) {
            ts = OCA_TOOL_ERROR;
            rest += 6;
        } else {
            r->why = "invalid tool state";
            return -1;
        }
        unsigned long long c1, c2;
        int consumed = 0;
        if (sscanf(rest, "%llu %llu%n", &c1, &c2, &consumed) != 2 ||
            (size_t)consumed != (size_t)(mline + mll - rest)) {
            r->why = "bad lengths";
            return -1;
        }
        if (c1 > OCA_TOOL_CALL_ID_MAX_BYTES ||
            c2 > OCA_TOOL_OUTPUT_MAX_BYTES ||
            c1 > OCA_SNAPSHOT_MAX_BYTES - c2) {
            r->why = "payload too large";
            return -1;
        }
        const char *pc, *pt;
        if (session_read_payload(r, (size_t)c1, &pc) != 0) return -1;
        if (session_read_payload(r, (size_t)c2, &pt) != 0) return -1;
        if (c1 == 0 || !utf8_text_valid(pc, (size_t)c1) ||
            !utf8_text_valid(pt, (size_t)c2)) {
            r->why = "invalid tool result";
            return -1;
        }
        m->role = OCA_MSG_TOOL;
        m->state = ts;
        m->call_id = xstrndup(pc, (size_t)c1);
        m->text = xstrndup(pt, (size_t)c2);
        if (!m->call_id || !m->text) {
            r->why = "out of memory";
            return -1;
        }
        return 0;
    }
    if (mll >= 13 && memcmp(mline, "message user ", 13) == 0) {
        const char *rest = mline + 13;
        unsigned long long tl;
        int consumed = 0;
        if (sscanf(rest, "%llu%n", &tl, &consumed) != 1 ||
            (size_t)consumed != mll - 13 || tl > OCA_SNAPSHOT_MAX_BYTES) {
            r->why = "bad length";
            return -1;
        }
        const char *pt;
        if (session_read_payload(r, (size_t)tl, &pt) != 0) return -1;
        if (tl > OCA_PROMPT_MAX_BYTES || !utf8_text_valid(pt, (size_t)tl)) {
            r->why = "invalid UTF-8";
            return -1;
        }
        m->role = OCA_MSG_USER;
        m->text = xstrndup(pt, (size_t)tl);
        if (!m->text) {
            r->why = "out of memory";
            return -1;
        }
        return 0;
    }
    if (mll >= 15 && memcmp(mline, "message system ", 15) == 0) {
        const char *rest = mline + 15;
        unsigned long long tl;
        int consumed = 0;
        if (sscanf(rest, "%llu%n", &tl, &consumed) != 1 ||
            (size_t)consumed != mll - 15 || tl > OCA_SNAPSHOT_MAX_BYTES) {
            r->why = "bad length";
            return -1;
        }
        const char *pt;
        if (session_read_payload(r, (size_t)tl, &pt) != 0) return -1;
        if (!utf8_text_valid(pt, (size_t)tl)) {
            r->why = "invalid UTF-8";
            return -1;
        }
        m->role = OCA_MSG_SYSTEM;
        m->text = xstrndup(pt, (size_t)tl);
        if (!m->text) {
            r->why = "out of memory";
            return -1;
        }
        return 0;
    }
    if (mll >= 18 && memcmp(mline, "message assistant ", 18) == 0) {
        const char *rest = mline + 18;
        size_t rl = mll - 18;
        int as;
        if (rl >= 9 && memcmp(rest, "complete ", 9) == 0) {
            as = MS_COMPLETE;
            rest += 9;
        } else if (rl >= 12 && memcmp(rest, "interrupted ", 12) == 0) {
            as = MS_INTERRUPTED;
            rest += 12;
        } else {
            r->why = "invalid assistant state";
            return -1;
        }
        unsigned long long l[7];
        int consumed = 0;
        if (sscanf(rest, "%llu %llu %llu %llu %llu %llu %llu%n",
                   &l[0], &l[1], &l[2], &l[3], &l[4], &l[5], &l[6],
                   &consumed) != 7 ||
            (size_t)consumed != (size_t)(mline + mll - rest)) {
            r->why = "bad lengths";
            return -1;
        }
        for (int q = 0; q < 7; q++) {
            if (l[q] > OCA_SNAPSHOT_MAX_BYTES) {
                r->why = "payload too large";
                return -1;
            }
        }
        if (l[3] > OCA_LLM_RESPONSE_MAX_BYTES ||
            l[4] > OCA_LLM_RESPONSE_MAX_BYTES - l[3] ||
            l[5] > OCA_MODEL_STATE_MAX_BYTES) {
            r->why = "assistant message exceeds the limit";
            return -1;
        }
        if (l[6] > OCA_TOOL_CALLS_MAX) {
            r->why = "call count exceeds the limit";
            return -1;
        }
        const char *pl[6];
        for (int q = 0; q < 6; q++) {
            if (session_read_payload(r, (size_t)l[q], &pl[q]) != 0) return -1;
        }
        if (l[0] == 0 || l[0] > OCA_LLM_MODEL_ID_MAX ||
            !utf8_text_valid(pl[0], (size_t)l[0]) ||
            !utf8_text_valid(pl[1], (size_t)l[1]) ||
            !utf8_text_valid(pl[3], (size_t)l[3]) ||
            (l[4] && !utf8_text_valid(pl[4], (size_t)l[4])) ||
            (l[5] && !utf8_text_valid(pl[5], (size_t)l[5])) ||
            !utf8_text_valid(pl[2], (size_t)l[2]) ||
            !((l[2] == 4 && memcmp(pl[2], "plan", 4) == 0) ||
              (l[2] == 6 && memcmp(pl[2], "normal", 6) == 0))) {
            r->why = "invalid UTF-8";
            return -1;
        }
        if (l[1] && !reasoning_name_valid(pl[1], (size_t)l[1])) {
            r->why = "invalid assistant reasoning";
            return -1;
        }
        m->role = OCA_MSG_ASSISTANT;
        m->state = as;
        m->model_id = xstrndup(pl[0], (size_t)l[0]);
        m->effort = xstrndup(pl[1], (size_t)l[1]);
        m->text = xstrndup(pl[3], (size_t)l[3]);
        m->reasoning = l[4] ? xstrndup(pl[4], (size_t)l[4]) : NULL;
        m->continuation = l[5] ? xstrndup(pl[5], (size_t)l[5]) : NULL;
        m->n_calls = (size_t)l[6];
        m->calls = m->n_calls ? calloc(m->n_calls, sizeof(*m->calls)) : NULL;
        if (!m->model_id || !m->effort || !m->text ||
           (m->n_calls && !m->calls)) {
            r->why = "out of memory";
            return -1;
        }
        for (size_t k = 0; k < m->n_calls; k++) {
            const char *cline;
            size_t cll;
            if (session_read_line(r, &cline, &cll) != 0) return -1;
            if (cll < 5 || memcmp(cline, "call ", 5) != 0) {
                r->why = "expected call record";
                return -1;
            }
            unsigned long long c1, c2, c3;
            int consumed = 0;
            if (sscanf(cline + 5, "%llu %llu %llu%n", &c1, &c2, &c3,
                       &consumed) != 3 ||
                (size_t)consumed != cll - 5) {
                r->why = "bad call lengths";
                return -1;
            }
            if (c1 > OCA_SNAPSHOT_MAX_BYTES || c2 > OCA_SNAPSHOT_MAX_BYTES ||
                c3 > OCA_SNAPSHOT_MAX_BYTES ||
                c1 > OCA_SNAPSHOT_MAX_BYTES - c2 ||
                c1 + c2 > OCA_SNAPSHOT_MAX_BYTES - c3) {
                r->why = "payload too large";
                return -1;
            }
            const char *pi, *pn, *pa;
            if (session_read_payload(r, (size_t)c1, &pi) != 0) return -1;
            if (session_read_payload(r, (size_t)c2, &pn) != 0) return -1;
            if (session_read_payload(r, (size_t)c3, &pa) != 0) return -1;
            if (c1 == 0 || c2 == 0 || c1 > OCA_TOOL_CALL_ID_MAX_BYTES ||
                c2 > OCA_TOOL_NAME_MAX_BYTES || c3 > OCA_TOOL_RESULT_MAX_BYTES ||
                !utf8_text_valid(pi, (size_t)c1) ||
                !utf8_text_valid(pn, (size_t)c2) ||
                !utf8_text_valid(pa, (size_t)c3)) {
                r->why = "invalid call identity";
                return -1;
            }
            for (size_t q = 0; q < k; q++) {
                if (strlen(m->calls[q].id) == c1 &&
                    memcmp(m->calls[q].id, pi, (size_t)c1) == 0) {
                    r->why = "duplicate call id";
                    return -1;
                }
            }
            for (size_t prior = 0; prior < previous_count; prior++) {
                if (previous[prior].role != OCA_MSG_ASSISTANT) continue;
                for (size_t q = 0; q < previous[prior].n_calls; q++) {
                    if (strlen(previous[prior].calls[q].id) == c1 &&
                        memcmp(previous[prior].calls[q].id, pi,
                               (size_t)c1) == 0) {
                        r->why = "duplicate call id";
                        return -1;
                    }
                }
            }
            m->calls[k].id = xstrndup(pi, (size_t)c1);
            m->calls[k].name = xstrndup(pn, (size_t)c2);
            m->calls[k].args = xstrndup(pa, (size_t)c3);
            if (!m->calls[k].id || !m->calls[k].name || !m->calls[k].args) {
                r->why = "out of memory";
                return -1;
            }
        }
        return 0;
    }
    if (mll == 10 && memcmp(mline, "compaction", 10) == 0) {
        r->why = "message count mismatch";
        return -1;
    }
    if (mll == 3 && memcmp(mline, "end", 3) == 0) {
        r->why = "message count mismatch";
        return -1;
    }
    r->why = "unknown message record";
    return -1;
}

static int session_deserialize(const char *data, size_t len, session_data *sd,
                               char *err, size_t errsz) {
    session_data_init(sd);
    session_reader r = { data, data + len, "syntax" };
    const char *s;
    size_t sl;

    if (session_expect_record(&r, "oca-session 1") != 0) goto fail;
    if (session_read_field(&r, "id", &s, &sl) != 0) goto fail;
    if (!hex_id_valid(s, sl)) {
        r.why = "invalid session id";
        goto fail;
    }
    memcpy(sd->id, s, 32);
    sd->id[32] = 0;
    if (session_read_time(&r, "created", &sd->created) != 0) goto fail;
    if (session_read_time(&r, "last-used", &sd->last_used) != 0) goto fail;
    if (session_read_field(&r, "title", &s, &sl) != 0) goto fail;
    if (!utf8_text_valid(s, sl)) {
        r.why = "invalid title";
        goto fail;
    }
    sd->title = xstrndup(s, sl);
    if (!sd->title) {
        r.why = "out of memory";
        goto fail;
    }
    if (session_read_field(&r, "workspace", &s, &sl) != 0) goto fail;
    if (sl == 0) {
        r.why = "empty workspace";
        goto fail;
    }
    if (!utf8_text_valid(s, sl)) {
        r.why = "invalid workspace";
        goto fail;
    }
    sd->workspace = xstrndup(s, sl);
    if (!sd->workspace) {
        r.why = "out of memory";
        goto fail;
    }
    if (session_read_field(&r, "agents", &s, &sl) != 0) goto fail;
    if (sl > 0) {
        if (!utf8_text_valid(s, sl)) {
            r.why = "project context is not valid UTF-8";
            goto fail;
        }
        if (sl > OCA_PROJECT_CONTEXT_MAX_BYTES) {
            r.why = "project context exceeds the limit";
            goto fail;
        }
        sd->agents = xstrndup(s, sl);
    }
    if (session_read_field(&r, "model", &s, &sl) != 0) goto fail;
    if (sl > OCA_LLM_MODEL_ID_MAX || !utf8_text_valid(s, sl)) {
        r.why = "invalid model";
        goto fail;
    }
    /* An empty model records an unconfigured conversation. */
    sd->model = sl ? xstrndup(s, sl) : xstrdup("");
    if (!sd->model) {
        r.why = "out of memory";
        goto fail;
    }
    if (session_read_field(&r, "reasoning", &s, &sl) != 0) goto fail;
    /* An empty reasoning records no configured effort. */
    if (sl && !reasoning_name_valid(s, sl)) {
        r.why = "invalid reasoning";
        goto fail;
    }
    sd->effort = xstrndup(s, sl);
    if (!sd->effort) {
        r.why = "out of memory";
        goto fail;
    }
    /* The mode record is one plain line. Read it once before matching. */
    {
        const char *line;
        size_t ll;
        if (session_read_line(&r, &line, &ll) != 0) goto fail;
        /* Historical plan sessions resume with the ordinary tool contract. */
        if (!((ll == 11 && memcmp(line, "mode normal", 11) == 0) ||
              (ll == 9 && memcmp(line, "mode plan", 9) == 0))) {
            r.why = "invalid mode";
            goto fail;
        }
    }
    /* goal record */
    {
        const char *line;
        size_t ll;
        if (session_read_line(&r, &line, &ll) != 0) goto fail;
        int gs = -1;
        if (ll >= 10 && memcmp(line, "goal none ", 10) == 0) gs = OCA_GOAL_NONE;
        else if (ll >= 11 && memcmp(line, "goal active ", 11) == 0) gs = OCA_GOAL_ACTIVE;
        else if (ll >= 11 && memcmp(line, "goal paused ", 11) == 0) gs = OCA_GOAL_PAUSED;
        else if (ll >= 14 && memcmp(line, "goal completed ", 14) == 0) gs = OCA_GOAL_COMPLETED;
        else if (ll >= 12 && memcmp(line, "goal blocked ", 12) == 0) gs = OCA_GOAL_BLOCKED;
        if (gs < 0) {
            r.why = "invalid goal state";
            goto fail;
        }
        const char *sp = memchr(line, ' ', ll);
        const char *sp2 = sp ? memchr(sp + 1, ' ', ll - (size_t)(sp - line) - 1) : NULL;
        if (!sp2) {
            r.why = "invalid goal record";
            goto fail;
        }
        char numbuf[32];
        size_t nl = ll - (size_t)(sp2 - line) - 1;
        if (nl >= sizeof(numbuf)) {
            r.why = "bad length";
            goto fail;
        }
        memcpy(numbuf, sp2 + 1, nl);
        numbuf[nl] = 0;
        errno = 0;
        char *number_end = NULL;
        unsigned long long gn = strtoull(numbuf, &number_end, 10);
        if (errno == ERANGE || !number_end || *number_end ||
            gn > OCA_SNAPSHOT_MAX_BYTES) {
            r.why = "bad length";
            goto fail;
        }
        if (session_read_payload(&r, (size_t)gn, &s) != 0) goto fail;
        sl = (size_t)gn;
        if (gs == OCA_GOAL_NONE && sl != 0) {
            r.why = "none goal requires zero length";
            goto fail;
        }
        if (gs != OCA_GOAL_NONE && sl == 0) {
            r.why = "goal state requires an objective";
            goto fail;
        }
        if (sl > 0 && !utf8_text_valid(s, sl)) {
            r.why = "goal is not valid UTF-8";
            goto fail;
        }
        sd->goal_state = gs;
        if (sl > 0) {
            sd->goal = xstrndup(s, sl);
            if (!sd->goal) {
                r.why = "out of memory";
                goto fail;
            }
            if (utf8_chars(sd->goal) > OCA_GOAL_MAX_CHARS) {
                r.why = "goal exceeds the character limit";
                goto fail;
            }
        }
    }
    if ((size_t)(r.end - r.p) >= 13 && !memcmp(r.p, "goal-elapsed ", 13)) {
        const char *line;
        size_t length;
        if (session_read_line(&r, &line, &length)) goto fail;
        char number[32];
        size_t n = length - 13;
        if (!n || n >= sizeof(number)) { r.why = "invalid goal duration"; goto fail; }
        memcpy(number, line + 13, n);
        number[n] = 0;
        errno = 0;
        unsigned long long elapsed = strtoull(number, NULL, 10);
        if (strspn(number, "0123456789") != n || errno == ERANGE || elapsed > LLONG_MAX ||
            (sd->goal_state == OCA_GOAL_NONE && elapsed)) { r.why = "invalid goal duration"; goto fail; }
        sd->goal_elapsed_ms = (uint64_t)elapsed;
    }
    /* messages count */
    {
        const char *line;
        size_t ll;
        if (session_read_line(&r, &line, &ll) != 0) goto fail;
        if (ll < 9 || memcmp(line, "messages ", 9) != 0) {
            r.why = "expected messages record";
            goto fail;
        }
        char numbuf[32];
        size_t nl = ll - 9;
        if (nl >= sizeof(numbuf)) {
            r.why = "bad count";
            goto fail;
        }
        memcpy(numbuf, line + 9, nl);
        numbuf[nl] = 0;
        errno = 0;
        char *number_end = NULL;
        unsigned long long mc = strtoull(numbuf, &number_end, 10);
        if (errno == ERANGE || !number_end || *number_end || mc > OCA_MESSAGES_MAX) {
            r.why = "message count exceeds the limit";
            goto fail;
        }
        sd->n_msgs = 0;
        sd->msgs = calloc(mc ? mc : 1, sizeof(*sd->msgs));
        if (!sd->msgs) {
            r.why = "out of memory";
            goto fail;
        }
        for (size_t mi = 0; mi < mc; mi++) {
            msg_rec *message = &sd->msgs[sd->n_msgs++];
            if (session_read_message(&r, message, sd->msgs, mi) != 0) goto fail;
        }
    }
    /* Compaction record. */
    {
        const char *line;
        size_t ll;
        if (session_read_line(&r, &line, &ll) != 0) goto fail;
        if (ll == 15 && memcmp(line, "compaction none", 15) == 0) {
            sd->first_kept = -1;
        } else if (ll >= 19 && memcmp(line, "compaction present ", 19) == 0) {
            long fk, tb, ta;
            unsigned long long slen;
            int consumed = 0;
            if (sscanf(line + 19, "%ld %ld %ld %llu%n", &fk, &tb, &ta, &slen,
                       &consumed) != 4 || (size_t)consumed != ll - 19) {
                r.why = "bad compaction record";
                goto fail;
            }
            if (fk < 0 || (size_t)fk >= sd->n_msgs || tb < 0 || ta < 0 || ta > tb ||
                slen > OCA_SNAPSHOT_MAX_BYTES) {
                r.why = "invalid compaction boundaries";
                goto fail;
            }
            if (sd->n_msgs > 0 && sd->msgs[fk].role != OCA_MSG_USER) {
                r.why = "compaction boundary is not a user message";
                goto fail;
            }
            const char *ps;
            if (session_read_payload(&r, (size_t)slen, &ps) != 0) goto fail;
            if (slen == 0 || !utf8_text_valid(ps, (size_t)slen)) {
                r.why = "invalid summary";
                goto fail;
            }
            sd->first_kept = fk;
            sd->tokens_before = tb;
            sd->tokens_after = ta;
            sd->summary = xstrndup(ps, (size_t)slen);
            if (!sd->summary) {
                r.why = "out of memory";
                goto fail;
            }
        } else {
            r.why = "invalid compaction record";
            goto fail;
        }
    }
    if (session_expect_record(&r, "end") != 0) goto fail;
    if (r.p != r.end) {
        r.why = "trailing data";
        goto fail;
    }
    if (validate_log_exchanges(sd->msgs, sd->n_msgs, err, errsz) != 0) {
        r.why = "invalid log";
        goto fail_no_why;
    }
    return 0;
fail:
    err_set(err, errsz, "session file rejected: %s", r.why ? r.why : "syntax");
fail_no_why:
    session_data_free(sd);
    session_data_init(sd);
    return -1;
}

/* ============================================================================
 * Core structure
 * ========================================================================== */

enum {
    ACT_SET_ROUTE,
    ACT_GOAL, ACT_NEW, ACT_SAVE, ACT_SESSIONS, ACT_RESUME, ACT_SKILLS, ACT_QUIT, ACT_REPLAY, ACT_BASH
};

typedef struct {
    int kind;
    char *text;
    oca_reasoning effort;
    int goal_cmd;
} action_rec;

/* Runtime hint preference and context state, adapted from ds4-agent.
 * See the ds4 copyright and permission notice in LICENSE. */
typedef struct {
    int enabled;
    int instructed;
    enum { HINTS_UNSET, HINTS_OFF, HINTS_ON, HINTS_UNKNOWN } applied;
} hints_state;

struct oca_core {
    hints_state hints;
    oca_tool_executor *executor;
    /* fixed at creation */
    oca_llm *llm;
    int flags;
    int interactive;
    int ephemeral;
    oca_core *parent;           /* borrowed until this core is destroyed */
    int side_in_use;
    char *side_snapshot;        /* immutable serialized copy, protected by mu */
    size_t side_snapshot_len;
    size_t side_prefix_count;
    size_t side_history_reserved;
    char *snapshot_goal;
    char *workspace;
    oca_core_route_cbs route_cbs;
    oca_trace_sink trace;
    skill_catalog catalog;

    /* session (worker-owned; reads take mu) */
    char sid[33];
    long long created, last_used;
    char *title;
    char *agents;
    int goal_state;
    char *goal_text;
    uint64_t goal_elapsed_ms, goal_started_ms;
    msg_rec *msgs;
    size_t n_msgs, cap_msgs;
    long comp_first_kept;
    long tokens_before, tokens_after;
    char *summary;
    int dirty;
    int conversation_dirty;
    int has_session;

    /* route */
    char route_model[OCA_LLM_MODEL_ID_MAX + 1];
    oca_reasoning route_effort;
    char staged_model[OCA_LLM_MODEL_ID_MAX + 1];
    oca_reasoning staged_effort;
    int has_staged_route;

    /* status */
    oca_core_state state;
    int has_cache;
    long cache_hit;
    int has_cache_input;
    long cache_input;
    int working;
    int working_paused;
    uint64_t working_start;
    uint64_t working_elapsed;

    /* sync */
    pthread_mutex_t mu;
    pthread_cond_t cond;
    pthread_t worker;
    int worker_started;
    atomic_int running;
    action_rec aq[ACTION_RING_SIZE];
    size_t aq_head, aq_tail;
    oca_core_event eq[EVENT_RING_SIZE];
    size_t eq_head, eq_tail;
    int wake_pending;
    int wake_pipe[2];
    atomic_int cancel_latch;

    /* worker-local */
    uint64_t request_seq;
    uint64_t manual_seq;
    char **followups;
    size_t n_followups;
    char *pending_turn_prompt;
    char *goal_first_turn;
    char *ia_answers_json;
    int ia_waiting;
    int save_answer;             /* -1 none, 0 no, 1 yes */
    int save_waiting;
    int quit_requested;
    int trace_failed;
    int trace_reported;
    int cli_route_locked;
};


static uint64_t core_now_ms(void);

/* Caller holds mu when the core has a worker. */
static uint64_t goal_elapsed(const oca_core *c) {
    uint64_t now = core_now_ms();
    return c->goal_elapsed_ms + (c->goal_started_ms && now > c->goal_started_ms ? now - c->goal_started_ms : 0);
}

/* Returns the configured route level name, or an empty string. */
static const char *route_reasoning_name(const oca_core *c) {
    const oca_model *m = oca_llm_model_resolve(c->llm, c->route_model);
    const char *name = oca_model_reasoning_name(m, c->route_effort);
    return name ? name : "";
}

/* Caller holds mu. The view borrows all fields and must not be passed to session_data_free. */
static void session_borrow(oca_core *c, session_data *view) {
    *view = (session_data){
        .created = c->created, .last_used = c->last_used,
        .title = c->title, .workspace = c->workspace, .agents = c->agents,
        .model = c->route_model, .effort = (char *)route_reasoning_name(c),
        .goal_state = c->goal_state, .goal = c->goal_text,
        .goal_elapsed_ms = goal_elapsed(c),
        .msgs = c->msgs, .n_msgs = c->n_msgs, .first_kept = c->comp_first_kept,
        .summary = c->summary, .tokens_before = c->tokens_before,
        .tokens_after = c->tokens_after,
    };
    memcpy(view->id, c->sid, sizeof(view->id));
}

/* The worker publishes complete exchange boundaries for concurrent readers.
 * Serialization stays in memory; it cannot persist conversation content. */
static void publish_side_snapshot(oca_core *c) {
    if (c->ephemeral || !c->interactive || !c->has_session) return;
    pthread_mutex_lock(&c->mu);
    session_data data;
    session_borrow(c, &data);
    msg_rec *pending = NULL;
    size_t group = c->n_msgs;
    while (group && c->msgs[group - 1].role == OCA_MSG_TOOL) group--;
    if (group && c->msgs[group - 1].role == OCA_MSG_ASSISTANT) {
        const msg_rec *assistant = &c->msgs[group - 1];
        size_t completed = c->n_msgs - group;
        if (completed < assistant->n_calls) {
            size_t remaining = assistant->n_calls - completed;
            pending = calloc(c->n_msgs + remaining, sizeof(*pending));
            if (!pending) {
                free(c->side_snapshot);
                c->side_snapshot = NULL;
                c->side_snapshot_len = 0;
                pthread_mutex_unlock(&c->mu);
                return;
            }
            memcpy(pending, c->msgs, c->n_msgs * sizeof(*pending));
            for (size_t i = 0; i < remaining; i++) {
                pending[c->n_msgs + i].role = OCA_MSG_TOOL;
                pending[c->n_msgs + i].state = OCA_TOOL_COMPLETE;
                pending[c->n_msgs + i].call_id = assistant->calls[completed + i].id;
                pending[c->n_msgs + i].text =
                    "This main-thread call is pending. Its result is outside this frozen snapshot.";
            }
            data.msgs = pending;
            data.n_msgs += remaining;
        }
    }
    sbuf buffer;
    sb_init(&buffer);
    int result = session_serialize(&data, &buffer);
    free(pending);
    free(c->side_snapshot);
    c->side_snapshot = result ? NULL : sb_take(&buffer);
    c->side_snapshot_len = c->side_snapshot ? strlen(c->side_snapshot) : 0;
    pthread_mutex_unlock(&c->mu);
    sb_free(&buffer);
}

static uint64_t core_now_ms(void) {
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000 + (uint64_t)value.tv_nsec / 1000000;
}

/* Caller holds mu. Queued turns start when they become the active work. */
static void working_start_locked(oca_core *c) {
    if (c->working) return;
    c->working = 1;
    c->working_paused = 0;
    c->working_elapsed = 0;
    c->working_start = core_now_ms();
}

static void working_start(oca_core *c) {
    pthread_mutex_lock(&c->mu);
    working_start_locked(c);
    pthread_mutex_unlock(&c->mu);
}

/* ============================================================================
 * Events and trace
 * ========================================================================== */

static _Thread_local int worker_event_producer;

/* A route is usable when its model resolves, holds credentials, and its
 * effort matches the configured levels of that model. */
static int route_usable(oca_core *c, const char *model_id, oca_reasoning effort) {
    const oca_model *m = oca_llm_model_resolve(c->llm, model_id);
    if (!m || !oca_llm_model_available(c->llm, m)) return 0;
    if (m->reasoning_count == 0) return effort == OCA_REASONING_NONE;
    return oca_model_supports_reasoning(m, effort);
}

static int route_available(oca_core *c) {
    return route_usable(c, c->route_model, c->route_effort);
}

static int ev_push(oca_core *c, oca_core_event *ev) {
    int need_wake = 0;
    pthread_mutex_lock(&c->mu);
    while ((c->eq_tail + 1) % EVENT_RING_SIZE == c->eq_head &&
           worker_event_producer && c->running)
        pthread_cond_wait(&c->cond, &c->mu);
    if ((c->eq_tail + 1) % EVENT_RING_SIZE == c->eq_head) {
        /* A consumer-side event must not wait for its own drain. */
        pthread_mutex_unlock(&c->mu);
        oca_core_event_release(ev);
        return -1;
    }
    c->eq[c->eq_tail] = *ev;
    c->eq_tail = (c->eq_tail + 1) % EVENT_RING_SIZE;
    if (!c->wake_pending) {
        c->wake_pending = 1;
        need_wake = 1;
    }
    pthread_mutex_unlock(&c->mu);
    pthread_cond_broadcast(&c->cond);
    if (need_wake) {
        char b = 'x';
        ssize_t w = write(c->wake_pipe[1], &b, 1);
        (void)w;
    }
    return 0;
}

static void fill_status_locked(oca_core *c, oca_core_status *st) {
    memset(st, 0, sizeof(*st));
    st->state = c->state;
    st->goal = (oca_goal_state)c->goal_state;
    st->goal_elapsed_ms = goal_elapsed(c);
    snprintf(st->model_id, sizeof(st->model_id), "%s", c->route_model);
    snprintf(st->reasoning_name, sizeof(st->reasoning_name), "%s",
             route_reasoning_name(c));
    st->route_available = route_available(c);
    const oca_model *m = oca_llm_model_resolve(c->llm, c->route_model);
    st->model_supports_reasoning = m ? m->reasoning_stream : 0;
    st->context_limit = m ? m->input_limit : 0;
    st->has_used_tokens = c->has_cache_input;
    st->used_tokens = c->has_cache_input ? c->cache_input : 0;
    st->has_cache = c->has_cache;
    st->cache_hit_tokens = c->cache_hit;
    st->has_cache_input = c->has_cache_input;
    st->cache_input_tokens = c->cache_input;
    st->working = c->working;
    st->working_paused = c->working_paused;
    st->cancelling = c->working && atomic_load(&c->cancel_latch);
    st->working_ms = c->working_elapsed;
    if (c->working && !c->working_paused)
        st->working_ms += core_now_ms() - c->working_start;
    st->conversation_dirty = c->conversation_dirty;
    st->followup_count = c->n_followups;
    if (c->n_followups) {
        size_t n = strlen(c->followups[0]);
        if (n >= sizeof(st->followup_preview)) n = sizeof(st->followup_preview) - 1;
        while (n && !utf8_valid(c->followups[0], n)) n--;
        memcpy(st->followup_preview, c->followups[0], n);
    }
    st->session_dirty = c->dirty;
    st->has_session = c->has_session;
    snprintf(st->session_id, sizeof(st->session_id), "%s", c->sid);
    snprintf(st->session_title, sizeof(st->session_title), "%s", c->title ? c->title : "");
    st->has_project_context = c->agents != NULL;
    st->skill_count = c->catalog.n;
}

static void push_status(oca_core *c) {
    oca_core_event ev = {0};
    ev.kind = OCA_CE_STATUS;
    pthread_mutex_lock(&c->mu);
    ev.state = c->state;
    ev.goal = (oca_goal_state)c->goal_state;
    snprintf(ev.model_id, sizeof(ev.model_id), "%s", c->route_model);
    snprintf(ev.reasoning_name, sizeof(ev.reasoning_name), "%s",
             route_reasoning_name(c));
    const oca_model *m = oca_llm_model_resolve(c->llm, c->route_model);
    ev.route_available = route_available(c);
    ev.has_used_tokens = c->has_cache_input;
    ev.used_tokens = c->has_cache_input ? c->cache_input : 0;
    ev.context_limit = m ? m->input_limit : 0;
    pthread_mutex_unlock(&c->mu);
    ev_push(c, &ev);
}

static void push_error(oca_core *c, const char *code, const char *fmt, ...) {
    oca_core_event ev = {0};
    ev.kind = OCA_CE_ERROR;
    va_list ap;
    va_start(ap, fmt);
    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    ev.name = xstrdup(code);
    ev.text = xstrdup(msg);
    ev_push(c, &ev);
}

static void trace_line(oca_core *c, const char *kind, const char *json_body) {
    if (!c->trace.write_record) return;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    sbuf line;
    sb_init(&line);
    sb_printf(&line, "%lld.%03d %s ", (long long)tv.tv_sec,
              (int)(tv.tv_usec / 1000), kind);
    sb_puts(&line, json_body);
    sb_puts(&line, "\n");
    if (line.oom || !line.buf || c->trace.write_record(c->trace.user, line.buf) != 0)
        c->trace_failed = 1;
    sb_free(&line);
}

static void trace_tool(oca_core *c, const char *event, const char *name,
                       const char *call_id, const char *args_json, long bytes,
                       const char *state, long exit_code) {
    sbuf b;
    sb_init(&b);
    sb_puts(&b, "{\"event\":");
    sb_json_str(&b, event);
    sb_puts(&b, ",\"name\":");
    sb_json_str(&b, name);
    if (call_id) {
        sb_puts(&b, ",\"call_id\":");
        sb_json_str(&b, call_id);
    }
    if (args_json) {
        sb_puts(&b, ",\"args\":");
        sb_puts(&b, args_json);
    }
    if (bytes >= 0) sb_printf(&b, ",\"bytes\":%ld", bytes);
    if (state) {
        sb_puts(&b, ",\"state\":");
        sb_json_str(&b, state);
    }
    if (exit_code >= 0) sb_printf(&b, ",\"exit\":%ld", exit_code);
    sb_puts(&b, "}");
    if (!b.oom) trace_line(c, "tool", b.buf);
    sb_free(&b);
}

static void trace_llm(oca_core *c, const char *event, oca_purpose purpose,
                      const char *model, long a, long b) {
    static const char *const P[] = { "turn", "goal_continuation", "compaction" };
    sbuf j;
    sb_init(&j);
    sb_puts(&j, "{\"event\":");
    sb_json_str(&j, event);
    sb_printf(&j, ",\"purpose\":\"%s\",\"model\":", P[purpose]);
    sb_json_str(&j, model ? model : "");
    sb_printf(&j, ",\"request_id\":%llu", (unsigned long long)c->request_seq);
    if (a >= 0) sb_printf(&j, ",\"estimate\":%ld", a);
    if (b >= 0) sb_printf(&j, ",\"tokens\":%ld", b);
    sb_puts(&j, "}");
    if (!j.oom) trace_line(c, "llm", j.buf);
    sb_free(&j);
}

static void trace_llm_error(oca_core *c, const oca_event *event) {
    if (!c->trace.write_record) return;
    sbuf j;
    sb_init(&j);
    sb_printf(&j, "{\"event\":\"error\",\"request_id\":%llu,\"model\":",
              (unsigned long long)event->request_id);
    sb_json_str(&j, c->route_model);
    sb_puts(&j, ",\"category\":");
    sb_json_str(&j, event->category);
    const oca_llm_diagnostic *d = event->diagnostic;
    if (d) {
        sb_puts(&j, ",\"diagnostic\":{");
        sb_puts(&j, "\"provider_event\":");
        sb_json_str(&j, d->provider_event);
        sb_puts(&j, ",\"check\":");
        sb_json_str(&j, d->check);
        sb_puts(&j, ",\"field\":");
        sb_json_str(&j, d->field);
        sb_puts(&j, ",\"expected\":");
        sb_json_str(&j, d->expected);
        sb_puts(&j, ",\"received\":");
        sb_json_str(&j, d->received);
        sb_printf(&j, ",\"event_index\":%zu,\"frame_bytes\":%zu,\"wire_bytes\":%zu,\"response_bytes\":%zu",
                  d->event_index, d->frame_bytes, d->wire_bytes, d->response_bytes);
        if (d->has_http_status) sb_printf(&j, ",\"http_status\":%ld", d->http_status);
        else if (d->has_transport) sb_puts(&j, ",\"http_status\":null");
        if (d->has_item_index) sb_printf(&j, ",\"item_index\":%ld", d->item_index);
        if (d->has_limit) sb_printf(&j, ",\"actual\":%zu,\"limit\":%zu", d->actual, d->limit);
        if (d->has_transport) {
            const oca_llm_transport_diagnostic *t = &d->transport;
            sb_printf(&j, ",\"transport\":{\"attempt\":%u,\"curl_code\":%d,\"curl_message\":",
                      t->attempt, t->curl_code);
            sb_json_str(&j, t->curl_message);
            sb_printf(&j, ",\"body_received\":%s,\"request_bytes\":%zu",
                      t->body_received ? "true" : "false", t->request_bytes);
            const struct { const char *name; int64_t value; } metrics[] = {
                {"os_errno", t->os_errno}, {"uploaded_bytes", t->uploaded_bytes},
                {"downloaded_bytes", t->downloaded_bytes}, {"dns_us", t->dns_us},
                {"connect_us", t->connect_us}, {"tls_us", t->tls_us},
                {"pretransfer_us", t->pretransfer_us}, {"first_byte_us", t->first_byte_us},
                {"total_us", t->total_us}, {"body_idle_us", t->body_idle_us}
            };
            for (size_t i = 0; i < sizeof(metrics) / sizeof(metrics[0]); i++) {
                sb_printf(&j, ",\"%s\":", metrics[i].name);
                if (metrics[i].value >= 0) sb_printf(&j, "%lld", (long long)metrics[i].value);
                else sb_puts(&j, "null");
            }
            sb_printf(&j, ",\"connect_timeout_ms\":%ld,\"total_timeout_ms\":%ld,"
                          "\"low_speed_seconds\":%ld,\"low_speed_bytes_per_second\":%ld}",
                      t->connect_timeout_ms, t->total_timeout_ms,
                      t->low_speed_seconds, t->low_speed_bytes_per_second);
        }
        sb_puts(&j, "}");
    }
    sb_puts(&j, "}");
    if (!j.oom) trace_line(c, "llm", j.buf);
    sb_free(&j);
}

/* ============================================================================
 * Session state helpers
 * ========================================================================== */

static void mark_dirty(oca_core *c) {
    pthread_mutex_lock(&c->mu);
    c->dirty = 1;
    c->conversation_dirty = 1;
    pthread_mutex_unlock(&c->mu);
}

static void set_state(oca_core *c, oca_core_state st) {
    pthread_mutex_lock(&c->mu);
    if (c->working) {
        int pause = st == OCA_S_WAITING_USER;
        if (pause && !c->working_paused)
            c->working_elapsed += core_now_ms() - c->working_start;
        if (!pause && c->working_paused) c->working_start = core_now_ms();
        c->working_paused = pause;
    }
    c->state = st;
    pthread_mutex_unlock(&c->mu);
    push_status(c);
}

static void set_goal_state(oca_core *c, int gs, int emit_event) {
    pthread_mutex_lock(&c->mu);
    if (gs == OCA_GOAL_NONE) c->goal_elapsed_ms = c->goal_started_ms = 0;
    else if (gs == OCA_GOAL_ACTIVE) {
        if (!c->goal_started_ms) c->goal_started_ms = core_now_ms();
    } else {
        c->goal_elapsed_ms = goal_elapsed(c);
        c->goal_started_ms = 0;
    }
    if (c->goal_state != gs) c->dirty = 1;
    c->goal_state = gs;
    pthread_mutex_unlock(&c->mu);
    if (emit_event) {
        oca_core_event ev = {0};
        ev.kind = OCA_CE_GOAL;
        pthread_mutex_lock(&c->mu);
        ev.goal = (oca_goal_state)c->goal_state;
        ev.goal_elapsed_ms = goal_elapsed(c);
        ev.text = c->goal_text ? xstrdup(c->goal_text) : NULL;
        pthread_mutex_unlock(&c->mu);
        ev_push(c, &ev);
    }
}

static msg_rec *append_msg(oca_core *c, oca_msg_role role) {
    if (c->n_msgs - (c->ephemeral ? c->side_prefix_count : 0) >= OCA_MESSAGES_MAX) return NULL;
    if (c->n_msgs == c->cap_msgs) {
        size_t nc = c->cap_msgs ? c->cap_msgs * 2 : 64;
        msg_rec *nm = realloc(c->msgs, nc * sizeof(*nm));
        if (!nm) return NULL;
        c->msgs = nm;
        c->cap_msgs = nc;
    }
    msg_rec *m = &c->msgs[c->n_msgs++];
    memset(m, 0, sizeof(*m));
    m->role = role;
    return m;
}

/* ============================================================================
 * Programming hints
 * ========================================================================== */

/* Instructions and state transitions adapted from ds4-agent. */
static const char HINTS_PROMPT[] =
    "The user enabled programming hints. Occasionally explain a useful concept "
    "behind the current work: a design choice, failure mechanism, trade-off, "
    "or way to verify correctness. Base each hint on code or results you have "
    "actually examined. Write one or two sentences as a normal Markdown "
    "blockquote starting with > **Hint:**. Hints are prose, not tool calls. "
    "Put them after the relevant discovery, including between tool calls. "
    "Prefer no hint to routine narration, repetition, or generic advice. "
    "Do not invent personal experience or a learner profile. Keep working "
    "without waiting for an answer. Later system notes may enable or disable "
    "hints; follow the latest setting.\n";

/* The full instructions are sent once per live context, not on each enable. */
static const char *hints_note(hints_state *hints) {
    const char *note;
    if (hints->enabled && !hints->instructed) {
        note = HINTS_PROMPT;
        hints->instructed = 1;
    } else if (hints->applied != HINTS_UNSET &&
               hints->applied != (hints->enabled ? HINTS_ON : HINTS_OFF)) {
        note = hints->enabled ?
            "The user enabled programming hints. Produce them from now on.\n" :
            "Programming hints are disabled. Do not produce teaching asides; "
            "continue the task normally.\n";
    } else return NULL;
    hints->applied = hints->enabled ? HINTS_ON : HINTS_OFF;
    return note;
}

static void hints_compacted(hints_state *hints) {
    /* The retained tail may still contain an old on/off note. */
    if (hints->applied != HINTS_UNSET) hints->applied = HINTS_UNKNOWN;
    hints->instructed = 0;
}

static int append_hints_context(oca_core *c) {
    pthread_mutex_lock(&c->mu);
    hints_state next = c->hints;
    const char *note = hints_note(&next);
    pthread_mutex_unlock(&c->mu);
    if (!note) return 0;
    char *text = xstrdup(note);
    if (!text) return -1;
    msg_rec *message = append_msg(c, OCA_MSG_SYSTEM);
    if (!message) { free(text); return -1; }
    message->text = text;
    pthread_mutex_lock(&c->mu);
    /* A newer user selection remains pending for the next boundary. */
    c->hints.instructed = next.instructed;
    c->hints.applied = next.applied;
    c->dirty = 1;
    pthread_mutex_unlock(&c->mu);
    trace_line(c, "hints", next.enabled ? "{\"enabled\":true}" : "{\"enabled\":false}");
    return 0;
}

static void set_title_if_first(oca_core *c, const char *prompt) {
    pthread_mutex_lock(&c->mu);
    if (c->title && c->title[0]) {
        pthread_mutex_unlock(&c->mu);
        return;
    }
    free(c->title);
    c->title = NULL;
    pthread_mutex_unlock(&c->mu);
    /* Single line, at most 80 bytes, cut on a UTF-8 boundary. */
    sbuf b;
    sb_init(&b);
    size_t n = strlen(prompt);
    if (n > 80) n = 80;
    while (n > 0 && ((unsigned char)prompt[n] & 0xC0) == 0x80) n--;
    if (n == 0) n = 1;
    for (size_t i = 0; i < n; i++) {
        if (prompt[i] == '\n') sb_put(&b, " ", 1);
        else sb_put(&b, prompt + i, 1);
    }
    pthread_mutex_lock(&c->mu);
    c->title = sb_take(&b);
    pthread_mutex_unlock(&c->mu);
}

/* Persists and applies one route. Returns zero on success. */
static int apply_route(oca_core *c, const char *model_id, oca_reasoning effort,
                       char *err, size_t errsz) {
    const oca_model *m = oca_llm_model_resolve(c->llm, model_id);
    if (!m) {
        err_set(err, errsz, "unknown model: %s", model_id);
        return -1;
    }
    if (!oca_llm_model_available(c->llm, m)) {
        err_set(err, errsz, "model is not available: %s", model_id);
        return -1;
    }
    if (m->reasoning_count == 0) {
        if (effort != OCA_REASONING_NONE) {
            err_set(err, errsz, "model has no configurable reasoning: %s", model_id);
            return -1;
        }
    } else if (!oca_model_supports_reasoning(m, effort)) {
        err_set(err, errsz, "model does not support the selected reasoning level");
        return -1;
    }
    const char *level = oca_model_reasoning_name(m, effort);
    if (c->route_cbs.persist) {
        if (c->route_cbs.persist(c->route_cbs.user, m->id, level, err, errsz) != 0) {
            return -1;
        }
    }
    pthread_mutex_lock(&c->mu);
    snprintf(c->route_model, sizeof(c->route_model), "%s", m->id);
    c->route_effort = effort;
    for (size_t i = 0; i < c->n_msgs; i++) {
        if (c->msgs[i].role == OCA_MSG_ASSISTANT && c->msgs[i].continuation &&
            (!c->msgs[i].model_id || strcmp(c->msgs[i].model_id, m->id) != 0)) {
            free(c->msgs[i].continuation);
            c->msgs[i].continuation = NULL;
        }
    }
    c->has_cache = 0;
    c->has_cache_input = 0;
    c->dirty = 1;
    pthread_mutex_unlock(&c->mu);
    oca_core_event ev = {0};
    ev.kind = OCA_CE_ROUTE;
    snprintf(ev.model_id, sizeof(ev.model_id), "%s", m->id);
    snprintf(ev.reasoning_name, sizeof(ev.reasoning_name), "%s", level ? level : "");
    ev.route_available = 1;
    publish_side_snapshot(c);
    ev_push(c, &ev);
    trace_line(c, "core", "{\"event\":\"route\"}");
    return 0;
}

static void apply_staged(oca_core *c) {
    char err[256];
    pthread_mutex_lock(&c->mu);
    int hsr = c->has_staged_route;
    char model[OCA_LLM_MODEL_ID_MAX + 1];
    oca_reasoning effort = c->staged_effort;
    memcpy(model, c->staged_model, sizeof(model));
    c->has_staged_route = 0;
    pthread_mutex_unlock(&c->mu);
    if (hsr) {
        if (apply_route(c, model, effort, err, sizeof(err)) != 0) {
            push_error(c, E_ROUTESET, "%s", err);
        }
    }
}

/* ============================================================================
 * System instructions, tool definitions, and request building
 * ========================================================================== */

static const char *const BASE_INSTRUCTIONS =
    "Help the user complete the requested task.\n"
    "Respect the user's scope and explicit constraints.\n"
    "Use the available tools when useful.\n"
    "Before declaring a task unsupported, check whether the available tools "
    "and installed programs can accomplish it.\n"
    "Be concise, omit filler, and match the response's detail to the task.";

static const char *const SYSTEM_IDENTITY = "You are OCA, a terminal agent.";

static const char *const GOAL_CONTINUATION =
    "Continue the active goal. Inspect progress with tools and keep working. "
    "Call goal_status with complete when the objective is achieved, "
    "or blocked when you cannot continue.";

static const char *const TOOL_READ_DESC =
    "Read one bounded text file or a range of lines. Relative paths start in the workspace.";
static const char *const TOOL_WRITE_DESC =
    "Create or atomically replace one text file. Use for new files or complete replacement. "
    "Read an existing file before replacing it.";
static const char *const TOOL_EDIT_DESC =
    "Replace exactly one matching byte sequence in one file. "
    "Read the file first and use edit for small, precise changes.";
static const char *const TOOL_BASH_DESC =
    "Run a synchronous bash -lc command from the workspace root. "
    "Use installed programs for tasks such as file operations, builds, and network requests. "
    "Commands inherit the process environment and operating-system permissions. "
    "Provide only a nonempty command field.";
static const char *const TOOL_ASK_DESC =
    "Ask one to three questions to obtain information from the user. "
    "Each question accepts only question and options, with two or three nonempty string options. "
    "The interface adds None of the above and optional free-text details.";
static const char *const TOOL_GOAL_DESC =
    "Report the active goal as complete or blocked. An optional message can explain the result.";
static const char *const SCHEMA_READ =
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
    "\"offset\":{\"type\":\"integer\"},\"limit\":{\"type\":\"integer\"}},"
    "\"required\":[\"path\"]}";
static const char *const SCHEMA_WRITE =
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
    "\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}";
static const char *const SCHEMA_EDIT =
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
    "\"old_text\":{\"type\":\"string\"},\"new_text\":{\"type\":\"string\"}},"
    "\"required\":[\"path\",\"old_text\",\"new_text\"]}";
static const char *const SCHEMA_BASH =
    "{\"type\":\"object\",\"additionalProperties\":false,"
    "\"properties\":{\"command\":{\"type\":\"string\",\"minLength\":1}},"
    "\"required\":[\"command\"]}";
static const char *const SCHEMA_ASK =
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"questions\":"
    "{\"type\":\"array\",\"minItems\":1,\"maxItems\":3,\"items\":"
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"question\":"
    "{\"type\":\"string\",\"minLength\":1},\"options\":{\"type\":\"array\","
    "\"minItems\":2,\"maxItems\":3,\"items\":{\"type\":\"string\",\"minLength\":1}}},"
    "\"required\":[\"question\",\"options\"]}}},\"required\":[\"questions\"]}";
static const char *const SCHEMA_GOAL =
    "{\"type\":\"object\",\"properties\":{\"state\":{\"type\":\"string\","
    "\"enum\":[\"complete\",\"blocked\"]},\"message\":{\"type\":\"string\"}},"
    "\"required\":[\"state\"]}";
static void render_system_text(oca_core *c, sbuf *sb) {
    if (c->ephemeral) {
        sb_puts(sb, "You are OCA in a temporary text conversation. Answer the side-chat user. "
                    "The main conversation is read-only background. Do not continue its goal. "
                    "No tools are available. Do not request or execute tools. "
                    "Later main-thread changes are outside this frozen context.");
        if (c->agents) {
            sb_puts(sb, "\n\nSaved main project context:\n");
            sb_puts(sb, c->agents);
        }
        if (c->snapshot_goal) {
            sb_puts(sb, "\n\nRead-only main goal:\n");
            sb_puts(sb, c->snapshot_goal);
        }
        return;
    }
    sb_puts(sb, SYSTEM_IDENTITY);
    sb_puts(sb, "\n\n");
    sb_puts(sb, BASE_INSTRUCTIONS);
    sb_puts(sb, "\n\n");
    if (c->agents) {
        sb_puts(sb, "Project context from AGENTS.md:\n\n");
        sb_puts(sb, c->agents);
        sb_puts(sb, "\n\n");
    }
    if (c->catalog.n > 0) {
        sb_puts(sb, "Available skills:\n");
        for (size_t i = 0; i < c->catalog.n; i++) {
            sb_printf(sb, "- %s: %s\n  SKILL.md: %s\n", c->catalog.skills[i].name,
                      c->catalog.skills[i].description, c->catalog.skills[i].path);
        }
        sb_puts(sb, "\nUse read to load relevant skill instructions from their listed paths. "
                    "Resolve relative references from the directory of the skill file containing them. "
                    "Use write and edit to create or change skill files. "
                    "The skill catalog refreshes at startup.");
    }
    if (c->goal_state == OCA_GOAL_ACTIVE && c->goal_text) {
        sb_puts(sb, "\n\nActive goal:\n");
        sb_puts(sb, c->goal_text);
    }
}

static void build_tool_defs(oca_core *c, oca_request *req) {
    if (c->ephemeral) return;
    oca_request_add_tool(req, "read", TOOL_READ_DESC, SCHEMA_READ);
    oca_request_add_tool(req, "bash", TOOL_BASH_DESC, SCHEMA_BASH);
    oca_request_add_tool(req, "write", TOOL_WRITE_DESC, SCHEMA_WRITE);
    oca_request_add_tool(req, "edit", TOOL_EDIT_DESC, SCHEMA_EDIT);
    if (c->interactive) {
        oca_request_add_tool(req, "ask_user", TOOL_ASK_DESC, SCHEMA_ASK);
        if (c->goal_state == OCA_GOAL_ACTIVE) {
            oca_request_add_tool(req, "goal_status", TOOL_GOAL_DESC, SCHEMA_GOAL);
        }
    }
}

/* Builds one complete request for the active context. pending_item is a
 * request-only user message or NULL. */
static int stream_cancel_poll(void *user);

/* Keep complete output for the UI/session while bounding the text sent back
 * to the model. Read results already fit this limit. */
static char *tool_context_text(const char *text) {
    if (!text) return xstrdup("");
    size_t n = strlen(text);
    if (n <= OCA_TOOL_RESULT_MAX_BYTES) return xstrdup(text);
    size_t head = (OCA_TOOL_RESULT_MAX_BYTES - 128) / 2, tail = n - head;
    while (head && ((unsigned char)text[head] & 0xc0) == 0x80) head--;
    while (tail < n && ((unsigned char)text[tail] & 0xc0) == 0x80) tail++;
    sbuf out;
    sb_init(&out);
    sb_put(&out, text, head);
    sb_puts(&out, "\n...[tool result shortened for model context]\n");
    sb_put(&out, text + tail, n - tail);
    if (out.oom) { sb_free(&out); return NULL; }
    return sb_take(&out);
}

static oca_request *build_agent_request(oca_core *c, const char *pending_item,
                                        oca_purpose purpose, sbuf *sys_text) {
    sb_init(sys_text);
    oca_request *req = oca_request_create(purpose, ++c->request_seq);
    if (!req) return NULL;
    const oca_model *m = oca_llm_model_resolve(c->llm, c->route_model);
    oca_request_set_route(req, m, purpose == OCA_PURPOSE_COMPACTION
                                      ? oca_model_min_reasoning(m) : c->route_effort);
    oca_request_set_cache_key(req, c->sid);
    oca_request_set_cancel(req, stream_cancel_poll, c);
    render_system_text(c, sys_text);
    oca_request_push_msg(req, OCA_MSG_SYSTEM, sys_text->buf ? sys_text->buf : "", NULL, NULL);
    if (c->comp_first_kept >= 0 && c->summary) {
        oca_request_push_msg(req, OCA_MSG_SYSTEM, c->summary, NULL, NULL);
    }
    size_t first = c->comp_first_kept > 0 ? (size_t)c->comp_first_kept : 0;
    if (c->ephemeral && first >= c->side_prefix_count)
        oca_request_push_msg(req, OCA_MSG_SYSTEM, "The following messages belong to the side chat.", NULL, NULL);
    for (size_t i = first; i < c->n_msgs; i++) {
        if (c->ephemeral && i == c->side_prefix_count && first < c->side_prefix_count)
            oca_request_push_msg(req, OCA_MSG_SYSTEM, "The main snapshot ends here. The side chat follows.", NULL, NULL);
        msg_rec *mmsg = &c->msgs[i];
        if (mmsg->role == OCA_MSG_SYSTEM) {
            oca_request_push_msg(req, OCA_MSG_SYSTEM, mmsg->text, NULL, NULL);
        } else if (mmsg->role == OCA_MSG_USER) {
            oca_request_push_msg(req, OCA_MSG_USER, mmsg->text, NULL, NULL);
        } else if (mmsg->role == OCA_MSG_ASSISTANT) {
            const char *cont = NULL;
            if (mmsg->continuation && mmsg->model_id &&
                strcmp(mmsg->model_id, c->route_model) == 0) {
                cont = mmsg->continuation;
            }
            oca_request_push_msg(req, OCA_MSG_ASSISTANT, mmsg->text, mmsg->reasoning, NULL);
            if (cont) oca_request_msg_set_state(req, cont);
            for (size_t k = 0; k < mmsg->n_calls; k++) {
                oca_request_msg_add_call(req, mmsg->calls[k].id, mmsg->calls[k].name,
                                         mmsg->calls[k].args);
            }
        } else if (mmsg->role == OCA_MSG_TOOL) {
            const char *text = mmsg->text ? mmsg->text : "";
            if (strlen(text) > OCA_TOOL_RESULT_MAX_BYTES) {
                if (!mmsg->tool_context) mmsg->tool_context = tool_context_text(text);
                text = mmsg->tool_context;
                if (!text) { oca_request_destroy(req); return NULL; }
            }
            oca_request_push_msg(req, OCA_MSG_TOOL, text, NULL, mmsg->call_id);
        }
    }
    if (pending_item) {
        oca_request_push_msg(req, OCA_MSG_USER, pending_item, NULL, NULL);
    }
    if (purpose == OCA_PURPOSE_TURN || purpose == OCA_PURPOSE_GOAL_CONT) {
        build_tool_defs(c, req);
    }
    return req;
}

/* ============================================================================
 * Compaction
 * ========================================================================== */

static long msg_token_estimate(const msg_rec *m) {
    long t = 6;
    if (m->text) {
        size_t n = strlen(m->text);
        if (m->role == OCA_MSG_TOOL && n > OCA_TOOL_RESULT_MAX_BYTES) n = OCA_TOOL_RESULT_MAX_BYTES;
        t += (long)((n + 3) / 4);
    }
    if (m->reasoning) t += 2 + (long)((strlen(m->reasoning) + 3) / 4);
    if (m->continuation) t += 2 + (long)((strlen(m->continuation) + 3) / 4);
    if (m->call_id) t += 4 + (long)((strlen(m->call_id) + 3) / 4);
    for (size_t k = 0; k < m->n_calls; k++) {
        t += 8;
        if (m->calls[k].name) t += (long)((strlen(m->calls[k].name) + 3) / 4);
        if (m->calls[k].args) t += (long)((strlen(m->calls[k].args) + 3) / 4);
    }
    return t;
}

static long compact_recent_target(long context) {
    long t = context / 5;
    return t < 20000 ? t : 20000;
}

/* ============================================================================
 * Tools
 * ========================================================================== */

typedef struct {
    char *text;
    oca_tool_state state;
} tool_outcome;

static void outcome_fail(tool_outcome *o, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    o->text = xstrdup(msg);
    o->state = OCA_TOOL_ERROR;
}

static int check_only_keys(const cJSON *args, const char *k1, const char *k2,
                           const char *k3) {
    const cJSON *field;
    cJSON_ArrayForEach(field, args) {
        const char *k = field->string;
        if (strcmp(k, k1) == 0) continue;
        if (k2 && strcmp(k, k2) == 0) continue;
        if (k3 && strcmp(k, k3) == 0) continue;
        return 0;
    }
    return 1;
}

static void tool_read(const char *workspace, const cJSON *args, tool_outcome *o) {
    const char *path = json_text(args, "path");
    const cJSON *joff = cJSON_GetObjectItemCaseSensitive(args, "offset");
    const cJSON *jlim = cJSON_GetObjectItemCaseSensitive(args, "limit");
    long offset = 1, limit = 0;
    if (joff && !json_integer(joff, &offset)) {
        outcome_fail(o, "offset must be an integer");
        return;
    }
    if (jlim && !json_integer(jlim, &limit)) {
        outcome_fail(o, "limit must be an integer");
        return;
    }
    if (offset < 1 || limit < 0 || limit > 100000) {
        outcome_fail(o, "offset must be at least 1 and limit must be from 0 through 100000");
        return;
    }
    if (!path || !*path) {
        outcome_fail(o, "path is required");
        return;
    }
    char *full = path_join_alloc(workspace, path);
    if (!full) {
        outcome_fail(o, "out of memory");
        return;
    }
    size_t flen = 0;
    char why[256];
    char *data = read_whole_file(full, OCA_EDIT_FILE_MAX_BYTES, &flen, why, sizeof(why));
    free(full);
    if (!data) {
        outcome_fail(o, "%s", why);
        return;
    }
    if (!utf8_text_valid(data, flen)) {
        free(data);
        outcome_fail(o, "the file is not valid UTF-8 text");
        return;
    }
    /* Apply the line window. */
    sbuf b;
    sb_init(&b);
    size_t line_no = 0;
    size_t i = 0;
    int truncated = 0;
    while (i < flen) {
        size_t eol = i;
        while (eol < flen && data[eol] != '\n') eol++;
        line_no++;
        if (line_no >= (size_t)offset &&
            (limit == 0 || line_no - (size_t)offset < (size_t)limit)) {
            if (b.len + (eol - i) + 1 > OCA_READ_OUTPUT_MAX_BYTES) {
                truncated = 1;
                break;
            }
            sb_put(&b, data + i, eol - i);
            sb_put(&b, "\n", 1);
        }
        if (eol >= flen) break;
        i = eol + 1;
    }
    if (truncated) {
        char marker[64];
        int marker_len = snprintf(marker, sizeof(marker), "...[truncated at %d bytes]\n",
                                  OCA_READ_OUTPUT_MAX_BYTES);
        size_t keep = OCA_READ_OUTPUT_MAX_BYTES - (size_t)marker_len;
        if (b.len > keep) {
            b.len = keep;
            while (b.len && !utf8_valid(b.buf, b.len)) b.len--;
            b.buf[b.len] = '\0';
        }
        sb_put(&b, marker, (size_t)marker_len);
    }
    free(data);
    o->text = sb_take(&b);
    o->state = OCA_TOOL_COMPLETE;
    if (!o->text) outcome_fail(o, "out of memory");
}

static int atomic_write_text(const char *path, const char *data, size_t len,
                             char *err, size_t errsz) {
    char tmp[4400];
    struct stat st;
    mode_t mode = 0644;
    int have = stat(path, &st) == 0;
    if (have) mode = st.st_mode & 0777;
    /* Create the temporary file in the target directory. */
    size_t pl = strlen(path);
    if (pl + 32 >= sizeof(tmp)) {
        err_set(err, errsz, "path too long");
        return -1;
    }
    snprintf(tmp, sizeof(tmp), "%s.ocatmp.%ld", path, (long)getpid());
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, mode ? mode : 0644);
    if (fd < 0) {
        err_set(err, errsz, "cannot create %s: %s", tmp, strerror(errno));
        return -1;
    }
    if (fchmod(fd, mode ? mode : 0644) != 0) {
        err_set(err, errsz, "cannot protect %s: %s", tmp, strerror(errno));
        goto failed;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t written = write(fd, data + off, len - off);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            if (!written) errno = EIO;
            err_set(err, errsz, "cannot write %s: %s", tmp, strerror(errno));
            goto failed;
        }
        off += (size_t)written;
    }
    if (fsync(fd) != 0) {
        err_set(err, errsz, "cannot fsync %s: %s", tmp, strerror(errno));
        goto failed;
    }
    int closed = close(fd);
    fd = -1;
    if (closed != 0) {
        err_set(err, errsz, "cannot close %s: %s", tmp, strerror(errno));
        goto failed;
    }
    if (rename(tmp, path) != 0) {
        err_set(err, errsz, "cannot replace %s: %s", path, strerror(errno));
        goto failed;
    }
    return 0;
failed:
    if (fd >= 0) close(fd);
    unlink(tmp);
    return -1;
}

static size_t edit_line_count(const char *text) {
    size_t count = 0;
    while (*text) {
        count++;
        const char *end = strchr(text, '\n');
        if (!end) break;
        text = end + 1;
    }
    return count;
}

/* ---- numbered file diffs ---- */

#define WRITE_DIFF_MAX_CELLS ((size_t)4 * 1024 * 1024)

typedef struct {
    const char *ptr;
    size_t len;
    uint64_t hash;
} write_diff_line;

static uint64_t write_diff_hash(const char *text, size_t len) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < len; i++) {
        hash ^= (unsigned char)text[i];
        hash *= UINT64_C(1099511628211);
    }
    hash ^= len;
    hash *= UINT64_C(1099511628211);
    return hash;
}

static int write_diff_line_equal(const write_diff_line *a, const write_diff_line *b) {
    return a->len == b->len && a->hash == b->hash &&
           (a->ptr[a->len] == '\n') == (b->ptr[b->len] == '\n') && !memcmp(a->ptr, b->ptr, a->len);
}

/* Split text into hashed line spans without the newline bytes; caller frees. */
static write_diff_line *write_diff_lines(const char *text, size_t *out_count) {
    write_diff_line *lines = malloc((edit_line_count(text) + 1) * sizeof(*lines));
    if (!lines) return NULL;
    size_t count = 0;
    const char *p = text;
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        lines[count].ptr = p;
        lines[count].len = len;
        lines[count].hash = write_diff_hash(p, len);
        count++;
        if (!end) break;
        p = end + 1;
    }
    *out_count = count;
    return lines;
}

/* A bounded LCS produces the edit script. Large unrelated spans use a full
 * replacement script; their contents are still emitted without truncation. */
static char *file_diff(const char *before, const char *after, const char *label) {
    size_t na = 0, nb = 0;
    write_diff_line *a = write_diff_lines(before, &na), *b = write_diff_lines(after, &nb);
    char *steps = malloc(na + nb + 1);
    uint32_t *cells = NULL;
    sbuf out;
    sb_init(&out);
    if (!a || !b || !steps) goto failed;
    size_t first = 0, ae = na, be = nb, n = 0;
    while (first < na && first < nb && write_diff_line_equal(a + first, b + first)) first++;
    while (ae > first && be > first && write_diff_line_equal(a + ae - 1, b + be - 1)) { ae--; be--; }
    size_t rows = ae - first + 1, cols = be - first + 1;
    if (rows > 1 && cols > 1 && rows <= WRITE_DIFF_MAX_CELLS / cols) {
        cells = calloc(rows * cols, sizeof(*cells));
        if (!cells) goto failed;
        for (size_t i = rows - 1; i-- > 0;) {
            for (size_t j = cols - 1; j-- > 0;) {
                uint32_t down = cells[(i + 1) * cols + j], right = cells[i * cols + j + 1];
                cells[i * cols + j] = write_diff_line_equal(a + first + i, b + first + j) ?
                    cells[(i + 1) * cols + j + 1] + 1 : down > right ? down : right;
            }
        }
    }
    for (size_t i = 0; i < first; i++) steps[n++] = ' ';
    size_t i = first, j = first, added = 0, removed = 0;
    while (i < ae || j < be) {
        if (i < ae && j < be && write_diff_line_equal(a + i, b + j)) {
            steps[n++] = ' '; i++; j++;
        } else if (i < ae && (j == be || !cells ||
                   cells[(i - first + 1) * cols + j - first] >= cells[(i - first) * cols + j - first + 1])) {
            steps[n++] = '-'; i++; removed++;
        } else { steps[n++] = '+'; j++; added++; }
    }
    while (i++ < na) steps[n++] = ' ';
    sb_printf(&out, "%s (+%zu -%zu)\n", label, added, removed);
    i = j = 0;
    size_t last_change = SIZE_MAX;
    int gap = 0, printed = 0;
    for (size_t k = 0; k < n; k++) {
        char sign = steps[k];
        int show = sign != ' ' || (last_change != SIZE_MAX && k - last_change <= 3);
        if (sign != ' ') last_change = k;
        for (size_t next = k + 1; !show && next < n && next <= k + 3; next++) show = steps[next] != ' ';
        if (show) {
            const write_diff_line *line = sign == '-' ? a + i : b + j;
            if (line->len + 64 > OCA_TOOL_OUTPUT_MAX_BYTES - out.len) goto failed;
            if (gap && printed) sb_puts(&out, "\n");
            sb_printf(&out, "%zu %c ", (sign == '-' ? i : j) + 1, sign);
            sb_put(&out, line->ptr, line->len);
            sb_puts(&out, "\n");
            if (sign != ' ' && line->ptr[line->len] != '\n') sb_puts(&out, "\\ no newline at end of file\n");
            gap = 0; printed = 1;
        } else gap = 1;
        if (sign != '+') i++;
        if (sign != '-') j++;
    }
    free(a); free(b); free(steps); free(cells);
    if (out.oom) { sb_free(&out); return NULL; }
    return sb_take(&out);
failed:
    free(a); free(b); free(steps); free(cells);
    sb_free(&out);
    return NULL;
}

static void tool_write(const char *workspace, const cJSON *args, tool_outcome *o) {
    const char *path = json_text(args, "path");
    const char *content = json_text(args, "content");
    if (!path || !*path) {
        outcome_fail(o, "path is required");
        return;
    }
    if (!content) {
        outcome_fail(o, "content is required");
        return;
    }
    char *full = path_join_alloc(workspace, path);
    if (!full) {
        outcome_fail(o, "out of memory");
        return;
    }
    size_t previous_len = 0;
    char why[256];
    char *previous = read_whole_file(full, OCA_EDIT_FILE_MAX_BYTES, &previous_len, why, sizeof(why));
    struct stat state;
    if ((!previous && (!lstat(full, &state) || errno != ENOENT)) ||
        (previous && !utf8_text_valid(previous, previous_len))) {
        outcome_fail(o, "cannot read the existing text file for its diff");
        free(previous);
        free(full);
        return;
    }
    char *feedback = file_diff(previous ? previous : "", content, "File written.");
    free(previous);
    if (!feedback) {
        outcome_fail(o, "out of memory");
        free(full);
        return;
    }
    char err[256];
    if (atomic_write_text(full, content, strlen(content), err, sizeof(err)) != 0) {
        outcome_fail(o, "%s", err);
        free(feedback);
    } else {
        o->text = feedback;
        o->state = OCA_TOOL_COMPLETE;
    }
    free(full);
}

static void tool_edit(const char *workspace, const cJSON *args, tool_outcome *o) {
    const char *path = json_text(args, "path");
    const char *old_text = json_text(args, "old_text");
    const char *new_text = json_text(args, "new_text");
    if (!path || !*path) {
        outcome_fail(o, "path is required");
        return;
    }
    if (!old_text || !*old_text) {
        outcome_fail(o, "old_text is required and must not be empty");
        return;
    }
    if (!new_text) {
        outcome_fail(o, "new_text is required");
        return;
    }
    char *data = NULL, *edited = NULL, *feedback = NULL;
    char *full = path_join_alloc(workspace, path);
    if (!full) {
        outcome_fail(o, "out of memory");
        return;
    }
    size_t flen = 0;
    char why[256];
    data = read_whole_file(full, OCA_EDIT_FILE_MAX_BYTES, &flen, why, sizeof(why));
    if (!data) {
        outcome_fail(o, "%s", why);
        goto cleanup;
    }
    if (!utf8_text_valid(data, flen)) {
        outcome_fail(o, "the file is not valid UTF-8 text");
        goto cleanup;
    }
    size_t ol = strlen(old_text);
    size_t nl2 = strlen(new_text);
    if (ol > flen) {
        outcome_fail(o, "old_text not found");
        goto cleanup;
    }
    size_t count = 0, pos = 0, match_pos = 0;
    while (pos <= flen - ol) {
        if (memcmp(data + pos, old_text, ol) == 0) {
            count++;
            match_pos = pos;
            pos += ol;
        } else {
            pos++;
        }
    }
    if (count == 0) {
        outcome_fail(o, "old_text not found");
        goto cleanup;
    }
    if (count > 1) {
        outcome_fail(o, "old_text matches %zu times", count);
        goto cleanup;
    }
    size_t suffix = flen - match_pos - ol;
    if (nl2 > SIZE_MAX - (flen - ol) || flen - ol + nl2 == SIZE_MAX) {
        outcome_fail(o, "edited file is too large");
        goto cleanup;
    }
    size_t edited_len = flen - ol + nl2;
    edited = malloc(edited_len + 1);
    if (!edited) {
        outcome_fail(o, "out of memory");
        goto cleanup;
    }
    if (match_pos) memcpy(edited, data, match_pos);
    if (nl2) memcpy(edited + match_pos, new_text, nl2);
    if (suffix) memcpy(edited + match_pos + nl2, data + match_pos + ol, suffix);
    edited[edited_len] = '\0';
    feedback = file_diff(data, edited, "Edit applied.");
    free(data);
    data = NULL;
    if (!feedback) {
        outcome_fail(o, "out of memory");
        goto cleanup;
    }
    char err[256];
    int rc = atomic_write_text(full, edited, edited_len, err, sizeof(err));
    if (rc != 0) {
        outcome_fail(o, "%s", err);
    } else {
        o->text = feedback;
        feedback = NULL;
        o->state = OCA_TOOL_COMPLETE;
    }
cleanup:
    free(feedback);
    free(edited);
    free(data);
    free(full);
}

/* ============================================================================
 * Internal tool executor
 * ============================================================================
 * The core owns one executor. Every tool call runs in a fresh copy of this
 * executable so a crashing, blocking, or cancelled tool cannot affect agent
 * state. Workers inherit the environment and ordinary filesystem and network
 * access. Each worker is bounded by the shared timeout and output limits.
 * ========================================================================== */

enum { EXEC_RUN = 2, EXEC_CANCEL = 3, EXEC_OUTPUT = 4, EXEC_RESULT = 5 };
enum { JOB_RUNNING, JOB_CANCELLED, JOB_TIMED_OUT, JOB_FAILED, JOB_OUTPUT_LIMIT };
#define EXEC_PACKET_MAX OCA_TOOL_OUTPUT_MAX_BYTES
#define EXEC_IO_MS 10000
#define EXEC_BOOT_MAX (2u * 1024u * 1024u)
#define EXEC_PATH_MAX 4096

typedef struct {
    uint32_t kind;
    uint32_t size;
    uint32_t sequence;
    uint32_t state;
} executor_packet;

struct oca_tool_executor {
    pid_t owner;
    uint32_t sequence;
    int borrowed;
    char *home;             /* Owned absolute home directory. */
    char *workspace;        /* Owned absolute worker working directory. */
    char *executable;       /* Owned path used to start fresh workers. */
    oca_core *progress;     /* Borrowed only for the current core tool call. */
    int ready;
};

static void executor_driver_call(oca_tool_executor *executor, const char *name,
    const char *json, const atomic_int *cancel, tool_outcome *out);

static long long executor_now_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int executor_socket_pair(int pair[2]) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) return -1;
    for (int i = 0; i < 2; i++) {
        if (fcntl(pair[i], F_SETFD, FD_CLOEXEC) || fcntl(pair[i], F_SETFL, O_NONBLOCK)) goto failed;
#ifdef SO_NOSIGPIPE
        int yes = 1;
        if (setsockopt(pair[i], SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes))) goto failed;
#endif
    }
    return 0;
failed:
    close(pair[0]);
    close(pair[1]);
    return -1;
}

static int executor_transfer(int fd, void *buffer, size_t length, int sending) {
    long long started = executor_now_ms();
    if (started < 0) return -1;
    size_t offset = 0;
    while (offset < length) {
        ssize_t n;
        if (sending) {
            int flags = 0;
#ifdef MSG_NOSIGNAL
            flags = MSG_NOSIGNAL;
#endif
            n = send(fd, (char *)buffer + offset, length - offset, flags);
        } else n = recv(fd, (char *)buffer + offset, length - offset, 0);
        if (n > 0) {
            offset += (size_t)n;
            continue;
        }
        if (!n) return -1;
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
        long long now = executor_now_ms();
        if (now < 0 || now - started >= EXEC_IO_MS) return -1;
        struct pollfd wait = {fd, sending ? POLLOUT : POLLIN, 0};
        if (poll(&wait, 1, 50) < 0 && errno != EINTR) return -1;
    }
    return 0;
}

static int executor_send(int fd, uint32_t kind, uint32_t sequence, uint32_t state,
                          const char *data, size_t size) {
    if (size > EXEC_PACKET_MAX || (size && !data)) return -1;
    executor_packet packet = {kind, (uint32_t)size, sequence, state};
    if (executor_transfer(fd, &packet, sizeof(packet), 1)) return -1;
    return size ? executor_transfer(fd, (void *)data, size, 1) : 0;
}

/* The receiver owns the returned payload, including its extra NUL byte. */
static int executor_receive(int fd, executor_packet *packet, char **data) {
    *data = NULL;
    if (executor_transfer(fd, packet, sizeof(*packet), 0) || packet->size > EXEC_PACKET_MAX) return -1;
    char *payload = malloc((size_t)packet->size + 1);
    if (!payload) return -1;
    if (packet->size && executor_transfer(fd, payload, packet->size, 0)) {
        free(payload);
        return -1;
    }
    payload[packet->size] = 0;
    *data = payload;
    return 0;
}

/* The helper starts before threads. No inherited descriptor can reach a tool. */
static int executor_detach(int channel) {
    if (channel != 3 && dup2(channel, 3) < 0) return -1;
    if (fcntl(3, F_SETFD, FD_CLOEXEC)) return -1;
#ifdef __linux__
#ifdef SYS_close_range
    int closed = (int)syscall(SYS_close_range, 4u, ~0u, 0u);
#else
    int closed = -1;
#endif
    if (closed) {
        DIR *directory = opendir("/proc/self/fd");
        if (!directory) return -1;
        int listing = dirfd(directory);
        struct dirent *entry;
        while ((entry = readdir(directory))) {
            char *end;
            long fd = strtol(entry->d_name, &end, 10);
            if (*entry->d_name && !*end && fd > 3 && fd <= INT_MAX && fd != listing) close((int)fd);
        }
        closedir(directory);
    }
#else
    return -1;
#endif
    int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null_fd < 0) return -1;
    if (null_fd < 3) {
        int copy = fcntl(null_fd, F_DUPFD_CLOEXEC, 4);
        close(null_fd);
        if (copy < 0) return -1;
        null_fd = copy;
    }
    for (int fd = 0; fd < 3; fd++) {
        if (dup2(null_fd, fd) < 0) {
            close(null_fd);
            return -1;
        }
    }
    close(null_fd);
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    const int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGCHLD};
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++)
        if (sigaction(signals[i], &action, NULL)) return -1;
    sigset_t empty;
    sigemptyset(&empty);
    if (sigprocmask(SIG_SETMASK, &empty, NULL)) return -1;
    return 0;
}

static int executor_high_fd(int fd) {
    if (fd < 0) return -1;
    int copy = fcntl(fd, F_DUPFD_CLOEXEC, 10);
    close(fd);
    return copy;
}

static void executor_native(const char *workspace, const char *name,
                             const cJSON *args, tool_outcome *out) {
    if (!strcmp(name, "read")) tool_read(workspace, args, out);
    else if (!strcmp(name, "write")) tool_write(workspace, args, out);
    else if (!strcmp(name, "edit")) tool_edit(workspace, args, out);
    else outcome_fail(out, "unknown filesystem tool");
}

static void executor_bash_result(sbuf *output, int status, int truncated, tool_outcome *out) {
    if (output->buf) utf8_sanitize_in_place(output->buf, output->len);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (truncated) sb_puts(output, "\nTool output exceeded the capture limit; the command was terminated.");
    sb_printf(output, "%s[exit %d]", output->len ? "\n" : "", code);
    out->text = sb_take(output);
    out->state = !truncated && code == 0 ? OCA_TOOL_COMPLETE : OCA_TOOL_ERROR;
    if (!out->text) outcome_fail(out, "out of memory");
}

/* Hold only an incomplete UTF-8 tail between chunks. Sanitize binary bytes
 * once, so streamed text and the final captured result agree. */
static int executor_stream(int channel, uint32_t sequence, sbuf *output, size_t *sent, int final) {
    size_t end = output->len;
    if (!final && end > *sent) {
        size_t start = end - 1;
        while (start > *sent && ((unsigned char)output->buf[start] & 0xc0) == 0x80) start--;
        unsigned char c = (unsigned char)output->buf[start];
        size_t need = c >= 0xf0 && c <= 0xf4 ? 4 : c >= 0xe0 && c <= 0xef ? 3 :
                      c >= 0xc2 && c <= 0xdf ? 2 : 1;
        if (end - start < need) end = start;
    }
    if (end == *sent) return 0;
    utf8_sanitize_in_place(output->buf + *sent, end - *sent);
    int result = executor_send(channel, EXEC_OUTPUT, sequence, 0, output->buf + *sent, end - *sent);
    *sent = end;
    return result;
}

static void executor_collect(sbuf *buffer, int fd, size_t cap, int *truncated) {
    char data[4096];
    ssize_t n = read(fd, data, sizeof(data));
    if (n <= 0) return;
    size_t amount = (size_t)n;
    size_t room = cap - buffer->len;
    if (amount > room) {
        amount = room;
        *truncated = 1;
    }
    sb_put(buffer, data, amount);
}

static void executor_job(int channel, uint32_t sequence, const char *workspace,
                         const char *name, const cJSON *args, tool_outcome *out) {
    int pair[2];
    if (executor_socket_pair(pair)) {
        outcome_fail(out, "cannot create the tool channel");
        return;
    }
    int bash = !strcmp(name, "bash");
    const char *command = bash ? json_text(args, "command") : NULL;
    if (bash && (!command || !*command)) {
        close(pair[0]);
        close(pair[1]);
        outcome_fail(out, "command is required");
        return;
    }
    pid_t child = fork();
    if (child < 0) {
        close(pair[0]);
        close(pair[1]);
        outcome_fail(out, "cannot start the tool process");
        return;
    }
    if (!child) {
        close(channel);
        close(pair[0]);
        if (setpgid(0, 0)) _exit(127);
        if (bash) {
            int flags = fcntl(pair[1], F_GETFL);
            if (flags < 0 || fcntl(pair[1], F_SETFL, flags & ~O_NONBLOCK)) _exit(127);
            if (dup2(pair[1], STDOUT_FILENO) < 0 || dup2(pair[1], STDERR_FILENO) < 0) _exit(127);
            close(pair[1]);
            if (chdir(workspace)) _exit(127);
            execl("/bin/bash", "bash", "-lc", command, (char *)NULL);
            _exit(127);
        }
        tool_outcome result = {0};
        executor_native(workspace, name, args, &result);
        const char *text = result.text ? result.text : "tool process failed";
        int rc = executor_send(pair[1], EXEC_RESULT, sequence, result.state, text, strlen(text));
        free(result.text);
        close(pair[1]);
        _exit(rc ? 1 : 0);
    }
    setpgid(child, child);
    close(pair[1]);
    sbuf output;
    sb_init(&output);
    int truncated = 0, status = 0, stopped = JOB_RUNNING, exited = 0, lost = 0;
    size_t streamed = 0;
    long long started = executor_now_ms(), stop_time = 0;
    for (;;) {
        struct pollfd waits[] = {{channel, POLLIN, 0}, {pair[0], POLLIN, 0}};
        int ready = poll(waits, 2, 25);
        if (ready < 0 && errno != EINTR) {
            stopped = JOB_FAILED;
        }
        if (waits[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
            executor_packet control;
            char *payload = NULL;
            if (executor_receive(channel, &control, &payload) || control.size ||
                (control.kind != EXEC_CANCEL || control.sequence != sequence)) {
                stopped = JOB_FAILED;
            } else {
                if (!stopped) stopped = JOB_CANCELLED;
            }
            free(payload);
        }
        executor_collect(&output, pair[0], bash ? OCA_TOOL_OUTPUT_MAX_BYTES - 1024 :
            sizeof(executor_packet) + OCA_TOOL_OUTPUT_MAX_BYTES, &truncated);
        if (bash && executor_stream(channel, sequence, &output, &streamed, 0)) stopped = JOB_FAILED;
        if (truncated && !stopped) stopped = JOB_OUTPUT_LIMIT;
        siginfo_t information;
        memset(&information, 0, sizeof(information));
        int result = waitid(P_PID, (id_t)child, &information, WEXITED | WNOHANG | WNOWAIT);
        if (!result && information.si_pid == child) {
            exited = 1;
            break;
        }
        if (result < 0 && errno != EINTR) {
            lost = errno == ECHILD;
            stopped = JOB_FAILED;
            break;
        }
        long long now = executor_now_ms();
        if (started < 0 || now < 0) {
            stopped = JOB_FAILED;
        }
        if (!stopped && now - started >= OCA_BASH_TIMEOUT_SECONDS * 1000LL) stopped = JOB_TIMED_OUT;
        if (stopped && !stop_time) {
            kill(-child, SIGTERM);
            stop_time = now > 0 ? now : 1;
        }
        if (stopped && (now < 0 || now - stop_time >= OCA_BASH_GRACE_SECONDS * 1000LL)) {
            kill(-child, SIGKILL);
            kill(child, SIGKILL);
        }
    }
    /* Keep the child unreaped until group cleanup prevents identifier reuse. */
    if (!lost) {
        kill(-child, SIGKILL);
        if (!exited) kill(child, SIGKILL);
        pid_t result;
        do {
            result = waitpid(child, &status, 0);
        } while (result < 0 && errno == EINTR);
        if (result != child) {
            stopped = JOB_FAILED;
        }
    }
    for (;;) {
        size_t before = output.len;
        executor_collect(&output, pair[0], bash ? OCA_TOOL_OUTPUT_MAX_BYTES - 1024 :
            sizeof(executor_packet) + OCA_TOOL_OUTPUT_MAX_BYTES, &truncated);
        if (bash && executor_stream(channel, sequence, &output, &streamed, 0)) stopped = JOB_FAILED;
        if (truncated && !stopped) stopped = JOB_OUTPUT_LIMIT;
        if (output.len == before) break;
    }
    close(pair[0]);
    if (bash) {
        if (executor_stream(channel, sequence, &output, &streamed, 1)) stopped = JOB_FAILED;
        if (stopped == JOB_CANCELLED) sb_puts(&output, "\nTool execution was canceled.");
        else if (stopped == JOB_TIMED_OUT) sb_puts(&output, "\nTool exceeded the runtime limit and was terminated.");
        else if (stopped == JOB_FAILED) sb_puts(&output, "\nTool executor failed.");
        executor_bash_result(&output, status, truncated, out);
        if (stopped) out->state = stopped == JOB_CANCELLED ? OCA_TOOL_INTERRUPTED : OCA_TOOL_ERROR;
    } else if (stopped == JOB_CANCELLED) {
        out->state = OCA_TOOL_INTERRUPTED;
        out->text = xstrdup("Tool execution was canceled.");
    } else if (stopped == JOB_TIMED_OUT) outcome_fail(out, "Tool exceeded the runtime limit and was terminated.");
    else if (stopped || output.oom) outcome_fail(out, "tool executor failed");
    else {
        executor_packet packet;
        int valid = output.len >= sizeof(packet) && !truncated && WIFEXITED(status) && !WEXITSTATUS(status);
        if (valid) {
            memcpy(&packet, output.buf, sizeof(packet));
            valid = packet.kind == EXEC_RESULT && packet.sequence == sequence && packet.state <= OCA_TOOL_ERROR &&
                packet.size <= OCA_TOOL_OUTPUT_MAX_BYTES && output.len - sizeof(packet) == packet.size &&
                utf8_text_valid(output.buf + sizeof(packet), packet.size);
        }
        if (valid) {
            out->state = (int)packet.state;
            out->text = xstrndup(output.buf + sizeof(packet), packet.size);
            if (!out->text) outcome_fail(out, "out of memory");
        } else outcome_fail(out, "the tool process returned an invalid result");
    }
    sb_free(&output);
}

void oca_tool_executor_destroy(oca_tool_executor *executor) {
    if (!executor) return;
    if (executor->owner != getpid()) {
        free(executor);
        return;
    }
    free(executor->home);
    free(executor->workspace);
    free(executor->executable);
    free(executor);
}

static void executor_call(oca_tool_executor *executor, const char *workspace,
    const char *name, const char *json, const atomic_int *cancel, tool_outcome *out) {
    if (!executor || !executor->workspace || strcmp(workspace, executor->workspace)) {
        outcome_fail(out, "the tool executor is unavailable for this workspace");
        return;
    }
    executor_driver_call(executor, name, json, cancel, out);
}

/* Build the worker request. The caller frees the returned text. */
static char *executor_bootstrap(const oca_tool_executor *executor, const char *name, const char *json) {
    cJSON *root = cJSON_CreateObject();
    int valid = root && cJSON_AddStringToObject(root, "workspace", executor->workspace) &&
        cJSON_AddStringToObject(root, "name", name) &&
        cJSON_AddStringToObject(root, "args", json);
    char *text = valid ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (text && strlen(text) > EXEC_BOOT_MAX) {
        cJSON_free(text);
        return NULL;
    }
    return text;
}

/* Validate one worker request. Returned strings borrow the document. */
static cJSON *executor_bootstrap_decode(const char *text, size_t length,
    const char **workspace, const char **name, const char **arguments, char *err, size_t errsz) {
    *workspace = *name = *arguments = NULL;
    if (!utf8_text_valid(text, length)) goto invalid;
    int quoted = 0;
    for (size_t i = 0; i < length; i++) {
        if (quoted && text[i] == '\\') {
            i++;
            if (i < length && length - i >= 5 && !memcmp(text + i, "u0000", 5)) goto invalid;
        } else if (text[i] == '"') quoted = !quoted;
    }
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(text, length + 1, &end, 1);
    if (!cJSON_IsObject(root) || cJSON_GetArraySize(root) != 3 || end != text + length) {
        cJSON_Delete(root);
        goto invalid;
    }
    const cJSON *directory = cJSON_GetObjectItemCaseSensitive(root, "workspace");
    const cJSON *operation = cJSON_GetObjectItemCaseSensitive(root, "name");
    const cJSON *values = cJSON_GetObjectItemCaseSensitive(root, "args");
    if (!cJSON_IsString(directory) || directory->valuestring[0] != '/' ||
        strlen(directory->valuestring) >= EXEC_PATH_MAX ||
        !cJSON_IsString(operation) || !*operation->valuestring ||
        strlen(operation->valuestring) > OCA_TOOL_NAME_MAX_BYTES ||
        !cJSON_IsString(values) || strlen(values->valuestring) > OCA_TOOL_RESULT_MAX_BYTES) {
        cJSON_Delete(root);
        goto invalid;
    }
    *workspace = directory->valuestring;
    *name = operation->valuestring;
    *arguments = values->valuestring;
    return root;
invalid:
    err_set(err, errsz, "tool request rejected");
    return NULL;
}

/* Internal executable entry. Consumes one request on descriptor 3 and exits. */
int oca_tool_worker_main(void) {
    if (executor_detach(3)) return 125;
    executor_packet packet;
    char *payload = NULL;
    if (executor_receive(3, &packet, &payload)) return 125;
    if (packet.kind != EXEC_RUN || !packet.sequence || packet.state) {
        free(payload);
        const char *message = "tool request rejected";
        executor_send(3, EXEC_RESULT, packet.sequence, OCA_TOOL_ERROR, message, strlen(message));
        return 125;
    }
    char err[256] = "tool request rejected";
    const char *workspace = NULL, *name = NULL, *arguments = NULL;
    cJSON *document = executor_bootstrap_decode(payload, packet.size,
        &workspace, &name, &arguments, err, sizeof(err));
    free(payload);
    if (!document) {
        executor_send(3, EXEC_RESULT, packet.sequence, OCA_TOOL_ERROR, err, strlen(err));
        return 125;
    }
    const char *why = NULL;
    cJSON *args = json_parse(arguments, strlen(arguments), &why);
    tool_outcome out = {0};
    if (!strcmp(name, "probe") && args && cJSON_IsObject(args)) {
        out.text = xstrdup("Executor ready.");
        out.state = OCA_TOOL_COMPLETE;
    } else if ((!strcmp(name, "read") || !strcmp(name, "write") || !strcmp(name, "edit") ||
        !strcmp(name, "bash")) && args && cJSON_IsObject(args)) {
        executor_job(3, packet.sequence, workspace, name, args, &out);
    } else outcome_fail(&out, "tool request rejected");
    cJSON_Delete(args);
    cJSON_Delete(document);
    const char *result = out.text ? out.text : "tool worker failed";
    int rc = executor_send(3, EXEC_RESULT, packet.sequence, out.state, result, strlen(result));
    free(out.text);
    close(3);
    return rc ? 125 : 0;
}

static void executor_diagnostic(sbuf *text, int fd) {
    char buffer[512];
    ssize_t n = read(fd, buffer, sizeof(buffer));
    if (n <= 0) return;
    size_t keep = (size_t)n;
    if (keep > 2048 - text->len) keep = 2048 - text->len;
    for (size_t i = 0; i < keep; i++)
        if ((unsigned char)buffer[i] < 32 && buffer[i] != '\n' && buffer[i] != '\t') buffer[i] = '?';
    sb_put(text, buffer, keep);
}

static void executor_driver_call(oca_tool_executor *executor, const char *name,
    const char *json, const atomic_int *cancel, tool_outcome *out) {
    if (!executor || executor->owner != getpid() || !executor->workspace ||
        !executor->executable || executor->sequence == UINT32_MAX) {
        outcome_fail(out, "the tool executor is unavailable");
        return;
    }
    if (cancel && atomic_load(cancel)) {
        out->state = OCA_TOOL_INTERRUPTED;
        out->text = xstrdup("Tool execution was canceled.");
        return;
    }
    char *bootstrap = executor_bootstrap(executor, name, json);
    int channel[2] = {-1, -1}, diagnostics[2] = {-1, -1}, null_fd = -1;
    if (!bootstrap) {
        outcome_fail(out, "cannot prepare tool startup");
        return;
    }
    if (executor_socket_pair(channel) || pipe(diagnostics)) goto failed;
    for (int i = 0; i < 2; i++) {
        channel[i] = executor_high_fd(channel[i]);
        diagnostics[i] = executor_high_fd(diagnostics[i]);
        if (channel[i] < 0 || diagnostics[i] < 0) goto failed;
    }
    null_fd = executor_high_fd(open("/dev/null", O_RDWR | O_CLOEXEC));
    if (null_fd < 0 || fcntl(diagnostics[0], F_SETFL, O_NONBLOCK)) goto failed;
    pid_t child = fork();
    if (child < 0) goto failed;
    if (!child) {
        /* Mark inherited descriptors close-on-exec when the kernel supports it.
         * The worker closes every unrelated descriptor before any tool runs. */
#if defined(__linux__) && defined(SYS_close_range)
        (void)syscall(SYS_close_range, 3u, ~0u, 4u);
#endif
        if (dup2(channel[1], 3) < 0 || dup2(null_fd, 0) < 0 ||
            dup2(null_fd, 1) < 0 || dup2(diagnostics[1], 2) < 0) _exit(125);
        fcntl(3, F_SETFD, 0);
        char *launch[] = {"oca", (char *)OCA_TOOL_WORKER_FLAG, NULL};
        execv(executor->executable, launch);
        _exit(125);
    }
    close(channel[1]);
    channel[1] = -1;
    close(diagnostics[1]);
    diagnostics[1] = -1;
    close(null_fd);
    null_fd = -1;
    uint32_t sequence = ++executor->sequence;
    int sent = executor_send(channel[0], EXEC_RUN, sequence, 0, bootstrap, strlen(bootstrap));
    cJSON_free(bootstrap);
    bootstrap = NULL;
    int canceled = 0, completed = 0, exited = 0, status = 0, failure = sent != 0;
    long long started = executor_now_ms(), stop_started = 0;
    sbuf diagnostic, captured;
    sb_init(&diagnostic);
    sb_init(&captured);
    while (!exited || (!completed && !failure)) {
        struct pollfd waits[] = {{channel[0], POLLIN, 0}, {diagnostics[0], POLLIN, 0}};
        if (poll(waits, 2, 25) < 0 && errno != EINTR) failure = 1;
        executor_diagnostic(&diagnostic, diagnostics[0]);
        if (!completed && !failure && waits[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
            executor_packet packet;
            char *payload = NULL;
            if (executor_receive(channel[0], &packet, &payload) || packet.sequence != sequence ||
                (packet.kind != EXEC_RESULT && packet.kind != EXEC_OUTPUT) ||
                packet.state > OCA_TOOL_ERROR || !utf8_text_valid(payload, packet.size) ||
                (packet.kind == EXEC_OUTPUT && (strcmp(name, "bash") || packet.state))) {
                free(payload);
                failure = 1;
            } else if (packet.kind == EXEC_OUTPUT) {
                if (packet.size > OCA_TOOL_OUTPUT_MAX_BYTES - 1024 - captured.len) failure = 1;
                else sb_put(&captured, payload, packet.size);
                if (captured.oom) failure = 1;
                if (executor->progress) {
                    oca_core_event event = {.kind = OCA_CE_TOOL_OUTPUT, .text = payload};
                    ev_push(executor->progress, &event);
                } else free(payload);
            } else {
                out->text = payload;
                out->state = (int)packet.state;
                completed = 1;
            }
        }
        long long now = executor_now_ms();
        if (!completed && !canceled && cancel && atomic_load(cancel)) {
            canceled = 1;
            stop_started = now;
            if (executor_send(channel[0], EXEC_CANCEL, sequence, 0, NULL, 0)) failure = 1;
        }
        long long limit = !strcmp(name, "probe") ? EXEC_IO_MS :
            (OCA_BASH_TIMEOUT_SECONDS + OCA_BASH_GRACE_SECONDS) * 1000LL + EXEC_IO_MS;
        if (started < 0 || now < 0 || now - started > limit) failure = 1;
        if (failure || (canceled && now - stop_started > (OCA_BASH_GRACE_SECONDS + 1) * 1000LL)) kill(child, SIGKILL);
        pid_t reaped = exited ? 0 : waitpid(child, &status, WNOHANG);
        if (reaped == child) exited = 1;
        else if (reaped < 0 && errno != EINTR) {
            failure = 1;
            exited = 1;
        }
    }
    for (int i = 0; i < 8; i++) executor_diagnostic(&diagnostic, diagnostics[0]);
    close(channel[0]);
    close(diagnostics[0]);
    if (failure || !completed || !WIFEXITED(status) || WEXITSTATUS(status)) {
        char *previous = out->text;
        out->text = NULL;
        if (diagnostic.buf) utf8_sanitize_in_place(diagnostic.buf, diagnostic.len);
        const char *detail = previous ? previous : "";
        if (diagnostic.len) detail = diagnostic.buf;
        outcome_fail(out, "tool startup or execution failed%s%s",
            *detail ? ": " : "", detail);
        free(previous);
        if (captured.len && !captured.oom) {
            sb_puts(&captured, "\n");
            sb_puts(&captured, out->text ? out->text : "tool executor failed");
            free(out->text);
            out->text = sb_take(&captured);
        }
    }
    if (canceled) {
        if (!out->text) out->text = xstrdup("Tool execution was canceled.");
        out->state = OCA_TOOL_INTERRUPTED;
    }
    sb_free(&diagnostic);
    sb_free(&captured);
    return;
failed:
    if (null_fd >= 0) close(null_fd);
    for (int i = 0; i < 2; i++) {
        if (channel[i] >= 0) close(channel[i]);
        if (diagnostics[i] >= 0) close(diagnostics[i]);
    }
    cJSON_free(bootstrap);
    outcome_fail(out, "cannot prepare the tool process: %s", strerror(errno));
}

/* Locate the executable used to start fresh workers. The caller owns it. */
static char *executor_path_alloc(void) {
#ifdef __linux__
    return xstrdup("/proc/self/exe");
#else
    return NULL;
#endif
}

oca_tool_executor *oca_tool_executor_create(const char *home, const char *workspace,
                                            char *err, size_t errsz) {
    if (!home || !*home || !workspace || !*workspace) {
        err_set(err, errsz, "home and workspace are required");
        return NULL;
    }
    oca_tool_executor *executor = calloc(1, sizeof(*executor));
    if (!executor) {
        err_set(err, errsz, "out of memory");
        return NULL;
    }
    executor->owner = getpid();
    executor->home = realpath(home, NULL);
    executor->workspace = realpath(workspace, NULL);
    executor->executable = executor_path_alloc();
    struct stat state;
    if (!executor->home || !executor->workspace || !executor->executable ||
        stat(executor->home, &state) || !S_ISDIR(state.st_mode) ||
        stat(executor->workspace, &state) || !S_ISDIR(state.st_mode)) {
        err_set(err, errsz, "home and workspace must be existing directories");
        oca_tool_executor_destroy(executor);
        return NULL;
    }
    tool_outcome probe = {0};
    executor_driver_call(executor, "probe", "{}", NULL, &probe);
    if (probe.state != OCA_TOOL_COMPLETE || !probe.text || strcmp(probe.text, "Executor ready.")) {
        err_set(err, errsz, "%s", probe.text ? probe.text : "tool executor activation failed");
        free(probe.text);
        oca_tool_executor_destroy(executor);
        return NULL;
    }
    free(probe.text);
    executor->ready = 1;
    return executor;
}

/* Update the worker working directory after session selection. */
static int executor_rebind_workspace(oca_tool_executor *executor, const char *workspace,
                                     char *err, size_t errsz) {
    char *resolved = realpath(workspace, NULL);
    struct stat state;
    if (!resolved || stat(resolved, &state) || !S_ISDIR(state.st_mode)) {
        free(resolved);
        err_set(err, errsz, "the saved workspace is unavailable");
        return -1;
    }
    if (!strcmp(executor->workspace, resolved)) {
        free(resolved);
        return 0;
    }
    free(executor->workspace);
    executor->workspace = resolved;
    return 0;
}

/* ============================================================================
 * Stream event handling
 * ========================================================================== */

typedef struct {
    oca_core *c;
    sbuf text;
    sbuf reasoning;
    char *state_block;
    char *finish;
    char *err_category;
    char *err_message;
    call_rec calls[OCA_TOOL_CALLS_MAX];
    size_t n_calls;
    int call_open;
    sbuf open_args;
    int seen_start;
    int seen_terminal;
    int seen_usage;
    int seen_state;
    int invalid;
    int quiet;             /* Compaction collects without emitting events. */
    int has_cache;
    long cache_hit;
    int has_input;
    long input;
} stream_ctx;

static int stream_cancel_poll(void *user) {
    oca_core *c = user;
    return atomic_load(&c->cancel_latch);
}

static int call_id_used(const oca_core *c, const char *id) {
    for (size_t i = 0; i < c->n_msgs; i++) {
        if (c->msgs[i].role != OCA_MSG_ASSISTANT) continue;
        for (size_t k = 0; k < c->msgs[i].n_calls; k++) {
            if (c->msgs[i].calls[k].id && !strcmp(c->msgs[i].calls[k].id, id))
                return 1;
        }
    }
    return 0;
}

static int stream_text_fits(stream_ctx *sc, size_t added) {
    size_t used = sc->text.len + sc->reasoning.len;
    if (used > OCA_LLM_RESPONSE_MAX_BYTES || added > OCA_LLM_RESPONSE_MAX_BYTES - used) return 0;
    if (sc->c->ephemeral && used + added > OCA_TOOL_RESULT_MAX_BYTES) {
        size_t extra = used >= OCA_TOOL_RESULT_MAX_BYTES ? added : used + added - OCA_TOOL_RESULT_MAX_BYTES;
        pthread_mutex_lock(&sc->c->mu);
        int fits = extra <= OCA_EPHEMERAL_HISTORY_MAX_BYTES - sc->c->side_history_reserved;
        if (fits) sc->c->side_history_reserved += extra;
        pthread_mutex_unlock(&sc->c->mu);
        if (!fits) return 0;
    }
    return 1;
}

static int stream_cb(const oca_event *ev, void *user) {
    stream_ctx *sc = user;
    if (sc->invalid) return 1;
    const oca_model *model = oca_llm_model_resolve(sc->c->llm, sc->c->route_model);
    if (ev->kind != OCA_EV_RESPONSE_START && ev->kind != OCA_EV_ERROR &&
        !sc->seen_start) {
        goto invalid;
    }
    if (sc->seen_terminal) {
        goto invalid;
    }
    switch (ev->kind) {
    case OCA_EV_RESPONSE_START:
        if (sc->seen_start || sc->seen_terminal ||
            ev->request_id != sc->c->request_seq || !ev->model_id ||
            strcmp(ev->model_id, sc->c->route_model) != 0) {
            goto invalid;
        }
        sc->seen_start = 1;
        if (!sc->quiet) set_state(sc->c, OCA_S_STREAMING);
        break;
    case OCA_EV_REASONING_DELTA:
        if (!ev->text || (!model || !model->reasoning_stream)) {
            goto invalid;
        }
        if (!stream_text_fits(sc, strlen(ev->text))) {
            goto invalid;
        }
        sb_puts(&sc->reasoning, ev->text);
        if (!sc->quiet) {
            oca_core_event e2 = {0};
            e2.kind = OCA_CE_ASSISTANT_REASON;
            e2.text = xstrdup(ev->text);
            ev_push(sc->c, &e2);
        }
        break;
    case OCA_EV_TEXT_DELTA:
        if (!ev->text) {
            goto invalid;
        }
        if (!stream_text_fits(sc, strlen(ev->text))) {
            goto invalid;
        }
        sb_puts(&sc->text, ev->text);
        if (!sc->quiet) {
            oca_core_event e2 = {0};
            e2.kind = OCA_CE_ASSISTANT_TEXT;
            e2.text = xstrdup(ev->text);
            ev_push(sc->c, &e2);
        }
        break;
    case OCA_EV_TOOL_CALL_START:
        if (sc->call_open || !ev->call_id || !*ev->call_id ||
            !ev->call_name || !*ev->call_name ||
            strlen(ev->call_id) > OCA_TOOL_CALL_ID_MAX_BYTES ||
            strlen(ev->call_name) > OCA_TOOL_NAME_MAX_BYTES ||
            call_id_used(sc->c, ev->call_id)) {
            goto invalid;
        }
        if (sc->n_calls >= OCA_TOOL_CALLS_MAX) {
            goto invalid;
        }
        for (size_t i = 0; i < sc->n_calls; i++) {
            if (strcmp(sc->calls[i].id, ev->call_id) == 0) {
                goto invalid;
            }
        }
        sc->call_open = 1;
        sc->calls[sc->n_calls].id = xstrdup(ev->call_id);
        sc->calls[sc->n_calls].name = xstrdup(ev->call_name);
        if (!sc->calls[sc->n_calls].id || !sc->calls[sc->n_calls].name) {
            goto invalid;
        }
        sb_init(&sc->open_args);
        break;
    case OCA_EV_TOOL_CALL_ARGUMENTS:
        if (!sc->call_open || !ev->call_id ||
            strcmp(sc->calls[sc->n_calls].id, ev->call_id) != 0 || !ev->arguments) {
            goto invalid;
        }
        if (sc->open_args.len + strlen(ev->arguments) > OCA_TOOL_RESULT_MAX_BYTES) {
            goto invalid;
        }
        sb_puts(&sc->open_args, ev->arguments);
        break;
    case OCA_EV_TOOL_CALL_END:
        if (!sc->call_open || !ev->call_id ||
            strcmp(sc->calls[sc->n_calls].id, ev->call_id) != 0 || !ev->arguments ||
            strlen(ev->arguments) != sc->open_args.len ||
            memcmp(ev->arguments, sc->open_args.buf ? sc->open_args.buf : "",
                   sc->open_args.len) != 0) {
            goto invalid;
        }
        sc->calls[sc->n_calls].args = sb_take(&sc->open_args);
        if (!sc->calls[sc->n_calls].args) {
            goto invalid;
        }
        sc->n_calls++;
        sc->call_open = 0;
        break;
    case OCA_EV_MODEL_STATE:
        if (sc->seen_state || !ev->state || strlen(ev->state) > OCA_MODEL_STATE_MAX_BYTES) {
            goto invalid;
        }
        sc->seen_state = 1;
        free(sc->state_block);
        sc->state_block = xstrdup(ev->state);
        break;
    case OCA_EV_USAGE:
        if (sc->seen_usage ||
            (!model || (!model->usage && !model->cache_metrics)) ||
            (ev->has_input && ev->input_tokens < 0) ||
            (ev->has_output && ev->output_tokens < 0) ||
            (ev->has_reasoning_tokens && ev->reasoning_tokens < 0) ||
            (ev->has_total && ev->total_tokens < 0) ||
            (ev->has_cache_hit && ev->cache_hit_tokens < 0) ||
            (ev->has_cache_miss && ev->cache_miss_tokens < 0)) {
            goto invalid;
        }
        sc->seen_usage = 1;
        sc->has_cache = ev->has_cache_hit;
        sc->cache_hit = ev->cache_hit_tokens;
        sc->has_input = ev->has_input;
        sc->input = ev->input_tokens;
        if (!sc->has_input && ev->has_cache_hit && ev->has_cache_miss &&
            ev->cache_hit_tokens <= LONG_MAX - ev->cache_miss_tokens) {
            sc->has_input = 1;
            sc->input = ev->cache_hit_tokens + ev->cache_miss_tokens;
        }
        break;
    case OCA_EV_RESPONSE_DONE:
        if (!ev->finish || (strcmp(ev->finish, "stop") != 0 &&
                            strcmp(ev->finish, "tool_calls") != 0) ||
            ev->request_id != sc->c->request_seq || !ev->model_id ||
            strcmp(ev->model_id, sc->c->route_model) != 0) {
            goto invalid;
        }
        if ((strcmp(ev->finish, "tool_calls") == 0 && sc->n_calls == 0) ||
            (strcmp(ev->finish, "stop") == 0 && sc->n_calls != 0)) {
            goto invalid;
        }
        if (sc->call_open) {
            /* The adapter must combine arguments before tool_call_end. */
            goto invalid;
        }
        sc->finish = xstrdup(ev->finish);
        sc->seen_terminal = 1;
        break;
    case OCA_EV_ERROR:
        if (!ev->category || !*ev->category || !ev->message || !*ev->message) {
            goto invalid;
        }
        trace_llm_error(sc->c, ev);
        sc->err_category = xstrdup(ev->category);
        sc->err_message = xstrdup(ev->message);
        sc->seen_terminal = 1;
        break;
    }
    return 0;
invalid:
    sc->invalid = 1;
    return 1;
}

static void publish_usage(stream_ctx *sc) {
    if (sc->quiet) return;
    pthread_mutex_lock(&sc->c->mu);
    sc->c->has_cache = sc->has_cache;
    sc->c->cache_hit = sc->cache_hit;
    sc->c->has_cache_input = sc->has_input;
    sc->c->cache_input = sc->has_input ? sc->input : 0;
    pthread_mutex_unlock(&sc->c->mu);
}

static void stream_free(stream_ctx *sc) {
    sb_free(&sc->text);
    sb_free(&sc->reasoning);
    sb_free(&sc->open_args);
    free(sc->state_block);
    free(sc->finish);
    free(sc->err_category);
    free(sc->err_message);
    for (size_t i = 0; i < OCA_TOOL_CALLS_MAX; i++) {
        free(sc->calls[i].id);
        free(sc->calls[i].name);
        free(sc->calls[i].args);
    }
    memset(sc, 0, sizeof(*sc));
}

/* ============================================================================
 * Tool call validation and execution
 * ========================================================================== */

/* Malformed argument JSON returns -2: it cannot enter normalized history.
 * Other contract rejections return -1 and may permit model correction. */
static int validate_call(oca_core *c, const char *name, const char *args_json,
                         char *why, size_t whysz) {
    if (c->ephemeral) {
        snprintf(why, whysz, "tools are unavailable in the ephemeral chat");
        return -1;
    }
    const char *jw = NULL;
    cJSON *args = json_parse(args_json, strlen(args_json), &jw);
    if (!args || !cJSON_IsObject(args)) {
        cJSON_Delete(args);
        snprintf(why, whysz, "arguments for %s must be one JSON object (%s)", name,
                 jw ? jw : "syntax");
        return -2;
    }
    int rc = -1;
    const char *path = json_text(args, "path");
    long offset = 0, limit = 0;
    if (strcmp(name, "read") == 0) {
        if (!check_only_keys(args, "path", "offset", "limit")) {
            snprintf(why, whysz, "read accepts only path, offset, and limit");
            goto out;
        }
        if (!path || !*path) {
            snprintf(why, whysz, "read requires path");
            goto out;
        }
        const cJSON *jo = cJSON_GetObjectItemCaseSensitive(args, "offset");
        const cJSON *jl = cJSON_GetObjectItemCaseSensitive(args, "limit");
        if (jo && !json_integer(jo, &offset)) {
            snprintf(why, whysz, "read offset must be an integer");
            goto out;
        }
        if (jl && !json_integer(jl, &limit)) {
            snprintf(why, whysz, "read limit must be an integer");
            goto out;
        }
        if ((jo && offset < 1) || limit < 0 || limit > 100000) {
            snprintf(why, whysz, "read offset or limit is out of range");
            goto out;
        }
        rc = 0;
    } else if (strcmp(name, "write") == 0) {
        if (!check_only_keys(args, "path", "content", NULL)) {
            snprintf(why, whysz, "write accepts only path and content");
            goto out;
        }
        if (!path || !*path) {
            snprintf(why, whysz, "write requires path");
            goto out;
        }
        if (!json_text(args, "content")) {
            snprintf(why, whysz, "write requires content");
            goto out;
        }
        rc = 0;
    } else if (strcmp(name, "edit") == 0) {
        if (!check_only_keys(args, "path", "old_text", "new_text")) {
            snprintf(why, whysz, "edit accepts only path, old_text, and new_text");
            goto out;
        }
        if (!path || !*path) {
            snprintf(why, whysz, "edit requires path");
            goto out;
        }
        const char *old_text = json_text(args, "old_text");
        if (!old_text || !*old_text) {
            snprintf(why, whysz, "edit requires non-empty old_text");
            goto out;
        }
        if (!json_text(args, "new_text")) {
            snprintf(why, whysz, "edit requires new_text");
            goto out;
        }
        rc = 0;
    } else if (strcmp(name, "bash") == 0) {
        if (!check_only_keys(args, "command", NULL, NULL)) {
            snprintf(why, whysz, "bash accepts only command");
            goto out;
        }
        const char *command = json_text(args, "command");
        if (!command || !*command) {
            snprintf(why, whysz, "bash requires command");
            goto out;
        }
        rc = 0;
    } else if (strcmp(name, "ask_user") == 0) {
        if (!c->interactive) {
            snprintf(why, whysz, "ask_user is not available in this run");
            goto out;
        }
        if (!check_only_keys(args, "questions", NULL, NULL)) {
            snprintf(why, whysz, "ask_user accepts only questions");
            goto out;
        }
        const cJSON *qs = cJSON_GetObjectItemCaseSensitive(args, "questions");
        int count = cJSON_GetArraySize(qs);
        if (!cJSON_IsArray(qs) || count < 1 || count > OCA_INTERACTION_MAX_QUESTIONS) {
            snprintf(why, whysz, "ask_user requires one to three questions");
            goto out;
        }
        const cJSON *q;
        cJSON_ArrayForEach(q, qs) {
            if (!cJSON_IsObject(q)) {
                snprintf(why, whysz, "each question must be an object");
                goto out;
            }
            if (cJSON_GetArraySize(q) != 2) {
                snprintf(why, whysz, "each question accepts only question and options");
                goto out;
            }
            const char *qt = json_text(q, "question");
            if (!qt || !*qt) {
                snprintf(why, whysz, "each question requires non-empty text");
                goto out;
            }
            const cJSON *opts = cJSON_GetObjectItemCaseSensitive(q, "options");
            int option_count = cJSON_GetArraySize(opts);
            if (!cJSON_IsArray(opts) || option_count < 2 || option_count > 3) {
                snprintf(why, whysz, "each question requires two or three options");
                goto out;
            }
            const cJSON *option;
            cJSON_ArrayForEach(option, opts) {
                const char *ot = cJSON_GetStringValue(option);
                if (!ot || !*ot) {
                    snprintf(why, whysz, "each option must be non-empty text");
                    goto out;
                }
            }
        }
        rc = 0;
    } else if (strcmp(name, "goal_status") == 0) {
        if (c->goal_state != OCA_GOAL_ACTIVE) {
            snprintf(why, whysz, "goal_status requires an active goal");
            goto out;
        }
        if (!check_only_keys(args, "state", "message", NULL)) {
            snprintf(why, whysz, "goal_status accepts only state and message");
            goto out;
        }
        const char *st = json_text(args, "state");
        if (!st || (strcmp(st, "complete") != 0 && strcmp(st, "blocked") != 0)) {
            snprintf(why, whysz, "goal_status state must be complete or blocked");
            goto out;
        }
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(args, "message");
        if (msg && (!cJSON_GetStringValue(msg) || !*cJSON_GetStringValue(msg))) {
            snprintf(why, whysz, "goal_status message must be non-empty when present");
            goto out;
        }
        rc = 0;
    } else {
        snprintf(why, whysz, "unknown tool: %s", name);
    }
out:
    cJSON_Delete(args);
    return rc;
}

/* Runs ask_user: emits the interaction and waits for all answers. */
static void tool_ask_user(oca_core *c, const cJSON *args, tool_outcome *o) {
    const cJSON *qs = cJSON_GetObjectItemCaseSensitive(args, "questions");
    size_t question_count = (size_t)cJSON_GetArraySize(qs);
    oca_core_event ev = {.kind = OCA_CE_INTERACTION, .question_count = question_count};
    ev.questions = calloc(question_count, sizeof(*ev.questions));
    if (!ev.questions) goto allocation_failed;
    for (size_t i = 0; i < question_count; i++) {
        const cJSON *q = cJSON_GetArrayItem(qs, (int)i);
        const cJSON *options = cJSON_GetObjectItemCaseSensitive(q, "options");
        oca_interaction_question *question = &ev.questions[i];
        question->question = xstrdup(json_text(q, "question"));
        question->option_count = (size_t)cJSON_GetArraySize(options);
        question->options = calloc(question->option_count, sizeof(*question->options));
        if (!question->question || !question->options) goto allocation_failed;
        for (size_t k = 0; k < question->option_count; k++) {
            question->options[k] = xstrdup(cJSON_GetStringValue(cJSON_GetArrayItem(options, (int)k)));
            if (!question->options[k]) goto allocation_failed;
        }
    }
    /* Clear the previous slot before publishing an answerable interaction. */
    pthread_mutex_lock(&c->mu);
    free(c->ia_answers_json);
    c->ia_answers_json = NULL;
    c->ia_waiting = 1;
    pthread_mutex_unlock(&c->mu);
    set_state(c, OCA_S_WAITING_USER);
    ev_push(c, &ev);
    pthread_mutex_lock(&c->mu);
    while (!c->ia_answers_json && !atomic_load(&c->cancel_latch) && c->running) {
        pthread_cond_wait(&c->cond, &c->mu);
    }
    char *answers = c->ia_answers_json;
    c->ia_answers_json = NULL;
    c->ia_waiting = 0;
    int was_cancel = atomic_load(&c->cancel_latch);
    pthread_mutex_unlock(&c->mu);
    if (!answers || was_cancel) {
        o->text = xstrdup("Interaction cancelled by the user.");
        o->state = OCA_TOOL_INTERRUPTED;
        free(answers);
        return;
    }
    const char *jw = NULL;
    cJSON *doc = json_parse(answers, strlen(answers), &jw);
    free(answers);
    if (!cJSON_IsArray(doc) || (size_t)cJSON_GetArraySize(doc) != question_count) {
        cJSON_Delete(doc);
        outcome_fail(o, "answers did not match the questions");
        return;
    }
    sbuf b;
    sb_init(&b);
    for (size_t i = 0; i < question_count; i++) {
        const cJSON *a = cJSON_GetArrayItem(doc, (int)i);
        const char *q = json_text(a, "question");
        const char *ans = json_text(a, "answer");
        const char *expected = json_text(cJSON_GetArrayItem(qs, (int)i), "question");
        if (!cJSON_IsObject(a) || cJSON_GetArraySize(a) != 2 || !q || !ans || !*ans ||
            !expected || strcmp(q, expected) != 0 ||
            !check_only_keys(a, "question", "answer", NULL)) {
            cJSON_Delete(doc);
            sb_free(&b);
            outcome_fail(o, "answers did not match the questions");
            return;
        }
        sb_printf(&b, "Q: %s\nA: %s\n", q, ans);
    }
    cJSON_Delete(doc);
    o->text = sb_take(&b);
    o->state = OCA_TOOL_COMPLETE;
    if (!o->text) outcome_fail(o, "out of memory");
    return;
allocation_failed:
    oca_core_event_release(&ev);
    outcome_fail(o, "out of memory");
}

static void tool_goal_status(oca_core *c, const cJSON *args, tool_outcome *o) {
    const char *st = json_text(args, "state");
    const char *msg = json_text(args, "message");
    int ns = strcmp(st, "complete") == 0 ? OCA_GOAL_COMPLETED : OCA_GOAL_BLOCKED;
    sbuf j;
    sb_init(&j);
    sb_puts(&j, "{\"event\":\"state\",\"state\":");
    sb_json_str(&j, st);
    if (msg) {
        sb_puts(&j, ",\"message\":");
        sb_json_str(&j, msg);
    }
    sb_puts(&j, "}");
    if (!j.oom) trace_line(c, "goal", j.buf);
    sb_free(&j);
    set_goal_state(c, ns, 1);
    o->text = xstrdup("Goal state recorded.");
    o->state = OCA_TOOL_COMPLETE;
}

static void run_tool_call(oca_core *c, const char *name, const char *args_json,
                          tool_outcome *o) {
    memset(o, 0, sizeof(*o));
    const char *jw = NULL;
    cJSON *args = json_parse(args_json, strlen(args_json), &jw);
    if (!args) {
        outcome_fail(o, "arguments are not valid JSON");
        return;
    }
    if (c->executor && (!strcmp(name, "read") || !strcmp(name, "write") ||
        !strcmp(name, "edit") || !strcmp(name, "bash"))) {
        c->executor->progress = c;
        executor_call(c->executor, c->workspace, name, args_json, &c->cancel_latch, o);
        c->executor->progress = NULL;
    } else if (strcmp(name, "ask_user") == 0) {
        tool_ask_user(c, args, o);
    } else if (strcmp(name, "goal_status") == 0) {
        tool_goal_status(c, args, o);
    } else {
        outcome_fail(o, "unknown tool: %s", name);
    }
    cJSON_Delete(args);
}

/* ============================================================================
 * Compaction execution
 * ========================================================================== */

static int run_compaction(oca_core *c, long est_before, char *err, size_t errsz) {
    const oca_model *m = oca_llm_model_resolve(c->llm, c->route_model);
    if (!m) {
        err_set(err, errsz, "route is unavailable");
        return -1;
    }
    long context = m->input_limit;
    long target = compact_recent_target(context);
    /* Find the first kept index at a user boundary. */
    long last_user = -1;
    for (long i = (long)c->n_msgs - 1; i >= 0; i--) {
        if (c->msgs[i].role == OCA_MSG_USER) {
            last_user = i;
            break;
        }
    }
    if (last_user < 0) {
        err_set(err, errsz, "no user message boundary exists");
        return -1;
    }
    long acc = 0;
    for (long i = last_user; i < (long)c->n_msgs; i++) acc += msg_token_estimate(&c->msgs[i]);
    long first_kept = last_user;
    if (acc < target) {
        for (long i = last_user - 1; i >= 0; i--) {
            acc += msg_token_estimate(&c->msgs[i]);
            if (c->msgs[i].role == OCA_MSG_USER && acc >= target) {
                first_kept = i;
                break;
            }
        }
        if (first_kept < 0) first_kept = 0;
    }
    if (first_kept <= 0) {
        /* Nothing can leave the verbatim segment. */
        err_set(err, errsz, "the conversation does not fit the context limit");
        return -1;
    }
    /* Build the summary request. */
    sbuf body;
    sb_init(&body);
    sb_puts(&body, "Summarize this conversation for a continuation. Keep every "
                   "identifier, decision, and open problem.\n\n");
    if (c->summary) {
        sb_puts(&body, "Previous summary:\n");
        sb_puts(&body, c->summary);
        sb_puts(&body, "\n\n");
    }
    /* The previous summary already covers messages before this boundary. */
    long summary_start = c->comp_first_kept > 0 ? c->comp_first_kept : 0;
    sb_printf(&body, "Messages leaving the verbatim segment (%ld through %ld):\n\n",
              summary_start, first_kept - 1);
    for (long i = summary_start; i < first_kept; i++) {
        const msg_rec *mm = &c->msgs[i];
        if (mm->role == OCA_MSG_USER) sb_printf(&body, "user: %s\n", mm->text);
        else if (mm->role == OCA_MSG_ASSISTANT) sb_printf(&body, "assistant: %s\n", mm->text ? mm->text : "");
        else if (mm->role == OCA_MSG_TOOL) {
            char *text = tool_context_text(mm->text);
            if (!text) { sb_free(&body); err_set(err, errsz, "out of memory"); return -1; }
            sb_printf(&body, "tool result: %s\n", text);
            free(text);
        }
    }
    oca_request *req = oca_request_create(OCA_PURPOSE_COMPACTION, ++c->request_seq);
    if (!req) {
        sb_free(&body);
        err_set(err, errsz, "out of memory");
        return -1;
    }
    oca_request_set_route(req, m, oca_model_min_reasoning(m));
    oca_request_set_cache_key(req, c->sid);
    oca_request_set_cancel(req, stream_cancel_poll, c);
    oca_request_push_msg(req, OCA_MSG_USER, body.buf, NULL, NULL);
    stream_ctx sc = {.c = c, .quiet = 1};
    set_state(c, OCA_S_COMPACTING);
    trace_llm(c, "request", OCA_PURPOSE_COMPACTION, m->id, -1, -1);
    char gerr[256];
    int grc = oca_llm_generate(c->llm, req, stream_cb, &sc, gerr, sizeof(gerr));
    oca_request_destroy(req);
    sb_free(&body);
    int ok = 0;
    if (grc != 0) {
        err_set(err, errsz, "%s", gerr);
    } else if (sc.invalid) {
        err_set(err, errsz, "the summary stream was invalid");
    } else if (sc.err_category) {
        err_set(err, errsz, "summary request failed: %s", sc.err_message);
    } else if (!sc.finish || strcmp(sc.finish, "stop") != 0 || sc.text.len == 0) {
        err_set(err, errsz, "the summary response was not usable");
    } else {
        ok = 1;
    }
    if (!ok) {
        stream_free(&sc);
        return -1;
    }
    char *summary = sb_take(&sc.text);
    stream_free(&sc);
    if (!summary) {
        err_set(err, errsz, "out of memory");
        return -1;
    }
    /* Rebuild the probe with the summary and measure again. A failure keeps
     * the old checkpoint unchanged. */
    char *old_summary = c->summary;
    long old_first_kept = c->comp_first_kept;
    c->summary = summary;
    c->comp_first_kept = first_kept;
    sbuf sys;
    oca_request *probe = build_agent_request(c, NULL, OCA_PURPOSE_TURN, &sys);
    pthread_mutex_lock(&c->mu);
    hints_state next_hints = c->hints;
    pthread_mutex_unlock(&c->mu);
    hints_compacted(&next_hints);
    const char *restore_hints = hints_note(&next_hints);
    if (probe && restore_hints) oca_request_push_msg(probe, OCA_MSG_SYSTEM, restore_hints, NULL, NULL);
    long after = 0;
    int fit = 0;
    if (probe && oca_llm_estimate_input(probe, &after, err, errsz) == 0) {
        fit = after <= context;
    }
    if (probe) oca_request_destroy(probe);
    sb_free(&sys);
    if (!fit) {
        free(summary);
        c->summary = old_summary;
        c->comp_first_kept = old_first_kept;
        err_set(err, errsz, "the compacted context does not fit");
        return -1;
    }
    free(old_summary);
    c->tokens_before = est_before;
    c->tokens_after = after;
    pthread_mutex_lock(&c->mu);
    c->has_cache = 0;
    c->has_cache_input = 0;
    c->cache_input = 0;
    c->dirty = 1;
    hints_compacted(&c->hints);
    pthread_mutex_unlock(&c->mu);
    sbuf j;
    sb_init(&j);
    sb_printf(&j, "{\"first_kept\":%ld,\"before\":%ld,\"after\":%ld}", first_kept,
              est_before, after);
    if (!j.oom) trace_line(c, "compaction", j.buf);
    sb_free(&j);
    return 0;
}

/* ============================================================================
 * Turn engine
 * ========================================================================== */

static void append_assistant_msg(oca_core *c, stream_ctx *sc, int state, int with_calls) {
    msg_rec *m = append_msg(c, OCA_MSG_ASSISTANT);
    if (!m) return;
    m->state = state;
    m->text = xstrdup(sc->text.buf ? sc->text.buf : "");
    m->reasoning = sc->reasoning.len ? xstrdup(sc->reasoning.buf) : NULL;
    m->model_id = xstrdup(c->route_model);
    m->effort = xstrdup(route_reasoning_name(c));
    m->continuation = sc->state_block ? xstrdup(sc->state_block) : NULL;
    if (with_calls && sc->n_calls > 0) {
        m->n_calls = sc->n_calls;
        m->calls = calloc(sc->n_calls, sizeof(*m->calls));
        if (m->calls) {
            for (size_t k = 0; k < sc->n_calls; k++) {
                m->calls[k].id = xstrdup(sc->calls[k].id);
                m->calls[k].name = xstrdup(sc->calls[k].name);
                m->calls[k].args = xstrdup(sc->calls[k].args);
            }
        } else {
            m->n_calls = 0;
        }
    }
    mark_dirty(c);
}

static void append_tool_msg(oca_core *c, const char *call_id, const char *text, int state) {
    msg_rec *m = append_msg(c, OCA_MSG_TOOL);
    if (!m) return;
    m->call_id = xstrdup(call_id);
    m->text = xstrdup(text);
    m->state = state;
    mark_dirty(c);
}

static void fail_turn(oca_core *c, const char *code, const char *fmt, ...) {
    pthread_mutex_lock(&c->mu);
    c->has_cache = 0;
    c->has_cache_input = 0;
    pthread_mutex_unlock(&c->mu);
    va_list ap;
    va_start(ap, fmt);
    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    push_error(c, code, "%s", msg);
    sbuf j;
    sb_init(&j);
    sb_puts(&j, "{\"code\":");
    sb_json_str(&j, code);
    sb_puts(&j, ",\"message\":");
    sb_json_str(&j, msg);
    sb_puts(&j, "}");
    if (!j.oom) trace_line(c, "core", j.buf);
    sb_free(&j);
    set_state(c, OCA_S_ERROR);
    pthread_mutex_lock(&c->mu);
    int goal_active = c->goal_state == OCA_GOAL_ACTIVE;
    pthread_mutex_unlock(&c->mu);
    if (goal_active) {
        set_goal_state(c, OCA_GOAL_BLOCKED, 1);
        sbuf g;
        sb_init(&g);
        sb_puts(&g, "{\"event\":\"state\",\"state\":\"blocked\",\"reason\":");
        sb_json_str(&g, code);
        sb_puts(&g, "}");
        if (!g.oom) trace_line(c, "goal", g.buf);
        sb_free(&g);
    }
    set_state(c, OCA_S_IDLE);
}


/* Combine queued messages only between agent turns. Keep the queue
 * intact if allocation fails, and preserve its submission order. */
static int take_followups(oca_core *c, char **out) {
    *out = NULL;
    pthread_mutex_lock(&c->mu);
    size_t count = c->n_followups;
    if (count == 1) {
        *out = c->followups[0];
    } else if (count) {
        sbuf text;
        sb_init(&text);
        for (size_t i = 0; i < count; i++) {
            if (i) sb_puts(&text, "\n\n");
            sb_printf(&text, "Queued user message %zu:\n%s", i + 1, c->followups[i]);
        }
        if (text.oom) {
            sb_free(&text);
            pthread_mutex_unlock(&c->mu);
            return -1;
        }
        *out = sb_take(&text);
    }
    char **items = c->followups;
    c->followups = NULL;
    c->n_followups = 0;
    pthread_mutex_unlock(&c->mu);
    for (size_t i = 0; i < count; i++) {
        if (c->interactive) {
            oca_core_event event = {0};
            event.kind = OCA_CE_USER_TEXT;
            event.text = count == 1 ? xstrdup(items[i]) : items[i];
            ev_push(c, &event);
        } else if (count > 1) {
            free(items[i]);
        }
    }
    free(items);
    return 0;
}

static int append_user_prompt(oca_core *c, const char *prompt) {
    if (append_hints_context(c)) return -1;
    char *text = xstrdup(prompt);
    msg_rec *m = text ? append_msg(c, OCA_MSG_USER) : NULL;
    if (!m) { free(text); return -1; }
    m->text = text;
    set_title_if_first(c, prompt);
    mark_dirty(c);
    return 0;
}

/* Runs one agent turn to completion. Returns zero on success, one when the
 * user cancelled, and a negative value on failure. */
static int run_agent_turn(oca_core *c, const char *prompt, oca_purpose purpose,
                          int announce, char **out_final) {
    int validation_retried = 0;
    *out_final = NULL;
    working_start(c);
    if (purpose == OCA_PURPOSE_TURN && append_user_prompt(c, prompt)) {
        fail_turn(c, E_ADAPTER, "cannot allocate user prompt");
        return -1;
    }
    if (announce && c->interactive) {
        oca_core_event event = {0};
        event.kind = OCA_CE_USER_TEXT;
        event.text = xstrdup(prompt);
        ev_push(c, &event);
    }
    if (atomic_load(&c->cancel_latch)) goto cancelled;
    if (!route_available(c)) {
        fail_turn(c, E_ROUTE, "the selected model is not available");
        return -1;
    }
    for (;;) {
        if (atomic_load(&c->cancel_latch)) goto cancelled;
        if (append_hints_context(c) != 0) {
            fail_turn(c, E_ADAPTER, "cannot allocate hint instructions");
            return -1;
        }
        const char *pending = purpose == OCA_PURPOSE_GOAL_CONT ? GOAL_CONTINUATION : NULL;
        sbuf sys;
        oca_request *req = build_agent_request(c, pending, purpose, &sys);
        if (!req) {
            sb_free(&sys);
            fail_turn(c, E_ADAPTER, "out of memory");
            return -1;
        }
        const oca_model *m = oca_llm_model_resolve(c->llm, c->route_model);
        long est = 0;
        char err[256];
        if (oca_llm_estimate_input(req, &est, err, sizeof(err)) != 0) {
            oca_request_destroy(req);
            sb_free(&sys);
            fail_turn(c, E_ADAPTER, "%s", err);
            return -1;
        }
        if (est > m->input_limit) {
            set_state(c, OCA_S_COMPACTING);
            if (run_compaction(c, est, err, sizeof(err)) != 0) {
                oca_request_destroy(req);
                sb_free(&sys);
                fail_turn(c, E_COMPACT, "%s", err);
                return -1;
            }
            oca_request_destroy(req);
            sb_free(&sys);
            continue; /* Rebuild with the new checkpoint. */
        }
        publish_side_snapshot(c);
        set_state(c, OCA_S_REQUESTING);
        trace_llm(c, "request", purpose, m->id, est, -1);
        stream_ctx sc = {.c = c};
        int grc = oca_llm_generate(c->llm, req, stream_cb, &sc, err, sizeof(err));
        oca_request_destroy(req);
        sb_free(&sys);
        if (grc != 0) {
            stream_free(&sc);
            fail_turn(c, E_ADAPTER, "%s", err);
            return -1;
        }
        if (sc.invalid) {
            stream_free(&sc);
            fail_turn(c, E_STREAM, "the response stream was invalid");
            return -1;
        }
        if (sc.err_category) {
            int cancelled = strcmp(sc.err_category, "cancelled") == 0;
            if (cancelled) {
                /* Keep valid partial text with state interrupted. */
                if (sc.text.len || sc.reasoning.len) {
                    append_assistant_msg(c, &sc, MS_INTERRUPTED, 0);
                }
                stream_free(&sc);
                goto cancelled;
            }
            char msg[256];
            snprintf(msg, sizeof(msg), "%s", sc.err_message ? sc.err_message : "error");
            stream_free(&sc);
            fail_turn(c, E_ADAPTER, "request failed: %s", msg);
            return -1;
        }
        if (!sc.finish) {
            stream_free(&sc);
            fail_turn(c, E_STREAM, "the stream ended without a terminal event");
            return -1;
        }
        publish_usage(&sc);
        if (strcmp(sc.finish, "stop") == 0) {
            append_assistant_msg(c, &sc, MS_COMPLETE, 0);
            *out_final = xstrdup(sc.text.buf ? sc.text.buf : "");
            long out_tokens = (long)((sc.text.len + 3) / 4);
            trace_llm(c, "response", purpose, m->id, est, est + out_tokens);
            stream_free(&sc);
            set_state(c, OCA_S_IDLE);
            return 0;
        }
        /* Tool calls: validate every call before any effect. */
        int valid = 1, repairable = !c->ephemeral;
        char why[256], first_error[256] = "";
        for (size_t k = 0; k < sc.n_calls; k++) {
            int validation = validate_call(c, sc.calls[k].name, sc.calls[k].args, why, sizeof(why));
            if (validation != 0) {
                if (valid) snprintf(first_error, sizeof(first_error), "%s", why);
                valid = 0;
                if (validation == -2 ||
                    (strcmp(sc.calls[k].name, "bash") && strcmp(sc.calls[k].name, "ask_user")) ||
                    (!c->interactive && !strcmp(sc.calls[k].name, "ask_user")))
                    repairable = 0;
            }
        }
        if (!valid && (!repairable || validation_retried)) {
            stream_free(&sc);
            fail_turn(c, E_TOOLVAL, "%s", first_error);
            return -1;
        }
        /* Allow one correction for these argument errors per turn. Every
         * call in the rejected group gets an error result without execution. */
        if (!valid) validation_retried = 1;
        append_assistant_msg(c, &sc, MS_COMPLETE, 1);
        long out_tokens = (long)((sc.text.len + 3) / 4);
        trace_llm(c, "response", purpose, m->id, est, est + out_tokens);
        size_t n_calls = sc.n_calls;
        int interrupted = 0, goal_ended = 0;
        for (size_t k = 0; k < n_calls; k++) {
            if (atomic_load(&c->cancel_latch)) {
                /* Every remaining call gets one interrupted result. */
                for (size_t r = k; r < n_calls; r++) {
                    append_tool_msg(c, sc.calls[r].id, "Cancelled before execution.",
                                    OCA_TOOL_INTERRUPTED);
                    oca_core_event ev = {0};
                    ev.kind = OCA_CE_TOOL_START;
                    ev.name = xstrdup(sc.calls[r].name);
                    ev.detail = xstrdup(sc.calls[r].id);
                    ev.text = xstrdup(sc.calls[r].args);
                    ev_push(c, &ev);
                    ev = (oca_core_event){0};
                    ev.kind = OCA_CE_TOOL_RESULT;
                    ev.name = xstrdup(sc.calls[r].name);
                    ev.detail = xstrdup(sc.calls[r].id);
                    ev.text = xstrdup("Cancelled before execution.");
                    ev.tool_state = OCA_TOOL_INTERRUPTED;
                    ev_push(c, &ev);
                }
                interrupted = 1;
                break;
            }
            publish_side_snapshot(c);
            set_state(c, OCA_S_RUNNING_TOOL);
            {
                oca_core_event ev = {0};
                ev.kind = OCA_CE_TOOL_START;
                ev.name = xstrdup(sc.calls[k].name);
                ev.detail = xstrdup(sc.calls[k].id);
                ev.text = xstrdup(sc.calls[k].args);
                ev_push(c, &ev);
            }
            trace_tool(c, "start", sc.calls[k].name, sc.calls[k].id, sc.calls[k].args,
                       -1, NULL, -1);
            tool_outcome out = {0};
            if (valid) {
                run_tool_call(c, sc.calls[k].name, sc.calls[k].args, &out);
            } else if (validate_call(c, sc.calls[k].name, sc.calls[k].args, why, sizeof(why))) {
                outcome_fail(&out, "Invalid tool arguments: %s. No calls in this group ran. "
                             "Correct the arguments using the tool schema and try again.", why);
            } else {
                outcome_fail(&out, "Not executed: another call in this group was rejected. "
                             "Resubmit this call if it is still needed.");
            }
            if (!out.text) out.text = xstrdup("");
            if (!strcmp(sc.calls[k].name, "goal_status") &&
                (c->goal_state == OCA_GOAL_COMPLETED || c->goal_state == OCA_GOAL_BLOCKED)) goal_ended = 1;
            append_tool_msg(c, sc.calls[k].id, out.text, out.state);
            {
                oca_core_event ev = {0};
                ev.kind = OCA_CE_TOOL_RESULT;
                ev.name = xstrdup(sc.calls[k].name);
                ev.detail = xstrdup(sc.calls[k].id);
                ev.text = xstrdup(out.text);
                ev.tool_state = out.state;
                ev_push(c, &ev);
            }
            trace_tool(c, "end", sc.calls[k].name, sc.calls[k].id, NULL,
                       (long)strlen(out.text),
                       out.state == OCA_TOOL_COMPLETE ? "complete" :
                       out.state == OCA_TOOL_INTERRUPTED ? "interrupted" : "error", -1);
            free(out.text);
        }
        stream_free(&sc);
        if (interrupted || atomic_load(&c->cancel_latch)) goto cancelled;
        if (goal_ended) { set_state(c, OCA_S_IDLE); return 0; }
        /* Finish this turn's tool continuations before consuming queued input. */
    }

cancelled:
    pthread_mutex_lock(&c->mu);
    c->has_cache = 0;
    c->has_cache_input = 0;
    pthread_mutex_unlock(&c->mu);
    set_state(c, OCA_S_CANCELLING);
    atomic_store(&c->cancel_latch, 0);
    pthread_mutex_lock(&c->mu);
    int goal_active = c->goal_state == OCA_GOAL_ACTIVE;
    pthread_mutex_unlock(&c->mu);
    if (goal_active) {
        set_goal_state(c, OCA_GOAL_PAUSED, 1);
        trace_line(c, "goal", "{\"event\":\"state\",\"state\":\"paused\"}");
    }
    set_state(c, OCA_S_IDLE);
    return 1;
}

/* ============================================================================
 * Worker
 * ========================================================================== */

static void push_turn_done(oca_core *c, const char *final_text) {
    publish_side_snapshot(c);
    atomic_store(&c->cancel_latch, 0);
    pthread_mutex_lock(&c->mu);
    c->working = 0;
    c->working_paused = 0;
    c->working_elapsed = 0;
    pthread_mutex_unlock(&c->mu);
    oca_core_event ev = {0};
    ev.kind = OCA_CE_TURN_DONE;
    ev.text = xstrdup(final_text ? final_text : "");
    ev_push(c, &ev);
}

static char *pop_followup(oca_core *c) {
    pthread_mutex_lock(&c->mu);
    char *fu = NULL;
    if (c->n_followups > 0) {
        fu = c->followups[0];
        memmove(c->followups, c->followups + 1, (c->n_followups - 1) * sizeof(char *));
        c->n_followups--;
    }
    pthread_mutex_unlock(&c->mu);
    return fu;
}

static void drain_conversation(oca_core *c) {
    if (!c->running) return;
    if (c->trace_failed) {
        if (!c->trace_reported) {
            c->trace_reported = 1;
            push_error(c, E_TRACE, "the diagnostic file failed; stopping this invocation");
            pthread_mutex_lock(&c->mu);
            int goal_active = c->goal_state == OCA_GOAL_ACTIVE;
            pthread_mutex_unlock(&c->mu);
            if (goal_active) set_goal_state(c, OCA_GOAL_BLOCKED, 1);
            push_turn_done(c, "");
            oca_core_event event = {0};
            event.kind = OCA_CE_QUIT_OK;
            ev_push(c, &event);
        }
        c->quit_requested = 1;
        return;
    }
    char *prompt = NULL;
    int announce = 0;
    pthread_mutex_lock(&c->mu);
    if (c->pending_turn_prompt) {
        prompt = c->pending_turn_prompt;
        c->pending_turn_prompt = NULL;
        announce = 1;
    } else if (c->goal_first_turn) {
        prompt = c->goal_first_turn;
        c->goal_first_turn = NULL;
    }
    pthread_mutex_unlock(&c->mu);
    if (!prompt && take_followups(c, &prompt)) {
        fail_turn(c, E_ADAPTER, "cannot combine queued prompts");
        oca_core_stop(c);
        return;
    }
    int continue_goal = 0;
    if (!prompt) {
        pthread_mutex_lock(&c->mu);
        continue_goal = c->goal_state == OCA_GOAL_ACTIVE && c->interactive;
        pthread_mutex_unlock(&c->mu);
    }
    if (prompt || continue_goal) {
        apply_staged(c);
        char *final = NULL;
        oca_purpose purpose = prompt ? OCA_PURPOSE_TURN : OCA_PURPOSE_GOAL_CONT;
        run_agent_turn(c, prompt, purpose, announce, &final);
        free(prompt);
        push_turn_done(c, final ? final : "");
        free(final);
    } else if (atomic_exchange(&c->cancel_latch, 0)) {
        pthread_mutex_lock(&c->mu);
        c->working = 0;
        pthread_mutex_unlock(&c->mu);
        push_status(c);
    }
}

static int save_session_now(oca_core *c, char *err, size_t errsz) {
    if (!c->has_session) {
        err_set(err, errsz, "no session is active");
        return -1;
    }
    char *dir = state_dir_alloc("sessions");
    if (!dir) {
        err_set(err, errsz, "HOME is not set");
        return -1;
    }
    char path[4096 + 64];
    snprintf(path, sizeof(path), "%s/%s.oca", dir, c->sid);
    free(dir);
    session_data view;
    sbuf buffer;
    sb_init(&buffer);
    pthread_mutex_lock(&c->mu);
    session_borrow(c, &view);
    view.last_used = now_unix();
    int rc = session_serialize(&view, &buffer);
    pthread_mutex_unlock(&c->mu);
    if (rc != 0) {
        sb_free(&buffer);
        err_set(err, errsz, "the session exceeds the snapshot limit");
        return -1;
    }
    rc = write_file_atomic_0600(path, buffer.buf, buffer.len, err, errsz);
    sb_free(&buffer);
    if (rc != 0) return -1;
    pthread_mutex_lock(&c->mu);
    c->last_used = now_unix();
    c->dirty = 0;
    c->conversation_dirty = 0;
    pthread_mutex_unlock(&c->mu);
    sbuf j;
    sb_init(&j);
    sb_puts(&j, "{\"event\":\"save\",\"id\":");
    sb_json_str(&j, c->sid);
    sb_puts(&j, "}");
    if (!j.oom) trace_line(c, "core", j.buf);
    sb_free(&j);
    return 0;
}

/* Loads one session file into the core. Returns zero on success. */
static int load_session(oca_core *c, const char *sid, char *err, size_t errsz) {
    if (!hex_id_valid(sid, strlen(sid))) {
        err_set(err, errsz, "invalid session id");
        return -1;
    }
    char *dir = state_dir_alloc("sessions");
    if (!dir) {
        err_set(err, errsz, "HOME is not set");
        return -1;
    }
    char path[4096 + 64];
    snprintf(path, sizeof(path), "%s/%s.oca", dir, sid);
    free(dir);
    size_t flen = 0;
    char *data = read_whole_file(path, OCA_SNAPSHOT_MAX_BYTES, &flen, err, errsz);
    if (!data) return -1;
    session_data sd;
    int rc = session_deserialize(data, flen, &sd, err, errsz);
    free(data);
    if (rc != 0) return -1;
    if (strcmp(sd.id, sid) != 0) {
        session_data_free(&sd);
        err_set(err, errsz, "session identifier does not match its file name");
        return -1;
    }
    if (c->worker_started && c->executor) {
        char *resolved = realpath(sd.workspace, NULL);
        if (!resolved || executor_rebind_workspace(c->executor, resolved, err, errsz)) {
            free(resolved);
            session_data_free(&sd);
            return -1;
        }
        free(sd.workspace);
        sd.workspace = resolved;
    }
    /* Adopt the session while status readers are excluded. */
    pthread_mutex_lock(&c->mu);
    msg_free_all(c->msgs, c->n_msgs);
    c->msgs = sd.msgs;
    c->n_msgs = sd.n_msgs;
    c->cap_msgs = sd.n_msgs;
    memcpy(c->sid, sd.id, 33);
    c->created = sd.created;
    c->last_used = sd.last_used;
    free(c->title);
    c->title = sd.title;
    free(c->workspace);
    c->workspace = sd.workspace;
    free(c->agents);
    c->agents = sd.agents;
    c->hints = (hints_state){.applied = HINTS_UNKNOWN};
    c->goal_state = sd.goal_state;
    c->goal_elapsed_ms = sd.goal_elapsed_ms;
    c->goal_started_ms = sd.goal_state == OCA_GOAL_ACTIVE ? core_now_ms() : 0;
    free(c->goal_text);
    c->goal_text = sd.goal;
    c->comp_first_kept = sd.first_kept;
    c->tokens_before = sd.tokens_before;
    c->tokens_after = sd.tokens_after;
    free(c->summary);
    c->summary = sd.summary;
    /* Capture the saved route before releasing the session data. */
    snprintf(c->route_model, sizeof(c->route_model), "%s", sd.model);
    {
        const oca_model *m = oca_llm_model_resolve(c->llm, sd.model);
        c->route_effort = m ? oca_model_reasoning_index(m, sd.effort) : OCA_REASONING_NONE;
    }
    free(sd.model);
    free(sd.effort);
    c->has_session = 1;
    c->has_cache = 0;
    c->has_cache_input = 0;
    c->dirty = 0;
    c->conversation_dirty = 0;
    pthread_mutex_unlock(&c->mu);
    return 0;
}

/* Creates one new session from the current workspace. */
static int create_session(oca_core *c, char *err, size_t errsz) {
    unsigned char rnd[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0 || read(fd, rnd, sizeof(rnd)) != (ssize_t)sizeof(rnd)) {
        if (fd >= 0) close(fd);
        err_set(err, errsz, "cannot read the random source");
        return -1;
    }
    close(fd);
    char id[33];
    static const char HEX[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        id[i * 2] = HEX[rnd[i] >> 4];
        id[i * 2 + 1] = HEX[rnd[i] & 0xF];
    }
    id[32] = 0;
    char agents_err[256];
    char *agents = NULL;
    char ap[4096 + 32];
    snprintf(ap, sizeof(ap), "%s/AGENTS.md", c->workspace);
    size_t alen = 0;
    agents = context_file_read(ap, OCA_PROJECT_CONTEXT_MAX_BYTES, 1, &alen,
                               agents_err, sizeof(agents_err));
    if (!agents) {
        err_set(err, errsz, "%s", agents_err);
        return -1;
    }
    if (agents && !utf8_text_valid(agents, alen)) {
        free(agents);
        err_set(err, errsz, "AGENTS.md is not valid UTF-8");
        return -1;
    }
    if (!alen) {
        free(agents);
        agents = NULL;
    }
    /* Replace session fields as one status transition. */
    pthread_mutex_lock(&c->mu);
    msg_free_all(c->msgs, c->n_msgs);
    c->msgs = NULL;
    c->n_msgs = c->cap_msgs = 0;
    free(c->title);
    c->title = xstrdup("");
    free(c->agents);
    c->agents = agents;
    memcpy(c->sid, id, 33);
    c->created = c->last_used = now_unix();
    c->hints = (hints_state){0};
    c->goal_state = OCA_GOAL_NONE;
    c->goal_elapsed_ms = c->goal_started_ms = 0;
    free(c->goal_text);
    c->goal_text = NULL;
    c->comp_first_kept = -1;
    c->tokens_before = c->tokens_after = 0;
    free(c->summary);
    c->summary = NULL;
    c->has_session = 1;
    for (size_t i = 0; i < c->n_followups; i++) free(c->followups[i]);
    c->n_followups = 0;
    c->dirty = 0;
    c->conversation_dirty = 0;
    c->has_cache = 0;
    c->has_cache_input = 0;
    pthread_mutex_unlock(&c->mu);
    sbuf j;
    sb_init(&j);
    sb_puts(&j, "{\"event\":\"session_new\",\"id\":");
    sb_json_str(&j, id);
    sb_puts(&j, "}");
    if (!j.oom) trace_line(c, "core", j.buf);
    sb_free(&j);
    return 0;
}

static void push_session_event(oca_core *c, int replaced) {
    publish_side_snapshot(c);
    oca_core_event ev = {0};
    ev.kind = OCA_CE_SESSION;
    ev.session_replaced = replaced != 0;
    ev.session_new = replaced == 2;
    pthread_mutex_lock(&c->mu);
    ev.name = xstrdup(c->workspace);
    snprintf(ev.model_id, sizeof(ev.model_id), "%s", c->route_model);
    ev.text = xstrdup(c->title ? c->title : "");
    ev.detail = xstrdup(c->sid);
    ev.goal = (oca_goal_state)c->goal_state;
    pthread_mutex_unlock(&c->mu);
    ev_push(c, &ev);
}

static void push_sessions_list(oca_core *c) {
    char *dir = state_dir_alloc("sessions");
    oca_session_summary *arr = NULL;
    size_t n = 0;
    char err[256];
    if (!dir) {
        push_error(c, E_SESSION, "HOME is not set");
        return;
    }
    if (oca_core_scan_sessions(dir, &arr, &n, err, sizeof(err)) != 0) {
        free(dir);
        push_error(c, E_SESSION, "%s", err);
        return;
    }
    free(dir);
    oca_core_event ev = {0};
    ev.kind = OCA_CE_SESSIONS;
    ev.sessions = arr;
    ev.session_count = n;
    ev_push(c, &ev);
}

static void push_skills_list(oca_core *c) {
    oca_skill_info *arr = calloc(c->catalog.n ? c->catalog.n : 1, sizeof(*arr));
    if (!arr) return;
    size_t n = 0;
    for (size_t i = 0; i < c->catalog.n; i++) {
        arr[n].name = xstrdup(c->catalog.skills[i].name);
        arr[n].description = xstrdup(c->catalog.skills[i].description);
        arr[n].path = xstrdup(c->catalog.skills[i].path);
        if (arr[n].name && arr[n].description && arr[n].path) {
            n++;
        } else {
            free(arr[n].name);
            free(arr[n].description);
            free(arr[n].path);
            arr[n] = (oca_skill_info){0};
        }
    }
    oca_core_event ev = {0};
    ev.kind = OCA_CE_SKILLS;
    ev.skills = arr;
    ev.skill_count = n;
    ev_push(c, &ev);
}

/* Waits for one save answer. Returns one for yes, zero for no, and minus one
 * when the user cancelled the pending action. */
static int wait_save_answer(oca_core *c) {
    pthread_mutex_lock(&c->mu);
    while (c->save_answer < 0 && !atomic_load(&c->cancel_latch) && c->running) {
        pthread_cond_wait(&c->cond, &c->mu);
    }
    int ans = c->save_answer;
    int cancelled = atomic_load(&c->cancel_latch);
    c->save_answer = -1;
    c->save_waiting = 0;
    pthread_mutex_unlock(&c->mu);
    if (cancelled) {
        atomic_store(&c->cancel_latch, 0);
        return -1;
    }
    return ans;
}

static void prepare_save_answer(oca_core *c) {
    pthread_mutex_lock(&c->mu);
    c->save_answer = -1;
    c->save_waiting = 1;
    pthread_mutex_unlock(&c->mu);
}

/* Runs the save handshake for quit, new, and resume. Returns zero when the
 * action may proceed and minus one when the user cancelled it. */
static int save_handshake(oca_core *c) {
    if (!c->interactive || !c->has_session) return 0;
    pthread_mutex_lock(&c->mu);
    int dirty = c->conversation_dirty;
    pthread_mutex_unlock(&c->mu);
    if (!dirty) return 0;
    oca_core_event ev = {0};
    ev.kind = OCA_CE_SAVE_ASK;
    prepare_save_answer(c);
    ev_push(c, &ev);
    int ans = wait_save_answer(c);
    if (ans < 0) return -1;
    if (ans == 1) {
        char err[256];
        if (save_session_now(c, err, sizeof(err)) == 0) return 0;
        push_error(c, E_SAVE, "%s", err);
        ev = (oca_core_event){0};
        ev.kind = OCA_CE_SAVE_ASK_CONT;
        prepare_save_answer(c);
        ev_push(c, &ev);
        ans = wait_save_answer(c);
        if (ans < 0) return -1;
        if (ans != 1) return -1;
    }
    pthread_mutex_lock(&c->mu);
    c->conversation_dirty = 0;
    pthread_mutex_unlock(&c->mu);
    return 0;
}

static int transcript_entry_add(oca_core_event *batch, oca_core_event_kind kind,
                                const char *text, const char *name, const char *detail,
                                int state) {
    oca_transcript_entry *entry = &batch->entries[batch->entry_count++];
    entry->kind = kind;
    entry->text = text ? xstrdup(text) : NULL;
    entry->name = name ? xstrdup(name) : NULL;
    entry->detail = detail ? xstrdup(detail) : NULL;
    entry->tool_state = state;
    return (text && !entry->text) || (name && !entry->name) ||
           (detail && !entry->detail) ? -1 : 0;
}

/* Three entries per message cover user text, assistant channels, and tools. */
static void push_transcript(oca_core *c) {
    oca_core_event batch = {0};
    batch.kind = OCA_CE_TRANSCRIPT;
    size_t capacity = c->n_msgs * 3 + 1;
    batch.entries = calloc(capacity, sizeof(*batch.entries));
    if (!batch.entries) goto failed;
    const msg_rec *assistant = NULL;
    for (size_t i = c->ephemeral ? c->side_prefix_count : 0; i < c->n_msgs; i++) {
        const msg_rec *message = &c->msgs[i];
        if (message->role == OCA_MSG_USER) {
            if (transcript_entry_add(&batch, OCA_CE_USER_TEXT, message->text,
                                     NULL, NULL, 0)) goto failed;
        } else if (message->role == OCA_MSG_ASSISTANT) {
            assistant = message;
            if (message->reasoning && *message->reasoning &&
                transcript_entry_add(&batch, OCA_CE_ASSISTANT_REASON,
                                     message->reasoning, NULL, NULL, 0)) goto failed;
            if (message->text && *message->text &&
                transcript_entry_add(&batch, OCA_CE_ASSISTANT_TEXT,
                                     message->text, NULL, NULL, 0)) goto failed;
            if (transcript_entry_add(&batch, OCA_CE_TURN_DONE,
                                     NULL, NULL, NULL, 0)) goto failed;
        } else if (message->role == OCA_MSG_TOOL) {
            const char *name = "tool", *args = "{}";
            if (assistant) for (size_t k = 0; k < assistant->n_calls; k++) {
                if (!strcmp(assistant->calls[k].id, message->call_id)) {
                    name = assistant->calls[k].name;
                    args = assistant->calls[k].args;
                    break;
                }
            }
            if (transcript_entry_add(&batch, OCA_CE_TOOL_START, args, name,
                                     message->call_id, 0) ||
                transcript_entry_add(&batch, OCA_CE_TOOL_RESULT, message->text, name,
                                     message->call_id, message->state)) goto failed;
        }
    }
    ev_push(c, &batch);
    return;
failed:
    oca_core_event_release(&batch);
    push_error(c, E_ADAPTER, "cannot copy the session transcript");
}

static void manual_bash(oca_core *c, const char *command) {
    working_start(c);
    set_state(c, OCA_S_RUNNING_TOOL);
    sbuf args;
    sb_init(&args);
    sb_puts(&args, "{\"command\":");
    sb_json_str(&args, command);
    sb_puts(&args, "}");
    if (args.oom) {
        sb_free(&args);
        push_error(c, E_ADAPTER, "cannot prepare the Bash command");
        set_state(c, OCA_S_IDLE);
        push_turn_done(c, "");
        return;
    }
    char id[64];
    snprintf(id, sizeof(id), "manual_%llu", (unsigned long long)++c->manual_seq);
    oca_core_event event = {0};
    event.kind = OCA_CE_TOOL_START;
    event.name = xstrdup("bash");
    event.detail = xstrdup(id);
    event.text = xstrdup(args.buf);
    ev_push(c, &event);
    trace_tool(c, "start", "bash", id, args.buf, -1, NULL, -1);
    tool_outcome outcome;
    if (atomic_load(&c->cancel_latch)) {
        memset(&outcome, 0, sizeof(outcome));
        outcome.text = xstrdup("Cancelled before execution.");
        outcome.state = OCA_TOOL_INTERRUPTED;
    } else run_tool_call(c, "bash", args.buf, &outcome);
    sb_free(&args);
    event = (oca_core_event){0};
    event.kind = OCA_CE_TOOL_RESULT;
    event.name = xstrdup("bash");
    event.detail = xstrdup(id);
    event.text = outcome.text;
    event.tool_state = outcome.state;
    trace_tool(c, "end", "bash", id, NULL,
               outcome.text ? (long)strlen(outcome.text) : 0,
               outcome.state == OCA_TOOL_COMPLETE ? "complete" :
               outcome.state == OCA_TOOL_INTERRUPTED ? "interrupted" : "error", -1);
    ev_push(c, &event);
    if (atomic_load(&c->cancel_latch) && c->goal_state == OCA_GOAL_ACTIVE)
        set_goal_state(c, OCA_GOAL_PAUSED, 1);
    set_state(c, OCA_S_IDLE);
    push_turn_done(c, "");
}

static void handle_action(oca_core *c, action_rec *a) {
    char err[256];
    switch (a->kind) {
    case ACT_SET_ROUTE: {
        pthread_mutex_lock(&c->mu);
        if (c->state != OCA_S_IDLE) {
            snprintf(c->staged_model, sizeof(c->staged_model), "%s", a->text);
            c->staged_effort = a->effort;
            c->has_staged_route = 1;
            pthread_mutex_unlock(&c->mu);
            push_status(c);
            break;
        }
        pthread_mutex_unlock(&c->mu);
        if (apply_route(c, a->text, a->effort, err, sizeof(err)) != 0) {
            push_error(c, E_ROUTESET, "%s", err);
        } else {
            push_status(c);
        }
        break;
    }
    case ACT_GOAL: {
        switch (a->goal_cmd) {
        case OCA_GOAL_CMD_SET: {
            char *first_turn = xstrdup(a->text);
            if (!first_turn) {
                push_error(c, E_ADAPTER, "out of memory");
                break;
            }
            pthread_mutex_lock(&c->mu);
            free(c->goal_text);
            c->goal_text = a->text;
            a->text = NULL;
            c->goal_state = OCA_GOAL_ACTIVE;
            c->goal_elapsed_ms = 0;
            c->goal_started_ms = core_now_ms();
            free(c->goal_first_turn);
            c->goal_first_turn = first_turn;
            c->dirty = 1;
            pthread_mutex_unlock(&c->mu);
            trace_line(c, "goal", "{\"event\":\"set\"}");
            set_goal_state(c, OCA_GOAL_ACTIVE, 1);
            drain_conversation(c);
            break;
        }
        case OCA_GOAL_CMD_EDIT: {
            set_goal_state(c, OCA_GOAL_PAUSED, 0);
            pthread_mutex_lock(&c->mu);
            c->dirty = 1;
            char *obj = c->goal_text ? xstrdup(c->goal_text) : NULL;
            pthread_mutex_unlock(&c->mu);
            oca_core_event ev = {0};
            ev.kind = OCA_CE_GOAL;
            ev.goal = OCA_GOAL_PAUSED;
            ev.text = obj;
            ev.name = xstrdup("edit");
            ev_push(c, &ev);
            trace_line(c, "goal", "{\"event\":\"edit\"}");
            break;
        }
        case OCA_GOAL_CMD_PAUSE: {
            pthread_mutex_lock(&c->mu);
            if (c->goal_state == OCA_GOAL_ACTIVE) {
                c->goal_state = OCA_GOAL_PAUSED;
                c->dirty = 1;
                pthread_mutex_unlock(&c->mu);
                set_goal_state(c, OCA_GOAL_PAUSED, 1);
                trace_line(c, "goal", "{\"event\":\"state\",\"state\":\"paused\"}");
            } else {
                pthread_mutex_unlock(&c->mu);
            }
            break;
        }
        case OCA_GOAL_CMD_RESUME: {
            pthread_mutex_lock(&c->mu);
            if (c->goal_state == OCA_GOAL_PAUSED || c->goal_state == OCA_GOAL_BLOCKED) {
                c->goal_state = OCA_GOAL_ACTIVE;
                c->dirty = 1;
                pthread_mutex_unlock(&c->mu);
                set_goal_state(c, OCA_GOAL_ACTIVE, 1);
                trace_line(c, "goal", "{\"event\":\"state\",\"state\":\"active\"}");
                drain_conversation(c);
            } else {
                pthread_mutex_unlock(&c->mu);
            }
            break;
        }
        case OCA_GOAL_CMD_CLEAR: {
            pthread_mutex_lock(&c->mu);
            int had_goal = c->goal_state != OCA_GOAL_NONE || c->goal_text != NULL;
            free(c->goal_text);
            c->goal_text = NULL;
            c->goal_state = OCA_GOAL_NONE;
            if (had_goal) c->dirty = 1;
            pthread_mutex_unlock(&c->mu);
            set_goal_state(c, OCA_GOAL_NONE, 1);
            trace_line(c, "goal", "{\"event\":\"clear\"}");
            break;
        }
        }
        break;
    }
    case ACT_NEW: {
        if (save_handshake(c) != 0) {
            push_status(c);
            break;
        }
        if (create_session(c, err, sizeof(err)) != 0) {
            push_error(c, E_SESSION, "%s", err);
            push_status(c);
            break;
        }
        push_session_event(c, 2);
        push_status(c);
        break;
    }
    case ACT_RESUME: {
        if (save_handshake(c) != 0) {
            push_status(c);
            break;
        }
        if (load_session(c, a->text, err, sizeof(err)) != 0) {
            push_error(c, E_SESSION, "%s", err);
            push_status(c);
            break;
        }
        /* load_session stored the saved route in the route fields. A CLI
         * override from startup keeps priority over the saved route. */
        if (!c->cli_route_locked) {
            char smodel[OCA_LLM_MODEL_ID_MAX + 1];
            oca_reasoning seff;
            pthread_mutex_lock(&c->mu);
            snprintf(smodel, sizeof(smodel), "%s", c->route_model);
            seff = c->route_effort;
            pthread_mutex_unlock(&c->mu);
            const oca_model *sm = oca_llm_model_resolve(c->llm, smodel);
            int usable = route_usable(c, smodel, seff);
            if (usable) {
                if (apply_route(c, smodel, seff, err, sizeof(err)) != 0) {
                    push_error(c, E_ROUTESET, "%s", err);
                }
            } else {
                /* Unavailable and retired routes remain visible until the user selects a model. */
                const char *level = oca_model_reasoning_name(sm, seff);
                oca_core_event ev = {.kind = OCA_CE_ROUTE};
                snprintf(ev.model_id, sizeof(ev.model_id), "%s", smodel);
                snprintf(ev.reasoning_name, sizeof(ev.reasoning_name), "%s", level ? level : "");
                ev.route_available = 0;
                ev_push(c, &ev);
            }
        }
        push_session_event(c, 1);
        push_status(c);
        break;
    }
    case ACT_SAVE: {
        char serr[256];
        if (save_session_now(c, serr, sizeof(serr)) != 0) {
            push_error(c, E_SAVE, "%s", serr);
        } else {
            push_session_event(c, 0);
        }
        break;
    }
    case ACT_SESSIONS:
        push_sessions_list(c);
        break;
    case ACT_SKILLS:
        push_skills_list(c);
        break;
    case ACT_REPLAY:
        push_transcript(c);
        break;
    case ACT_BASH:
        manual_bash(c, a->text);
        break;
    case ACT_QUIT: {
        if (save_handshake(c) != 0) {
            c->quit_requested = 0;
            push_status(c);
            break;
        }
        oca_core_event ev = {0};
        ev.kind = OCA_CE_QUIT_OK;
        ev_push(c, &ev);
        c->quit_requested = 1;
        break;
    }
    }
    free(a->text);
}

static void *worker_main(void *arg) {
    worker_event_producer = 1;
    oca_core *c = arg;
    for (;;) {
        pthread_mutex_lock(&c->mu);
        while (c->aq_head == c->aq_tail && !c->pending_turn_prompt &&
               !c->goal_first_turn && c->n_followups == 0 &&
               (!c->interactive || c->goal_state != OCA_GOAL_ACTIVE || !route_available(c)) &&
               !c->trace_failed && !atomic_load(&c->cancel_latch) && c->running) {
            pthread_cond_wait(&c->cond, &c->mu);
        }
        if (!c->running) {
            pthread_mutex_unlock(&c->mu);
            break;
        }
        if (c->aq_head == c->aq_tail) {
            pthread_mutex_unlock(&c->mu);
            drain_conversation(c);
            if (c->quit_requested) break;
            continue;
        }
        action_rec a = c->aq[c->aq_head];
        c->aq_head = (c->aq_head + 1) % ACTION_RING_SIZE;
        pthread_mutex_unlock(&c->mu);
        handle_action(c, &a);
        if (c->quit_requested) break;
    }
    pthread_mutex_lock(&c->mu);
    c->state = OCA_S_STOPPED;
    c->running = 0;
    pthread_mutex_unlock(&c->mu);
    push_status(c);
    pthread_cond_broadcast(&c->cond);
    return NULL;
}

/* ============================================================================
 * Public action API
 * ========================================================================== */

static int enqueue_action(oca_core *c, int kind, const char *text, oca_reasoning effort,
                          int goal_cmd, char *err, size_t errsz) {
    if (!(c->flags & OCA_CORE_RUNTIME)) {
        err_set(err, errsz, "the core has no worker in command mode");
        return -1;
    }
    action_rec a;
    memset(&a, 0, sizeof(a));
    a.kind = kind;
    a.text = text ? xstrdup(text) : NULL;
    a.effort = effort;
    a.goal_cmd = goal_cmd;
    if (text && !a.text) {
        err_set(err, errsz, "out of memory");
        return -1;
    }
    pthread_mutex_lock(&c->mu);
    if (!c->running || c->state == OCA_S_STOPPED) {
        pthread_mutex_unlock(&c->mu);
        free(a.text);
        err_set(err, errsz, "the core worker is stopped");
        return -1;
    }
    if ((c->aq_tail + 1) % ACTION_RING_SIZE == c->aq_head) {
        pthread_mutex_unlock(&c->mu);
        free(a.text);
        err_set(err, errsz, "the action queue is full");
        return -1;
    }
    if (kind == ACT_BASH ||
        (kind == ACT_GOAL && goal_cmd == OCA_GOAL_CMD_SET))
        working_start_locked(c);
    c->aq[c->aq_tail] = a;
    c->aq_tail = (c->aq_tail + 1) % ACTION_RING_SIZE;
    pthread_mutex_unlock(&c->mu);
    pthread_cond_broadcast(&c->cond);
    return 0;
}

static int validate_prompt_text(const char *text, char *err, size_t errsz) {
    if (!text) {
        err_set(err, errsz, "the prompt is missing");
        return -1;
    }
    size_t n = strlen(text);
    if (n == 0) {
        err_set(err, errsz, "the prompt is empty");
        return -1;
    }
    if (!utf8_valid(text, n)) {
        err_set(err, errsz, "the prompt is not valid UTF-8");
        return -1;
    }
    if (n > OCA_PROMPT_MAX_BYTES) {
        err_set(err, errsz, "the prompt exceeds the limit of %d bytes", OCA_PROMPT_MAX_BYTES);
        return -1;
    }
    return 0;
}

static int submit_prompt(oca_core *c, const char *text, char *err, size_t errsz) {
    pthread_mutex_lock(&c->mu);
    if (!c->running || !(c->flags & OCA_CORE_RUNTIME)) {
        pthread_mutex_unlock(&c->mu);
        err_set(err, errsz, "the core worker is stopped");
        return -1;
    }
    size_t bytes = strlen(text);
    if (c->n_followups) {
        bytes += 32;
        for (size_t i = 0; i < c->n_followups; i++) bytes += strlen(c->followups[i]) + 32;
    }
    if (c->n_followups >= OCA_FOLLOWUP_MAX || bytes > OCA_PROMPT_MAX_BYTES) {
        pthread_mutex_unlock(&c->mu);
        err_set(err, errsz, "the follow-up queue is full");
        return -1;
    }
    char *copy = xstrdup(text);
    if (copy && c->state == OCA_S_IDLE && !c->pending_turn_prompt &&
        !c->goal_first_turn && !c->n_followups && c->goal_state != OCA_GOAL_ACTIVE) {
        c->pending_turn_prompt = copy;
        working_start_locked(c);
        pthread_cond_broadcast(&c->cond);
        pthread_mutex_unlock(&c->mu);
        return 0;
    }
    char **items = copy ? realloc(c->followups, (c->n_followups + 1) * sizeof(*items)) : NULL;
    if (!items) {
        free(copy);
        pthread_mutex_unlock(&c->mu);
        err_set(err, errsz, "out of memory");
        return -1;
    }
    c->followups = items;
    c->followups[c->n_followups++] = copy;
    working_start_locked(c);
    pthread_cond_broadcast(&c->cond);
    pthread_mutex_unlock(&c->mu);
    push_status(c);
    return 0;
}

int oca_core_submit_prompt(oca_core *c, const char *text, char *err, size_t errsz) {
    if (validate_prompt_text(text, err, errsz) != 0) return -1;
    /* Reject an unusable route before storing or consuming the input. */
    pthread_mutex_lock(&c->mu);
    char route_model[OCA_LLM_MODEL_ID_MAX + 1];
    snprintf(route_model, sizeof(route_model), "%s", c->route_model);
    oca_reasoning route_effort = c->route_effort;
    pthread_mutex_unlock(&c->mu);
    const oca_model *m = oca_llm_model_resolve(c->llm, route_model);
    if (!route_usable(c, route_model, route_effort)) {
        err_set(err, errsz, m && m->reasoning_count > 0 ?
            "select a model and a reasoning level before submitting a prompt" :
            "select a model before submitting a prompt");
        return -1;
    }
    size_t reserved = strlen(text) + OCA_TOOL_RESULT_MAX_BYTES;
    if (c->ephemeral) {
        pthread_mutex_lock(&c->mu);
        if (reserved > OCA_EPHEMERAL_HISTORY_MAX_BYTES - c->side_history_reserved) {
            pthread_mutex_unlock(&c->mu);
            err_set(err, errsz, "the ephemeral history limit was reached; close this chat to start another");
            return -1;
        }
        c->side_history_reserved += reserved;
        pthread_mutex_unlock(&c->mu);
    }
    int result = submit_prompt(c, text, err, errsz);
    if (result && c->ephemeral) {
        pthread_mutex_lock(&c->mu);
        c->side_history_reserved -= reserved;
        pthread_mutex_unlock(&c->mu);
    }
    return result;
}


void oca_core_cancel(oca_core *c) {
    pthread_mutex_lock(&c->mu);
    if (c->working || c->state != OCA_S_IDLE || c->save_waiting ||
        c->goal_state == OCA_GOAL_ACTIVE)
        atomic_store(&c->cancel_latch, 1);
    pthread_cond_broadcast(&c->cond);
    pthread_mutex_unlock(&c->mu);
}

int oca_core_pop_followup(oca_core *c, char **text, char *err, size_t errsz) {
    if (!text) { err_set(err, errsz, "the text output is required"); return -1; }
    *text = pop_followup(c);
    pthread_mutex_lock(&c->mu);
    if (*text && c->ephemeral) c->side_history_reserved -= strlen(*text) + OCA_TOOL_RESULT_MAX_BYTES;
    if (!c->n_followups && c->state == OCA_S_IDLE && c->aq_head == c->aq_tail &&
        !c->goal_first_turn && !c->pending_turn_prompt)
        c->working = 0;
    pthread_mutex_unlock(&c->mu);
    push_status(c);
    return 0;
}

void oca_core_send_queued(oca_core *c) {
    pthread_mutex_lock(&c->mu);
    if (c->n_followups && c->state != OCA_S_IDLE) atomic_store(&c->cancel_latch, 1);
    pthread_cond_broadcast(&c->cond);
    pthread_mutex_unlock(&c->mu);
}

int oca_core_set_hints(oca_core *c, int enabled, char *err, size_t errsz) {
    if (!c || (!c->interactive && !c->ephemeral) || (enabled != 0 && enabled != 1)) {
        err_set(err, errsz, "hints require an interactive conversation and on or off");
        return -1;
    }
    pthread_mutex_lock(&c->mu);
    c->hints.enabled = enabled;
    pthread_mutex_unlock(&c->mu);
    return 0;
}

int oca_core_hints_enabled(oca_core *c) {
    if (!c) return 0;
    pthread_mutex_lock(&c->mu);
    int enabled = c->hints.enabled;
    pthread_mutex_unlock(&c->mu);
    return enabled;
}

int oca_core_set_route(oca_core *c, const char *model_id_or_alias, const char *reasoning_name,
                       char *err, size_t errsz) {
    if (!model_id_or_alias) {
        err_set(err, errsz, "the model is missing");
        return -1;
    }
    const oca_model *m = oca_llm_model_resolve(c->llm, model_id_or_alias);
    if (!m) {
        err_set(err, errsz, "unknown model: %s", model_id_or_alias);
        return -1;
    }
    if (!oca_llm_model_available(c->llm, m)) {
        err_set(err, errsz, "model is not available: %s", model_id_or_alias);
        return -1;
    }
    oca_reasoning effort = OCA_REASONING_NONE;
    if (m->reasoning_count == 0) {
        if (reasoning_name && *reasoning_name) {
            err_set(err, errsz, "model has no configurable reasoning: %s", m->id);
            return -1;
        }
    } else if (reasoning_name && *reasoning_name) {
        effort = oca_model_reasoning_index(m, reasoning_name);
        if (effort == OCA_REASONING_NONE) {
            oca_model_reasoning_error(m, reasoning_name, err, errsz);
            return -1;
        }
    } else {
        pthread_mutex_lock(&c->mu);
        char current[OCA_REASONING_NAME_MAX + 1];
        snprintf(current, sizeof(current), "%s", route_reasoning_name(c));
        pthread_mutex_unlock(&c->mu);
        effort = oca_model_reasoning_index(m, current);
        if (effort == OCA_REASONING_NONE) {
            err_set(err, errsz, "select a reasoning level for %s", m->id);
            return -1;
        }
    }
    pthread_mutex_lock(&c->mu);
    if (c->state != OCA_S_IDLE) {
        snprintf(c->staged_model, sizeof(c->staged_model), "%s", m->id);
        c->staged_effort = effort;
        c->has_staged_route = 1;
        pthread_mutex_unlock(&c->mu);
        push_status(c);
        return 0;
    }
    pthread_mutex_unlock(&c->mu);
    return enqueue_action(c, ACT_SET_ROUTE, m->id, effort, 0, err, errsz);
}

int oca_core_submit_answers(oca_core *c, const char *answers_json, char *err, size_t errsz) {
    if (!answers_json) {
        err_set(err, errsz, "answers are missing");
        return -1;
    }
    const char *why = NULL;
    cJSON *doc = json_parse(answers_json, strlen(answers_json), &why);
    if (!doc) {
        err_set(err, errsz, "answers are not valid JSON: %s", why ? why : "syntax");
        return -1;
    }
    cJSON_Delete(doc);
    char *copy = xstrdup(answers_json);
    if (!copy) {
        err_set(err, errsz, "out of memory");
        return -1;
    }
    pthread_mutex_lock(&c->mu);
    if (!c->ia_waiting || c->state != OCA_S_WAITING_USER || c->ia_answers_json ||
        atomic_load(&c->cancel_latch) || !c->running) {
        pthread_mutex_unlock(&c->mu);
        free(copy);
        err_set(err, errsz, "no user interaction is waiting for answers");
        return -1;
    }
    free(c->ia_answers_json);
    c->ia_answers_json = copy;
    pthread_mutex_unlock(&c->mu);
    pthread_cond_broadcast(&c->cond);
    return 0;
}

int oca_core_goal(oca_core *c, int goal_cmd, const char *objective, char *err, size_t errsz) {
    if (goal_cmd < OCA_GOAL_CMD_SHOW || goal_cmd > OCA_GOAL_CMD_CLEAR) {
        err_set(err, errsz, "invalid goal command");
        return -1;
    }
    /* Reading the goal must not wait behind an active generation. */
    if (goal_cmd == OCA_GOAL_CMD_SHOW) {
        oca_core_event ev = {.kind = OCA_CE_GOAL};
        ev.name = xstrdup("show");
        pthread_mutex_lock(&c->mu);
        ev.goal = (oca_goal_state)c->goal_state;
        ev.goal_elapsed_ms = goal_elapsed(c);
        ev.text = c->goal_text ? xstrdup(c->goal_text) : NULL;
        int failed = !ev.name || (c->goal_text && !ev.text);
        pthread_mutex_unlock(&c->mu);
        if (failed) {
            oca_core_event_release(&ev);
            err_set(err, errsz, "out of memory");
            return -1;
        }
        if (ev_push(c, &ev)) {
            err_set(err, errsz, "the event queue is full; try /goal again");
            return -1;
        }
        return 0;
    }
    if (!c->interactive) {
        err_set(err, errsz, "goals need the interactive runtime");
        return -1;
    }
    if (goal_cmd == OCA_GOAL_CMD_SET) {
        if (!objective || !*objective) {
            err_set(err, errsz, "the objective is empty");
            return -1;
        }
        if (!utf8_valid(objective, strlen(objective))) {
            err_set(err, errsz, "the objective is not valid UTF-8");
            return -1;
        }
        if (utf8_chars(objective) > OCA_GOAL_MAX_CHARS) {
            err_set(err, errsz, "the objective exceeds %d characters", OCA_GOAL_MAX_CHARS);
            return -1;
        }
    }
    if (goal_cmd == OCA_GOAL_CMD_RESUME) {
        pthread_mutex_lock(&c->mu);
        int gs = c->goal_state;
        pthread_mutex_unlock(&c->mu);
        if (gs != OCA_GOAL_PAUSED && gs != OCA_GOAL_BLOCKED) {
            err_set(err, errsz, "only a paused or blocked goal can resume");
            return -1;
        }
    }
    if (goal_cmd == OCA_GOAL_CMD_EDIT) {
        pthread_mutex_lock(&c->mu);
        int gs = c->goal_state;
        pthread_mutex_unlock(&c->mu);
        if (gs == OCA_GOAL_NONE) {
            err_set(err, errsz, "no goal exists to edit");
            return -1;
        }
    }
    return enqueue_action(c, ACT_GOAL, objective, OCA_REASONING_NONE, goal_cmd, err, errsz);
}

int oca_core_new_session(oca_core *c, char *err, size_t errsz) {
    if (!c->interactive) {
        err_set(err, errsz, "session replacement needs the interactive runtime");
        return -1;
    }
    return enqueue_action(c, ACT_NEW, NULL, OCA_REASONING_NONE, 0, err, errsz);
}

int oca_core_save_session(oca_core *c, char *err, size_t errsz) {
    if (!(c->flags & OCA_CORE_RUNTIME) || c->ephemeral) {
        err_set(err, errsz, "saving needs a main conversation runtime");
        return -1;
    }
    return enqueue_action(c, ACT_SAVE, NULL, OCA_REASONING_NONE, 0, err, errsz);
}

int oca_core_list_sessions(oca_core *c, char *err, size_t errsz) {
    return enqueue_action(c, ACT_SESSIONS, NULL, OCA_REASONING_NONE, 0, err, errsz);
}

int oca_core_resume_session(oca_core *c, const char *session_id, char *err, size_t errsz) {
    if (!c->interactive) {
        err_set(err, errsz, "session resume needs the interactive runtime");
        return -1;
    }
    if (!hex_id_valid(session_id, strlen(session_id))) {
        err_set(err, errsz, "invalid session id");
        return -1;
    }
    return enqueue_action(c, ACT_RESUME, session_id, OCA_REASONING_NONE, 0, err, errsz);
}

int oca_core_bash(oca_core *c, const char *command, char *err, size_t errsz) {
    if (!c->interactive) {
        err_set(err, errsz, "manual Bash needs the interactive runtime");
        return -1;
    }
    if (validate_prompt_text(command, err, errsz) != 0) return -1;
    const char *at = command;
    while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r') at++;
    if (!*at) {
        err_set(err, errsz, "the Bash command is empty");
        return -1;
    }
    return enqueue_action(c, ACT_BASH, command, OCA_REASONING_NONE, 0, err, errsz);
}

int oca_core_replay(oca_core *c, char *err, size_t errsz) {
    return enqueue_action(c, ACT_REPLAY, NULL, OCA_REASONING_NONE, 0, err, errsz);
}

int oca_core_list_skills(oca_core *c, char *err, size_t errsz) {
    return enqueue_action(c, ACT_SKILLS, NULL, OCA_REASONING_NONE, 0, err, errsz);
}

int oca_core_quit(oca_core *c, char *err, size_t errsz) {
    return enqueue_action(c, ACT_QUIT, NULL, OCA_REASONING_NONE, 0, err, errsz);
}

int oca_core_save_answer(oca_core *c, int yes, char *err, size_t errsz) {
    pthread_mutex_lock(&c->mu);
    if (!c->save_waiting || c->save_answer >= 0) {
        pthread_mutex_unlock(&c->mu);
        err_set(err, errsz, "no save question is waiting for an answer");
        return -1;
    }
    c->save_answer = yes ? 1 : 0;
    pthread_mutex_unlock(&c->mu);
    pthread_cond_broadcast(&c->cond);
    return 0;
}

/* ============================================================================
 * Event polling
 * ========================================================================== */

/* Caller holds mu. Removing an event releases space for the worker producer. */
static int pop_event_locked(oca_core *c, oca_core_event *out) {
    if (c->eq_head == c->eq_tail) return 0;
    *out = c->eq[c->eq_head];
    c->eq_head = (c->eq_head + 1) % EVENT_RING_SIZE;
    if (c->eq_head == c->eq_tail) c->wake_pending = 0;
    pthread_cond_broadcast(&c->cond);
    return 1;
}

int oca_core_poll_event(oca_core *c, oca_core_event *out) {
    pthread_mutex_lock(&c->mu);
    int ready = pop_event_locked(c, out);
    pthread_mutex_unlock(&c->mu);
    return ready;
}

int oca_core_wait_event(oca_core *c, oca_core_event *out) {
    pthread_mutex_lock(&c->mu);
    while (c->eq_head == c->eq_tail && c->running)
        pthread_cond_wait(&c->cond, &c->mu);
    int ready = pop_event_locked(c, out);
    pthread_mutex_unlock(&c->mu);
    return ready;
}

void oca_core_event_release(oca_core_event *event) {
    if (!event) return;
    free(event->text);
    free(event->name);
    free(event->detail);
    for (size_t i = 0; event->sessions && i < event->session_count; i++) {
        free(event->sessions[i].id);
        free(event->sessions[i].title);
        free(event->sessions[i].model);
        free(event->sessions[i].workspace);
    }
    free(event->sessions);
    for (size_t i = 0; event->skills && i < event->skill_count; i++) {
        free(event->skills[i].name);
        free(event->skills[i].description);
        free(event->skills[i].path);
    }
    free(event->skills);
    for (size_t i = 0; event->questions && i < event->question_count; i++) {
        free(event->questions[i].question);
        for (size_t k = 0; event->questions[i].options && k < event->questions[i].option_count; k++)
            free(event->questions[i].options[k]);
        free(event->questions[i].options);
    }
    free(event->questions);
    for (size_t i = 0; event->entries && i < event->entry_count; i++) {
        free(event->entries[i].text);
        free(event->entries[i].name);
        free(event->entries[i].detail);
    }
    free(event->entries);
    memset(event, 0, sizeof(*event));
}

int oca_core_wake_fd(const oca_core *c) {
    return c->wake_pipe[0];
}

/* ============================================================================
 * Status and catalog view
 * ========================================================================== */

void oca_core_get_status(const oca_core *core, oca_core_status *out) {
    oca_core *c = (oca_core *)core;
    pthread_mutex_lock(&c->mu);
    fill_status_locked(c, out);
    pthread_mutex_unlock(&c->mu);
}

size_t oca_core_model_count(const oca_core *c) {
    return oca_llm_model_count(c->llm);
}

const oca_model *oca_core_model_at(const oca_core *c, size_t index) {
    return oca_llm_model_at(c->llm, index);
}

int oca_core_model_available(const oca_core *c, const oca_model *m) {
    return oca_llm_model_available(c->llm, m);
}

size_t oca_core_skill_count(const oca_core *c) {
    return c->catalog.n;
}

const char *oca_core_skill_name(const oca_core *c, size_t i) {
    return i < c->catalog.n ? c->catalog.skills[i].name : NULL;
}

const char *oca_core_skill_description(const oca_core *c, size_t i) {
    return i < c->catalog.n ? c->catalog.skills[i].description : NULL;
}

size_t oca_core_skill_warning_count(const oca_core *c) {
    return c->catalog.n_warns;
}

/* ============================================================================
 * Session storage helpers
 * ========================================================================== */

int oca_core_scan_sessions(const char *dir, oca_session_summary **out,
                           size_t *out_count, char *err, size_t errsz) {
    *out = NULL;
    *out_count = 0;
    DIR *d = opendir(dir);
    if (!d) {
        if (errno == ENOENT) return 0;
        err_set(err, errsz, "cannot open %s: %s", dir, strerror(errno));
        return -1;
    }
    size_t cap = 0, n = 0;
    oca_session_summary *arr = NULL;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *fn = de->d_name;
        size_t fl = strlen(fn);
        if (fl != 32 + 4 || strcmp(fn + 32, ".oca") != 0) continue;
        char path[4096 + 64];
        snprintf(path, sizeof(path), "%s/%s", dir, fn);
        size_t flen = 0;
        char why[256];
        char *data = read_whole_file(path, OCA_SNAPSHOT_MAX_BYTES, &flen, why, sizeof(why));
        if (!data) continue;
        session_data sd;
        if (session_deserialize(data, flen, &sd, why, sizeof(why)) != 0) {
            free(data);
            continue;
        }
        free(data);
        if (memcmp(sd.id, fn, 32) != 0) {
            session_data_free(&sd);
            continue;
        }
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 8;
            oca_session_summary *na = realloc(arr, nc * sizeof(*na));
            if (!na) {
                session_data_free(&sd);
                continue;
            }
            arr = na;
            cap = nc;
        }
        arr[n].id = xstrdup(sd.id);
        arr[n].title = xstrdup(sd.title ? sd.title : "");
        arr[n].model = xstrdup(sd.model ? sd.model : "");
        arr[n].workspace = xstrdup(sd.workspace ? sd.workspace : "");
        arr[n].last_used = sd.last_used;
        n++;
        session_data_free(&sd);
    }
    closedir(d);
    /* Sort by last-used, newest first. */
    for (size_t i = 1; i < n; i++) {
        oca_session_summary key = arr[i];
        size_t j = i;
        while (j > 0 && arr[j - 1].last_used < key.last_used) {
            arr[j] = arr[j - 1];
            j--;
        }
        arr[j] = key;
    }
    *out = arr;
    *out_count = n;
    return 0;
}

int oca_core_delete_session(const char *dir, const char *session_id, char *err, size_t errsz) {
    if (!hex_id_valid(session_id, strlen(session_id))) {
        err_set(err, errsz, "invalid session id");
        return -1;
    }
    char path[4096 + 64];
    snprintf(path, sizeof(path), "%s/%s.oca", dir, session_id);
    if (unlink(path) != 0) {
        err_set(err, errsz, "cannot delete session: %s", strerror(errno));
        return -1;
    }
    return 0;
}

void oca_core_free_summaries(oca_session_summary *sessions, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(sessions[i].id);
        free(sessions[i].title);
        free(sessions[i].model);
        free(sessions[i].workspace);
    }
    free(sessions);
}

/* ============================================================================
 * Creation and destruction
 * ========================================================================== */

static int select_initial_route(oca_core *c, const oca_core_options *options,
                                 char *err, size_t errsz) {
    oca_llm *llm = c->llm;
    char chosen[OCA_LLM_MODEL_ID_MAX + 1] = "";
    oca_reasoning effort = OCA_REASONING_NONE;
    const char *saved_level = NULL;
    if (options->model) {
        const oca_model *m = oca_llm_model_resolve(llm, options->model);
        if (!m) {
            err_set(err, errsz, "unknown model: %s", options->model);
            return -1;
        }
        if (!oca_llm_model_available(llm, m)) {
            err_set(err, errsz, "model is not available: %s", options->model);
            return -1;
        }
        snprintf(chosen, sizeof(chosen), "%s", m->id);
        /* round(N / 2), with one-based positions and halves rounded up. */
        if (m->reasoning_count) effort = (oca_reasoning)((m->reasoning_count - 1) / 2);
    } else if (options->resume_id) {
        /* The resumed session keeps its saved route, usable or not. */
        snprintf(chosen, sizeof(chosen), "%s", c->route_model);
        effort = c->route_effort;
        const oca_model *m = oca_llm_model_resolve(llm, chosen);
        if (m && m->reasoning_count == 0) effort = OCA_REASONING_NONE;
    } else if (options->last_used_model && *options->last_used_model) {
        const oca_model *m = oca_llm_model_resolve(llm, options->last_used_model);
        if (m && oca_llm_model_available(llm, m)) {
            snprintf(chosen, sizeof(chosen), "%s", m->id);
            saved_level = options->last_used_reasoning;
        }
    }
    if (!chosen[0]) {
        /* No usable selection: start unconfigured. */
        c->route_model[0] = '\0';
        c->route_effort = OCA_REASONING_NONE;
        return 0;
    }
    const oca_model *chosen_model = oca_llm_model_resolve(llm, chosen);
    if (options->reasoning_name) {
        /* An explicit override resolves by the chosen model's own levels. */
        effort = chosen_model ?
            oca_model_reasoning_index(chosen_model, options->reasoning_name) :
            OCA_REASONING_NONE;
        if (effort == OCA_REASONING_NONE) {
            oca_model_reasoning_error(chosen_model, options->reasoning_name, err, errsz);
            return -1;
        }
    } else if (saved_level) {
        effort = oca_model_reasoning_index(chosen_model, saved_level);
        /* An invalid remembered level leaves the route incomplete; the user
         * chooses a level instead of receiving an invented one. */
    }
    int chosen_available = route_usable(c, chosen, effort);
    if (c->route_cbs.persist && chosen_available) {
        const char *level = oca_model_reasoning_name(chosen_model, effort);
        char perr[256];
        if (c->route_cbs.persist(c->route_cbs.user, chosen, level, perr, sizeof(perr)) != 0) {
            err_set(err, errsz, "cannot persist the route: %s", perr);
            return -1;
        }
    }
    int route_changed = options->resume_id &&
        (strcmp(c->route_model, chosen) != 0 || c->route_effort != effort);
    snprintf(c->route_model, sizeof(c->route_model), "%s", chosen);
    c->route_effort = effort;
    if (route_changed) c->dirty = 1;
    for (size_t i = 0; i < c->n_msgs; i++) {
        if (c->msgs[i].role == OCA_MSG_ASSISTANT && c->msgs[i].continuation &&
            (!c->msgs[i].model_id || strcmp(c->msgs[i].model_id, chosen) != 0)) {
            free(c->msgs[i].continuation);
            c->msgs[i].continuation = NULL;
        }
    }
    return 0;
}

static oca_core *core_allocate(oca_llm *llm, const oca_core_options *options,
                                char *err, size_t errsz) {
    if (!options) {
        err_set(err, errsz, "core options are required");
        return NULL;
    }
    if (options->executor && (options->executor->owner != getpid() ||
        !options->executor->ready || options->executor->borrowed || !(options->flags & OCA_CORE_RUNTIME))) {
        err_set(err, errsz, "the executor is not available for this core");
        return NULL;
    }
    if (!llm || !options->workspace || !*options->workspace) {
        err_set(err, errsz, "the LLM layer and workspace are required");
        return NULL;
    }
    if (options->flags != OCA_CORE_COMMAND && options->flags != OCA_CORE_RUNTIME &&
        options->flags != OCA_CORE_INTERACTIVE) {
        err_set(err, errsz, "invalid core flags");
        return NULL;
    }
    struct stat st;
    if (stat(options->workspace, &st) != 0 || !S_ISDIR(st.st_mode)) {
        err_set(err, errsz, "workspace is not a directory: %s", options->workspace);
        return NULL;
    }
    oca_core *c = calloc(1, sizeof(*c));
    if (!c) {
        err_set(err, errsz, "out of memory");
        return NULL;
    }
    atomic_init(&c->running, 0);
    c->llm = llm;
    c->flags = options->flags;
    c->interactive = options->flags == OCA_CORE_INTERACTIVE;
    c->workspace = xstrdup(options->workspace);
    if (!c->workspace) {
        err_set(err, errsz, "out of memory");
        free(c);
        return NULL;
    }
    c->cli_route_locked = options->model != NULL;
    c->comp_first_kept = -1;
    c->state = OCA_S_IDLE;
    if (options->route) c->route_cbs = *options->route;
    if (options->trace) c->trace = *options->trace;
    if (pthread_mutex_init(&c->mu, NULL) != 0) {
        goto failed_allocation;
    }
    if (pthread_cond_init(&c->cond, NULL) != 0) {
        goto failed_mutex;
    }
    if (pipe(c->wake_pipe) != 0) {
        goto failed_condition;
    }
    atomic_init(&c->cancel_latch, 0);
    c->executor = options->executor;
    if (options->executor) options->executor->borrowed = 1;
    if (fcntl(c->wake_pipe[0], F_SETFL, O_NONBLOCK) < 0 ||
        fcntl(c->wake_pipe[1], F_SETFL, O_NONBLOCK) < 0 ||
        fcntl(c->wake_pipe[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(c->wake_pipe[1], F_SETFD, FD_CLOEXEC) < 0) {
        err_set(err, errsz, "cannot initialize the core");
        goto failed;
    }
    return c;
failed:
    oca_core_destroy(c);
    return NULL;
failed_condition:
    pthread_cond_destroy(&c->cond);
failed_mutex:
    pthread_mutex_destroy(&c->mu);
failed_allocation:
    err_set(err, errsz, "cannot initialize the core");
    free(c->workspace);
    free(c);
    return NULL;
}

oca_core *oca_core_create(oca_llm *llm, const oca_core_options *options,
                          char *err, size_t errsz) {
    oca_core *c = core_allocate(llm, options, err, errsz);
    if (!c) return NULL;
    /* Skill discovery happens once during process startup. */
    {
        char *sdir = state_dir_alloc("skills");
        if (sdir) {
            char *resolved = realpath(sdir, NULL);
            catalog_discover(resolved ? resolved : sdir, &c->catalog);
            free(resolved);
            free(sdir);
        }
    }
    if (!(options->flags & OCA_CORE_RUNTIME)) {
        return c;
    }
    /* Session creation or resume. */
    if (options->resume_id) {
        char *selected_workspace = xstrdup(c->workspace);
        if (!selected_workspace) {
            err_set(err, errsz, "out of memory");
            goto failed;
        }
        if (load_session(c, options->resume_id, err, errsz) != 0) {
            free(selected_workspace);
            goto failed;
        }
        int workspace_changed = strcmp(c->workspace, selected_workspace) != 0;
        free(c->workspace);
        c->workspace = selected_workspace;
        if (workspace_changed) c->dirty = 1;
    } else if (create_session(c, err, errsz) != 0) {
        goto failed;
    }
    if (select_initial_route(c, options, err, errsz) != 0) goto failed;
    /* Enqueue the route event and skill warnings for the consumer. */
    {
        oca_core_event ev = {0};
        ev.kind = OCA_CE_ROUTE;
        snprintf(ev.model_id, sizeof(ev.model_id), "%s", c->route_model);
        snprintf(ev.reasoning_name, sizeof(ev.reasoning_name), "%s",
                 route_reasoning_name(c));
        ev.route_available = route_available(c);
        ev_push(c, &ev);
    }
    for (size_t i = 0; i < c->catalog.n_warns; i++) {
        oca_core_event ev = {0};
        ev.kind = OCA_CE_WARN;
        ev.text = xstrdup(c->catalog.warns[i]);
        ev_push(c, &ev);
    }
    trace_line(c, "core", "{\"event\":\"start\"}");
    if (c->trace_failed) {
        err_set(err, errsz, "cannot write the diagnostic file");
        goto failed;
    }
    publish_side_snapshot(c);
    /* Start the worker. */
    c->running = 1;
    if (pthread_create(&c->worker, NULL, worker_main, c) != 0) {
        c->running = 0;
        c->worker_started = 0;
        err_set(err, errsz, "cannot start the worker thread");
        goto failed;
    }
    c->worker_started = 1;
    return c;
failed:
    oca_core_destroy(c);
    return NULL;
}

oca_core *oca_core_create_ephemeral(oca_core *parent, char *err, size_t errsz) {
    if (!parent || !parent->interactive || parent->ephemeral) {
        err_set(err, errsz, "an interactive main conversation is required");
        return NULL;
    }
    pthread_mutex_lock(&parent->mu);
    if (!parent->running || parent->side_in_use || !parent->side_snapshot) {
        pthread_mutex_unlock(&parent->mu);
        err_set(err, errsz, "the side chat exists or its context is unavailable");
        return NULL;
    }
    size_t length = parent->side_snapshot_len;
    char *snapshot = xstrndup(parent->side_snapshot, length);
    int hints_enabled = parent->hints.enabled;
    parent->side_in_use = 1;
    pthread_mutex_unlock(&parent->mu);
    session_data data;
    session_data_init(&data);
    oca_core *side = NULL;
    if (!snapshot) {
        err_set(err, errsz, "cannot copy the main context");
        goto failed;
    }
    int parsed = session_deserialize(snapshot, length, &data, err, errsz);
    free(snapshot);
    if (parsed) goto failed;
    side = core_allocate(parent->llm, &(oca_core_options){
        .workspace = data.workspace,
        .flags = OCA_CORE_COMMAND,
    }, err, errsz);
    if (!side) goto failed;
    side->ephemeral = 1;
    side->hints = (hints_state){.enabled = hints_enabled, .applied = HINTS_UNKNOWN};
    side->flags = OCA_CORE_RUNTIME;
    side->side_prefix_count = data.n_msgs;
    side->msgs = data.msgs;
    side->n_msgs = side->cap_msgs = data.n_msgs;
    data.msgs = NULL;
    data.n_msgs = 0;
    side->agents = data.agents;
    data.agents = NULL;
    side->summary = data.summary;
    data.summary = NULL;
    side->comp_first_kept = data.first_kept;
    side->tokens_before = data.tokens_before;
    side->tokens_after = data.tokens_after;
    side->snapshot_goal = data.goal;
    data.goal = NULL;
    memcpy(side->sid, data.id, sizeof(side->sid));
    side->sid[31] = side->sid[31] == '0' ? '1' : '0';
    snprintf(side->route_model, sizeof(side->route_model), "%s", data.model);
    {
        const oca_model *m = oca_llm_model_resolve(side->llm, data.model);
        side->route_effort = m ? oca_model_reasoning_index(m, data.effort) : OCA_REASONING_NONE;
    }
    session_data_free(&data);
    side->running = 1;
    if (pthread_create(&side->worker, NULL, worker_main, side)) {
        side->running = 0;
        err_set(err, errsz, "cannot start the side-chat worker");
        goto failed;
    }
    side->worker_started = 1;
    side->parent = parent;
    return side;
failed:
    session_data_free(&data);
    if (side) oca_core_destroy(side);
    pthread_mutex_lock(&parent->mu);
    parent->side_in_use = 0;
    pthread_mutex_unlock(&parent->mu);
    return NULL;
}

void oca_core_stop(oca_core *c) {
    if (!c) return;
    atomic_store(&c->cancel_latch, 1);
    pthread_mutex_lock(&c->mu);
    c->running = 0;
    pthread_cond_broadcast(&c->cond);
    pthread_mutex_unlock(&c->mu);
}

void oca_core_destroy(oca_core *c) {
    if (!c) return;
    oca_core_stop(c);
    if (c->worker_started) {
        pthread_join(c->worker, NULL);
    }
    if (c->executor) c->executor->borrowed = 0;
    /* Release queued events and actions. */
    while (c->eq_head != c->eq_tail) {
        oca_core_event ev = c->eq[c->eq_head];
        c->eq_head = (c->eq_head + 1) % EVENT_RING_SIZE;
        oca_core_event_release(&ev);
    }
    while (c->aq_head != c->aq_tail) {
        action_rec a = c->aq[c->aq_head];
        c->aq_head = (c->aq_head + 1) % ACTION_RING_SIZE;
        free(a.text);
    }
    msg_free_all(c->msgs, c->n_msgs);
    free(c->title);
    free(c->agents);
    free(c->goal_text);
    free(c->summary);
    free(c->workspace);
    free(c->side_snapshot);
    free(c->snapshot_goal);
    if (c->parent) {
        pthread_mutex_lock(&c->parent->mu);
        c->parent->side_in_use = 0;
        pthread_mutex_unlock(&c->parent->mu);
    }
    free(c->goal_first_turn);
    free(c->ia_answers_json);
    for (size_t i = 0; i < c->n_followups; i++) free(c->followups[i]);
    free(c->followups);
    free(c->pending_turn_prompt);
    catalog_free(&c->catalog);
    close(c->wake_pipe[0]);
    close(c->wake_pipe[1]);
    pthread_mutex_destroy(&c->mu);
    pthread_cond_destroy(&c->cond);
    free(c);
}
