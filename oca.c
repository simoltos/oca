/* OCA process entry point. */
#define _XOPEN_SOURCE 700
#include "oca_core.h"
#include "oca_llm.h"
#include "oca_tui.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>
#include <cjson/cJSON.h>

#define OCA_PATH_MAX 4096

typedef struct {
    char model[OCA_LLM_MODEL_ID_MAX + 1];
    char reasoning[OCA_REASONING_NAME_MAX + 1];
    int has_route;
    cJSON *document;
    const cJSON *model_aliases; /* borrowed from document */
    oca_tui_options display;
} config_store;

typedef struct {
    const char *model;
    const char *prompt;
    const char *workspace;
    const char *resume_id;
    const char *reasoning_name;
    int workspace_set;
    int trace;
    int exec_mode;
    int save;
    int persistence_disabled;
} run_options;

typedef struct {
    int persistence_disabled;
} config_callbacks;

typedef struct {
    FILE *file;
    int failed;
} trace_file;

typedef enum { RUN_INTERACTIVE, RUN_RESUME, RUN_EXEC } run_kind;
/* ============================================================================
 * Command-line help
 * ============================================================================ */

static void usage(FILE *out) {
    fputs(
        "OCA\n"
        "An agent.\n\n"
        "Usage:\n"
        "  oca [options] [--] [PROMPT]\n"
        "  oca <command> [options]\n\n"
        "Commands:\n"
        "  exec       Run an agent turn without the TUI.\n"
        "  models     List models and local authentication methods.\n"
        "  sessions   List saved sessions.\n"
        "  resume     Resume a session in the TUI.\n"
        "  delete     Delete a saved session.\n"
        "  logout     Remove provider credentials.\n\n"
        "Options:\n"
        "  -m, --model MODEL      Select a model by ID or alias.\n"
        "  -C, --chdir PATH       Select the workspace root.\n"
        "  --reasoning LEVEL      Select a supported reasoning effort.\n"
        "  --diagnose             Create a diagnostic file for this run.\n"
        "  -h, --help             Show this help.\n\n"
        "Selecting a model can request provider credentials before the TUI starts.\n",
        out);
}

static int fail_usage(const char *command, const char *message) {
    if (message) fprintf(stderr, "oca: %s\n", message);
    fprintf(stderr, "Try 'oca%s%s --help' for usage.\n", command ? " " : "", command ? command : "");
    return 2;
}

/* ============================================================================
 * Configuration store
 * ============================================================================ */

static int state_path(char *out, size_t outsz, const char *leaf) {
    const char *home = getenv("HOME");
    if (!home || !*home) return -1;
    int n = snprintf(out, outsz, "%s/.oca/%s", home, leaf);
    return n >= 0 && (size_t)n < outsz ? 0 : -1;
}

static int mkdir_parents(const char *path, char *err, size_t errsz) {
    char copy[OCA_PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(copy)) {
        snprintf(err, errsz, "state path is too long");
        return -1;
    }
    memcpy(copy, path, len + 1);
    for (size_t i = 1; i < len; i++) {
        if (copy[i] != '/') continue;
        copy[i] = '\0';
        if (mkdir(copy, 0700) == 0) {
            if (chmod(copy, 0700) != 0) {
                snprintf(err, errsz, "cannot protect %s: %s", copy, strerror(errno));
                return -1;
            }
        } else if (errno != EEXIST) {
            snprintf(err, errsz, "cannot create %s: %s", copy, strerror(errno));
            return -1;
        }
        copy[i] = '/';
    }
    return 0;
}

static int write_atomic(const char *path, const char *data, size_t len,
                        char *err, size_t errsz) {
    if (mkdir_parents(path, err, errsz) != 0) return -1;
    char temp[OCA_PATH_MAX + 64];
    int n = snprintf(temp, sizeof(temp), "%s.tmp.%ld", path, (long)getpid());
    if (n < 0 || (size_t)n >= sizeof(temp)) {
        snprintf(err, errsz, "state path is too long");
        return -1;
    }
    unlink(temp);
    int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        snprintf(err, errsz, "cannot create %s: %s", temp, strerror(errno));
        return -1;
    }
    if (fchmod(fd, 0600) != 0) goto io_fail;
    size_t off = 0;
    while (off < len) {
        ssize_t written = write(fd, data + off, len - off);
        if (written < 0) {
            if (errno == EINTR) continue;
            goto io_fail;
        }
        if (!written) {
            errno = EIO;
            goto io_fail;
        }
        off += (size_t)written;
    }
    if (fsync(fd) != 0) goto io_fail;
    if (close(fd) != 0) {
        snprintf(err, errsz, "cannot sync %s: %s", temp, strerror(errno));
        unlink(temp);
        return -1;
    }
    if (rename(temp, path) != 0) {
        snprintf(err, errsz, "cannot replace %s: %s", path, strerror(errno));
        unlink(temp);
        return -1;
    }
    return 0;
io_fail:
    snprintf(err, errsz, "cannot write %s: %s", temp, strerror(errno));
    close(fd);
    unlink(temp);
    return -1;
}

#define OCA_CONFIG_MAX_BYTES (1024u * 1024u)

/* Parse complete JSON objects and reject embedded string terminators. */
static cJSON *config_parse_document(const char *text, size_t length) {
    int quoted = 0;
    for (size_t i = 0; i < length; i++) {
        if (quoted && text[i] == '\\') {
            if (++i >= length) return NULL;
            if (text[i] == 'u' && length - i >= 5 && !memcmp(text + i, "u0000", 5)) return NULL;
        } else if (text[i] == '"') quoted = !quoted;
    }
    const char *end = NULL;
    cJSON *document = cJSON_ParseWithLengthOpts(text, length + 1, &end, 0);
    if (!cJSON_IsObject(document)) {
        cJSON_Delete(document);
        return NULL;
    }
    while (end < text + length && isspace((unsigned char)*end)) end++;
    if (end != text + length) {
        cJSON_Delete(document);
        return NULL;
    }
    return document;
}

/* Returns an owned document. A missing file produces an empty document. */
static cJSON *read_document(const char *path, const char *label, int *missing,
                            char *err, size_t errsz) {
    if (missing) *missing = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0 && errno == ENOENT) {
        if (missing) *missing = 1;
        cJSON *document = cJSON_CreateObject();
        if (document) return document;
        snprintf(err, errsz, "cannot create the %s document", label);
        return NULL;
    }
    if (fd < 0) {
        snprintf(err, errsz, "cannot read %s file: %s", label, strerror(errno));
        return NULL;
    }
    struct stat state;
    if (fstat(fd, &state) || !S_ISREG(state.st_mode) || state.st_size >= OCA_CONFIG_MAX_BYTES) {
        close(fd);
        snprintf(err, errsz, "%s file is not a bounded regular file", label);
        return NULL;
    }
    char *text = malloc(OCA_CONFIG_MAX_BYTES + 1);
    if (!text) {
        close(fd);
        snprintf(err, errsz, "out of memory");
        return NULL;
    }
    size_t length = 0;
    int valid = 1;
    while (length < OCA_CONFIG_MAX_BYTES) {
        ssize_t count = read(fd, text + length, OCA_CONFIG_MAX_BYTES - length);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            valid = 0;
            break;
        }
        if (!count) break;
        length += (size_t)count;
    }
    close(fd);
    text[length] = 0;
    cJSON *document = valid && length < OCA_CONFIG_MAX_BYTES ? config_parse_document(text, length) : NULL;
    free(text);
    if (!document) snprintf(err, errsz, "%s file rejected", label);
    return document;
}

static cJSON *config_read_document(char *err, size_t errsz) {
    char path[OCA_PATH_MAX];
    if (state_path(path, sizeof(path), "config.json")) {
        snprintf(err, errsz, "HOME is not set");
        return NULL;
    }
    return read_document(path, "config", NULL, err, errsz);
}

static void free_config(config_store *cfg) {
    cJSON_Delete(cfg->document);
    memset(cfg, 0, sizeof(*cfg));
}

static int config_keys(const cJSON *object, const char *allowed) {
    if (!cJSON_IsObject(object)) return -1;
    const cJSON *field;
    cJSON_ArrayForEach(field, object) {
        char key[128];
        if (!field->string || strpbrk(field->string, " \t\r\n") ||
            snprintf(key, sizeof(key), " %s ", field->string) >= (int)sizeof(key) ||
            !strstr(allowed, key)) return -1;
        for (const cJSON *prior = object->child; prior != field; prior = prior->next)
            if (!strcmp(prior->string, field->string)) return -1;
    }
    return 0;
}

static int empty_config(config_store *cfg, char *err, size_t errsz) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->display = (oca_tui_options)OCA_TUI_DEFAULT_OPTIONS;
    cfg->document = cJSON_CreateObject();
    if (cfg->document) return 0;
    snprintf(err, errsz, "cannot create the configuration document");
    return -1;
}

static int config_name_valid(const char *name, size_t maximum) {
    if (!name || !*name || strlen(name) > maximum) return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (*p <= 0x20 || *p >= 0x7f) return 0;
    return 1;
}

static int model_aliases_valid(const cJSON *aliases) {
    if (!cJSON_IsObject(aliases) || cJSON_GetArraySize(aliases) > (int)OCA_LLM_MODELS_MAX)
        return 0;
    size_t model_count = 0;
    const oca_model_definition *models = oca_llm_builtin_models(&model_count);
    const cJSON *field;
    cJSON_ArrayForEach(field, aliases) {
        if (!config_name_valid(field->string, OCA_LLM_MODEL_ID_MAX) ||
            !cJSON_IsString(field) ||
            !config_name_valid(field->valuestring, OCA_LLM_MODEL_NAME_MAX)) return 0;
        for (const cJSON *prior = aliases->child; prior != field; prior = prior->next)
            if (!strcmp(prior->string, field->string) ||
                !strcmp(prior->valuestring, field->valuestring)) return 0;
        for (size_t i = 0; i < model_count; i++)
            if (!strcmp(field->valuestring, models[i].id) &&
                strcmp(field->string, models[i].id)) return 0;
    }
    return 1;
}

static int load_config(config_store *cfg, char *err, size_t errsz) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->display = (oca_tui_options)OCA_TUI_DEFAULT_OPTIONS;
    cfg->document = config_read_document(err, errsz);
    if (!cfg->document) return -1;
    if (config_keys(cfg->document, " last_used  show  model_aliases ")) goto invalid;
    const cJSON *aliases = cJSON_GetObjectItemCaseSensitive(cfg->document, "model_aliases");
    if (aliases && !model_aliases_valid(aliases)) goto invalid;
    cfg->model_aliases = aliases;
    const cJSON *route = cJSON_GetObjectItemCaseSensitive(cfg->document, "last_used");
    if (route) {
        if (config_keys(route, " model  reasoning ") || cJSON_GetArraySize(route) != 2) goto invalid;
        const cJSON *model = cJSON_GetObjectItemCaseSensitive(route, "model");
        const cJSON *reasoning = cJSON_GetObjectItemCaseSensitive(route, "reasoning");
        if ((!cJSON_IsNull(model) && !cJSON_IsString(model)) ||
            (!cJSON_IsNull(reasoning) && !cJSON_IsString(reasoning))) goto invalid;
        if (cJSON_IsString(model)) {
            if (!config_name_valid(model->valuestring, OCA_LLM_MODEL_ID_MAX)) goto invalid;
            snprintf(cfg->model, sizeof(cfg->model), "%s", model->valuestring);
            cfg->has_route = 1;
        }
        if (cJSON_IsString(reasoning)) {
            if (!config_name_valid(reasoning->valuestring, OCA_REASONING_NAME_MAX)) goto invalid;
            snprintf(cfg->reasoning, sizeof(cfg->reasoning), "%s", reasoning->valuestring);
        }
    }
    const cJSON *show = cJSON_GetObjectItemCaseSensitive(cfg->document, "show");
    if (show) {
        if (config_keys(show, " reasoning  read  write  edit  bash ")) goto invalid;
        const struct { const char *name; int *value; } fields[] = {
            {"reasoning", &cfg->display.show_reasoning}, {"read", &cfg->display.show_read},
            {"write", &cfg->display.show_write}, {"edit", &cfg->display.show_edit},
            {"bash", &cfg->display.show_bash},
        };
        for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
            const cJSON *field = cJSON_GetObjectItemCaseSensitive(show, fields[i].name);
            if (!field) continue;
            if (!cJSON_IsBool(field)) goto invalid;
            *fields[i].value = cJSON_IsTrue(field);
        }
    }
    return 0;
invalid:
    free_config(cfg);
    snprintf(err, errsz, "config file rejected: check last_used, show, and model_aliases");
    return -1;
}

static int write_document(const char *path, const cJSON *document, const char *label,
                          char *err, size_t errsz) {
    char *text = cJSON_Print(document);
    if (!text) {
        snprintf(err, errsz, "cannot serialize the %s", label);
        return -1;
    }
    size_t length = strlen(text);
    int rc;
    if (length >= OCA_CONFIG_MAX_BYTES) {
        snprintf(err, errsz, "%s file is too large", label);
        rc = -1;
    } else rc = write_atomic(path, text, length, err, errsz);
    cJSON_free(text);
    return rc;
}

static int config_write_document(const cJSON *document, char *err, size_t errsz) {
    char path[OCA_PATH_MAX];
    if (state_path(path, sizeof(path), "config.json")) {
        snprintf(err, errsz, "HOME is not set");
        return -1;
    }
    return write_document(path, document, "config", err, errsz);
}

/* The state directory stays stable when config.json is replaced atomically. */
static int config_lock(char *err, size_t errsz) {
    char path[OCA_PATH_MAX];
    if (state_path(path, sizeof(path), "") || mkdir_parents(path, err, errsz)) return -1;
    path[strlen(path) - 1] = 0;
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        snprintf(err, errsz, "cannot open the configuration lock: %s", strerror(errno));
        return -1;
    }
    struct stat state;
    if (fstat(fd, &state) || !S_ISDIR(state.st_mode) || state.st_uid != geteuid() ||
        ((state.st_mode & 0777) != 0700 && fchmod(fd, 0700))) {
        close(fd);
        snprintf(err, errsz, "configuration lock rejected");
        return -1;
    }
    struct timespec started, now;
    if (clock_gettime(CLOCK_MONOTONIC, &started)) {
        close(fd);
        snprintf(err, errsz, "cannot time the configuration lock");
        return -1;
    }
    while (flock(fd, LOCK_EX | LOCK_NB)) {
        if (errno != EACCES && errno != EAGAIN && errno != EINTR) {
            snprintf(err, errsz, "cannot lock configuration: %s", strerror(errno));
            close(fd);
            return -1;
        }
        if (clock_gettime(CLOCK_MONOTONIC, &now) || now.tv_sec - started.tv_sec >= 5) {
            close(fd);
            snprintf(err, errsz, "configuration is busy");
            return -1;
        }
        struct timespec pause = {0, 10 * 1000 * 1000};
        nanosleep(&pause, NULL);
    }
    return fd;
}

/* Copy known legacy aliases once. The old catalog stays untouched. */
static int migrate_model_aliases(config_store *cfg, char *err, size_t errsz) {
    if (cfg->model_aliases) return 0;
    char path[OCA_PATH_MAX];
    if (state_path(path, sizeof(path), "models.json")) {
        snprintf(err, errsz, "HOME is not set");
        return -1;
    }
    int missing = 0;
    cJSON *legacy = read_document(path, "previous models", &missing, err, errsz);
    if (!legacy) return -1;
    if (missing) { cJSON_Delete(legacy); return 0; }
    cJSON *aliases = cJSON_CreateObject();
    if (!aliases) {
        cJSON_Delete(legacy);
        snprintf(err, errsz, "out of memory");
        return -1;
    }
    size_t count = 0, model_count = 0;
    const oca_model_definition *models = oca_llm_builtin_models(&model_count);
    for (size_t i = 0; i < model_count; i++) {
        const cJSON *entry = cJSON_GetObjectItemCaseSensitive(legacy, models[i].id);
        const cJSON *alias = cJSON_GetObjectItemCaseSensitive(entry, "alias");
        if (!alias) continue;
        if (!cJSON_IsString(alias) ||
            !config_name_valid(alias->valuestring, OCA_LLM_MODEL_NAME_MAX)) {
            fprintf(stderr, "oca: warning: invalid previous alias for %s ignored\n", models[i].id);
            continue;
        }
        if (!cJSON_AddStringToObject(aliases, models[i].id, alias->valuestring)) {
            snprintf(err, errsz, "out of memory");
            goto failed;
        }
        if (!model_aliases_valid(aliases)) {
            cJSON_DeleteItemFromObjectCaseSensitive(aliases, models[i].id);
            fprintf(stderr, "oca: warning: conflicting previous alias for %s ignored\n", models[i].id);
        } else count++;
    }
    cJSON_Delete(legacy);
    legacy = NULL;
    if (!count) { cJSON_Delete(aliases); return 0; }
    int lock = config_lock(err, errsz);
    if (lock < 0) goto failed;
    config_store current;
    if (load_config(&current, err, errsz)) { close(lock); goto failed; }
    if (current.model_aliases) {
        free_config(&current);
        close(lock);
        cJSON_Delete(aliases);
        return 0;
    }
    if (!cJSON_AddItemToObject(current.document, "model_aliases", aliases)) {
        snprintf(err, errsz, "cannot add model aliases to configuration");
        free_config(&current);
        close(lock);
        goto failed;
    }
    current.model_aliases = aliases;
    aliases = NULL;
    int result = config_write_document(current.document, err, errsz);
    close(lock);
    if (result) {
        free_config(&current);
        return -1;
    }
    free_config(cfg);
    *cfg = current;
    fprintf(stderr, "oca: copied %zu model alias(es) into config.json\n", count);
    return 0;
failed:
    cJSON_Delete(aliases);
    cJSON_Delete(legacy);
    return -1;
}

/* Reload user edits before replacing the preference record. A malformed
 * document is never overwritten through preference persistence. */
static int save_config(void *user, const char *model, const char *reasoning,
                       char *err, size_t errsz) {
    const config_callbacks *callbacks = user;
    if (callbacks && callbacks->persistence_disabled) return 0;
    if (!model || !*model) {
        snprintf(err, errsz, "cannot persist an empty model selection");
        return -1;
    }
    int lock = config_lock(err, errsz);
    if (lock < 0) return -1;
    config_store current;
    if (load_config(&current, err, errsz)) {
        close(lock);
        return -1;
    }
    cJSON *document = current.document;
    const cJSON *previous = cJSON_GetObjectItemCaseSensitive(document, "last_used");
    const cJSON *previous_model = cJSON_GetObjectItemCaseSensitive(previous, "model");
    const cJSON *previous_reasoning = cJSON_GetObjectItemCaseSensitive(previous, "reasoning");
    int same = cJSON_IsString(previous_model) && !strcmp(previous_model->valuestring, model);
    if (same) {
        if (!reasoning || !*reasoning) same = cJSON_IsNull(previous_reasoning);
        else same = cJSON_IsString(previous_reasoning) && !strcmp(previous_reasoning->valuestring, reasoning);
    }
    if (same) {
        free_config(&current);
        close(lock);
        return 0;
    }
    cJSON *route = cJSON_CreateObject();
    int valid = route && cJSON_AddStringToObject(route, "model", model);
    if (valid) {
        if (reasoning && *reasoning) valid = cJSON_AddStringToObject(route, "reasoning", reasoning) != NULL;
        else valid = cJSON_AddNullToObject(route, "reasoning") != NULL;
    }
    if (valid) {
        while (cJSON_GetObjectItemCaseSensitive(document, "last_used"))
            cJSON_DeleteItemFromObjectCaseSensitive(document, "last_used");
        valid = cJSON_AddItemToObject(document, "last_used", route);
        if (valid) route = NULL;
    }
    cJSON_Delete(route);
    int rc;
    if (!valid) {
        snprintf(err, errsz, "cannot prepare the preference record");
        rc = -1;
    } else rc = config_write_document(document, err, errsz);
    free_config(&current);
    close(lock);
    return rc;
}

static int trace_write(void *user, const char *line) {
    trace_file *trace = user;
    if (trace->failed) return -1;
    if (fputs(line, trace->file) == EOF || fflush(trace->file) != 0) {
        trace->failed = 1;
        return -1;
    }
    return 0;
}

static int open_trace(trace_file *trace, char *err, size_t errsz) {
    memset(trace, 0, sizeof(*trace));
    char directory[OCA_PATH_MAX], path[OCA_PATH_MAX], stamp[32];
    time_t now = time(NULL);
    struct tm utc;
    if (state_path(directory, sizeof(directory), "diagnose") ||
        !gmtime_r(&now, &utc) || !strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &utc)) {
        snprintf(err, errsz, "cannot select the diagnostic directory or time");
        return -1;
    }
    int fd = -1;
    for (unsigned suffix = 0; suffix < 1000; suffix++) {
        int length = suffix ? snprintf(path, sizeof(path), "%s/%s-%ld-%u.log", directory, stamp, (long)getpid(), suffix) :
                             snprintf(path, sizeof(path), "%s/%s-%ld.log", directory, stamp, (long)getpid());
        if (length < 0 || (size_t)length >= sizeof(path)) {
            snprintf(err, errsz, "diagnostic path is too long");
            return -1;
        }
        if (mkdir_parents(path, err, errsz)) return -1;
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) {
        snprintf(err, errsz, "cannot open diagnostic file: %s", strerror(errno));
        return -1;
    }
    if (fchmod(fd, 0600)) {
        snprintf(err, errsz, "cannot protect diagnostic file: %s", strerror(errno));
        close(fd);
        unlink(path);
        return -1;
    }
    trace->file = fdopen(fd, "w");
    if (!trace->file) {
        snprintf(err, errsz, "cannot open diagnostic stream: %s", strerror(errno));
        close(fd);
        unlink(path);
        return -1;
    }
    return 0;
}

/* ============================================================================
 * Process commands
 * ========================================================================== */

static void models_usage(FILE *out) {
    fputs("List models and local authentication methods.\n\n"
          "Usage:\n  oca models\n", out);
}

static int command_models(oca_llm *llm) {
    int model_width = 5, provider_width = 8;
    for (size_t i = 0; i < oca_llm_model_count(llm); i++) {
        const oca_model *model = oca_llm_model_at(llm, i);
        if ((int)strlen(model->id) > model_width) model_width = (int)strlen(model->id);
        if ((int)strlen(model->provider) > provider_width) provider_width = (int)strlen(model->provider);
    }
    printf("%-*s  %-*s  %-10s  %s\n", model_width, "MODEL", provider_width, "PROVIDER", "AUTH", "REASONING");
    for (size_t i = 0; i < oca_llm_model_count(llm); i++) {
        const oca_model *model = oca_llm_model_at(llm, i);
        const char *method = oca_llm_auth_method(llm, model->provider);
        printf("%-*s  %-*s  %-10s  ", model_width, model->id,
               provider_width, model->provider, method ? method : "signed out");
        for (size_t level = 0; level < model->reasoning_count; level++)
            printf("%s%s", level ? ", " : "", model->reasoning_levels[level]);
        putchar('\n');
    }
    return 0;
}

/* ============================================================================
 * Session command
 * ============================================================================ */

static void sessions_usage(FILE *out) {
    fputs("List saved sessions.\n\nUsage:\n  oca sessions\n", out);
}

static void resume_usage(FILE *out) {
    fputs("Resume a session in the TUI.\n\n"
          "Usage:\n  oca resume ID [options]\n\n"
          "Options:\n"
          "  -m, --model MODEL   Select a model by ID or alias.\n"
          "  -C, --chdir PATH    Override the saved workspace.\n"
          "  --reasoning LEVEL   Select a supported reasoning effort.\n"
          "  --diagnose          Create a diagnostic file for this run.\n"
          "  -h, --help          Show this help.\n", out);
}

static void delete_usage(FILE *out) {
    fputs("Delete a saved session.\n\n"
          "Usage:\n  oca delete ID [-y|--yes]\n\n"
          "Options:\n"
          "  -y, --yes   Confirm deletion without a question.\n"
          "  -h, --help  Show this help.\n", out);
}

static void logout_usage(FILE *out) {
    fputs("Remove provider credentials.\n\n"
          "Usage:\n  oca logout PROVIDER [-y|--yes]\n\n"
          "Options:\n"
          "  -y, --yes   Confirm logout without a question.\n"
          "  -h, --help  Show this help.\n", out);
}

static int confirm(const char *prompt) {
    char line[16];
    fputs(prompt, stderr);
    fflush(stderr);
    if (!fgets(line, sizeof(line), stdin)) return 0;
    return line[0] == 'y' || line[0] == 'Y';
}

static int parse_confirmed_target(int argc, char **argv, const char **target, int *yes) {
    *target = NULL;
    *yes = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-y") || !strcmp(argv[i], "--yes")) {
            if (*yes) return -1;
            *yes = 1;
        } else if (argv[i][0] != '-' && argv[i][0] && !*target) {
            *target = argv[i];
        } else return -1;
    }
    return *target ? 0 : -1;
}

static int command_sessions(int argc) {
    if (argc) {
        return fail_usage("sessions", "unexpected argument");
    }
    char dir[OCA_PATH_MAX], err[256];
    if (state_path(dir, sizeof(dir), "sessions") != 0) {
        fputs("oca: HOME is not set\n", stderr);
        return 1;
    }
    oca_session_summary *items = NULL;
    size_t count = 0;
    if (oca_core_scan_sessions(dir, &items, &count, err, sizeof(err)) != 0) {
        fprintf(stderr, "oca: %s\n", err);
        return 1;
    }
    if (!count) puts("No sessions found.");
    if (count) printf("%-32s  %-16s  %s\n", "ID", "LAST USED", "TITLE");
    for (size_t i = 0; i < count; i++) {
        time_t timestamp = (time_t)items[i].last_used;
        struct tm local;
        char date[32] = "unknown";
        if (localtime_r(&timestamp, &local))
            strftime(date, sizeof(date), "%Y-%m-%d %H:%M", &local);
        printf("%s  %-16s  %s\n", items[i].id, date, items[i].title);
    }
    oca_core_free_summaries(items, count);
    return 0;
}

static int command_delete(int argc, char **argv) {
    const char *id;
    int yes;
    if (parse_confirmed_target(argc, argv, &id, &yes)) {
        return fail_usage("delete", "expected a session ID and optional -y or --yes");
    }
    if (!yes && !confirm("Delete saved session? (y/n) ")) return 0;
    char dir[OCA_PATH_MAX], err[256];
    if (state_path(dir, sizeof(dir), "sessions")) {
        fputs("oca: HOME is not set\n", stderr);
        return 1;
    }
    if (oca_core_delete_session(dir, id, err, sizeof(err))) {
        fprintf(stderr, "oca: %s\n", err);
        return 1;
    }
    return 0;
}

static int command_logout(oca_llm *llm, const char *provider, int yes) {
    if (!yes && !confirm("Remove provider credentials? (y/n) ")) return 0;
    char err[256];
    if (oca_llm_auth_logout(llm, provider, err, sizeof(err))) {
        fprintf(stderr, "oca: %s\n", err);
        return 1;
    }
    return 0;
}

/* ============================================================================
 * Provider login
 * ========================================================================== */

#define OCA_KEY_INPUT_SIZE 4098

static volatile sig_atomic_t key_interrupted;

static void key_signal(int signal_number) {
    key_interrupted = signal_number;
}

/* Every key buffer has this fixed capacity. Clear canceled input as well. */
static void free_key(char *key) {
    if (!key) return;
    volatile unsigned char *bytes = (volatile unsigned char *)key;
    for (size_t i = 0; i < OCA_KEY_INPUT_SIZE; i++) bytes[i] = 0;
    free(key);
}

/* Owns input flags and signal handlers until return. The caller owns the
 * result and must release it with free_key. No credential enters output. */
static char *read_key(char *err, size_t errsz) {
    const int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGPIPE};
    struct sigaction previous[sizeof(signals) / sizeof(signals[0])], action;
    struct termios old, hidden;
    size_t installed = 0, used = 0;
    int input_flags = -1, hidden_input = 0, complete = 0;
    struct winsize window = {0};
    int columns = ioctl(STDERR_FILENO, TIOCGWINSZ, &window) == 0 && window.ws_col ? window.ws_col : 80;
    int column = (int)((sizeof("API key: ") - 2) % (size_t)columns) + 1;
    char *key = calloc(OCA_KEY_INPUT_SIZE, 1);
    const char *failure = "cannot read API key";
    if (!key) goto cleanup;
    memset(&action, 0, sizeof(action));
    action.sa_handler = key_signal;
    sigemptyset(&action.sa_mask);
    key_interrupted = 0;
    for (; installed < sizeof(signals) / sizeof(signals[0]); installed++) {
        if (sigaction(signals[installed], &action, &previous[installed]) != 0)
            goto cleanup;
    }
    input_flags = fcntl(STDIN_FILENO, F_GETFL);
    if (input_flags < 0 || fcntl(STDIN_FILENO, F_SETFL, input_flags | O_NONBLOCK) != 0)
        goto cleanup;
    if (tcgetattr(STDIN_FILENO, &old) != 0) goto cleanup;
    hidden = old;
    hidden.c_lflag &= (tcflag_t)~(ECHO | ECHONL | ICANON);
    hidden.c_cc[VMIN] = 1;
    hidden.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) != 0) goto cleanup;
    hidden_input = 1;
    if (fputs("API key: ", stderr) == EOF || fflush(stderr) != 0) goto cleanup;
    while (!key_interrupted && used + 1 < OCA_KEY_INPUT_SIZE) {
        struct pollfd input = {.fd = STDIN_FILENO, .events = POLLIN};
        int ready = poll(&input, 1, 100);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (ready && (input.revents & (POLLERR | POLLNVAL)))) goto cleanup;
        if (!ready || key_interrupted) continue;
        unsigned char byte;
        ssize_t count = read(STDIN_FILENO, &byte, 1);
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (count < 0) goto cleanup;
        if (!count || byte == 4) break;
        if (byte == '\n' || byte == '\r') {
            complete = used > 0;
            break;
        }
        if (byte == 8 || byte == 127 || byte == 21) {
            size_t remove = byte == 21 ? used : used > 0;
            while (remove--) {
                key[--used] = 0;
                /* A full row can leave the cursor in delayed-wrap state. */
                if (column == 0 || column == columns) {
                    if (!column && fputs("\x1b[A", stderr) == EOF) goto cleanup;
                    if (fprintf(stderr, "\r\x1b[%dC \r\x1b[%dC", columns - 1, columns - 1) < 0)
                        goto cleanup;
                    column = columns - 1;
                } else {
                    if (fputs("\b \b", stderr) == EOF) goto cleanup;
                    column--;
                }
            }
            if (fflush(stderr) != 0) goto cleanup;
            continue;
        }
        if (byte <= 0x20 || byte >= 0x7f) {
            failure = "API key contains invalid characters";
            goto cleanup;
        }
        key[used++] = (char)byte;
        if (fputc('*', stderr) == EOF || fflush(stderr) != 0) goto cleanup;
        column = column == columns ? 1 : column + 1;
    }
    if (used > 4096) {
        failure = "API key exceeds the size limit";
        complete = 0;
    } else if (!complete || key_interrupted) {
        failure = "credential entry canceled";
        complete = 0;
    }
cleanup:
    if (hidden_input) {
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &old) != 0) {
            failure = "cannot restore terminal input";
            complete = 0;
        }
        fputc('\n', stderr);
    }
    if (input_flags >= 0 && fcntl(STDIN_FILENO, F_SETFL, input_flags) != 0) {
        failure = "cannot restore input flags";
        complete = 0;
    }
    while (installed) {
        installed--;
        if (sigaction(signals[installed], &previous[installed], NULL) != 0) {
            failure = "cannot restore signal handlers";
            complete = 0;
        }
    }
    if (key_interrupted) {
        failure = "credential entry canceled";
        complete = 0;
    }
    if (!complete) {
        free_key(key);
        snprintf(err, errsz, "%s", failure);
        return NULL;
    }
    return key;
}

static int login_cancelled(void *user) {
    (void)user;
    return key_interrupted != 0;
}

static int login_browser(void *user, const char *url) {
    (void)user;
    if (fprintf(stderr, "Open this URL to sign in to OpenAI:\n%s\n", url) < 0 || fflush(stderr)) return -1;
    pid_t child = fork();
    if (!child) {
        int null = open("/dev/null", O_RDWR);
        if (null < 0 || dup2(null, STDIN_FILENO) < 0 || dup2(null, STDOUT_FILENO) < 0 ||
            dup2(null, STDERR_FILENO) < 0) _exit(1);
        if (null > STDERR_FILENO) close(null);
        pid_t browser = fork();
        if (browser != 0) _exit(browser < 0 ? 1 : 0);
        execlp("xdg-open", "xdg-open", url, (char *)NULL);
        _exit(1);
    }
    if (child > 0) {
        int status;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    }
    return login_cancelled(NULL);
}

static int browser_login(oca_llm *llm, const char *provider, char *err, size_t errsz) {
    const char *kind = oca_provider_credential_kind(provider);
    if (!kind || strcmp(kind, "oauth")) {
        snprintf(err, errsz, "provider browser login is not available for %s", provider);
        return -1;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        snprintf(err, errsz, "OAuth login requires an interactive terminal; run oca -m MODEL");
        return -1;
    }
    const int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT};
    struct sigaction previous[4], action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = key_signal;
    sigemptyset(&action.sa_mask);
    key_interrupted = 0;
    size_t installed = 0;
    int rc = -1;
    for (; installed < sizeof(signals) / sizeof(signals[0]); installed++) {
        if (sigaction(signals[installed], &action, &previous[installed])) {
            snprintf(err, errsz, "cannot install login cancellation handlers");
            goto cleanup;
        }
    }
    oca_auth_ui ui = {.open_browser = login_browser, .cancelled = login_cancelled};
    rc = oca_llm_auth_login(llm, provider, &ui, err, errsz);
cleanup:
    while (installed) {
        installed--;
        if (sigaction(signals[installed], &previous[installed], NULL)) {
            snprintf(err, errsz, "cannot restore login signal handlers");
            rc = -1;
        }
    }
    return rc;
}

/* ============================================================================
 * Run mode
 * ========================================================================== */

static int parse_run_options(int argc, char **argv, run_options *opts,
                             run_kind kind, char *err, size_t errsz) {
    memset(opts, 0, sizeof(*opts));
    opts->workspace = ".";
    opts->exec_mode = kind == RUN_EXEC;
#define FAIL(...) do { \
        snprintf(err, errsz, __VA_ARGS__); \
        return -1; \
    } while (0)
#define TAKE_VALUE(name, field) do { \
        if (++i >= argc) FAIL("option %s requires a value", name); \
        if (opts->field) FAIL("option %s given more than once", name); \
        opts->field = argv[i]; \
    } while (0)
    for (int i = 0; i < argc; i++) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--")) {
            if (kind == RUN_RESUME || opts->prompt) FAIL("duplicate prompt");
            if (i + 1 >= argc) FAIL("missing prompt");
            if (i + 2 != argc) FAIL("unexpected argument %s", argv[i + 1]);
            opts->prompt = argv[++i];
        } else if (arg[0] != '-') {
            if (kind == RUN_RESUME || opts->prompt) FAIL("unexpected argument %s", arg);
            opts->prompt = arg;
        } else if (opts->exec_mode && !strcmp(arg, "--save")) {
            if (opts->save) FAIL("option --save given more than once");
            opts->save = 1;
        } else if (!strcmp(arg, "-m") || !strcmp(arg, "--model")) {
            TAKE_VALUE(arg, model);
        } else if (!strcmp(arg, "-C") || !strcmp(arg, "--chdir")) {
            if (++i >= argc) FAIL("option %s requires a value", arg);
            if (opts->workspace_set) FAIL("option %s given more than once", arg);
            opts->workspace = argv[i];
            opts->workspace_set = 1;
        } else if (!strcmp(arg, "--diagnose")) {
            if (opts->trace) FAIL("option --diagnose given more than once");
            opts->trace = 1;
        } else if (!strcmp(arg, "--reasoning")) {
            if (++i >= argc) FAIL("option %s requires a value", arg);
            if (opts->reasoning_name) FAIL("option %s given more than once", arg);
            if (!*argv[i]) FAIL("invalid value for option %s", arg);
            opts->reasoning_name = argv[i];
        } else {
            FAIL("unknown option: %s", arg);
        }
    }
#undef TAKE_VALUE
    if (opts->exec_mode && (!opts->prompt || !*opts->prompt)) FAIL("missing prompt");
#undef FAIL
    return 0;
}

static void exec_usage(FILE *out) {
    fputs("Run one agent turn without the TUI.\n\n"
          "Usage:\n  oca exec [options] [--] PROMPT\n\n"
          "Options:\n"
          "  --save              Save the session after a successful turn.\n"
          "  -m, --model MODEL   Select a model by ID or alias.\n"
          "  -C, --chdir PATH    Select the workspace root.\n"
          "  --reasoning LEVEL   Select a supported reasoning effort.\n"
          "  --diagnose          Create a diagnostic file for this run.\n"
          "  -h, --help          Show this help.\n\n"
          "The response uses stdout. The session_id=<ID> line uses stderr.\n",
          out);
}

static int append_text(char **buffer, size_t *length, const char *text) {
    size_t add = strlen(text);
    if (add > SIZE_MAX - *length - 1) return -1;
    char *next = realloc(*buffer, *length + add + 1);
    if (!next) return -1;
    memcpy(next + *length, text, add + 1);
    *buffer = next;
    *length += add;
    return 0;
}

static volatile sig_atomic_t run_interrupted;

static void run_signal(int number) {
    run_interrupted = number;
}

/* Poll outside the signal handler so cancellation can take the core lock. */
static int run_wait_event(oca_core *core, oca_core_event *event) {
    for (;;) {
        if (run_interrupted) oca_core_cancel(core);
        if (oca_core_poll_event(core, event)) return 1;
        oca_core_status status;
        oca_core_get_status(core, &status);
        if (status.state == OCA_S_STOPPED) return 0;
        struct pollfd descriptor = {.fd = oca_core_wake_fd(core), .events = POLLIN};
        int ready = poll(&descriptor, 1, 100);
        if (ready < 0 && errno != EINTR) return 0;
        if (ready > 0) {
            char wake[128];
            while (read(descriptor.fd, wake, sizeof(wake)) > 0) {}
        }
    }
}

static int run_save_session(oca_core *core) {
    char err[256];
    if (oca_core_save_session(core, err, sizeof(err))) {
        fprintf(stderr, "oca: %s\n", err);
        return 1;
    }
    oca_core_event event;
    while (run_wait_event(core, &event)) {
        int saved = event.kind == OCA_CE_SESSION;
        int failed = event.kind == OCA_CE_ERROR;
        if (failed) fprintf(stderr, "oca: %s\n", event.text ? event.text : "session save failed");
        oca_core_event_release(&event);
        if (saved || failed) return failed;
    }
    fputs("oca: core stopped before saving the session\n", stderr);
    return 1;
}

static int run_non_interactive(oca_core *core, const char *prompt, int save, int show_id) {
    char err[256];
    char *answer = NULL;
    size_t answer_len = 0;
    int result = 1;
    struct sigaction action, old_int, old_term;
    memset(&action, 0, sizeof(action));
    action.sa_handler = run_signal;
    sigemptyset(&action.sa_mask);
    run_interrupted = 0;
    if (sigaction(SIGINT, &action, &old_int)) {
        fputs("oca: cannot install cancellation handler\n", stderr);
        return 1;
    }
    if (sigaction(SIGTERM, &action, &old_term)) {
        sigaction(SIGINT, &old_int, NULL);
        fputs("oca: cannot install cancellation handler\n", stderr);
        return 1;
    }
    if (show_id) {
        oca_core_status status;
        oca_core_get_status(core, &status);
        if (fprintf(stderr, "session_id=%s\n", status.session_id) < 0 || fflush(stderr)) {
            goto cleanup;
        }
    }
    int rc = oca_core_submit_prompt(core, prompt, err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "oca: %s\n", err);
        goto cleanup;
    }
    int failed = 0;
    for (;;) {
        oca_core_event event;
        if (!run_wait_event(core, &event)) {
            fputs("oca: core stopped before the turn completed\n", stderr);
            failed = 1;
            break;
        }
        if (event.kind == OCA_CE_ASSISTANT_TEXT && event.text) {
            if (append_text(&answer, &answer_len, event.text) != 0) failed = 1;
        } else if (event.kind == OCA_CE_TOOL_START) {
            free(answer);
            answer = NULL;
            answer_len = 0;
        } else if (event.kind == OCA_CE_ERROR) {
            free(answer);
            answer = NULL;
            answer_len = 0;
            fprintf(stderr, "oca: %s%s%s\n", event.name ? event.name : "error",
                    event.text ? ": " : "", event.text ? event.text : "");
            failed = 1;
        } else if (event.kind == OCA_CE_INTERACTION) {
            fputs("oca: the model requested user input in non-interactive mode\n", stderr);
            failed = 1;
            oca_core_cancel(core);
        } else if (event.kind == OCA_CE_TURN_DONE) {
            if (event.text && *event.text) {
                free(answer);
                answer = strdup(event.text);
                answer_len = answer ? strlen(answer) : 0;
            }
            oca_core_event_release(&event);
            break;
        } else if (event.kind == OCA_CE_QUIT_OK) {
            failed = 1;
            oca_core_event_release(&event);
            break;
        }
        oca_core_event_release(&event);
    }
    if (run_interrupted) failed = 1;
    if (!failed && save) failed = run_save_session(core);
    if (!failed && !run_interrupted && answer) {
        fputs(answer, stdout);
        if (answer_len && answer[answer_len - 1] != '\n') fputc('\n', stdout);
    }
    result = failed ? 1 : 0;
    if (run_interrupted) result = 128 + run_interrupted;
cleanup:
    free(answer);
    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGTERM, &old_term, NULL);
    return result;
}

static int validate_saved_config(oca_llm *llm, const config_store *cfg,
                                 char *err, size_t errsz) {
    if (!cfg->has_route) return 0;
    const oca_model *model = oca_llm_model_resolve(llm, cfg->model);
    if (!model) {
        snprintf(err, errsz, "saved model is not configured; select a model");
        return -1;
    }
    if (model->reasoning_count == 0) {
        if (cfg->reasoning[0]) {
            snprintf(err, errsz, "saved reasoning level is not configured for this model");
            return -1;
        }
    } else if (!cfg->reasoning[0] ||
               oca_model_reasoning_index(model, cfg->reasoning) == OCA_REASONING_NONE) {
        snprintf(err, errsz, "saved reasoning level is not configured for this model");
        return -1;
    }
    return 0;
}

static int saved_workspace(const char *session_id, char *out, size_t outsz,
                           char *err, size_t errsz) {
    char directory[OCA_PATH_MAX];
    if (state_path(directory, sizeof(directory), "sessions") != 0) {
        snprintf(err, errsz, "HOME is not set");
        return -1;
    }
    oca_session_summary *sessions = NULL;
    size_t count = 0;
    if (oca_core_scan_sessions(directory, &sessions, &count, err, errsz) != 0)
        return -1;
    int found = 0;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(sessions[i].id, session_id) != 0) continue;
        int n = snprintf(out, outsz, "%s", sessions[i].workspace);
        found = n >= 0 && (size_t)n < outsz;
        break;
    }
    oca_core_free_summaries(sessions, count);
    if (!found) snprintf(err, errsz, "saved session was not found or is invalid");
    return found ? 0 : -1;
}

/* Explicit selection determines the provider before terminal startup.
 * The LLM layer owns model resolution, credential validation, and storage. */
static int login_selected_model(oca_llm *llm, const run_options *opts,
                                 char *err, size_t errsz) {
    if (!opts->model) return 0;
    const oca_model *model = oca_llm_model_resolve(llm, opts->model);
    if (!model) {
        snprintf(err, errsz, "unknown model: %s", opts->model);
        return -1;
    }
    if (opts->reasoning_name &&
        (model->reasoning_count == 0 ||
         oca_model_reasoning_index(model, opts->reasoning_name) == OCA_REASONING_NONE)) {
        oca_model_reasoning_error(model, opts->reasoning_name, err, errsz);
        return -1;
    }
    if (oca_llm_model_available(llm, model)) return 0;
    const char *provider = model->provider;
    const char *kind = oca_provider_credential_kind(provider);
    if (opts->exec_mode || !isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        snprintf(err, errsz, "model %s needs credentials; run oca -m %s in an interactive terminal first",
                 model->id, model->id);
        return -1;
    }
    if (kind && !strcmp(kind, "oauth")) return browser_login(llm, provider, err, errsz);
    if (!kind || strcmp(kind, "api-key")) {
        snprintf(err, errsz, "provider login is not available for %s", provider);
        return -1;
    }
    fprintf(stderr, "Model %s requires credentials for %s.\n", model->id, provider);
    char *key = read_key(err, errsz);
    if (!key) return -1;
    int rc = oca_llm_auth_set_api_key(llm, provider, key, err, errsz);
    free_key(key);
    return rc;
}

static int run_agent(oca_llm *llm, const config_store *cfg, const run_options *opts) {
    int non_interactive = opts->exec_mode;
    char workspace[OCA_PATH_MAX], resume_workspace[OCA_PATH_MAX], err[256];
    const char *selected_workspace = opts->workspace;
    if (opts->resume_id && !opts->workspace_set) {
        if (saved_workspace(opts->resume_id, resume_workspace, sizeof(resume_workspace),
                            err, sizeof(err)) != 0) {
            fprintf(stderr, "oca: %s\n", err);
            return 1;
        }
        selected_workspace = resume_workspace;
    }
    if (chdir(selected_workspace) != 0 || !getcwd(workspace, sizeof(workspace))) {
        fprintf(stderr, "oca: cannot select workspace %s: %s\n",
                selected_workspace, strerror(errno));
        return 1;
    }
    if (login_selected_model(llm, opts, err, sizeof(err)) != 0) {
        fprintf(stderr, "oca: %s\n", err);
        return 1;
    }

    int rc = 1;
    oca_core *core = NULL;
    oca_tool_executor *executor = NULL;
    trace_file trace = {0};
    oca_trace_sink trace_sink = {.user = &trace, .write_record = trace_write};
    config_callbacks callbacks = {.persistence_disabled = opts->persistence_disabled};
    oca_core_route_cbs route = {.user = &callbacks, .persist = save_config};
    char config_path[OCA_PATH_MAX];
    if (state_path(config_path, sizeof(config_path), "config.json") ||
        mkdir_parents(config_path, err, sizeof(err))) goto failed;
    executor = oca_tool_executor_create(getenv("HOME"), workspace, err, sizeof(err));
    if (!executor) goto failed;
    if (opts->trace && open_trace(&trace, err, sizeof(err)) != 0) goto failed;
    oca_core_options options = {
        .workspace = workspace,
        .flags = non_interactive ? OCA_CORE_RUNTIME : OCA_CORE_INTERACTIVE,
        .resume_id = opts->resume_id,
        .model = opts->model,
        .reasoning_name = opts->reasoning_name,
        .last_used_model = cfg->has_route ? cfg->model : NULL,
        .last_used_reasoning = (cfg->has_route && cfg->reasoning[0]) ? cfg->reasoning : NULL,
        .route = &route,
        .trace = opts->trace ? &trace_sink : NULL,
        .executor = executor,
    };
    core = oca_core_create(llm, &options, err, sizeof(err));
    if (!core) goto failed;
    if (non_interactive)
        rc = run_non_interactive(core, opts->prompt, opts->save, opts->exec_mode);
    else
        rc = oca_tui_run(core, workspace, opts->prompt, &cfg->display);
    goto cleanup;

failed:
    fprintf(stderr, "oca: %s\n", err);
cleanup:
    oca_core_destroy(core);
    oca_tool_executor_destroy(executor);
    if (trace.failed) {
        if (!rc) fputs("oca: diagnostic write failed\n", stderr);
        rc = 1;
    }
    if (trace.file && fclose(trace.file) != 0) rc = 1;
    return rc;
}

/* ============================================================================
 * Process entry
 * ============================================================================ */

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], OCA_TOOL_WORKER_FLAG)) return oca_tool_worker_main();
    const struct { const char *name; void (*help)(FILE *); } commands[] = {
        {"models", models_usage}, {"sessions", sessions_usage}, {"resume", resume_usage},
        {"delete", delete_usage}, {"logout", logout_usage}, {"exec", exec_usage}
    };
    void (*command_help)(FILE *) = NULL;
    for (size_t i = 0; argc > 1 && i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (!strcmp(argv[1], commands[i].name)) {
            command_help = commands[i].help;
            break;
        }
    }
    for (int i = 1; i < argc && strcmp(argv[i], "--"); i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            (command_help ? command_help : usage)(stdout);
            return 0;
        }
    }
    if (command_help && argc == 2 && strcmp(argv[1], "models") && strcmp(argv[1], "sessions")) {
        const char *message = !strcmp(argv[1], "resume") || !strcmp(argv[1], "delete") ?
            "missing session ID" : !strcmp(argv[1], "logout") ? "missing provider" : "missing prompt";
        return fail_usage(argv[1], message);
    }
    char err[256];
    run_options opts = {0};
    int agent = !command_help || !strcmp(argv[1], "exec") || !strcmp(argv[1], "resume");
    if (agent) {
        run_kind kind = argc > 1 && !strcmp(argv[1], "exec") ? RUN_EXEC :
            argc > 1 && !strcmp(argv[1], "resume") ? RUN_RESUME : RUN_INTERACTIVE;
        int offset = kind == RUN_EXEC ? 2 : kind == RUN_RESUME ? 3 : 1;
        if (kind == RUN_RESUME && (argv[2][0] == '-' || !argv[2][0])) {
            return fail_usage("resume", "missing session ID");
        }
        if (parse_run_options(argc - offset, argv + offset, &opts, kind, err, sizeof(err))) {
            return fail_usage(kind == RUN_INTERACTIVE ? NULL : argv[1], err);
        }
        if (kind == RUN_RESUME) opts.resume_id = argv[2];
    }
    if (!agent && !strcmp(argv[1], "sessions")) return command_sessions(argc - 2);
    if (!agent && !strcmp(argv[1], "delete")) return command_delete(argc - 2, argv + 2);
    if (!agent) {
        if (!strcmp(argv[1], "models") && argc != 2)
            return fail_usage("models", "unexpected argument");
        const char *provider = NULL;
        int yes = 0;
        if (!strcmp(argv[1], "logout")) {
            if (parse_confirmed_target(argc - 2, argv + 2, &provider, &yes))
                return fail_usage("logout", "expected a provider and optional -y or --yes");
        }
        size_t count = 0;
        const oca_model_definition *models = oca_llm_builtin_models(&count);
        int listing = !strcmp(argv[1], "models");
        oca_llm *llm = oca_llm_create(NULL, listing ? models : NULL,
                                      listing ? count : 0, err, sizeof(err));
        if (!llm) {
            fprintf(stderr, "oca: %s\n", err);
            return 1;
        }
        size_t ignored = oca_llm_unsupported_provider_count(llm);
        if (ignored)
            fprintf(stderr, "oca: warning: %zu unsupported stored provider(s) ignored; credentials preserved\n", ignored);
        int rc = listing ? command_models(llm) : command_logout(llm, provider, yes);
        oca_llm_destroy(llm);
        return rc;
    }
    config_store cfg;
    if (load_config(&cfg, err, sizeof(err))) {
        fprintf(stderr, "oca: warning: %s; using an empty configuration\n", err);
        opts.persistence_disabled = 1;
        if (empty_config(&cfg, err, sizeof(err))) {
            fprintf(stderr, "oca: %s\n", err);
            return 1;
        }
    }
    if (!opts.persistence_disabled && migrate_model_aliases(&cfg, err, sizeof(err)))
        fprintf(stderr, "oca: warning: previous model aliases not transferred: %s\n", err);
    size_t model_count = 0;
    const oca_model_definition *builtins = oca_llm_builtin_models(&model_count);
    oca_model_definition *definitions = calloc(model_count, sizeof(*definitions));
    if (!definitions) {
        fputs("oca: out of memory\n", stderr);
        free_config(&cfg);
        return 1;
    }
    for (size_t i = 0; i < model_count; i++) {
        definitions[i] = builtins[i];
        const cJSON *alias = cJSON_GetObjectItemCaseSensitive(cfg.model_aliases, builtins[i].id);
        if (alias) definitions[i].name = alias->valuestring;
    }
    oca_llm *llm = oca_llm_create(NULL, definitions, model_count, err, sizeof(err));
    free(definitions);
    if (!llm) {
        fprintf(stderr, "oca: %s\n", err);
        free_config(&cfg);
        return 1;
    }
    size_t ignored = oca_llm_unsupported_provider_count(llm);
    if (ignored)
        fprintf(stderr, "oca: warning: %zu unsupported stored provider(s) ignored; credentials preserved\n", ignored);
    if (validate_saved_config(llm, &cfg, err, sizeof(err)) != 0) {
        fprintf(stderr, "oca: warning: %s; saved route ignored\n", err);
        cfg.has_route = 0;
    }
    int rc = run_agent(llm, &cfg, &opts);
    oca_llm_destroy(llm);
    free_config(&cfg);
    return rc;
}
