/* OCA LLM boundary implementation. */
#include "oca_llm.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <poll.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

/* ==========================================================================
 * Internal helpers
 * ========================================================================== */

static void err_set(char *err, size_t errsz, const char *fmt, ...) {
    va_list ap;
    if (!err || errsz == 0) return;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}

/* Creates every missing parent of one state path with mode 0700. A created
 * directory is restored to 0700 after the umask. Existing modes stay
 * unchanged. */
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

/* Atomically replaces one state file with mode 0600 content. */
static int write_file_atomic_0600(const char *path, const char *data, size_t len,
                                  char *err, size_t errsz) {
    char tmp[4300];
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

/* ============================================================================
 * Bounded buffers and provider JSON
 * ============================================================================ */

#define LLM_BODY_MAX (8u * 1024u * 1024u)
#define JSON_DEPTH_MAX 64

typedef struct {
    char *data;
    size_t len, cap;
} byte_buffer;

static void buffer_free(byte_buffer *b) {
    free(b->data);
    memset(b, 0, sizeof(*b));
}

static int buffer_append(byte_buffer *b, const char *data, size_t len, size_t limit) {
    if (b->len > limit || len > limit - b->len) return -1;
    size_t need = b->len + len + 1;
    if (need > b->cap) {
        size_t cap = b->cap ? b->cap : 128;
        while (cap < need) cap *= 2;
        char *p = realloc(b->data, cap);
        if (!p) return -1;
        b->data = p;
        b->cap = cap;
    }
    if (len) memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = 0;
    return 0;
}

static int utf8_text_valid(const char *text, size_t len) {
    const unsigned char *s = (const unsigned char *)text;
    for (size_t i = 0; i < len;) {
        unsigned char c = s[i++];
        if (c < 0x80) {
            if (!c) return 0;
            continue;
        }
        size_t n;
        if (c >= 0xc2 && c <= 0xdf) n = 1;
        else if (c >= 0xe0 && c <= 0xef) n = 2;
        else if (c >= 0xf0 && c <= 0xf4) n = 3;
        else return 0;
        if (n > len - i) return 0;
        if ((c == 0xe0 && s[i] < 0xa0) || (c == 0xed && s[i] >= 0xa0) ||
            (c == 0xf0 && s[i] < 0x90) || (c == 0xf4 && s[i] >= 0x90)) return 0;
        while (n--) if ((s[i++] & 0xc0) != 0x80) return 0;
    }
    return 1;
}

static int json_tree_valid(const cJSON *node, size_t *count) {
    if (++*count > 32768) return 0;
    if (cJSON_IsNumber(node) && !isfinite(node->valuedouble)) return 0;
    for (const cJSON *child = node->child; child; child = child->next) {
        if (cJSON_IsObject(node)) {
            for (const cJSON *other = node->child; other != child; other = other->next)
                if (!strcmp(other->string, child->string)) return 0;
        }
        if (!json_tree_valid(child, count)) return 0;
    }
    return 1;
}

/* Bound nesting before the parser allocates. Reject NUL escapes because
 * normalized strings are NUL-terminated, including decoded provider strings. */
static cJSON *json_parse(const char *text, size_t len) {
    if (!text || len > LLM_BODY_MAX || !utf8_text_valid(text, len)) return NULL;
    int quoted = 0, depth = 0;
    for (size_t i = 0; i < len; i++) {
        if (quoted && text[i] == '\\') {
            if (++i >= len) return NULL;
            if (text[i] == 'u' && len - i >= 5 && !memcmp(text + i, "u0000", 5))
                return NULL;
        } else if (text[i] == '"') quoted = !quoted;
        else if (!quoted && (text[i] == '{' || text[i] == '[')) {
            if (++depth > JSON_DEPTH_MAX) return NULL;
        } else if (!quoted && (text[i] == '}' || text[i] == ']')) {
            if (--depth < 0) return NULL;
        } else if (!quoted && (text[i] == '-' || (text[i] >= '0' && text[i] <= '9'))) {
            size_t at = i;
            if (text[at] == '-') at++;
            if (at >= len) return NULL;
            if (text[at] == '0') at++;
            else {
                if (text[at] < '1' || text[at] > '9') return NULL;
                while (at < len && text[at] >= '0' && text[at] <= '9') at++;
            }
            if (at < len && text[at] == '.') {
                size_t digits = ++at;
                while (at < len && text[at] >= '0' && text[at] <= '9') at++;
                if (at == digits) return NULL;
            }
            if (at < len && (text[at] == 'e' || text[at] == 'E')) {
                at++;
                if (at < len && (text[at] == '+' || text[at] == '-')) at++;
                size_t digits = at;
                while (at < len && text[at] >= '0' && text[at] <= '9') at++;
                if (at == digits) return NULL;
            }
            if (at < len && !strchr(" \t\r\n,]}", text[at])) return NULL;
            i = at - 1;
        }
    }
    if (quoted || depth) return NULL;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(text, len + 1, &end, 1);
    size_t count = 0;
    if (!root || end != text + len || !json_tree_valid(root, &count)) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

/* The result borrows the object's string, or is NULL for a missing or non-string field. */
static const char *json_text(const cJSON *object, const char *key) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
}

static int json_count(const cJSON *node, long *out) {
    if (!cJSON_IsNumber(node) || !isfinite(node->valuedouble) ||
        node->valuedouble < 0 || node->valuedouble >= (double)LONG_MAX ||
        floor(node->valuedouble) != node->valuedouble) return -1;
    *out = (long)node->valuedouble;
    return 0;
}

/* Diagnostic values select protocol metadata, never message or argument text. */
static void diagnostic_token(char out[OCA_LLM_DIAGNOSTIC_TEXT_MAX], const char *text) {
    if (!text) {
        snprintf(out, OCA_LLM_DIAGNOSTIC_TEXT_MAX, "<missing>");
        return;
    }
    size_t length = strlen(text);
    for (size_t i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-')) {
            snprintf(out, OCA_LLM_DIAGNOSTIC_TEXT_MAX, "<invalid-metadata>");
            return;
        }
    }
    snprintf(out, OCA_LLM_DIAGNOSTIC_TEXT_MAX, "%s", text);
    if (length >= OCA_LLM_DIAGNOSTIC_TEXT_MAX) out[OCA_LLM_DIAGNOSTIC_TEXT_MAX - 2] = '~';
}

static int diagnostic_set(oca_llm_diagnostic *d, const char *check, const char *field,
                          const char *expected, const char *received) {
    if (!*d->check) {
        snprintf(d->check, sizeof(d->check), "%s", check);
        snprintf(d->field, sizeof(d->field), "%s", field);
        snprintf(d->expected, sizeof(d->expected), "%s", expected);
        snprintf(d->received, sizeof(d->received), "%s", received);
    }
    return -1;
}

static int diagnostic_field(oca_llm_diagnostic *d, const char *check, const char *field,
                            const char *expected, const cJSON *value, int metadata) {
    char received[OCA_LLM_DIAGNOSTIC_TEXT_MAX];
    if (metadata && cJSON_IsString(value)) diagnostic_token(received, value->valuestring);
    else if (cJSON_IsNumber(value)) snprintf(received, sizeof(received), "%.17g", value->valuedouble);
    else {
        const char *type = "string";
        if (!value) type = "<missing>";
        else if (cJSON_IsNull(value)) type = "null";
        else if (cJSON_IsBool(value)) type = cJSON_IsTrue(value) ? "true" : "false";
        else if (cJSON_IsArray(value)) type = "array";
        else if (cJSON_IsObject(value)) type = "object";
        snprintf(received, sizeof(received), "%s", type);
    }
    return diagnostic_set(d, check, field, expected, received);
}

static int diagnostic_limit(oca_llm_diagnostic *d, const char *field, size_t actual, size_t limit) {
    if (!*d->check) {
        char expected[64], received[64];
        snprintf(expected, sizeof(expected), "at most %zu bytes", limit);
        snprintf(received, sizeof(received), "%zu bytes", actual);
        d->has_limit = 1;
        d->actual = actual;
        d->limit = limit;
        diagnostic_set(d, "size_limit", field, expected, received);
    }
    return -1;
}

static int diagnostic_append(oca_llm_diagnostic *d, byte_buffer *buffer,
                             const char *data, size_t length, size_t limit, const char *field) {
    if (buffer->len > limit || length > limit - buffer->len)
        return diagnostic_limit(d, field, length > SIZE_MAX - buffer->len ? SIZE_MAX : buffer->len + length, limit);
    if (buffer_append(buffer, data, length, limit))
        return diagnostic_set(d, "allocation", field, "allocated buffer", "allocation failed");
    return 0;
}

/* Takes item ownership on every path. Returns the attached item or NULL. */
static cJSON *json_add(cJSON *parent, const char *key, cJSON *item, int *ok) {
    if (!*ok || !parent || !item ||
        !(key ? cJSON_AddItemToObject(parent, key, item) : cJSON_AddItemToArray(parent, item))) {
        *ok = 0;
        cJSON_Delete(item);
        return NULL;
    }
    return item;
}

/* ==========================================================================
 * Model catalog
 * ========================================================================== */

/* Fixed provider registry. It is independent of the configured model count. */
typedef struct {
    const char *id;
    oca_credential_kind credential_kind;
    int tools;
    int reasoning_stream;
    int usage;
    int cache_metrics;
} provider_record;

static const provider_record PROVIDERS[] = {
    {"kimi", OCA_CREDENTIAL_API_KEY, 1, 1, 1, 1},
    {"openai", OCA_CREDENTIAL_OAUTH, 1, 1, 1, 1},
    {"deepseek", OCA_CREDENTIAL_API_KEY, 1, 1, 1, 1},
};
#define PROVIDER_COUNT (sizeof(PROVIDERS) / sizeof(PROVIDERS[0]))

static const char *const GPT_LEVELS[] = {"low", "medium", "high", "xhigh", "max"};
static const char *const DEEPSEEK_LEVELS[] = {"off", "low", "high", "max"};
static const char *const KIMI_LEVELS[] = {"low", "high", "max"};

static const oca_model_definition BUILTIN_MODELS[] = {
    /* Codex OAuth profile: 872000 context, 95% usable input. Ordinary efforts
     * exclude Codex's multi-agent ultra mode. Audited 2026-10-01. */
    {"gpt-6-astra", "gpt-6-astra", "openai", 872000, GPT_LEVELS, 5, 828400},
    {"gpt-6.1-sol", "gpt-6.1-sol", "openai", 872000, GPT_LEVELS, 5, 828400},
    {"gpt-6-sol", "gpt-6-sol", "openai", 872000, GPT_LEVELS, 5, 828400},
    {"gpt-6-luna", "gpt-6-luna", "openai", 872000, GPT_LEVELS, 5, 828400},
    {"gpt-5.6-sol", "gpt-5.6-sol", "openai", 872000, GPT_LEVELS, 5, 828400},
    {"gpt-5.6-terra", "gpt-5.6-terra", "openai", 872000, GPT_LEVELS, 5, 828400},
    {"gpt-5.6-luna", "gpt-5.6-luna", "openai", 872000, GPT_LEVELS, 5, 828400},
    /* Reserve the largest default output (131072), not the API maximum. */
    {"deepseek-flash", "deepseek-flash", "deepseek", 1048576, DEEPSEEK_LEVELS, 4, 917504},
    /* Kimi keeps OCA's local 16384 reserve; k3 uses the Plus/Moderato cap. */
    {"k3", "k3", "kimi", 262144, KIMI_LEVELS, 3, 245760},
    {"k3-256k", "k3-256k", "kimi", 262144, KIMI_LEVELS, 3, 245760},
    {"kimi-for-coding", "kimi-for-coding", "kimi", 1048576, KIMI_LEVELS, 3, 1032192},
};

const oca_model_definition *oca_llm_builtin_models(size_t *count) {
    if (count) *count = sizeof(BUILTIN_MODELS) / sizeof(BUILTIN_MODELS[0]);
    return BUILTIN_MODELS;
}

static const provider_record *provider_lookup(const char *id) {
    if (!id) return NULL;
    for (size_t i = 0; i < PROVIDER_COUNT; i++)
        if (!strcmp(PROVIDERS[i].id, id)) return &PROVIDERS[i];
    return NULL;
}

const char *oca_model_reasoning_name(const oca_model *m, oca_reasoning effort) {
    if (!m || effort < 0 || (size_t)effort >= m->reasoning_count) return NULL;
    return m->reasoning_levels[effort];
}

oca_reasoning oca_model_reasoning_index(const oca_model *m, const char *name) {
    if (!m || !name) return OCA_REASONING_NONE;
    for (size_t i = 0; i < m->reasoning_count; i++)
        if (!strcmp(m->reasoning_levels[i], name)) return (oca_reasoning)i;
    return OCA_REASONING_NONE;
}

int oca_model_supports_reasoning(const oca_model *m, oca_reasoning effort) {
    return oca_model_reasoning_name(m, effort) != NULL;
}

void oca_model_reasoning_error(const oca_model *model, const char *name,
                               char *error, size_t size) {
    if (!size) return;
    snprintf(error, size, "model %s does not support reasoning level %s\nAvailable levels: ",
             model ? model->id : "(not selected)", name ? name : "(missing)");
    size_t used = strlen(error);
    if (model) for (size_t i = 0; i < model->reasoning_count && used < size - 1; i++) {
        snprintf(error + used, size - used, "%s%s", i ? ", " : "", model->reasoning_levels[i]);
        used = strlen(error);
    }
}

oca_reasoning oca_model_min_reasoning(const oca_model *m) {
    return m && m->reasoning_count > 0 ? 0 : OCA_REASONING_NONE;
}

/* A configured level name is one bounded printable token. */
static int reasoning_name_valid(const char *name) {
    if (!name || !*name || strlen(name) > OCA_REASONING_NAME_MAX) return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (*p <= 0x20 || *p >= 0x7f) return 0;
    return 1;
}

static void model_free(oca_model *model) {
    free((char *)model->id);
    free((char *)model->name);
    free((char *)model->provider);
    for (size_t i = 0; i < model->reasoning_count; i++)
        free((char *)model->reasoning_levels[i]);
    free((char *)model->reasoning_levels);
    memset(model, 0, sizeof(*model));
}

/* Copies one definition into the owned catalog. Returns zero on success. */
static int model_copy(oca_model *model, const oca_model_definition *def) {
    memset(model, 0, sizeof(*model));
    const provider_record *provider = provider_lookup(def->provider);
    if (!provider || !def->name || !*def->name ||
        strlen(def->name) > (!strcmp(def->name, def->id ? def->id : "") ?
                             OCA_LLM_MODEL_ID_MAX : OCA_LLM_MODEL_NAME_MAX) ||
        !def->id || !*def->id || strlen(def->id) > OCA_LLM_MODEL_ID_MAX ||
        def->context_limit <= 0 || def->context_limit > OCA_LLM_CONTEXT_LIMIT_MAX ||
        def->input_limit < 0 || def->input_limit > def->context_limit ||
        def->reasoning_count > OCA_LLM_REASONING_LEVELS_MAX ||
        (def->reasoning_count && !def->reasoning_levels)) return -1;
    model->id = strdup(def->id);
    model->name = strdup(def->name);
    model->provider = strdup(def->provider);
    model->reasoning_levels = calloc(def->reasoning_count ? def->reasoning_count : 1,
                                     sizeof(*model->reasoning_levels));
    if (!model->id || !model->name || !model->provider || !model->reasoning_levels) goto failed;
    /* model_free frees every partially copied level through this count. */
    model->reasoning_count = def->reasoning_count;
    for (size_t i = 0; i < def->reasoning_count; i++) {
        if (!reasoning_name_valid(def->reasoning_levels[i])) goto failed;
        for (size_t k = 0; k < i; k++)
            if (!strcmp(def->reasoning_levels[k], def->reasoning_levels[i])) goto failed;
        model->reasoning_levels[i] = strdup(def->reasoning_levels[i]);
        if (!model->reasoning_levels[i]) goto failed;
    }
    model->context_limit = def->context_limit;
    long reserve = def->context_limit / 4;
    if (reserve > 16384) reserve = 16384;
    model->input_limit = def->input_limit ? def->input_limit : def->context_limit - reserve;
    model->tools = provider->tools;
    model->reasoning_stream = provider->reasoning_stream;
    model->usage = provider->usage;
    model->cache_metrics = provider->cache_metrics;
    model->credential_kind = provider->credential_kind;
    return 0;
failed:
    model_free(model);
    return -1;
}

static void secret_free(char *text) {
    if (!text) return;
    volatile unsigned char *p = (volatile unsigned char *)text;
    size_t length = strlen(text);
    while (length--) *p++ = 0;
    free(text);
}

/* ==========================================================================
 * Credentials
 * ========================================================================== */

/* Serialize credential refresh and header snapshots, not response streams. */
static pthread_mutex_t oauth_refresh_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    char *provider;
    char *api_key;
    char *access_token;
    char *refresh_token;
    char *account_id;
    int64_t expires_at;
} auth_member;

struct oca_llm {
    cJSON *unsupported_providers;
    char auth_path[4096];
    oca_model *models;
    size_t model_count;
    atomic_int *available;
    char *kimi_env_key;
    char *deepseek_env_key;
    auth_member *members;
    size_t member_count, member_cap;
};

static int provider_known(const char *id) {
    return provider_lookup(id) != NULL;
}

static auth_member *find_member(const oca_llm *l, const char *provider) {
    for (size_t i = 0; i < l->member_count; i++) {
        if (strcmp(l->members[i].provider, provider) == 0) return &l->members[i];
    }
    return NULL;
}

static int token_valid(const char *text, size_t limit) {
    if (!text || !*text || strlen(text) > limit) return 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if (*p <= 0x20 || *p >= 0x7f) return 0;
    return 1;
}

static const char *provider_api_key(const oca_llm *llm, const char *provider) {
    const char *fallback;
    if (!strcmp(provider, "kimi")) fallback = llm->kimi_env_key;
    else if (!strcmp(provider, "deepseek")) fallback = llm->deepseek_env_key;
    else return NULL;
    const auth_member *member = find_member(llm, provider);
    return member ? member->api_key : fallback;
}


static int oauth_valid(const auth_member *member) {
    return member && !member->api_key && token_valid(member->access_token, 16384) &&
           token_valid(member->refresh_token, 16384) && token_valid(member->account_id, 256) &&
           member->expires_at > 0;
}

/* The TUI reads availability while the worker can replace OAuth strings. */
static void refresh_availability(oca_llm *llm) {
    for (size_t i = 0; i < llm->model_count; i++) {
        const oca_model *model = &llm->models[i];
        int available = 1;
        if (model->credential_kind == OCA_CREDENTIAL_OAUTH)
            available = oauth_valid(find_member(llm, model->provider));
        else if (model->credential_kind != OCA_CREDENTIAL_NONE)
            available = token_valid(provider_api_key(llm, model->provider), 4096);
        atomic_store(&llm->available[i], available);
    }
}

static void member_free(auth_member *member) {
    free(member->provider);
    secret_free(member->api_key);
    secret_free(member->access_token);
    secret_free(member->refresh_token);
    free(member->account_id);
    memset(member, 0, sizeof(*member));
}

static int member_reserve(oca_llm *llm) {
    if (llm->member_count < llm->member_cap) return 0;
    size_t capacity = llm->member_cap ? llm->member_cap * 2 : 4;
    auth_member *members = realloc(llm->members, capacity * sizeof(*members));
    if (!members) return -1;
    llm->members = members;
    llm->member_cap = capacity;
    return 0;
}

static int read_auth_members(oca_llm *llm, const cJSON *providers,
                             char *err, size_t errsz) {
    int valid = cJSON_IsObject(providers);
    const cJSON *item;
    cJSON_ArrayForEach(item, providers) {
        if (!valid || !cJSON_IsObject(item) || !provider_known(item->string)) {
            valid = 0;
            break;
        }
        const char *kind = oca_provider_credential_kind(item->string);
        const char *method = json_text(item, "auth_method");
        auth_member member = {0};
        if (!method || strcmp(kind, method) || !strcmp(kind, "none")) {
            valid = 0;
            break;
        }
        member.provider = strdup(item->string);
        if (!strcmp(kind, "api-key")) {
            const char *key = json_text(item, "api_key");
            valid = cJSON_GetArraySize(item) == 2 && token_valid(key, 4096);
            if (valid) member.api_key = strdup(key);
            valid = valid && member.api_key;
        } else {
            const char *access = json_text(item, "access_token");
            const char *refresh = json_text(item, "refresh_token");
            const char *account = json_text(item, "account_id");
            const cJSON *expiry = cJSON_GetObjectItemCaseSensitive(item, "expires_at");
            valid = cJSON_GetArraySize(item) == 5 && token_valid(access, 16384) &&
                    token_valid(refresh, 16384) && token_valid(account, 256) && cJSON_IsNumber(expiry) &&
                    isfinite(expiry->valuedouble) && expiry->valuedouble > 0 &&
                    expiry->valuedouble <= 253402300799.0 && floor(expiry->valuedouble) == expiry->valuedouble;
            if (valid) {
                member.access_token = strdup(access);
                member.refresh_token = strdup(refresh);
                member.account_id = strdup(account);
                member.expires_at = (int64_t)expiry->valuedouble;
                valid = oauth_valid(&member);
            }
        }
        if (!valid || !member.provider || member_reserve(llm)) {
            member_free(&member);
            valid = 0;
            break;
        }
        llm->members[llm->member_count++] = member;
    }
    if (!valid) err_set(err, errsz, "auth file rejected: invalid provider credentials");
    return valid ? 0 : -1;
}

/* The complete document is validated before this scanner copies opaque values. */
static const char *auth_value_end(const char *p) {
    int depth = 0, quoted = 0;
    for (; *p; p++) {
        if (quoted) {
            if (*p == '\\' && p[1]) {
                p++;
                continue;
            }
            if (*p == '"') {
                quoted = 0;
                if (!depth) return p + 1;
            }
        } else if (*p == '"') {
            quoted = 1;
        } else if (*p == '{' || *p == '[') {
            depth++;
        } else if (*p == '}' || *p == ']') {
            if (!depth) return p;
            if (!--depth) return p + 1;
        } else if (!depth && *p == ',') {
            return p;
        }
    }
    return p;
}

static const char *auth_space_end(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

/* The LLM owns inactive JSON records until destruction. Preserve their raw
 * values because unknown schemas can contain numbers beyond double precision. */
static int read_auth_document(oca_llm *llm, const char *data, size_t length,
                                 char *err, size_t errsz) {
    cJSON *root = json_parse(data, length);
    cJSON *providers = cJSON_GetObjectItemCaseSensitive(root, "providers");
    if (!cJSON_IsObject(root) || cJSON_GetArraySize(root) != 1 ||
        !cJSON_IsObject(providers)) {
        cJSON_Delete(root);
        err_set(err, errsz, "auth file rejected: invalid structure");
        return -1;
    }
    cJSON *item;
    llm->unsupported_providers = cJSON_CreateObject();
    if (!llm->unsupported_providers) {
        cJSON_Delete(root);
        err_set(err, errsz, "out of memory");
        return -1;
    }
    const char *source = auth_space_end(data);
    if (*source != '{') goto invalid;
    source = auth_space_end(source + 1);
    if (*source != '"') goto invalid;
    source = auth_space_end(auth_value_end(source));
    if (*source != ':') goto invalid;
    source = auth_space_end(source + 1);
    if (*source != '{') goto invalid;
    source++;
    item = providers->child;
    while (item) {
        cJSON *next = item->next;
        source = auth_space_end(source);
        if (*source != '"') goto invalid;
        source = auth_space_end(auth_value_end(source));
        if (*source != ':') goto invalid;
        const char *value = auth_space_end(source + 1);
        source = auth_value_end(value);
        if (!provider_known(item->string)) {
            size_t size = (size_t)(source - value);
            char *raw = malloc(size + 1);
            if (raw) {
                memcpy(raw, value, size);
                raw[size] = 0;
            }
            cJSON *copy = raw ? cJSON_CreateRaw(raw) : NULL;
            free(raw);
            if (!copy || !cJSON_AddItemToObject(llm->unsupported_providers, item->string, copy)) {
                cJSON_Delete(copy);
                cJSON_Delete(root);
                err_set(err, errsz, "out of memory");
                return -1;
            }
            cJSON_Delete(cJSON_DetachItemViaPointer(providers, item));
        }
        source = auth_space_end(source);
        if (*source == ',') source++;
        item = next;
    }
    int rc = read_auth_members(llm, providers, err, errsz);
    cJSON_Delete(root);
    return rc;
invalid:
    cJSON_Delete(root);
    err_set(err, errsz, "auth file rejected: invalid structure");
    return -1;
}

size_t oca_llm_unsupported_provider_count(const oca_llm *llm) {
    return (size_t)cJSON_GetArraySize(llm->unsupported_providers);
}

static int write_auth_file(oca_llm *llm, char *err, size_t errsz) {
    cJSON *root = cJSON_CreateObject();
    cJSON *providers = cJSON_AddObjectToObject(root, "providers");
    int valid = root && providers;
    for (size_t i = 0; valid && i < llm->member_count; i++) {
        const auth_member *member = &llm->members[i];
        cJSON *item = cJSON_AddObjectToObject(providers, member->provider);
        valid = item && cJSON_AddStringToObject(item, "auth_method", member->api_key ? "api-key" : "oauth");
        if (valid && member->api_key) valid = cJSON_AddStringToObject(item, "api_key", member->api_key) != NULL;
        else if (valid) {
            valid = cJSON_AddStringToObject(item, "access_token", member->access_token) &&
                    cJSON_AddStringToObject(item, "refresh_token", member->refresh_token) &&
                    cJSON_AddStringToObject(item, "account_id", member->account_id) &&
                    cJSON_AddNumberToObject(item, "expires_at", (double)member->expires_at);
        }
    }
    const cJSON *inactive;
    cJSON_ArrayForEach(inactive, llm->unsupported_providers) {
        if (!valid) break;
        cJSON *copy = cJSON_Duplicate(inactive, 1);
        if (!copy || !cJSON_AddItemToObject(providers, inactive->string, copy)) {
            cJSON_Delete(copy);
            valid = 0;
        }
    }
    char *text = valid ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (!text) {
        err_set(err, errsz, "cannot serialize credentials");
        return -1;
    }
    size_t length = strlen(text);
    int rc = -1;
    if (length >= (1u << 20))
        err_set(err, errsz, "auth file is too large");
    else
        rc = write_file_atomic_0600(llm->auth_path, text, length, err, errsz);
    secret_free(text);
    return rc;
}

/* On success, transfers all replacement strings into the store. A failed
 * write preserves both the old memory record and the caller's ownership. */
static int store_member(oca_llm *llm, auth_member *replacement, char *err, size_t errsz) {
    auth_member *target = find_member(llm, replacement->provider);
    int added = target == NULL;
    if (added) {
        if (member_reserve(llm)) {
            err_set(err, errsz, "out of memory");
            return -1;
        }
        target = &llm->members[llm->member_count++];
        memset(target, 0, sizeof(*target));
    }
    auth_member old = *target;
    *target = *replacement;
    if (write_auth_file(llm, err, errsz)) {
        *target = old;
        if (added) llm->member_count--;
        return -1;
    }
    member_free(&old);
    memset(replacement, 0, sizeof(*replacement));
    refresh_availability(llm);
    return 0;
}

static int read_auth_file(oca_llm *llm, char *err, size_t errsz) {
    int fd = open(llm->auth_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return 0;
        err_set(err, errsz, "cannot read %s: %s", llm->auth_path, strerror(errno));
        return -1;
    }
    int rc = -1;
    char *data = NULL;
    size_t total = 0;
    struct stat state;
    if (fstat(fd, &state) != 0 || !S_ISREG(state.st_mode) || state.st_size >= (1 << 20)) {
        err_set(err, errsz, "auth file is not a bounded regular file");
        goto cleanup;
    }
    data = malloc(1 << 20);
    if (!data) {
        err_set(err, errsz, "out of memory");
        goto cleanup;
    }
    while (total < (1 << 20) - 1) {
        ssize_t got = read(fd, data + total, (1 << 20) - 1 - total);
        if (got < 0) {
            if (errno == EINTR) continue;
            err_set(err, errsz, "cannot read %s: %s", llm->auth_path, strerror(errno));
            goto cleanup;
        }
        if (!got) break;
        total += (size_t)got;
    }
    data[total] = 0;
    rc = read_auth_document(llm, data, total, err, errsz);
cleanup:
    close(fd);
    if (data) {
        data[total] = 0;
        secret_free(data);
    }
    return rc;
}

oca_llm *oca_llm_create(const char *auth_path, const oca_model_definition *definitions,
                        size_t definition_count, char *err, size_t errsz) {
    if (definition_count > OCA_LLM_MODELS_MAX || (definition_count && !definitions)) {
        err_set(err, errsz, "invalid model catalog");
        return NULL;
    }
    oca_llm *llm = calloc(1, sizeof(*llm));
    if (!llm) {
        err_set(err, errsz, "out of memory");
        return NULL;
    }
    if (definition_count) {
        llm->models = calloc(definition_count, sizeof(*llm->models));
        llm->available = calloc(definition_count, sizeof(*llm->available));
        if (!llm->models || !llm->available) {
            err_set(err, errsz, "out of memory");
            goto failed;
        }
        /* Destruction owns every record copied so far, including a partial one. */
        llm->model_count = definition_count;
    }
    for (size_t i = 0; i < definition_count; i++) {
        if (model_copy(&llm->models[i], &definitions[i]) != 0) {
            err_set(err, errsz, "invalid model definition: %s",
                    definitions[i].name ? definitions[i].name : "?");
            goto failed;
        }
        atomic_init(&llm->available[i], 0);
    }
    /* Remote identifiers and local names must resolve to one record. */
    for (size_t i = 0; i < llm->model_count; i++) {
        for (size_t k = i + 1; k < llm->model_count; k++) {
            if (!strcmp(llm->models[i].name, llm->models[k].name) ||
                !strcmp(llm->models[i].id, llm->models[k].id) ||
                !strcmp(llm->models[i].name, llm->models[k].id) ||
                !strcmp(llm->models[i].id, llm->models[k].name)) {
                err_set(err, errsz, "ambiguous model identity: %s", llm->models[i].name);
                goto failed;
            }
        }
    }
    const char *home = getenv("HOME");
    int path_len;
    if (auth_path) {
        path_len = snprintf(llm->auth_path, sizeof(llm->auth_path), "%s", auth_path);
    } else if (home && *home) {
        path_len = snprintf(llm->auth_path, sizeof(llm->auth_path), "%s/.oca/auth.json", home);
    } else {
        err_set(err, errsz, "HOME is not set and no auth path was given");
        goto failed;
    }
    if (path_len < 0 || (size_t)path_len >= sizeof(llm->auth_path)) {
        err_set(err, errsz, "auth path is too long");
        goto failed;
    }
    const struct { const char *name; char **target; } fallbacks[] = {
        {"KIMI_API_KEY", &llm->kimi_env_key},
        {"DEEPSEEK_API_KEY", &llm->deepseek_env_key},
    };
    for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); i++) {
        const char *key = getenv(fallbacks[i].name);
        if (!key || !*key) continue;
        if (!token_valid(key, 4096)) {
            err_set(err, errsz, "%s is not a valid API key", fallbacks[i].name);
            goto failed;
        }
        *fallbacks[i].target = strdup(key);
        if (!*fallbacks[i].target) {
            err_set(err, errsz, "out of memory");
            goto failed;
        }
    }
    if (read_auth_file(llm, err, errsz) != 0) goto failed;
    refresh_availability(llm);
    return llm;
failed:
    oca_llm_destroy(llm);
    return NULL;
}

void oca_llm_destroy(oca_llm *llm) {
    if (!llm) return;
    cJSON_Delete(llm->unsupported_providers);
    for (size_t i = 0; i < llm->model_count; i++) model_free(&llm->models[i]);
    free(llm->models);
    free(llm->available);
    for (size_t i = 0; i < llm->member_count; i++) {
        member_free(&llm->members[i]);
    }
    free(llm->members);
    secret_free(llm->kimi_env_key);
    secret_free(llm->deepseek_env_key);
    free(llm);
}

size_t oca_llm_model_count(const oca_llm *llm) {
    return llm ? llm->model_count : 0;
}

const oca_model *oca_llm_model_at(const oca_llm *llm, size_t index) {
    return llm && index < llm->model_count ? &llm->models[index] : NULL;
}

const oca_model *oca_llm_model_resolve(const oca_llm *llm, const char *name_or_id) {
    if (!llm || !name_or_id) return NULL;
    for (size_t i = 0; i < llm->model_count; i++) {
        if (!strcmp(llm->models[i].name, name_or_id) ||
            !strcmp(llm->models[i].id, name_or_id)) return &llm->models[i];
    }
    return NULL;
}

int oca_llm_model_available(const oca_llm *llm, const oca_model *model) {
    if (!llm || !model) return 0;
    /* A model borrowed from another catalog is unavailable here. */
    for (size_t i = 0; i < llm->model_count; i++)
        if (model == &llm->models[i]) return atomic_load(&llm->available[i]);
    return 0;
}

size_t oca_provider_count(void) {
    return PROVIDER_COUNT;
}

const char *oca_provider_id(size_t index) {
    return index < PROVIDER_COUNT ? PROVIDERS[index].id : NULL;
}

const char *oca_provider_credential_kind(const char *provider) {
    const provider_record *record = provider_lookup(provider);
    if (!record) return NULL;
    if (record->credential_kind == OCA_CREDENTIAL_OAUTH) return "oauth";
    if (record->credential_kind == OCA_CREDENTIAL_API_KEY) return "api-key";
    return "none";
}

int oca_llm_auth_set_api_key(oca_llm *llm, const char *provider, const char *api_key,
                             char *err, size_t errsz) {
    if (!provider_known(provider)) {
        err_set(err, errsz, "unknown provider: %s", provider);
        return -1;
    }
    if (!token_valid(api_key, 4096)) {
        err_set(err, errsz, "api key is empty or contains invalid characters");
        return -1;
    }
    if (strcmp(oca_provider_credential_kind(provider), "api-key") != 0) {
        err_set(err, errsz, "provider does not accept an API key: %s", provider);
        return -1;
    }
    auth_member member = {.provider = strdup(provider), .api_key = strdup(api_key)};
    int rc;
    if (!member.provider || !member.api_key) {
        err_set(err, errsz, "out of memory");
        rc = -1;
    } else rc = store_member(llm, &member, err, errsz);
    member_free(&member);
    return rc;
}

int oca_llm_auth_logout(oca_llm *llm, const char *provider, char *err, size_t errsz) {
    if (!provider_known(provider)) {
        err_set(err, errsz, "unknown provider: %s", provider);
        return -1;
    }
    auth_member *member = find_member(llm, provider);
    if (!member) return write_auth_file(llm, err, errsz);
    auth_member old = *member;
    auth_member *last = &llm->members[--llm->member_count];
    *member = *last;
    if (write_auth_file(llm, err, errsz)) {
        *last = *member;
        *member = old;
        llm->member_count++;
        return -1;
    }
    member_free(&old);
    memset(last, 0, sizeof(*last));
    refresh_availability(llm);
    return 0;
}

const char *oca_llm_auth_method(const oca_llm *llm, const char *provider) {
    const auth_member *m = find_member(llm, provider);
    if (!m && token_valid(provider_api_key(llm, provider), 4096))
        return "api-key";
    if (!m) return NULL;
    return m->api_key ? "api-key" : "oauth";
}

int64_t oca_llm_auth_expiry(const oca_llm *llm, const char *provider) {
    const auth_member *member = find_member(llm, provider);
    return member && oauth_valid(member) ? member->expires_at : 0;
}

size_t oca_llm_auth_member_count(const oca_llm *llm) {
    return llm->member_count;
}

/* ==========================================================================
 * Normalized requests
 * ========================================================================== */

typedef struct {
    const char *id;
    const char *name;
    const char *args;
} req_call;

typedef struct {
    oca_msg_role role;
    const char *text;
    const char *reasoning;
    const char *tool_call_id;
    const char *state;       /* opaque model continuation data */
    const req_call *calls;
    size_t calls_off;        /* arena offset so realloc can rebase pointers */
    size_t call_count;
} req_msg;

typedef struct {
    const char *name;
    const char *description;
    const char *args_json;
} req_tool;

struct oca_request {
    oca_purpose purpose;
    uint64_t request_id;
    const oca_model *model;
    oca_reasoning effort;
    const char *cache_key;
    int construction_failed;
    int (*cancel_poll)(void *);
    void *cancel_user;
    req_tool *tools;
    size_t tool_count, tool_cap;
    req_msg *msgs;
    size_t msg_count, msg_cap;
    req_call *calls;
    size_t call_count, call_cap;
};

oca_request *oca_request_create(oca_purpose purpose, uint64_t request_id) {
    oca_request *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->purpose = purpose;
    r->request_id = request_id;
    return r;
}

void oca_request_destroy(oca_request *request) {
    if (!request) return;
    free(request->tools);
    free(request->msgs);
    free(request->calls);
    free(request);
}

void oca_request_set_route(oca_request *r, const oca_model *model, oca_reasoning effort) {
    r->model = model;
    r->effort = effort;
}

void oca_request_set_cache_key(oca_request *r, const char *key) {
    r->cache_key = key;
}

void oca_request_set_cancel(oca_request *r, int (*poll)(void *), void *user) {
    r->cancel_poll = poll;
    r->cancel_user = user;
}

void oca_request_add_tool(oca_request *r, const char *name, const char *description,
                          const char *args_json) {
    if (r->tool_count == r->tool_cap) {
        size_t nc = r->tool_cap ? r->tool_cap * 2 : 8;
        req_tool *nt = realloc(r->tools, nc * sizeof(*nt));
        if (!nt) {
            r->construction_failed = 1;
            return;
        }
        r->tools = nt;
        r->tool_cap = nc;
    }
    req_tool *t = &r->tools[r->tool_count++];
    t->name = name;
    t->description = description;
    t->args_json = args_json;
}

void oca_request_push_msg(oca_request *r, oca_msg_role role, const char *text,
                          const char *reasoning, const char *tool_call_id) {
    if (r->msg_count == r->msg_cap) {
        size_t nc = r->msg_cap ? r->msg_cap * 2 : 16;
        req_msg *nm = realloc(r->msgs, nc * sizeof(*nm));
        if (!nm) {
            r->construction_failed = 1;
            return;
        }
        r->msgs = nm;
        r->msg_cap = nc;
    }
    req_msg *m = &r->msgs[r->msg_count++];
    memset(m, 0, sizeof(*m));
    m->role = role;
    m->text = text;
    m->reasoning = role == OCA_MSG_ASSISTANT ? reasoning : NULL;
    m->tool_call_id = role == OCA_MSG_TOOL ? tool_call_id : NULL;
}

void oca_request_msg_set_state(oca_request *r, const char *state) {
    if (r->msg_count == 0 || r->msgs[r->msg_count - 1].role != OCA_MSG_ASSISTANT) return;
    r->msgs[r->msg_count - 1].state = state;
}

void oca_request_msg_add_call(oca_request *r, const char *call_id, const char *name,
                              const char *args_json) {
    if (r->msg_count == 0 || r->msgs[r->msg_count - 1].role != OCA_MSG_ASSISTANT) return;
    if (r->call_count == r->call_cap) {
        size_t nc = r->call_cap ? r->call_cap * 2 : 8;
        req_call *ncl = realloc(r->calls, nc * sizeof(*ncl));
        if (!ncl) {
            r->construction_failed = 1;
            return;
        }
        r->calls = ncl;
        r->call_cap = nc;
        /* Rebase every message after the arena move. */
        for (size_t i = 0; i < r->msg_count; i++)
            r->msgs[i].calls = r->calls + r->msgs[i].calls_off;
    }
    req_call *cl = &r->calls[r->call_count++];
    cl->id = call_id;
    cl->name = name;
    cl->args = args_json;
    req_msg *m = &r->msgs[r->msg_count - 1];
    if (m->call_count == 0) m->calls_off = r->call_count - 1;
    m->calls = r->calls + m->calls_off;
    m->call_count++;
}

/* ==========================================================================
 * Request validation
 * ========================================================================== */

static int validate_request(const oca_request *r, char *err, size_t errsz) {
    if (!r || r->purpose < OCA_PURPOSE_TURN || r->purpose > OCA_PURPOSE_COMPACTION) {
        err_set(err, errsz, "invalid request purpose");
        return -1;
    }
    if (r->construction_failed) {
        err_set(err, errsz, "request construction failed");
        return -1;
    }
    if (!r->model) {
        err_set(err, errsz, "request has no model");
        return -1;
    }
    if (r->effort == OCA_REASONING_NONE) {
        if (r->model->reasoning_count > 0) {
            err_set(err, errsz, "model requires a reasoning level");
            return -1;
        }
    } else if (!oca_model_supports_reasoning(r->model, r->effort)) {
        err_set(err, errsz, "model does not support the selected reasoning level");
        return -1;
    }
    if (r->purpose == OCA_PURPOSE_COMPACTION && r->tool_count > 0) {
        err_set(err, errsz, "purpose does not permit tool definitions");
        return -1;
    }
    if (r->purpose == OCA_PURPOSE_COMPACTION &&
        r->effort != oca_model_min_reasoning(r->model)) {
        err_set(err, errsz, "compaction requires the lowest supported reasoning effort");
        return -1;
    }
    for (size_t i = 0; i < r->tool_count; i++) {
        if (!r->tools[i].name || !r->tools[i].description || !r->tools[i].args_json) {
            err_set(err, errsz, "invalid tool definition");
            return -1;
        }
    }
    for (size_t i = 0; i < r->msg_count; i++) {
        const req_msg *msg = &r->msgs[i];
        if (msg->role < OCA_MSG_SYSTEM || msg->role > OCA_MSG_TOOL || !msg->text ||
            (msg->role == OCA_MSG_TOOL && !msg->tool_call_id) ||
            (msg->role != OCA_MSG_TOOL && msg->tool_call_id) ||
            (msg->role != OCA_MSG_ASSISTANT && (msg->reasoning || msg->state || msg->call_count))) {
            err_set(err, errsz, "invalid normalized message");
            return -1;
        }
        for (size_t k = 0; k < msg->call_count; k++) {
            if (!msg->calls[k].id || !msg->calls[k].name || !msg->calls[k].args) {
                err_set(err, errsz, "invalid normalized tool call");
                return -1;
            }
        }
    }
    return 0;
}

static char *chat_serialize(const oca_request *r);
static char *openai_serialize(const oca_request *request);

int oca_llm_estimate_input(const oca_request *request, long *tokens, char *err, size_t errsz) {
    if (validate_request(request, err, errsz) != 0) return -1;
    if (!tokens) {
        err_set(err, errsz, "input estimation requires an output pointer");
        return -1;
    }
    if (!strcmp(request->model->provider, "kimi") ||
        !strcmp(request->model->provider, "deepseek") ||
        !strcmp(request->model->provider, "openai")) {
        char *body = !strcmp(request->model->provider, "openai") ? openai_serialize(request) : chat_serialize(request);
        if (!body) {
            err_set(err, errsz, "cannot serialize the bounded provider request");
            return -1;
        }
        /* UTF-8 bytes conservatively bound byte-fallback tokens. Include
         * serialized structure and extra framing without a network call. */
        *tokens = (long)strlen(body) + 256;
        cJSON_free(body);
        return 0;
    }
    err_set(err, errsz, "model has no input estimator");
    return -1;
}

/* ============================================================================
 * Provider transport
 * ============================================================================ */

static pthread_once_t curl_once = PTHREAD_ONCE_INIT;
static CURLcode curl_status;

static void curl_init(void) {
    curl_status = curl_global_init(CURL_GLOBAL_DEFAULT);
}

typedef struct {
    long connect_ms, total_ms, low_speed_seconds, low_speed_bytes_per_second;
} transport_limits;

static const transport_limits PROVIDER_TRANSPORT_LIMITS = {15000, 180000, 45, 1};
static const transport_limits OAUTH_TRANSPORT_LIMITS = {15000, 30000, 0, 0};

/* Borrows the body and headers until the caller finishes the synchronous transfer. */
static int configure_http_post(CURL *curl, const char *url, const char *body,
                                struct curl_slist *headers, const transport_limits *limits) {
    int ready = 1;
#define HTTP_OPT(option, value) do { if (curl_easy_setopt(curl, option, value) != CURLE_OK) ready = 0; } while (0)
    HTTP_OPT(CURLOPT_URL, url);
    HTTP_OPT(CURLOPT_USERAGENT, "OCA/dev (C11 agent)");
    HTTP_OPT(CURLOPT_HTTPHEADER, headers);
    HTTP_OPT(CURLOPT_POSTFIELDS, body);
    HTTP_OPT(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)strlen(body));
    HTTP_OPT(CURLOPT_NOPROGRESS, 0L);
    HTTP_OPT(CURLOPT_NOSIGNAL, 1L);
    HTTP_OPT(CURLOPT_CONNECTTIMEOUT_MS, limits->connect_ms);
    HTTP_OPT(CURLOPT_TIMEOUT_MS, limits->total_ms);
    HTTP_OPT(CURLOPT_LOW_SPEED_LIMIT, limits->low_speed_bytes_per_second);
    HTTP_OPT(CURLOPT_LOW_SPEED_TIME, limits->low_speed_seconds);
    HTTP_OPT(CURLOPT_FOLLOWLOCATION, 0L);
    HTTP_OPT(CURLOPT_SSL_VERIFYPEER, 1L);
    HTTP_OPT(CURLOPT_SSL_VERIFYHOST, 2L);
#undef HTTP_OPT
    return ready;
}


typedef struct {
    const transport_limits *limits;
    size_t request_bytes;
    unsigned attempt;
    int body_received, clock_valid;
    struct timespec last_body;
} transport_watch;

static void transport_received(transport_watch *watch) {
    watch->body_received = 1;
    watch->clock_valid = clock_gettime(CLOCK_MONOTONIC, &watch->last_body) == 0;
}

/* Capture before handle cleanup. Library counters remain separate from parser
 * counters, and a secondary CURL failure never replaces a parser diagnostic. */
static void diagnostic_transport(oca_llm_diagnostic *d, CURL *curl, CURLcode result,
                                  long status, const transport_watch *watch) {
    oca_llm_transport_diagnostic *t = &d->transport;
    memset(t, 0, sizeof(*t));
    d->has_transport = 1;
    d->has_http_status = status > 0;
    d->http_status = status;
    t->curl_code = (int)result;
    const char *message = curl_easy_strerror(result);
    snprintf(t->curl_message, sizeof(t->curl_message), "%s", message);
    if (strlen(message) >= sizeof(t->curl_message)) t->curl_message[sizeof(t->curl_message) - 2] = '~';
    t->attempt = watch->attempt;
    t->body_received = watch->body_received;
    t->request_bytes = watch->request_bytes;
    t->connect_timeout_ms = watch->limits->connect_ms;
    t->total_timeout_ms = watch->limits->total_ms;
    t->low_speed_seconds = watch->limits->low_speed_seconds;
    t->low_speed_bytes_per_second = watch->limits->low_speed_bytes_per_second;
    long os_error = 0;
    t->os_errno = curl_easy_getinfo(curl, CURLINFO_OS_ERRNO, &os_error) == CURLE_OK ? os_error : -1;
    const struct { CURLINFO info; int64_t *value; } metrics[] = {
        {CURLINFO_SIZE_UPLOAD_T, &t->uploaded_bytes},
        {CURLINFO_SIZE_DOWNLOAD_T, &t->downloaded_bytes},
        {CURLINFO_NAMELOOKUP_TIME_T, &t->dns_us},
        {CURLINFO_CONNECT_TIME_T, &t->connect_us},
        {CURLINFO_APPCONNECT_TIME_T, &t->tls_us},
        {CURLINFO_PRETRANSFER_TIME_T, &t->pretransfer_us},
        {CURLINFO_STARTTRANSFER_TIME_T, &t->first_byte_us},
        {CURLINFO_TOTAL_TIME_T, &t->total_us}
    };
    for (size_t i = 0; i < sizeof(metrics) / sizeof(metrics[0]); i++) {
        curl_off_t value = 0;
        *metrics[i].value = curl_easy_getinfo(curl, metrics[i].info, &value) == CURLE_OK &&
                           value >= 0 && value <= INT64_MAX ? (int64_t)value : -1;
    }
    t->body_idle_us = -1;
    struct timespec now;
    if (watch->clock_valid && !clock_gettime(CLOCK_MONOTONIC, &now) && now.tv_sec >= watch->last_body.tv_sec) {
        int64_t seconds = (int64_t)(now.tv_sec - watch->last_body.tv_sec);
        if (seconds <= (INT64_MAX - 999999) / 1000000) {
            int64_t elapsed = seconds * 1000000 + (now.tv_nsec - watch->last_body.tv_nsec) / 1000;
            if (elapsed >= 0) t->body_idle_us = elapsed;
        }
    }
}

/* ============================================================================
 * Chat Completions adapters: Kimi and DeepSeek
 * ============================================================================
 * Provider JSON and SSE stay in this layer. Errors exclude server bodies and credentials.
 */

#define KIMI_URL "https://api.kimi.com/coding/v1/chat/completions"
#define DEEPSEEK_URL "https://api.deepseek.com/chat/completions"
#define CHAT_FRAME_MAX (128u * 1024u)
#define CHAT_RESPONSE_MAX OCA_LLM_RESPONSE_MAX_BYTES
#define CHAT_ARGUMENTS_MAX (64u * 1024u)
#define CHAT_CALLS_MAX 32

static char *chat_serialize(const oca_request *r) {
    int ok = 1;
    cJSON *root = cJSON_CreateObject();
    int deepseek = !strcmp(r->model->provider, "deepseek");
    const char *level = oca_model_reasoning_name(r->model, r->effort);
    json_add(root, "model", cJSON_CreateString(r->model->id), &ok);
    /* A configured level named "off" disables DeepSeek thinking. */
    if (level && (!deepseek || strcmp(level, "off"))) {
        json_add(root, "reasoning_effort", cJSON_CreateString(level), &ok);
        if (deepseek) {
            cJSON *thinking = json_add(root, "thinking", cJSON_CreateObject(), &ok);
            json_add(thinking, "type", cJSON_CreateString("enabled"), &ok);
        }
    } else if (level) {
        cJSON *thinking = json_add(root, "thinking", cJSON_CreateObject(), &ok);
        json_add(thinking, "type", cJSON_CreateString("disabled"), &ok);
    }
    json_add(root, "stream", cJSON_CreateBool(1), &ok);
    cJSON *options = json_add(root, "stream_options", cJSON_CreateObject(), &ok);
    json_add(options, "include_usage", cJSON_CreateBool(1), &ok);
    /* DeepSeek context caching is automatic and has no cache-affinity key. */
    if (r->cache_key && !deepseek) {
        if (strlen(r->cache_key) > 128) ok = 0;
        json_add(root, "prompt_cache_key", cJSON_CreateString(r->cache_key), &ok);
    }
    cJSON *messages = json_add(root, "messages", cJSON_CreateArray(), &ok);
    static const char *const roles[] = {"system", "user", "assistant", "tool"};
    for (size_t i = 0; ok && i < r->msg_count; i++) {
        const req_msg *m = &r->msgs[i];
        cJSON *message = json_add(messages, NULL, cJSON_CreateObject(), &ok);
        json_add(message, "role", cJSON_CreateString(roles[m->role]), &ok);
        json_add(message, "content", cJSON_CreateString(m->text), &ok);
        if (m->role == OCA_MSG_ASSISTANT) {
            json_add(message, "reasoning_content",
                     cJSON_CreateString(m->reasoning ? m->reasoning : ""), &ok);
            if (m->call_count) {
                cJSON *calls = json_add(message, "tool_calls", cJSON_CreateArray(), &ok);
                for (size_t j = 0; ok && j < m->call_count; j++) {
                    const req_call *call = &m->calls[j];
                    cJSON *args = json_parse(call->args, strlen(call->args));
                    if (!cJSON_IsObject(args)) ok = 0;
                    cJSON_Delete(args);
                    cJSON *item = json_add(calls, NULL, cJSON_CreateObject(), &ok);
                    json_add(item, "id", cJSON_CreateString(call->id), &ok);
                    json_add(item, "type", cJSON_CreateString("function"), &ok);
                    cJSON *fn = json_add(item, "function", cJSON_CreateObject(), &ok);
                    json_add(fn, "name", cJSON_CreateString(call->name), &ok);
                    json_add(fn, "arguments", cJSON_CreateString(call->args), &ok);
                }
            }
        } else if (m->role == OCA_MSG_TOOL) {
            json_add(message, "tool_call_id", cJSON_CreateString(m->tool_call_id), &ok);
        }
    }
    if (r->tool_count) {
        cJSON *tools = json_add(root, "tools", cJSON_CreateArray(), &ok);
        for (size_t i = 0; ok && i < r->tool_count; i++) {
            const req_tool *tool = &r->tools[i];
            cJSON *schema = json_parse(tool->args_json, strlen(tool->args_json));
            if (!cJSON_IsObject(schema)) ok = 0;
            cJSON *item = json_add(tools, NULL, cJSON_CreateObject(), &ok);
            json_add(item, "type", cJSON_CreateString("function"), &ok);
            cJSON *fn = json_add(item, "function", cJSON_CreateObject(), &ok);
            json_add(fn, "name", cJSON_CreateString(tool->name), &ok);
            json_add(fn, "description", cJSON_CreateString(tool->description), &ok);
            json_add(fn, "parameters", schema, &ok);
        }
    }
    char *body = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (body && (strlen(body) > LLM_BODY_MAX || !utf8_text_valid(body, strlen(body)))) {
        cJSON_free(body);
        body = NULL;
    }
    return body;
}

typedef struct {
    byte_buffer id, name, args;
    int seen;
} chat_call;

typedef struct {
    const oca_request *req;
    oca_event_fn *callback;
    void *user;
    CURL *curl;
    byte_buffer line, data;
    chat_call calls[CHAT_CALLS_MAX];
    size_t text_bytes, wire_bytes;
    int started, done, finished, cancelled, have_usage;
    const char *category, *message;
    char finish[32];
    oca_event usage;
    oca_llm_diagnostic diagnostic;
    transport_watch transport;
} chat_run;

static void chat_fail(chat_run *run, const char *category, const char *message) {
    if (!run->category) {
        run->category = category;
        run->message = message;
    }
}

static int chat_poll(chat_run *run) {
    if (run->req->cancel_poll && run->req->cancel_poll(run->req->cancel_user))
        run->cancelled = 1;
    return run->cancelled;
}

static int chat_emit(chat_run *run, oca_event *ev) {
    ev->request_id = run->req->request_id;
    if (run->callback(ev, run->user)) run->cancelled = 1;
    return run->cancelled;
}

static int chat_count(const cJSON *object, const char *name, int *present, long *count) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!value || cJSON_IsNull(value)) return 0;
    long parsed;
    if (json_count(value, &parsed) != 0 || (*present && parsed != *count)) return -1;
    *count = parsed;
    *present = 1;
    return 0;
}

static int chat_usage(chat_run *run, const cJSON *usage) {
    if (!usage || cJSON_IsNull(usage)) return 0;
    if (!cJSON_IsObject(usage)) return -1;
    /* Merge repeated final counters and emit one normalized usage event. */
    oca_event *ev = &run->usage;
    ev->kind = OCA_EV_USAGE;
    if (chat_count(usage, "prompt_tokens", &ev->has_input, &ev->input_tokens) ||
        chat_count(usage, "completion_tokens", &ev->has_output, &ev->output_tokens) ||
        chat_count(usage, "total_tokens", &ev->has_total, &ev->total_tokens) ||
        chat_count(usage, "cached_tokens", &ev->has_cache_hit, &ev->cache_hit_tokens) ||
        chat_count(usage, "prompt_cache_hit_tokens", &ev->has_cache_hit, &ev->cache_hit_tokens) ||
        chat_count(usage, "prompt_cache_miss_tokens", &ev->has_cache_miss, &ev->cache_miss_tokens) ||
        chat_count(cJSON_GetObjectItemCaseSensitive(usage, "prompt_tokens_details"), "cached_tokens",
                   &ev->has_cache_hit, &ev->cache_hit_tokens) ||
        chat_count(cJSON_GetObjectItemCaseSensitive(usage, "completion_tokens_details"), "reasoning_tokens",
                   &ev->has_reasoning_tokens, &ev->reasoning_tokens)) return -1;
    if (ev->has_input && ev->has_cache_hit) {
        if (ev->cache_hit_tokens > ev->input_tokens) return -1;
        if (!ev->has_cache_miss) {
            ev->has_cache_miss = 1;
            ev->cache_miss_tokens = ev->input_tokens - ev->cache_hit_tokens;
        }
    }
    run->have_usage = 1;
    return 0;
}

static int chat_delta_text(chat_run *run, const cJSON *delta, const char *field,
                           oca_event_kind kind) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(delta, field);
    if (!value || cJSON_IsNull(value)) return 0;
    const char *text = cJSON_GetStringValue(value);
    if (!text || strlen(text) > CHAT_RESPONSE_MAX - run->text_bytes) return -1;
    if (!*text) return 0;
    run->text_bytes += strlen(text);
    oca_event ev = {.kind = kind, .text = text};
    return chat_emit(run, &ev) ? -1 : 0;
}

static int chat_delta_calls(chat_run *run, const cJSON *array) {
    if (!array || cJSON_IsNull(array)) return 0;
    if (!cJSON_IsArray(array) || !run->req->tool_count) return -1;
    unsigned seen = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, array) {
        long index;
        if (!cJSON_IsObject(item) || json_count(cJSON_GetObjectItemCaseSensitive(item, "index"), &index) ||
            index >= CHAT_CALLS_MAX || (seen & (1u << index))) return -1;
        seen |= 1u << index;
        chat_call *call = &run->calls[index];
        call->seen = 1;
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(item, "id");
        if (value) {
            const char *id = cJSON_GetStringValue(value);
            if (!id || !*id || strlen(id) > 256) return -1;
            if (call->id.len) {
                if (strcmp(call->id.data, id)) return -1;
            } else if (buffer_append(&call->id, id, strlen(id), 256)) return -1;
        }
        value = cJSON_GetObjectItemCaseSensitive(item, "type");
        if (value && (!cJSON_GetStringValue(value) || strcmp(value->valuestring, "function"))) return -1;
        const cJSON *fn = cJSON_GetObjectItemCaseSensitive(item, "function");
        if (fn && !cJSON_IsObject(fn)) return -1;
        value = cJSON_GetObjectItemCaseSensitive(fn, "name");
        if (value && (!cJSON_GetStringValue(value) ||
            buffer_append(&call->name, value->valuestring, strlen(value->valuestring), 64))) return -1;
        value = cJSON_GetObjectItemCaseSensitive(fn, "arguments");
        if (value && (!cJSON_GetStringValue(value) ||
            buffer_append(&call->args, value->valuestring, strlen(value->valuestring),
                        CHAT_ARGUMENTS_MAX))) return -1;
    }
    return 0;
}

static int chat_chunk(chat_run *run, const char *data, size_t len) {
    oca_llm_diagnostic *d = &run->diagnostic;
    d->event_index++;
    d->frame_bytes = len;
    snprintf(d->provider_event, sizeof(d->provider_event), "data");
    if (run->done)
        return diagnostic_set(d, "after_done", "frame", "end of stream", "additional frame");
    if (len == 6 && !memcmp(data, "[DONE]", 6)) {
        snprintf(d->provider_event, sizeof(d->provider_event), "[DONE]");
        if (!run->finished)
            return diagnostic_set(d, "missing_finish", "finish_reason", "stop or tool_calls", "missing");
        run->done = 1;
        return 0;
    }
    cJSON *root = json_parse(data, len);
    int ok = cJSON_IsObject(root);
    if (!ok) {
        cJSON_Delete(root);
        return diagnostic_set(d, "frame_json", "frame", "valid bounded JSON object", "JSON validation failed");
    }
    const char *object = json_text(root, "object");
    if (object) diagnostic_token(d->provider_event, object);
    if (cJSON_GetObjectItemCaseSensitive(root, "error")) {
        chat_fail(run, "provider", "Provider reported an error during generation.");
        cJSON_Delete(root);
        return -1;
    }
    const cJSON *model = cJSON_GetObjectItemCaseSensitive(root, "model");
    if (model && (!cJSON_GetStringValue(model) ||
        strcmp(model->valuestring, run->req->model->id))) {
        diagnostic_field(d, "model_mismatch", "model", run->req->model->id, model, 1);
        chat_fail(run, "model", "Provider returned a different model than requested.");
        cJSON_Delete(root);
        return -1;
    }
    const cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) > 1) {
        diagnostic_field(d, "choices_shape", "choices", "array with at most one choice", choices, 0);
        ok = 0;
    }
    if (ok && !run->started) {
        oca_event ev = {.kind = OCA_EV_RESPONSE_START, .model_id = run->req->model->id};
        run->started = 1;
        if (chat_emit(run, &ev)) ok = 0;
    }
    const cJSON *choice = cJSON_GetArrayItem(choices, 0);
    if (ok && choice) {
        long index;
        if (run->finished) {
            diagnostic_set(d, "after_finish", "choices", "usage-only frame", "additional choice");
            ok = 0;
        } else if (json_count(cJSON_GetObjectItemCaseSensitive(choice, "index"), &index) || index != 0) {
            diagnostic_field(d, "choice_index", "index", "0", cJSON_GetObjectItemCaseSensitive(choice, "index"), 0);
            ok = 0;
        }
        const cJSON *delta = cJSON_GetObjectItemCaseSensitive(choice, "delta");
        if (ok && !cJSON_IsObject(delta)) {
            diagnostic_field(d, "delta_shape", "delta", "object", delta, 0);
            ok = 0;
        }
        if (ok && (chat_delta_text(run, delta, "reasoning_content", OCA_EV_REASONING_DELTA) ||
                   chat_delta_text(run, delta, "content", OCA_EV_TEXT_DELTA) ||
                   chat_delta_calls(run, cJSON_GetObjectItemCaseSensitive(delta, "tool_calls")))) ok = 0;
        const cJSON *finish = cJSON_GetObjectItemCaseSensitive(choice, "finish_reason");
        if (ok && finish && !cJSON_IsNull(finish)) {
            const char *value = cJSON_GetStringValue(finish);
            if (!value) ok = 0;
            else if (!strcmp(value, "stop") || !strcmp(value, "tool_calls")) {
                snprintf(run->finish, sizeof(run->finish), "%s", value);
                run->finished = 1;
            } else {
                chat_fail(run, !strcmp(value, "length") ? "output_limit" : "provider",
                          !strcmp(value, "length") ? "Provider reached its generation limit."
                                                  : "Provider did not complete the response.");
                ok = 0;
            }
        }
        const cJSON *root_usage = cJSON_GetObjectItemCaseSensitive(root, "usage");
        if (ok && (!root_usage || cJSON_IsNull(root_usage)) &&
            chat_usage(run, cJSON_GetObjectItemCaseSensitive(choice, "usage")))
            ok = 0;
    }
    if (ok && chat_usage(run, cJSON_GetObjectItemCaseSensitive(root, "usage"))) {
        diagnostic_set(d, "usage_conflict", "usage", "consistent nonnegative counters", "invalid or conflicting counters");
        ok = 0;
    }
    cJSON_Delete(root);
    return ok ? 0 : -1;
}

static int chat_line(chat_run *run) {
    size_t len = run->line.len;
    char *line = run->line.data;
    if (len && line[len - 1] == '\r') line[--len] = 0;
    int rc = 0;
    if (!len) {
        if (run->data.len) rc = chat_chunk(run, run->data.data, run->data.len);
        run->data.len = 0;
        if (run->data.data) run->data.data[0] = 0;
    } else if (len >= 5 && !memcmp(line, "data:", 5)) {
        size_t at = 5 + (len > 5 && line[5] == ' ');
        if (run->data.len && diagnostic_append(&run->diagnostic, &run->data, "\n", 1,
                                               CHAT_FRAME_MAX, "frame_bytes")) rc = -1;
        if (!rc) rc = diagnostic_append(&run->diagnostic, &run->data, line + at,
                                        len - at, CHAT_FRAME_MAX, "frame_bytes");
        if (rc) {
            snprintf(run->diagnostic.provider_event, sizeof(run->diagnostic.provider_event), "<undecoded>");
            run->diagnostic.event_index++;
            run->diagnostic.frame_bytes = run->data.len + len - at;
        }
    }
    run->line.len = 0;
    if (line) line[0] = 0;
    return rc;
}

static size_t chat_write(char *data, size_t size, size_t count, void *user) {
    chat_run *run = user;
    if (size && count > SIZE_MAX / size) return 0;
    size_t len = size * count;
    if (len) transport_received(&run->transport);
    if (chat_poll(run)) return 0;
    if (len > LLM_BODY_MAX - run->wire_bytes) {
        diagnostic_limit(&run->diagnostic, "wire_bytes",
                         len > SIZE_MAX - run->wire_bytes ? SIZE_MAX : run->wire_bytes + len, LLM_BODY_MAX);
        chat_fail(run, "limit", "Provider response exceeded the transport limit.");
        return 0;
    }
    run->wire_bytes += len;
    long status = 0;
    curl_easy_getinfo(run->curl, CURLINFO_RESPONSE_CODE, &status);
    if (status != 200) return len;
    for (size_t i = 0; i < len; i++) {
        int failed = data[i] == '\n' ? chat_line(run) :
                     diagnostic_append(&run->diagnostic, &run->line, data + i, 1, CHAT_FRAME_MAX, "line_bytes");
        if (failed) {
            if (data[i] != '\n') {
                snprintf(run->diagnostic.provider_event, sizeof(run->diagnostic.provider_event), "<undecoded>");
                run->diagnostic.event_index++;
                run->diagnostic.frame_bytes = run->data.len + run->line.len + 1;
            }
            if (!run->cancelled)
                chat_fail(run, "stream", "Provider returned an invalid or oversized stream.");
            return 0;
        }
    }
    return len;
}

static int chat_progress(void *user, curl_off_t total_down, curl_off_t down,
                          curl_off_t total_up, curl_off_t up) {
    (void)total_down;
    (void)down;
    (void)total_up;
    (void)up;
    return chat_poll(user);
}

static int chat_deliver_calls(chat_run *run) {
    size_t count = 0;
    for (size_t i = 0; i < CHAT_CALLS_MAX; i++) {
        chat_call *call = &run->calls[i];
        if (!call->seen) continue;
        if (i != count++ || !call->id.len || !call->name.len || !call->args.len) return -1;
        for (size_t j = 0; j < i; j++)
            if (!strcmp(run->calls[j].id.data, call->id.data)) return -1;
        cJSON *args = json_parse(call->args.data, call->args.len);
        int valid = cJSON_IsObject(args);
        cJSON_Delete(args);
        if (!valid) return -1;
    }
    if ((count > 0) != (!strcmp(run->finish, "tool_calls"))) return -1;
    for (size_t i = 0; i < count; i++) {
        chat_call *call = &run->calls[i];
        oca_event ev = {.kind = OCA_EV_TOOL_CALL_START, .call_index = (long)i,
                       .call_id = call->id.data, .call_name = call->name.data};
        if (chat_emit(run, &ev)) return -1;
        ev.kind = OCA_EV_TOOL_CALL_ARGUMENTS;
        ev.arguments = call->args.data;
        if (chat_emit(run, &ev)) return -1;
        ev.kind = OCA_EV_TOOL_CALL_END;
        if (chat_emit(run, &ev)) return -1;
    }
    return 0;
}

/* The URL is an internal test seam. Public dispatch always uses KIMI_URL. */
static int chat_generate(oca_llm *llm, const oca_request *req,
                         oca_event_fn *callback, void *user, const char *url,
                         char *err, size_t errsz) {
    char authorization[4140];
    pthread_mutex_lock(&oauth_refresh_mutex);
    const char *key = provider_api_key(llm, req->model->provider);
    if (!token_valid(key, 4096)) {
        pthread_mutex_unlock(&oauth_refresh_mutex);
        err_set(err, errsz, "%s needs an API key; run oca -m %s in an interactive terminal",
                req->model->provider, req->model->id);
        return -1;
    }
    snprintf(authorization, sizeof(authorization), "Authorization: Bearer %s", key);
    pthread_mutex_unlock(&oauth_refresh_mutex);
    char *body = chat_serialize(req);
    if (!body) {
        memset(authorization, 0, sizeof(authorization));
        err_set(err, errsz, "cannot serialize the bounded provider request");
        return -1;
    }
    if (strlen(body) + 256 > (size_t)req->model->input_limit) {
        memset(authorization, 0, sizeof(authorization));
        cJSON_free(body);
        err_set(err, errsz, "Provider input exceeds the local context limit");
        return -1;
    }
    pthread_once(&curl_once, curl_init);
    CURL *curl = curl_status == CURLE_OK ? curl_easy_init() : NULL;
    if (!curl) {
        memset(authorization, 0, sizeof(authorization));
        cJSON_free(body);
        err_set(err, errsz, "cannot initialize HTTPS transport");
        return -1;
    }
    struct curl_slist *headers = NULL;
    const char *values[] = {authorization, "Content-Type: application/json", "Accept: text/event-stream", "Expect:"};
    int setup = 1;
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        struct curl_slist *next = curl_slist_append(headers, values[i]);
        if (!next) {
            setup = 0;
            break;
        }
        headers = next;
    }
    memset(authorization, 0, sizeof(authorization));
    chat_run run = {.req = req, .callback = callback, .user = user, .curl = curl};
    run.transport = (transport_watch){.limits = &PROVIDER_TRANSPORT_LIMITS,
                                      .request_bytes = strlen(body), .attempt = 1};
    if (!configure_http_post(curl, url, body, headers, &PROVIDER_TRANSPORT_LIMITS)) setup = 0;
#define CHAT_OPT(option, value) do { if (curl_easy_setopt(curl, option, value) != CURLE_OK) setup = 0; } while (0)
    CHAT_OPT(CURLOPT_WRITEFUNCTION, chat_write);
    CHAT_OPT(CURLOPT_WRITEDATA, &run);
    CHAT_OPT(CURLOPT_XFERINFOFUNCTION, chat_progress);
    CHAT_OPT(CURLOPT_XFERINFODATA, &run);
    int fixed_https = !strcmp(url, KIMI_URL) || !strcmp(url, DEEPSEEK_URL);
    CHAT_OPT(CURLOPT_PROTOCOLS_STR, fixed_https ? "https" : "http");
    if (!fixed_https) CHAT_OPT(CURLOPT_NOPROXY, "*");
#undef CHAT_OPT
    CURLcode result = setup ? curl_easy_perform(curl) : CURLE_FAILED_INIT;
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    diagnostic_transport(&run.diagnostic, curl, result, status, &run.transport);
    if (run.cancelled) chat_fail(&run, "cancelled", "Cancelled by user.");
    else if (status == 401 || status == 403)
        chat_fail(&run, "authentication", "Provider rejected access; check the API key and subscription model access.");
    else if (status == 429)
        chat_fail(&run, "rate_limit", "Provider quota or rate limit reached; retry later.");
    else if (status && status != 200)
        chat_fail(&run, "http", "Provider returned an HTTP error.");
    else if (result != CURLE_OK)
        chat_fail(&run, "transport", "Provider HTTPS transfer failed or timed out.");
    if (!run.category) {
        if ((run.line.len && chat_line(&run)) ||
            (run.data.len && chat_chunk(&run, run.data.data, run.data.len)) ||
            !run.done || !run.started || chat_deliver_calls(&run))
            chat_fail(&run, "stream", "Provider stream ended without a valid completed response.");
    }
    if (!run.category && run.have_usage) chat_emit(&run, &run.usage);
    if (run.cancelled) {
        run.category = "cancelled";
        run.message = "Cancelled by user.";
    }
    if (run.category && !*run.diagnostic.check) {
        char received[64];
        if (!strcmp(run.category, "cancelled"))
            diagnostic_set(&run.diagnostic, "cancelled", "request", "active request", "cancelled");
        else if (status && status != 200) {
            snprintf(received, sizeof(received), "%ld", status);
            diagnostic_set(&run.diagnostic, "http_status", "http_status", "200", received);
        } else if (!strcmp(run.category, "transport")) {
            snprintf(received, sizeof(received), "%d", (int)result);
            diagnostic_set(&run.diagnostic, "transport", "curl_code", "0", received);
        } else diagnostic_set(&run.diagnostic, run.category, "stream", "valid completed response", "rejected");
    }
    run.diagnostic.wire_bytes = run.wire_bytes;
    run.diagnostic.response_bytes = run.text_bytes;
    oca_event end = {.kind = run.category ? OCA_EV_ERROR : OCA_EV_RESPONSE_DONE,
                     .model_id = req->model->id, .finish = run.finish,
                     .diagnostic = run.category ? &run.diagnostic : NULL,
                     .category = run.category, .message = run.message};
    chat_emit(&run, &end);
    for (size_t i = 0; i < CHAT_CALLS_MAX; i++) {
        buffer_free(&run.calls[i].id);
        buffer_free(&run.calls[i].name);
        buffer_free(&run.calls[i].args);
    }
    buffer_free(&run.line);
    buffer_free(&run.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    cJSON_free(body);
    return 0;
}

/* ============================================================================
 * Native OpenAI OAuth
 * ============================================================================ */

#define OPENAI_ISSUER "https://auth.openai.com"
#define OPENAI_TOKEN_URL OPENAI_ISSUER "/oauth/token"
#define OPENAI_CLIENT_ID "app_EMoamEEZ73f0CkXaXp7hrann"
#define OAUTH_BODY_MAX (128u * 1024u)

static int oauth_cancelled(const oca_auth_ui *ui) {
    return ui && ui->cancelled && ui->cancelled(ui->user);
}

static int64_t monotonic_ms(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time)) return -1;
    return (int64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

static int oauth_before_deadline(int64_t deadline) {
    int64_t now = monotonic_ms();
    return now >= 0 && now < deadline;
}

static int base64url(const unsigned char *input, size_t length, char *out, size_t size) {
    if (length > INT_MAX || size < 4 * ((length + 2) / 3) + 1) return -1;
    int count = EVP_EncodeBlock((unsigned char *)out, input, (int)length);
    if (count < 0) return -1;
    while (count && out[count - 1] == '=') count--;
    out[count] = 0;
    for (int i = 0; i < count; i++) {
        if (out[i] == '+') out[i] = '-';
        if (out[i] == '/') out[i] = '_';
    }
    return 0;
}

static int oauth_secrets(char verifier[65], char challenge[65], char state[65]) {
    unsigned char random[32], digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (RAND_bytes(random, sizeof(random)) != 1 ||
        base64url(random, sizeof(random), verifier, 65) ||
        EVP_Digest(verifier, strlen(verifier), digest, &length, EVP_sha256(), NULL) != 1 ||
        base64url(digest, length, challenge, 65) ||
        RAND_bytes(random, sizeof(random)) != 1 ||
        base64url(random, sizeof(random), state, 65)) return -1;
    return 0;
}

/* Claims come only from the TLS-authenticated token endpoint. They identify
 * the account and expiration; they do not independently authorize a request. */
static cJSON *oauth_claims(const char *token) {
    const char *start = strchr(token, '.');
    const char *end = start ? strchr(start + 1, '.') : NULL;
    if (!start || !end || start == token || end == start + 1 || !end[1] || strchr(end + 1, '.')) return NULL;
    size_t length = (size_t)(end - ++start);
    if (length > 16384 || length % 4 == 1) return NULL;
    char encoded[16389];
    unsigned char decoded[16389];
    for (size_t i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)start[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')) return NULL;
        if (ch == '-') encoded[i] = '+';
        else if (ch == '_') encoded[i] = '/';
        else encoded[i] = (char)ch;
    }
    size_t padded = length;
    while (padded % 4) encoded[padded++] = '=';
    encoded[padded] = 0;
    int count = EVP_DecodeBlock(decoded, (unsigned char *)encoded, (int)padded);
    if (count < 0 || count < (int)(padded - length)) return NULL;
    count -= (int)(padded - length);
    decoded[count] = 0;
    return json_parse((const char *)decoded, (size_t)count);
}

static size_t oauth_write(char *data, size_t size, size_t count, void *user) {
    byte_buffer *body = user;
    if (size && count > SIZE_MAX / size) return 0;
    size_t length = size * count;
    if (buffer_append(body, data, length, OAUTH_BODY_MAX)) return 0;
    return length;
}

static int oauth_progress(void *user, curl_off_t total, curl_off_t now,
                           curl_off_t total_up, curl_off_t now_up) {
    (void)total;
    (void)now;
    (void)total_up;
    (void)now_up;
    return oauth_cancelled(user);
}

/* The URL parameter is an internal loopback seam for offline tests. */
static cJSON *oauth_post(const char *url, const char *body, const char *content_type,
                         const oca_auth_ui *ui, char *err, size_t errsz) {
    if (oauth_cancelled(ui)) {
        err_set(err, errsz, "OpenAI login canceled");
        return NULL;
    }
    pthread_once(&curl_once, curl_init);
    CURL *curl = curl_status == CURLE_OK ? curl_easy_init() : NULL;
    if (!curl) {
        err_set(err, errsz, "cannot initialize OAuth transport");
        return NULL;
    }
    byte_buffer response_body = {0};
    struct curl_slist *headers = curl_slist_append(NULL, content_type);
    int setup = headers != NULL;
    if (!configure_http_post(curl, url, body, headers, &OAUTH_TRANSPORT_LIMITS)) setup = 0;
#define OAUTH_OPT(option, value) do { if (curl_easy_setopt(curl, option, value) != CURLE_OK) setup = 0; } while (0)
    OAUTH_OPT(CURLOPT_WRITEFUNCTION, oauth_write);
    OAUTH_OPT(CURLOPT_WRITEDATA, &response_body);
    OAUTH_OPT(CURLOPT_XFERINFOFUNCTION, oauth_progress);
    OAUTH_OPT(CURLOPT_XFERINFODATA, ui);
    OAUTH_OPT(CURLOPT_PROTOCOLS_STR, !strcmp(url, OPENAI_TOKEN_URL) ? "https" : "http");
    if (strcmp(url, OPENAI_TOKEN_URL)) OAUTH_OPT(CURLOPT_NOPROXY, "*");
#undef OAUTH_OPT
    CURLcode result = setup ? curl_easy_perform(curl) : CURLE_FAILED_INIT;
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    cJSON *response = NULL;
    if (oauth_cancelled(ui)) err_set(err, errsz, "OpenAI login canceled");
    else if (result != CURLE_OK) err_set(err, errsz, "OpenAI token transfer failed or timed out");
    else if (status != 200) err_set(err, errsz, "OpenAI token exchange failed (HTTP %ld); run oca logout openai, then oca -m MODEL in an interactive terminal", status);
    else {
        response = json_parse(response_body.data, response_body.len);
        if (!cJSON_IsObject(response) || cJSON_GetObjectItemCaseSensitive(response, "error")) {
            cJSON_Delete(response);
            response = NULL;
            err_set(err, errsz, "OpenAI returned an invalid token response");
        }
    }
    secret_free(response_body.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

static int oauth_store_response(oca_llm *llm, const cJSON *response,
                                  const auth_member *previous, char *err, size_t errsz) {
    const char *access = json_text(response, "access_token");
    const char *refresh = json_text(response, "refresh_token");
    const char *id_token = json_text(response, "id_token");
    if (!refresh && previous) refresh = previous->refresh_token;
    if (!token_valid(access, 16384) || !token_valid(refresh, 16384)) {
        err_set(err, errsz, "OpenAI returned incomplete OAuth credentials");
        return -1;
    }
    cJSON *claims = oauth_claims(access);
    cJSON *identity = id_token && token_valid(id_token, 16384) ? oauth_claims(id_token) : NULL;
    const cJSON *expiry = cJSON_GetObjectItemCaseSensitive(claims, "exp");
    const cJSON *access_account = cJSON_GetObjectItemCaseSensitive(claims, "https://api.openai.com/auth");
    const cJSON *identity_account = cJSON_GetObjectItemCaseSensitive(identity, "https://api.openai.com/auth");
    const char *account = json_text(access_account, "chatgpt_account_id");
    if (!account) account = json_text(identity_account, "chatgpt_account_id");
    if (!account && previous) account = previous->account_id;
    int valid = token_valid(account, 256) && cJSON_IsNumber(expiry) && isfinite(expiry->valuedouble) &&
                expiry->valuedouble > (double)time(NULL) && expiry->valuedouble <= 253402300799.0 &&
                floor(expiry->valuedouble) == expiry->valuedouble;
    if (valid && previous && strcmp(account, previous->account_id)) valid = 0;
    auth_member member = {0};
    if (valid) {
        member.provider = strdup("openai");
        member.access_token = strdup(access);
        member.refresh_token = strdup(refresh);
        member.account_id = strdup(account);
        member.expires_at = (int64_t)expiry->valuedouble;
        valid = member.provider && oauth_valid(&member);
    }
    cJSON_Delete(claims);
    cJSON_Delete(identity);
    int rc = -1;
    if (!valid) err_set(err, errsz, "OpenAI returned invalid account or expiration data");
    else rc = store_member(llm, &member, err, errsz);
    member_free(&member);
    return rc;
}

static int openai_refresh_locked(oca_llm *llm, int force, const oca_auth_ui *ui,
                           const char *token_url, char *err, size_t errsz) {
    const auth_member *member = find_member(llm, "openai");
    if (!oauth_valid(member)) {
        err_set(err, errsz, "OpenAI needs OAuth; run oca logout openai, then oca -m MODEL in an interactive terminal");
        return -1;
    }
    time_t now = time(NULL);
    if (!force && now >= 0 && member->expires_at > (int64_t)now + 60) return 0;
    cJSON *request = cJSON_CreateObject();
    int valid = request && cJSON_AddStringToObject(request, "client_id", OPENAI_CLIENT_ID) &&
                cJSON_AddStringToObject(request, "grant_type", "refresh_token") &&
                cJSON_AddStringToObject(request, "refresh_token", member->refresh_token);
    char *body = valid ? cJSON_PrintUnformatted(request) : NULL;
    cJSON_Delete(request);
    if (!body) {
        err_set(err, errsz, "cannot prepare token refresh");
        return -1;
    }
    cJSON *response = oauth_post(token_url, body, "Content-Type: application/json", ui, err, errsz);
    secret_free(body);
    int rc = response ? oauth_store_response(llm, response, member, err, errsz) : -1;
    cJSON_Delete(response);
    return rc;
}

static int openai_refresh(oca_llm *llm, int force, const oca_auth_ui *ui,
                           const char *token_url, char *err, size_t errsz) {
    pthread_mutex_lock(&oauth_refresh_mutex);
    int rc = openai_refresh_locked(llm, force, ui, token_url, err, errsz);
    pthread_mutex_unlock(&oauth_refresh_mutex);
    return rc;
}

static int hex_value(unsigned char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static char *oauth_unescape(const char *text, size_t length) {
    char *result = malloc(length + 1);
    if (!result) return NULL;
    size_t used = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == '%') {
            if (i + 2 >= length || hex_value(text[i + 1]) < 0 || hex_value(text[i + 2]) < 0) goto invalid;
            ch = (unsigned char)((hex_value(text[i + 1]) << 4) | hex_value(text[i + 2]));
            i += 2;
        } else if (ch == '+') ch = ' ';
        if (ch < 0x20 || ch >= 0x7f) goto invalid;
        result[used++] = (char)ch;
    }
    result[used] = 0;
    return result;
invalid:
    memset(result, 0, length + 1);
    free(result);
    return NULL;
}

/* Returns an owned code only for the expected state. Unknown query fields
 * are ignored; duplicate security fields and provider errors are rejected. */
static char *oauth_callback(char *request, const char *expected_state, int *denied) {
    *denied = 0;
    if (strncmp(request, "GET /auth/callback?", 19)) return NULL;
    char *end = strstr(request + 19, " HTTP/1.");
    if (!end || !strstr(end, "\r\n\r\n")) return NULL;
    *end = 0;
    char *state = NULL, *code = NULL;
    int valid = 1, has_error = 0;
    for (char *part = request + 19; part && *part;) {
        char *next = strchr(part, '&');
        if (next) *next++ = 0;
        char *equal = strchr(part, '=');
        if (!equal) {
            valid = 0;
            break;
        }
        *equal++ = 0;
        char *name = oauth_unescape(part, strlen(part));
        char *value = oauth_unescape(equal, strlen(equal));
        if (!name || !value) valid = 0;
        else if (!strcmp(name, "state")) {
            if (state) valid = 0;
            else {
                state = value;
                value = NULL;
            }
        } else if (!strcmp(name, "code")) {
            if (code) valid = 0;
            else {
                code = value;
                value = NULL;
            }
        } else if (!strcmp(name, "error")) has_error = 1;
        free(name);
        secret_free(value);
        if (!valid) break;
        part = next;
    }
    *denied = valid && state && !strcmp(state, expected_state) && has_error;
    valid = valid && !has_error && state && !strcmp(state, expected_state) && token_valid(code, 4096);
    secret_free(state);
    if (!valid) {
        secret_free(code);
        return NULL;
    }
    return code;
}

static int oauth_listener(unsigned short *port) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return -1;
    if (fcntl(listener, F_SETFD, FD_CLOEXEC) || fcntl(listener, F_SETFL, O_NONBLOCK)) goto failed;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    address.sin_port = htons(*port);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 8)) goto failed;
    socklen_t length = sizeof(address);
    if (getsockname(listener, (struct sockaddr *)&address, &length)) goto failed;
    *port = ntohs(address.sin_port);
    return listener;
failed:
    close(listener);
    return -1;
}

static char *oauth_receive(int listener, const char *state, const oca_auth_ui *ui,
                           int64_t deadline, char *err, size_t errsz) {
    for (int attempts = 0; attempts < 128 && oauth_before_deadline(deadline);) {
        if (oauth_cancelled(ui)) break;
        struct pollfd pending = {.fd = listener, .events = POLLIN};
        int ready = poll(&pending, 1, 100);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) break;
        if (!ready) continue;
        int peer = accept(listener, NULL, NULL);
        if (peer < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (peer < 0) break;
        attempts++;
        if (fcntl(peer, F_SETFD, FD_CLOEXEC) || fcntl(peer, F_SETFL, O_NONBLOCK)) {
            close(peer);
            break;
        }
        char request[16385] = {0};
        size_t used = 0;
        int64_t peer_deadline = monotonic_ms() + 3000;
        if (peer_deadline > deadline) peer_deadline = deadline;
        while (used + 1 < sizeof(request) && oauth_before_deadline(peer_deadline) && !oauth_cancelled(ui)) {
            struct pollfd input = {.fd = peer, .events = POLLIN};
            ready = poll(&input, 1, 100);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0) break;
            if (!ready) continue;
            ssize_t count = read(peer, request + used, sizeof(request) - used - 1);
            if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (count <= 0) break;
            used += (size_t)count;
            if (strstr(request, "\r\n\r\n")) break;
        }
        int denied = 0;
        char *code = oauth_callback(request, state, &denied);
        const char *reply = code ?
            "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\nCache-Control: no-store\r\n\r\nReturn to OCA to complete sign-in.\n" :
            "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        (void)send(peer, reply, strlen(reply), MSG_NOSIGNAL);
        close(peer);
        memset(request, 0, sizeof(request));
        if (denied) {
            err_set(err, errsz, "OpenAI login canceled by the provider");
            return NULL;
        }
        if (code) return code;
    }
    err_set(err, errsz, oauth_cancelled(ui) ? "OpenAI login canceled" : "OpenAI login timed out or callback failed");
    return NULL;
}

static int openai_login(oca_llm *llm, const oca_auth_ui *ui, const char *issuer,
                         const char *token_url, unsigned short port, char *err, size_t errsz) {
    if (!ui || !ui->open_browser) {
        err_set(err, errsz, "OpenAI login requires browser presentation");
        return -1;
    }
    if (oauth_cancelled(ui)) {
        err_set(err, errsz, "OpenAI login canceled");
        return -1;
    }
    pthread_once(&curl_once, curl_init);
    if (curl_status != CURLE_OK) {
        err_set(err, errsz, "cannot initialize OAuth transport");
        return -1;
    }
    char verifier[65], challenge[65], state[65];
    if (oauth_secrets(verifier, challenge, state)) {
        err_set(err, errsz, "cannot create OAuth secrets");
        return -1;
    }
    int listener = oauth_listener(&port);
    if (listener < 0 && port == 1455) {
        port = 1457;
        listener = oauth_listener(&port);
    }
    if (listener < 0) {
        err_set(err, errsz, "cannot bind the OpenAI callback; close other login attempts and retry");
        return -1;
    }
    char redirect[128];
    snprintf(redirect, sizeof(redirect), "http://localhost:%u/auth/callback", port);
    char *encoded = curl_easy_escape(NULL, redirect, 0);
    char url[2048];
    int count = encoded ? snprintf(url, sizeof(url),
        "%s/oauth/authorize?response_type=code&client_id=%s&redirect_uri=%s"
        "&scope=openid%%20profile%%20email%%20offline_access&code_challenge=%s"
        "&code_challenge_method=S256&id_token_add_organizations=true"
        "&codex_cli_simplified_flow=true&state=%s&originator=oca",
        issuer, OPENAI_CLIENT_ID, encoded, challenge, state) : -1;
    curl_free(encoded);
    char *code = NULL;
    if (count < 0 || (size_t)count >= sizeof(url)) err_set(err, errsz, "cannot prepare the OpenAI login URL");
    else if (ui->open_browser(ui->user, url)) err_set(err, errsz, "OpenAI login canceled");
    else code = oauth_receive(listener, state, ui, monotonic_ms() + 300000, err, errsz);
    close(listener);
    int rc = -1;
    if (code && !oauth_cancelled(ui)) {
        char *encoded_code = curl_easy_escape(NULL, code, 0);
        char *encoded_redirect = curl_easy_escape(NULL, redirect, 0);
        size_t capacity = encoded_code && encoded_redirect ? strlen(encoded_code) + strlen(encoded_redirect) + 512 : 0;
        char *body = capacity ? malloc(capacity) : NULL;
        if (body) {
            snprintf(body, capacity, "grant_type=authorization_code&code=%s&redirect_uri=%s&client_id=%s&code_verifier=%s",
                     encoded_code, encoded_redirect, OPENAI_CLIENT_ID, verifier);
            cJSON *response = oauth_post(token_url, body, "Content-Type: application/x-www-form-urlencoded", ui, err, errsz);
            if (response) rc = oauth_store_response(llm, response, NULL, err, errsz);
            cJSON_Delete(response);
        } else err_set(err, errsz, "cannot prepare the OpenAI token exchange");
        secret_free(body);
        curl_free(encoded_code);
        curl_free(encoded_redirect);
    } else if (code) err_set(err, errsz, "OpenAI login canceled");
    secret_free(code);
    memset(verifier, 0, sizeof(verifier));
    return rc;
}

int oca_llm_auth_login(oca_llm *llm, const char *provider, const oca_auth_ui *ui,
                       char *err, size_t errsz) {
    if (!llm || !provider || strcmp(provider, "openai")) {
        err_set(err, errsz, "provider browser login is not available");
        return -1;
    }
    return openai_login(llm, ui, OPENAI_ISSUER, OPENAI_TOKEN_URL, 1455, err, errsz);
}

/* ============================================================================
 * OpenAI Responses adapter
 * ============================================================================ */

#define OPENAI_RESPONSES_URL "https://chatgpt.com/backend-api/codex/responses"
#define OPENAI_ITEMS_MAX 128
#define OPENAI_FRAME_MAX (4u * 1024u * 1024u)
#define OPENAI_WIRE_MAX (16u * 1024u * 1024u)

static char *openai_serialize(const oca_request *request) {
    int ok = 1;
    cJSON *root = cJSON_CreateObject();
    json_add(root, "model", cJSON_CreateString(request->model->id), &ok);
    json_add(root, "stream", cJSON_CreateBool(1), &ok);
    json_add(root, "store", cJSON_CreateBool(0), &ok);
    const char *level = oca_model_reasoning_name(request->model, request->effort);
    if (level) {
        cJSON *reasoning = json_add(root, "reasoning", cJSON_CreateObject(), &ok);
        json_add(reasoning, "effort", cJSON_CreateString(level), &ok);
        json_add(reasoning, "summary", cJSON_CreateString("auto"), &ok);
        cJSON *include = json_add(root, "include", cJSON_CreateArray(), &ok);
        json_add(include, NULL, cJSON_CreateString("reasoning.encrypted_content"), &ok);
    }
    if (request->cache_key) {
        if (!token_valid(request->cache_key, 128)) ok = 0;
        json_add(root, "prompt_cache_key", cJSON_CreateString(request->cache_key), &ok);
    }
    byte_buffer instructions = {0};
    cJSON *input = json_add(root, "input", cJSON_CreateArray(), &ok);
    static const char *const roles[] = {"system", "user", "assistant", "tool"};
    for (size_t i = 0; ok && i < request->msg_count; i++) {
        const req_msg *message = &request->msgs[i];
        if (message->role == OCA_MSG_SYSTEM) {
            if (instructions.len && buffer_append(&instructions, "\n\n", 2, LLM_BODY_MAX)) ok = 0;
            if (buffer_append(&instructions, message->text, strlen(message->text), LLM_BODY_MAX)) ok = 0;
            continue;
        }
        if (message->role == OCA_MSG_TOOL) {
            cJSON *item = json_add(input, NULL, cJSON_CreateObject(), &ok);
            json_add(item, "type", cJSON_CreateString("function_call_output"), &ok);
            json_add(item, "call_id", cJSON_CreateString(message->tool_call_id), &ok);
            json_add(item, "output", cJSON_CreateString(message->text), &ok);
            continue;
        }
        if (message->state) {
            cJSON *state = strlen(message->state) <= OCA_LLM_STATE_MAX_BYTES ?
                           json_parse(message->state, strlen(message->state)) : NULL;
            if (!cJSON_IsArray(state)) ok = 0;
            const cJSON *item;
            cJSON_ArrayForEach(item, state) {
                const char *type = json_text(item, "type");
                if (!type || strcmp(type, "reasoning") || !json_text(item, "encrypted_content")) ok = 0;
                if (ok) json_add(input, NULL, cJSON_Duplicate(item, 1), &ok);
            }
            cJSON_Delete(state);
        }
        if (*message->text || (!message->call_count && !message->state)) {
            cJSON *item = json_add(input, NULL, cJSON_CreateObject(), &ok);
            json_add(item, "role", cJSON_CreateString(roles[message->role]), &ok);
            json_add(item, "content", cJSON_CreateString(message->text), &ok);
        }
        for (size_t j = 0; ok && j < message->call_count; j++) {
            const req_call *call = &message->calls[j];
            cJSON *item = json_add(input, NULL, cJSON_CreateObject(), &ok);
            json_add(item, "type", cJSON_CreateString("function_call"), &ok);
            json_add(item, "call_id", cJSON_CreateString(call->id), &ok);
            json_add(item, "name", cJSON_CreateString(call->name), &ok);
            json_add(item, "arguments", cJSON_CreateString(call->args), &ok);
        }
    }
    json_add(root, "instructions", cJSON_CreateString(instructions.data ? instructions.data : ""), &ok);
    buffer_free(&instructions);
    if (request->tool_count) {
        cJSON *tools = json_add(root, "tools", cJSON_CreateArray(), &ok);
        for (size_t i = 0; ok && i < request->tool_count; i++) {
            const req_tool *tool = &request->tools[i];
            cJSON *item = json_add(tools, NULL, cJSON_CreateObject(), &ok);
            json_add(item, "type", cJSON_CreateString("function"), &ok);
            json_add(item, "name", cJSON_CreateString(tool->name), &ok);
            json_add(item, "description", cJSON_CreateString(tool->description), &ok);
            json_add(item, "strict", cJSON_CreateBool(0), &ok);
            cJSON *schema = json_parse(tool->args_json, strlen(tool->args_json));
            if (!cJSON_IsObject(schema)) ok = 0;
            json_add(item, "parameters", schema, &ok);
        }
        json_add(root, "tool_choice", cJSON_CreateString("auto"), &ok);
        json_add(root, "parallel_tool_calls", cJSON_CreateBool(1), &ok);
    }
    char *body = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (body && (strlen(body) > LLM_BODY_MAX || !utf8_text_valid(body, strlen(body)))) {
        cJSON_free(body);
        body = NULL;
    }
    return body;
}

typedef struct {
    cJSON *value;
    byte_buffer text, arguments;
    int done;
} openai_item;

typedef struct {
    const oca_request *request;
    oca_event_fn *callback;
    void *user;
    CURL *curl;
    byte_buffer line, data, error_body;
    openai_item items[OPENAI_ITEMS_MAX];
    size_t item_count, text_bytes, wire_bytes;
    int started, completed, cancelled, has_usage;
    oca_event usage;
    cJSON *state;
    const char *category, *message;
    char parameter_error[160];
    oca_llm_diagnostic diagnostic;
    transport_watch transport;
} openai_run;

static void openai_fail(openai_run *run, const char *category, const char *message) {
    if (!run->category) {
        run->category = category;
        run->message = message;
    }
}

static int openai_poll(void *user) {
    openai_run *run = user;
    if (run->request->cancel_poll && run->request->cancel_poll(run->request->cancel_user)) run->cancelled = 1;
    return run->cancelled;
}

static int openai_emit(openai_run *run, oca_event *event) {
    event->request_id = run->request->request_id;
    if (run->callback(event, run->user)) run->cancelled = 1;
    return run->cancelled;
}

static int openai_text(openai_run *run, openai_item *item, const char *text, int reasoning) {
    size_t length = strlen(text);
    if (length > CHAT_RESPONSE_MAX - run->text_bytes)
        return diagnostic_limit(&run->diagnostic, "response_bytes", run->text_bytes + length, CHAT_RESPONSE_MAX);
    run->text_bytes += length;
    if (!reasoning && diagnostic_append(&run->diagnostic, &item->text, text, length,
                                        CHAT_RESPONSE_MAX, "item.text_bytes")) return -1;
    oca_event event = {.kind = reasoning ? OCA_EV_REASONING_DELTA : OCA_EV_TEXT_DELTA, .text = text};
    return openai_emit(run, &event) ? -1 : 0;
}

static int openai_usage(openai_run *run, const cJSON *usage) {
    if (!usage || cJSON_IsNull(usage)) return 0;
    oca_llm_diagnostic *d = &run->diagnostic;
    if (!cJSON_IsObject(usage)) return diagnostic_field(d, "field_type", "usage", "object", usage, 0);
    oca_event *event = &run->usage;
    event->kind = OCA_EV_USAGE;
    const struct { const cJSON *parent; const char *field; int *present; long *count; } counters[] = {
        {usage, "input_tokens", &event->has_input, &event->input_tokens},
        {usage, "output_tokens", &event->has_output, &event->output_tokens},
        {usage, "total_tokens", &event->has_total, &event->total_tokens},
        {cJSON_GetObjectItemCaseSensitive(usage, "input_tokens_details"), "cached_tokens", &event->has_cache_hit, &event->cache_hit_tokens},
        {cJSON_GetObjectItemCaseSensitive(usage, "output_tokens_details"), "reasoning_tokens", &event->has_reasoning_tokens, &event->reasoning_tokens}
    };
    for (size_t i = 0; i < sizeof(counters) / sizeof(counters[0]); i++) {
        if (chat_count(counters[i].parent, counters[i].field, counters[i].present, counters[i].count))
            return diagnostic_field(d, "usage_counter", counters[i].field,
                "consistent nonnegative integer", cJSON_GetObjectItemCaseSensitive(counters[i].parent, counters[i].field), 0);
    }
    if (event->has_input && event->has_cache_hit) {
        if (event->cache_hit_tokens > event->input_tokens) {
            char expected[64];
            snprintf(expected, sizeof(expected), "at most %ld input tokens", event->input_tokens);
            return diagnostic_field(d, "usage_counter", "cached_tokens", expected,
                                    cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(usage, "input_tokens_details"), "cached_tokens"), 0);
        }
        event->has_cache_miss = 1;
        event->cache_miss_tokens = event->input_tokens - event->cache_hit_tokens;
    }
    run->has_usage = 1;
    return 0;
}

static int openai_item_identity(openai_run *run, const cJSON *item) {
    const char *id = json_text(item, "id");
    const char *type = json_text(item, "type");
    if (!token_valid(id, 256)) {
        diagnostic_field(&run->diagnostic, "item_identity", "item.id", "1..256 printable ASCII bytes", cJSON_GetObjectItemCaseSensitive(item, "id"), 1);
        return 0;
    }
    if (!type || (strcmp(type, "message") && strcmp(type, "reasoning") && strcmp(type, "function_call"))) {
        diagnostic_field(&run->diagnostic, "item_type", "item.type", "message | reasoning | function_call", cJSON_GetObjectItemCaseSensitive(item, "type"), 1);
        return 0;
    }
    return 1;
}

static int openai_item_finish(openai_run *run, size_t index, const cJSON *value) {
    oca_llm_diagnostic *d = &run->diagnostic;
    d->has_item_index = 1;
    d->item_index = (long)index;
    if (index >= OPENAI_ITEMS_MAX)
        return diagnostic_set(d, "item_index", "output_index", "catalog item range", "out of range");
    if (!openai_item_identity(run, value)) return -1;
    openai_item *item = &run->items[index];
    if (!item->value) {
        for (size_t i = 0; i < run->item_count; i++) {
            if (run->items[i].value && !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(run->items[i].value, "id")),
                                                json_text(value, "id")))
                return diagnostic_field(d, "duplicate_item", "item.id", "unique item id", cJSON_GetObjectItemCaseSensitive(value, "id"), 1);
        }
    } else {
        const char *fields[] = {"id", "type"};
        for (size_t i = 0; i < 2; i++) {
            const char *previous = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item->value, fields[i]));
            if (strcmp(previous, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(value, fields[i])))) {
                char expected[OCA_LLM_DIAGNOSTIC_TEXT_MAX];
                diagnostic_token(expected, previous);
                return diagnostic_field(d, "item_identity", i ? "item.type" : "item.id", expected,
                                        cJSON_GetObjectItemCaseSensitive(value, fields[i]), 1);
            }
        }
    }
    const char *type = json_text(value, "type");
    if (!strcmp(type, "message")) {
        const char *role = json_text(value, "role");
        const cJSON *content = cJSON_GetObjectItemCaseSensitive(value, "content");
        if (!role || strcmp(role, "assistant"))
            return diagnostic_field(d, "field_value", "item.role", "assistant", cJSON_GetObjectItemCaseSensitive(value, "role"), 1);
        if (!cJSON_IsArray(content)) return diagnostic_field(d, "field_type", "item.content", "array", content, 0);
        byte_buffer final = {0};
        const cJSON *part;
        int ok = 1;
        cJSON_ArrayForEach(part, content) {
            const char *part_type = json_text(part, "type");
            const char *text = json_text(part, "text");
            if (!part_type || strcmp(part_type, "output_text"))
                ok = !diagnostic_field(d, "field_value", "content[].type", "output_text", cJSON_GetObjectItemCaseSensitive(part, "type"), 1);
            else if (!text) ok = !diagnostic_field(d, "field_type", "content[].text", "string", cJSON_GetObjectItemCaseSensitive(part, "text"), 0);
            else if (diagnostic_append(d, &final, text, strlen(text), CHAT_RESPONSE_MAX, "item.text_bytes")) ok = 0;
            if (!ok) break;
        }
        if (ok && item->text.len && strcmp(item->text.data, final.data ? final.data : ""))
            ok = !diagnostic_set(d, "text_mismatch", "item.content", "same streamed text", "different completed text");
        if (ok && !item->text.len && final.len && openai_text(run, item, final.data, 0)) ok = 0;
        buffer_free(&final);
        if (!ok) return -1;
    } else if (!strcmp(type, "function_call")) {
        const char *id = json_text(value, "call_id");
        const char *name = json_text(value, "name");
        const char *args = json_text(value, "arguments");
        if (!token_valid(id, 256))
            return diagnostic_field(d, "field_value", "item.call_id", "1..256 printable ASCII bytes", cJSON_GetObjectItemCaseSensitive(value, "call_id"), 1);
        if (!token_valid(name, 64))
            return diagnostic_field(d, "field_value", "item.name", "1..64 printable ASCII bytes", cJSON_GetObjectItemCaseSensitive(value, "name"), 1);
        if (!args) return diagnostic_field(d, "field_type", "item.arguments", "string", cJSON_GetObjectItemCaseSensitive(value, "arguments"), 0);
        if (strlen(args) > CHAT_ARGUMENTS_MAX) return diagnostic_limit(d, "item.arguments_bytes", strlen(args), CHAT_ARGUMENTS_MAX);
        int known = 0;
        for (size_t i = 0; i < run->request->tool_count; i++)
            if (!strcmp(name, run->request->tools[i].name)) known = 1;
        if (!known) return diagnostic_field(d, "unknown_tool", "item.name", "a requested tool", cJSON_GetObjectItemCaseSensitive(value, "name"), 1);
        if (item->arguments.len && strcmp(item->arguments.data, args))
            return diagnostic_set(d, "arguments_mismatch", "item.arguments", "same streamed arguments", "different completed arguments");
        cJSON *json = json_parse(args, strlen(args));
        int valid = cJSON_IsObject(json);
        cJSON_Delete(json);
        if (!valid) return diagnostic_set(d, "arguments_json", "item.arguments", "valid JSON object", "invalid JSON object");
    } else {
        const cJSON *encrypted = cJSON_GetObjectItemCaseSensitive(value, "encrypted_content");
        if (encrypted && !cJSON_IsNull(encrypted) && !cJSON_GetStringValue(encrypted))
            return diagnostic_field(d, "field_type", "item.encrypted_content", "string or null", encrypted, 0);
    }
    {
        cJSON *copy = cJSON_Duplicate(value, 1);
        if (!copy) return diagnostic_set(d, "allocation", "item", "copied item", "allocation failed");
        cJSON_Delete(item->value);
        item->value = copy;
        item->done = 1;
    }
    if (index >= run->item_count) run->item_count = index + 1;
    return 0;
}

static int openai_index(openai_run *run, const cJSON *root, long *index) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "output_index");
    if (json_count(value, index) || *index >= OPENAI_ITEMS_MAX) {
        char expected[64];
        snprintf(expected, sizeof(expected), "integer from 0 through %d", OPENAI_ITEMS_MAX - 1);
        return diagnostic_field(&run->diagnostic, "item_index", "output_index", expected, value, 0);
    }
    run->diagnostic.has_item_index = 1;
    run->diagnostic.item_index = *index;
    return 0;
}

static int openai_chunk(openai_run *run, const char *data, size_t length) {
    oca_llm_diagnostic *d = &run->diagnostic;
    d->event_index++;
    d->frame_bytes = length;
    d->has_item_index = 0;
    if (length == 6 && !memcmp(data, "[DONE]", 6)) {
        snprintf(d->provider_event, sizeof(d->provider_event), "[DONE]");
        return run->completed ? 0 : diagnostic_set(d, "event_order", "response.completed", "before [DONE]", "missing");
    }
    cJSON *root = json_parse(data, length);
    const char *type = json_text(root, "type");
    diagnostic_token(d->provider_event, type);
    int ok = 1;
#define OPENAI_CHECK(condition, check, field, expected, value, metadata) do { \
        if (!(condition)) { \
            diagnostic_field(d, check, field, expected, value, metadata); \
            ok = 0; goto done; \
        } \
    } while (0)
    if (!root) {
        diagnostic_set(d, "frame_json", "frame", "valid bounded JSON", "invalid JSON, encoding, or structure");
        ok = 0;
        goto done;
    }
    OPENAI_CHECK(type, "field_type", "type", "event name string", cJSON_GetObjectItemCaseSensitive(root, "type"), 0);
    OPENAI_CHECK(!run->completed, "event_order", "type", "no event after completion", cJSON_GetObjectItemCaseSensitive(root, "type"), 1);
    if (!strcmp(type, "error") || !strcmp(type, "response.failed") || !strcmp(type, "response.incomplete")) {
        diagnostic_field(d, "provider_completion", "type", "successful completion", cJSON_GetObjectItemCaseSensitive(root, "type"), 1);
        openai_fail(run, !strcmp(type, "response.incomplete") ? "output_limit" : "provider",
                    "OpenAI did not complete the response");
        ok = 0;
    } else if (!strcmp(type, "response.created") || !strcmp(type, "response.in_progress")) {
        const cJSON *response = cJSON_GetObjectItemCaseSensitive(root, "response");
        const char *model = json_text(response, "model");
        OPENAI_CHECK(model && !strcmp(model, run->request->model->id), "model_mismatch", "response.model",
                     run->request->model->id, cJSON_GetObjectItemCaseSensitive(response, "model"), 1);
        if (!run->started) {
            oca_event event = {.kind = OCA_EV_RESPONSE_START, .model_id = run->request->model->id};
            run->started = 1;
            if (openai_emit(run, &event)) ok = 0;
        } else OPENAI_CHECK(strcmp(type, "response.created"), "duplicate_start", "type",
                            "one response.created event", cJSON_GetObjectItemCaseSensitive(root, "type"), 1);
    } else if (!strcmp(type, "keepalive")) {
        /* A heartbeat can precede the first response event. */
    } else {
        OPENAI_CHECK(run->started, "event_order", "type", "response.created or response.in_progress first", cJSON_GetObjectItemCaseSensitive(root, "type"), 1);
        if (!strcmp(type, "response.completed") || !strcmp(type, "response.done")) {
            const cJSON *response = cJSON_GetObjectItemCaseSensitive(root, "response");
            const char *model = json_text(response, "model");
            const char *status = json_text(response, "status");
            const cJSON *output = cJSON_GetObjectItemCaseSensitive(response, "output");
            int count = cJSON_GetArraySize(output);
            OPENAI_CHECK(model && !strcmp(model, run->request->model->id), "model_mismatch", "response.model",
                         run->request->model->id, cJSON_GetObjectItemCaseSensitive(response, "model"), 1);
            OPENAI_CHECK(status && !strcmp(status, "completed"), "response_status", "response.status", "completed", cJSON_GetObjectItemCaseSensitive(response, "status"), 1);
            OPENAI_CHECK(cJSON_IsArray(output), "field_type", "response.output", "array", output, 0);
            /* Empty output retains streamed items. Delivery still requires every
             * retained item to have passed completion validation. */
            if (count > OPENAI_ITEMS_MAX || (count > 0 && (size_t)count < run->item_count)) {
                char expected[64], received[32];
                snprintf(expected, sizeof(expected), "%zu..%d items", run->item_count, OPENAI_ITEMS_MAX);
                snprintf(received, sizeof(received), "%d items", count);
                diagnostic_set(d, "item_count", "response.output", expected, received);
                ok = 0;
            }
            for (int i = 0; ok && i < count; i++)
                if (openai_item_finish(run, (size_t)i, cJSON_GetArrayItem(output, i))) ok = 0;
            if (ok && openai_usage(run, cJSON_GetObjectItemCaseSensitive(response, "usage"))) ok = 0;
            if (ok) run->completed = 1;
        } else if (!strcmp(type, "response.output_item.added") || !strcmp(type, "response.output_item.done")) {
            long index;
            const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "item");
            if (openai_index(run, root, &index) || !openai_item_identity(run, item)) {
                ok = 0;
                goto done;
            }
            if (!strcmp(type, "response.output_item.done")) {
                if (openai_item_finish(run, (size_t)index, item)) ok = 0;
            } else {
                OPENAI_CHECK(!run->items[index].value, "duplicate_item", "output_index", "unused index", cJSON_GetObjectItemCaseSensitive(root, "output_index"), 0);
                for (size_t i = 0; i < run->item_count; i++) {
                    OPENAI_CHECK(!run->items[i].value || strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(run->items[i].value, "id")),
                                 json_text(item, "id")), "duplicate_item", "item.id", "unique item id", cJSON_GetObjectItemCaseSensitive(item, "id"), 1);
                }
                run->items[index].value = cJSON_Duplicate(item, 1);
                if (!run->items[index].value) {
                    diagnostic_set(d, "allocation", "item", "copied item", "allocation failed");
                    ok = 0;
                }
                if ((size_t)index >= run->item_count) run->item_count = (size_t)index + 1;
            }
        } else if (!strcmp(type, "response.output_text.delta") ||
                   !strcmp(type, "response.reasoning_summary_text.delta") ||
                   !strcmp(type, "response.function_call_arguments.delta")) {
            long index;
            const char *id = json_text(root, "item_id");
            const char *delta = json_text(root, "delta");
            if (openai_index(run, root, &index)) {
                ok = 0;
                goto done;
            }
            OPENAI_CHECK(run->items[index].value, "item_order", "output_index", "previously added item", cJSON_GetObjectItemCaseSensitive(root, "output_index"), 0);
            OPENAI_CHECK(!run->items[index].done, "item_order", "output_index", "unfinished item", cJSON_GetObjectItemCaseSensitive(root, "output_index"), 0);
            char expected_id[OCA_LLM_DIAGNOSTIC_TEXT_MAX];
            diagnostic_token(expected_id, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(run->items[index].value, "id")));
            OPENAI_CHECK(id && !strcmp(id, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(run->items[index].value, "id"))),
                         "item_identity", "item_id", expected_id, cJSON_GetObjectItemCaseSensitive(root, "item_id"), 1);
            OPENAI_CHECK(delta, "field_type", "delta", "string", cJSON_GetObjectItemCaseSensitive(root, "delta"), 0);
            openai_item *item = &run->items[index];
            const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item->value, "type"));
            if (!strcmp(type, "response.function_call_arguments.delta")) {
                OPENAI_CHECK(!strcmp(kind, "function_call"), "item_type", "item.type", "function_call", cJSON_GetObjectItemCaseSensitive(item->value, "type"), 1);
                if (diagnostic_append(d, &item->arguments, delta, strlen(delta), CHAT_ARGUMENTS_MAX, "item.arguments_bytes")) ok = 0;
            } else {
                int reasoning = !strcmp(type, "response.reasoning_summary_text.delta");
                OPENAI_CHECK(!strcmp(kind, reasoning ? "reasoning" : "message"), "item_type", "item.type",
                             reasoning ? "reasoning" : "message", cJSON_GetObjectItemCaseSensitive(item->value, "type"), 1);
                if (openai_text(run, item, delta, reasoning)) ok = 0;
            }
        } else if (strcmp(type, "response.output_text.done") && strcmp(type, "response.content_part.added") &&
                   strcmp(type, "response.content_part.done") && strcmp(type, "response.reasoning_summary_part.added") &&
                   strcmp(type, "response.reasoning_summary_part.done") && strcmp(type, "response.reasoning_summary_text.done") &&
                   strcmp(type, "response.function_call_arguments.done") && strcmp(type, "keepalive")) {
            diagnostic_field(d, "event_type", "type", "supported event type", cJSON_GetObjectItemCaseSensitive(root, "type"), 1);
            ok = 0;
        }
    }
done:
#undef OPENAI_CHECK
    cJSON_Delete(root);
    return ok ? 0 : -1;
}

static int openai_line(openai_run *run) {
    size_t length = run->line.len;
    char *line = run->line.data;
    if (length && line[length - 1] == '\r') line[--length] = 0;
    int rc = 0;
    if (!length) {
        if (run->data.len) rc = openai_chunk(run, run->data.data, run->data.len);
        run->data.len = 0;
        if (run->data.data) run->data.data[0] = 0;
    } else if (length >= 5 && !memcmp(line, "data:", 5)) {
        size_t offset = 5 + (length > 5 && line[5] == ' ');
        if (run->data.len && diagnostic_append(&run->diagnostic, &run->data, "\n", 1,
                                               OPENAI_FRAME_MAX, "frame_bytes")) rc = -1;
        if (!rc) rc = diagnostic_append(&run->diagnostic, &run->data, line + offset,
                                        length - offset, OPENAI_FRAME_MAX, "frame_bytes");
        if (rc) {
            snprintf(run->diagnostic.provider_event, sizeof(run->diagnostic.provider_event), "<undecoded>");
            run->diagnostic.event_index++;
            run->diagnostic.frame_bytes = run->data.len + length - offset;
            run->diagnostic.has_item_index = 0;
        }
    }
    run->line.len = 0;
    if (line) line[0] = 0;
    return rc;
}

static size_t openai_write(char *data, size_t size, size_t count, void *user) {
    openai_run *run = user;
    if (size && count > SIZE_MAX / size) {
        diagnostic_set(&run->diagnostic, "size_overflow", "transport_bytes", "representable byte count", "overflow");
        return 0;
    }
    size_t length = size * count;
    if (length) transport_received(&run->transport);
    if (openai_poll(run)) return 0;
    if (length > OPENAI_WIRE_MAX - run->wire_bytes) {
        diagnostic_limit(&run->diagnostic, "wire_bytes", length > SIZE_MAX - run->wire_bytes ? SIZE_MAX : run->wire_bytes + length, OPENAI_WIRE_MAX);
        return 0;
    }
    run->wire_bytes += length;
    if (run->curl) {
        long status = 0;
        if (curl_easy_getinfo(run->curl, CURLINFO_RESPONSE_CODE, &status) != CURLE_OK) return 0;
        if (status != 200) {
            if (diagnostic_append(&run->diagnostic, &run->error_body, data, length, 8192, "http_error_bytes")) return 0;
            return length;
        }
    }
    for (size_t i = 0; i < length; i++) {
        int failed = data[i] == '\n' ? openai_line(run) :
                     diagnostic_append(&run->diagnostic, &run->line, data + i, 1, OPENAI_FRAME_MAX, "line_bytes");
        if (failed) {
            if (data[i] != '\n') {
                snprintf(run->diagnostic.provider_event, sizeof(run->diagnostic.provider_event), "<undecoded>");
                run->diagnostic.event_index++;
                run->diagnostic.frame_bytes = run->data.len + run->line.len + 1;
                run->diagnostic.has_item_index = 0;
            }
            openai_fail(run, "stream", "OpenAI returned an invalid or oversized stream");
            return 0;
        }
    }
    return length;
}

static int openai_progress(void *user, curl_off_t total, curl_off_t now, curl_off_t total_up, curl_off_t now_up) {
    (void)total;
    (void)now;
    (void)total_up;
    (void)now_up;
    return openai_poll(user);
}

static int openai_deliver_items(openai_run *run) {
    oca_llm_diagnostic *d = &run->diagnostic;
    size_t count = 0;
    cJSON *state = cJSON_CreateArray();
    if (!state) return diagnostic_set(d, "allocation", "model_state", "allocated array", "allocation failed");
    run->state = state;
    for (size_t i = 0; i < run->item_count; i++) {
        const cJSON *item = run->items[i].value;
        d->has_item_index = 1;
        d->item_index = (long)i;
        if (!item || !run->items[i].done)
            return diagnostic_set(d, "item_order", "response.output", "completed item", item ? "unfinished item" : "missing item");
        const char *type = json_text(item, "type");
        if (!strcmp(type, "reasoning") && json_text(item, "encrypted_content")) {
            cJSON *copy = cJSON_Duplicate(item, 1);
            if (!copy || !cJSON_AddItemToArray(state, copy)) {
                cJSON_Delete(copy);
                return diagnostic_set(d, "allocation", "model_state", "copied reasoning state", "allocation failed");
            }
        }
        if (strcmp(type, "function_call")) continue;
        if (count >= CHAT_CALLS_MAX) {
            char expected[64], received[64];
            snprintf(expected, sizeof(expected), "at most %d calls", CHAT_CALLS_MAX);
            snprintf(received, sizeof(received), "%zu calls", count + 1);
            return diagnostic_set(d, "tool_count", "response.output", expected, received);
        }
        const char *id = json_text(item, "call_id");
        for (size_t j = 0; j < i; j++) {
            const char *previous = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(run->items[j].value, "call_id"));
            if (previous && !strcmp(id, previous))
                return diagnostic_field(d, "duplicate_call", "item.call_id", "unique call id", cJSON_GetObjectItemCaseSensitive(item, "call_id"), 1);
        }
        count++;
    }
    char *encoded = cJSON_GetArraySize(state) ? cJSON_PrintUnformatted(state) : NULL;
    if ((cJSON_GetArraySize(state) && !encoded) || (encoded && strlen(encoded) > OCA_LLM_STATE_MAX_BYTES)) {
        if (encoded) diagnostic_limit(d, "model_state_bytes", strlen(encoded), OCA_LLM_STATE_MAX_BYTES);
        else diagnostic_set(d, "allocation", "model_state", "serialized state", "allocation failed");
        cJSON_free(encoded);
        return -1;
    }
    count = 0;
    for (size_t i = 0; i < run->item_count; i++) {
        const cJSON *item = run->items[i].value;
        if (strcmp(json_text(item, "type"), "function_call")) continue;
        oca_event event = {.kind = OCA_EV_TOOL_CALL_START, .call_index = (long)count++,
            .call_id = json_text(item, "call_id"), .call_name = json_text(item, "name")};
        if (openai_emit(run, &event)) {
            cJSON_free(encoded);
            return -1;
        }
        event.kind = OCA_EV_TOOL_CALL_ARGUMENTS;
        event.arguments = json_text(item, "arguments");
        if (openai_emit(run, &event)) {
            cJSON_free(encoded);
            return -1;
        }
        event.kind = OCA_EV_TOOL_CALL_END;
        if (openai_emit(run, &event)) {
            cJSON_free(encoded);
            return -1;
        }
    }
    if (encoded) {
        oca_event event = {.kind = OCA_EV_MODEL_STATE, .state = encoded};
        int cancelled = openai_emit(run, &event);
        cJSON_free(encoded);
        if (cancelled) return -1;
    }
    return (int)count;
}

/* Return only a known request field. Provider error prose can contain private data. */
static const char *openai_error_parameter(const byte_buffer *body) {
    if (!body->data) return NULL;
    cJSON *document = json_parse(body->data, body->len);
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(document, "error");
    const char *parameter = json_text(error, "param");
    char parsed[64] = "";
    if (!parameter) {
        const char *message = json_text(document, "detail");
        if (!message) message = json_text(error, "message");
        const char *prefixes[] = {"Unsupported parameter: ", "Unknown parameter: ", "Unrecognized request argument supplied: "};
        for (size_t i = 0; message && i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
            if (strncmp(message, prefixes[i], strlen(prefixes[i]))) continue;
            message += strlen(prefixes[i]);
            message += strspn(message, " '\"");
            if (sscanf(message, "%63[a-zA-Z0-9_.]", parsed) == 1) parameter = parsed;
            size_t length = strlen(parsed);
            if (length && parsed[length - 1] == '.') parsed[length - 1] = 0;
            break;
        }
    }
    const char *fields[] = {"max_output_tokens", "instructions", "model", "input", "tools", "include",
        "reasoning", "reasoning.effort", "reasoning.summary", "store", "stream", "prompt_cache_key", "parallel_tool_calls"};
    const char *found = NULL;
    for (size_t i = 0; parameter && i < sizeof(fields) / sizeof(fields[0]); i++)
        if (!strcmp(parameter, fields[i])) {
            found = fields[i];
            break;
        }
    cJSON_Delete(document);
    return found;
}

static int openai_generate(oca_llm *llm, const oca_request *request, oca_event_fn *callback, void *user,
                            const char *url, const char *token_url, char *err, size_t errsz) {
    openai_run run = {.request = request, .callback = callback, .user = user};
    oca_auth_ui ui = {.user = &run, .cancelled = openai_poll};
    if (openai_poll(&run)) {
        oca_event event = {.kind = OCA_EV_ERROR, .category = "cancelled", .message = "Cancelled by user."};
        openai_emit(&run, &event);
        return 0;
    }
    char *body = openai_serialize(request);
    if (!body) {
        err_set(err, errsz, "cannot serialize the bounded OpenAI request");
        return -1;
    }
    if (strlen(body) + 256 > (size_t)request->model->input_limit) {
        cJSON_free(body);
        err_set(err, errsz, "OpenAI input exceeds the local context limit");
        return -1;
    }
    if (openai_refresh(llm, 0, &ui, token_url, err, errsz)) {
        cJSON_free(body);
        if (run.cancelled) {
            oca_event event = {.kind = OCA_EV_ERROR, .category = "cancelled", .message = "Cancelled by user."};
            openai_emit(&run, &event);
            return 0;
        }
        return -1;
    }
    long status = 0;
    CURLcode result = CURLE_FAILED_INIT;
    for (int attempt = 0; attempt < 2; attempt++) {
        run.transport = (transport_watch){.limits = &PROVIDER_TRANSPORT_LIMITS,
            .request_bytes = strlen(body), .attempt = (unsigned)attempt + 1};
        pthread_once(&curl_once, curl_init);
        CURL *curl = curl_status == CURLE_OK ? curl_easy_init() : NULL;
        if (!curl) {
            err_set(err, errsz, "cannot initialize OpenAI transport");
            cJSON_free(body);
            return -1;
        }
        char authorization[16432], account[288];
        pthread_mutex_lock(&oauth_refresh_mutex);
        const auth_member *member = find_member(llm, "openai");
        snprintf(authorization, sizeof(authorization), "Authorization: Bearer %s", member->access_token);
        snprintf(account, sizeof(account), "ChatGPT-Account-Id: %s", member->account_id);
        pthread_mutex_unlock(&oauth_refresh_mutex);
        const char *values[] = {authorization, account, "Content-Type: application/json", "Accept: text/event-stream", "originator: oca", "Expect:"};
        struct curl_slist *headers = NULL;
        int setup = 1;
        for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
            struct curl_slist *next = curl_slist_append(headers, values[i]);
            if (!next) {
                setup = 0;
                break;
            }
            headers = next;
        }
        memset(authorization, 0, sizeof(authorization));
        if (!configure_http_post(curl, url, body, headers, &PROVIDER_TRANSPORT_LIMITS)) setup = 0;
#define OPENAI_OPT(option, value) do { if (curl_easy_setopt(curl, option, value) != CURLE_OK) setup = 0; } while (0)
        OPENAI_OPT(CURLOPT_WRITEFUNCTION, openai_write);
        OPENAI_OPT(CURLOPT_WRITEDATA, &run);
        OPENAI_OPT(CURLOPT_XFERINFOFUNCTION, openai_progress);
        OPENAI_OPT(CURLOPT_XFERINFODATA, &run);
        OPENAI_OPT(CURLOPT_PROTOCOLS_STR, !strcmp(url, OPENAI_RESPONSES_URL) ? "https" : "http");
        if (strcmp(url, OPENAI_RESPONSES_URL)) OPENAI_OPT(CURLOPT_NOPROXY, "*");
#undef OPENAI_OPT
        run.curl = curl;
        result = setup ? curl_easy_perform(curl) : CURLE_FAILED_INIT;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        diagnostic_transport(&run.diagnostic, curl, result, status, &run.transport);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        if (attempt || status != 401 || run.started || run.cancelled) break;
        if (openai_refresh(llm, 1, &ui, token_url, err, errsz)) {
            openai_fail(&run, "authentication", "OpenAI refresh failed; run oca logout openai, then oca -m MODEL in an interactive terminal");
            break;
        }
        buffer_free(&run.line);
        buffer_free(&run.data);
        buffer_free(&run.error_body);
        run.category = run.message = NULL;
        run.wire_bytes = 0;
        memset(&run.diagnostic, 0, sizeof(run.diagnostic));
    }
    if (run.cancelled) openai_fail(&run, "cancelled", "Cancelled by user.");
    else if (status == 401 || status == 403) openai_fail(&run, "authentication", "OpenAI rejected account or model access; run oca logout openai, then oca -m MODEL in an interactive terminal");
    else if (status == 429) openai_fail(&run, "rate_limit", "OpenAI quota or rate limit reached");
    else if (status == 400) {
        const char *parameter = openai_error_parameter(&run.error_body);
        if (parameter) {
            snprintf(run.parameter_error, sizeof(run.parameter_error), "OpenAI rejected request parameter: %s", parameter);
            openai_fail(&run, "http", run.parameter_error);
        } else openai_fail(&run, "http", "OpenAI rejected request parameters");
    } else if (status && status != 200) openai_fail(&run, "http", "OpenAI returned an HTTP error");
    else if (result != CURLE_OK) openai_fail(&run, "transport", "OpenAI transfer failed or timed out");
    int calls = -1;
    if (!run.category && ((run.line.len && openai_line(&run)) ||
        (run.data.len && openai_chunk(&run, run.data.data, run.data.len)) || !run.started || !run.completed ||
        (calls = openai_deliver_items(&run)) < 0)) openai_fail(&run, "stream", "OpenAI stream ended without a valid completed response");
    if (run.category && !*run.diagnostic.check) {
        char received[64];
        if (run.cancelled) diagnostic_set(&run.diagnostic, "cancelled", "request", "active request", "cancelled");
        else if (status && status != 200) {
            snprintf(received, sizeof(received), "%ld", status);
            diagnostic_set(&run.diagnostic, "http_status", "http_status", "200", received);
        } else if (result != CURLE_OK) {
            snprintf(received, sizeof(received), "%d", (int)result);
            diagnostic_set(&run.diagnostic, "transport", "curl_code", "0", received);
        } else diagnostic_set(&run.diagnostic, "stream_end", run.started ? "response.completed" : "response.created",
                               "present before end of stream", "missing");
    }
    run.diagnostic.has_http_status = status > 0;
    run.diagnostic.http_status = status;
    run.diagnostic.wire_bytes = run.wire_bytes;
    run.diagnostic.response_bytes = run.text_bytes;
    if (!run.category && run.has_usage) openai_emit(&run, &run.usage);
    if (run.cancelled) {
        run.category = "cancelled";
        run.message = "Cancelled by user.";
    }
    oca_event end = {.kind = run.category ? OCA_EV_ERROR : OCA_EV_RESPONSE_DONE,
        .category = run.category, .message = run.message, .model_id = request->model->id,
        .diagnostic = run.category ? &run.diagnostic : NULL,
        .finish = calls > 0 ? "tool_calls" : "stop"};
    openai_emit(&run, &end);
    for (size_t i = 0; i < OPENAI_ITEMS_MAX; i++) {
        cJSON_Delete(run.items[i].value);
        buffer_free(&run.items[i].text);
        buffer_free(&run.items[i].arguments);
    }
    cJSON_Delete(run.state);
    buffer_free(&run.line);
    buffer_free(&run.data);
    buffer_free(&run.error_body);
    cJSON_free(body);
    return 0;
}

int oca_llm_generate(oca_llm *llm, const oca_request *request,
                     oca_event_fn *callback, void *user,
                     char *err, size_t errsz) {
    if (validate_request(request, err, errsz) != 0) return -1;
    if (!callback) {
        err_set(err, errsz, "generation requires an event callback");
        return -1;
    }
    if (!strcmp(request->model->provider, "kimi"))
        return chat_generate(llm, request, callback, user, KIMI_URL, err, errsz);
    if (!strcmp(request->model->provider, "deepseek"))
        return chat_generate(llm, request, callback, user, DEEPSEEK_URL, err, errsz);
    if (!strcmp(request->model->provider, "openai"))
        return openai_generate(llm, request, callback, user, OPENAI_RESPONSES_URL, OPENAI_TOKEN_URL, err, errsz);
    err_set(err, errsz, "model has no provider adapter");
    return -1;
}
