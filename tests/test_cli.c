#define _XOPEN_SOURCE 700
#include "fixture_server.h"
#include "fixture_models.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <ftw.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    int status;
    char out[16384];
    char err[16384];
} result;

static int cancel_after_id;
static int fail_id_output;
static const char *confirmation = "";

static void read_pipe(int fd, char *buffer, size_t size) {
    size_t used = 0;
    ssize_t got;
    while (used + 1 < size && (got = read(fd, buffer + used, size - used - 1)) > 0)
        used += (size_t)got;
    buffer[used] = '\0';
    close(fd);
}

static result run(const char *executable, char *const args[],
                  const char *home, const char *workspace) {
    int out_pipe[2], err_pipe[2], input_pipe[2];
    assert(pipe(out_pipe) == 0 && pipe(err_pipe) == 0 && pipe(input_pipe) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        alarm(30);
        assert(chdir(workspace) == 0);
        assert(setenv("HOME", home, 1) == 0);
        assert(dup2(input_pipe[0], STDIN_FILENO) >= 0);
        close(input_pipe[0]); close(input_pipe[1]);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        if (fail_id_output) {
            int full = open("/dev/full", O_WRONLY);
            assert(full >= 0 && dup2(full, STDERR_FILENO) >= 0);
            close(full);
        }
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        execv(executable, args);
        _exit(127);
    }
    close(input_pipe[0]);
    if (*confirmation) assert(write(input_pipe[1], confirmation, strlen(confirmation)) == (ssize_t)strlen(confirmation));
    close(input_pipe[1]);
    close(out_pipe[1]);
    close(err_pipe[1]);
    result value = {0};
    size_t prefix = 0;
    if (cancel_after_id) {
        while (prefix + 1 < sizeof(value.err) && !strchr(value.err, '\n')) {
            struct pollfd descriptor = {.fd = err_pipe[0], .events = POLLIN};
            assert(poll(&descriptor, 1, 5000) > 0);
            assert(read(err_pipe[0], value.err + prefix, 1) == 1);
            prefix++;
        }
        assert(strstr(value.err, "session_id="));
        assert(kill(child, cancel_after_id) == 0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    value.status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    read_pipe(out_pipe[0], value.out, sizeof(value.out));
    read_pipe(err_pipe[0], value.err + prefix, sizeof(value.err) - prefix);
    return value;
}

static void write_file(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fputs(text, file) >= 0);
    assert(fclose(file) == 0);
}

static int remove_cli_fixture(const char *path, const struct stat *state, int kind, struct FTW *walk) {
    (void)state; (void)walk;
    return kind == FTW_DP ? rmdir(path) : unlink(path);
}

static void session_id(const result *value, char id[33]) {
    assert(!strncmp(value->err, "session_id=", 11));
    memcpy(id, value->err + 11, 32);
    id[32] = 0;
    assert(strspn(id, "0123456789abcdef") == 32 && value->err[43] == '\n');
}

static char *snapshot_text(const char *path) {
    FILE *file = fopen(path, "rb");
    assert(file && !fseek(file, 0, SEEK_END));
    long length = ftell(file);
    assert(length > 0 && length < 1024 * 1024 && !fseek(file, 0, SEEK_SET));
    char *text = calloc((size_t)length + 1, 1);
    assert(text && fread(text, 1, (size_t)length, file) == (size_t)length);
    assert(!fclose(file));
    return text;
}

static int has_model_auth(const char *text, const char *id, const char *provider, const char *auth) {
    for (const char *row = text; row && *row; ) {
        char model[64], owner[64], method[32];
        if (sscanf(row, "%63s %63s %31[^\n]", model, owner, method) == 3 &&
            !strcmp(model, id) && !strcmp(owner, provider)) {
            const char *header = strstr(text, "AUTH");
            const char *levels = strstr(text, "REASONING");
            if (header && levels && levels > header) {
                size_t offset = (size_t)(header - text);
                if (!strncmp(row + offset, auth, strlen(auth)) && row[offset + strlen(auth)] == ' ')
                    return 1;
            }
        }
        row = strchr(row, '\n');
        if (row) row++;
    }
    return 0;
}

static void test_exec(const char *executable, const char *home, const char *workspace) {
    char id[33], saved_id[33], path[1024], saved_path[1024];
    char *plain[] = {(char *)executable, "exec", "exec-unsaved-fixture", "-m", "kimi-k3", NULL};
    result value = run(executable, plain, home, workspace);
    assert(!value.status && !strcmp(value.out, "Received: exec-unsaved-fixture\n"));
    session_id(&value, id);
    snprintf(path, sizeof(path), "%s/.oca/sessions/%s.oca", home, id);
    assert(access(path, F_OK));
    if (access("/dev/full", W_OK) == 0) {
        fail_id_output = 1;
        value = run(executable, plain, home, workspace);
        fail_id_output = 0;
        assert(value.status == 1 && !*value.out && !*value.err);
    }
    char *save[] = {(char *)executable, "exec", "--save", "exec-first-fixture", "-m", "kimi-k3", NULL};
    value = run(executable, save, home, workspace);
    assert(!value.status);
    session_id(&value, saved_id);
    snprintf(saved_path, sizeof(saved_path), "%s/.oca/sessions/%s.oca", home, saved_id);
    char *before = snapshot_text(saved_path);
    assert(strstr(before, "exec-first-fixture"));
    char *removed_resume[] = {(char *)executable, "exec", "--resume", saved_id, "unused", NULL};
    value = run(executable, removed_resume, home, workspace);
    assert(value.status == 2 && !*value.out);
    assert(strncmp(value.err, "session_id=", 11) && !strstr(value.err, "\nsession_id="));
    char *after = snapshot_text(saved_path);
    assert(!strcmp(before, after));
    free(after);

    char *failed_save[] = {(char *)executable, "exec", "--save", "fixture:error", "-m", "kimi-k3", NULL};
    value = run(executable, failed_save, home, workspace);
    assert(value.status == 1 && !*value.out);
    session_id(&value, id);
    snprintf(path, sizeof(path), "%s/.oca/sessions/%s.oca", home, id);
    assert(access(path, F_OK));
    failed_save[3] = "fixture:hang";
    const int signals[] = {SIGINT, SIGTERM};
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        cancel_after_id = signals[i];
        value = run(executable, failed_save, home, workspace);
        cancel_after_id = 0;
        assert(value.status == 128 + signals[i] && !*value.out);
        session_id(&value, id);
        snprintf(path, sizeof(path), "%s/.oca/sessions/%s.oca", home, id);
        assert(access(path, F_OK));
        after = snapshot_text(saved_path);
        assert(!strcmp(before, after));
        free(after);
    }
    char config_path[1024], auth_path[1024];
    snprintf(config_path, sizeof(config_path), "%s/.oca/config.json", home);
    snprintf(auth_path, sizeof(auth_path), "%s/.oca/auth.json", home);
    char *config_before = snapshot_text(config_path);
    char *auth_before = access(auth_path, F_OK) ? NULL : snapshot_text(auth_path);
    write_file(config_path, "{broken-config");
    write_file(auth_path, "{broken-auth");
    char *listed[] = {(char *)executable, "sessions", NULL};
    value = run(executable, listed, home, workspace);
    assert(!value.status && !*value.err && strstr(value.out, saved_id));
    assert(!strstr(value.out, workspace) && !strstr(value.out, "kimi-k3"));
    const char *session_row = strstr(value.out, saved_id);
    assert(strstr(value.out, "LAST USED") && strstr(value.out, "TITLE"));
    int year, month, day, hour, minute;
    assert(sscanf(session_row + 32, "%d-%d-%d %d:%d", &year, &month, &day, &hour, &minute) == 5);
    assert(year >= 2020 && month >= 1 && month <= 12 && day >= 1 && day <= 31);
    assert(hour >= 0 && hour < 24 && minute >= 0 && minute < 60);
    char *delete[] = {(char *)executable, "delete", saved_id, NULL};
    confirmation = "n\n";
    value = run(executable, delete, home, workspace);
    confirmation = "";
    assert(!value.status && strstr(value.err, "Delete saved session?") && !access(saved_path, F_OK));
    const char *yes[] = {"-y", "--yes"};
    for (size_t i = 0; i < 2; i++) {
        write_file(saved_path, before);
        char *remove[] = {(char *)executable, "delete", i ? saved_id : (char *)yes[i],
                          i ? (char *)yes[i] : saved_id, NULL};
        value = run(executable, remove, home, workspace);
        assert(!value.status && !*value.err && access(saved_path, F_OK));
    }
    write_file(config_path, config_before);
    free(config_before);
    if (auth_before) {
        write_file(auth_path, auth_before);
        free(auth_before);
    } else assert(!unlink(auth_path));
    free(before);
    snprintf(path, sizeof(path), "%s/.oca/sessions", home);
    assert(!rmdir(path));
    write_file(path, "blocked save directory");
    value = run(executable, save, home, workspace);
    assert(value.status == 1 && !*value.out && strstr(value.err, "session_id="));
    assert(!unlink(path));
    char *missing_resume[] = {(char *)executable, "resume", "not-a-session", NULL};
    value = run(executable, missing_resume, home, workspace);
    assert(value.status == 1 && !strstr(value.err, "session_id="));
    char *empty[] = {(char *)executable, "exec", "--save", NULL};
    value = run(executable, empty, home, workspace);
    assert(value.status == 2 && strncmp(value.err, "session_id=", 11));
}

static void test_alias_defaults(const char *executable, const char *home, const char *workspace) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/.oca/config.json", home);
    char *before = snapshot_text(path);
    write_file(path, "{\"model_aliases\":{\"kimi-k3\":\"fast\"},"
                     "\"last_used\":{\"model\":\"kimi-k3\",\"reasoning\":\"low\"}}");
    char *args[] = {(char *)executable, "exec", "-m", "fast", "hello", NULL, NULL, NULL};
    result value = run(executable, args, home, workspace);
    assert(!value.status && !strcmp(value.out, "Received: hello\n"));
    char *after = snapshot_text(path);
    assert(strstr(after, "high") && !strstr(after, "low") && strstr(after, "fast"));
    free(after);
    args[5] = "--reasoning";
    args[6] = "max";
    value = run(executable, args, home, workspace);
    assert(!value.status);
    after = snapshot_text(path);
    assert(strstr(after, "max"));
    free(after);
    args[6] = "invalid";
    value = run(executable, args, home, workspace);
    assert(value.status && !*value.out && strstr(value.err, "Available levels: low, high, max"));
    assert(!strstr(value.err, "session_id="));
    write_file(path, before);
    free(before);
}

static void test_config_recovery(const char *executable, const char *home, const char *workspace) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/.oca/config.json", home);
    char *saved = snapshot_text(path);
    const char *broken[] = {"{\"network\":tru\n", "{\"unknown\":true}",
        "{\"filesystem\":{\"relative\":\"write\"}}",
        "{\"models\":{\"x\":{\"id\":\"x\"}}}"};
    char *normal[] = {(char *)executable, "exec", "-m", "kimi-k3", "--reasoning", "low", "recovery fixture", NULL};
    for (size_t i = 0; i < sizeof(broken) / sizeof(broken[0]); i++) {
        write_file(path, broken[i]);
        result value = run(executable, normal, home, workspace);
        assert(value.status == 0 && strstr(value.out, "Received: recovery fixture"));
        assert(strstr(value.err, "using an empty configuration"));
        char *same = snapshot_text(path);
        assert(!strcmp(same, broken[i]));
        free(same);
    }
    assert(!unlink(path));
    result value = run(executable, normal, home, workspace);
    assert(value.status == 0 && !strstr(value.err, "config file rejected") &&
           strstr(value.out, "Received: recovery fixture"));
    write_file(path, saved);
    free(saved);
}

int main(int argc, char **argv) {
    assert(unsetenv("KIMI_API_KEY") == 0);
    assert(unsetenv("DEEPSEEK_API_KEY") == 0);
    fixture_server server = fixture_server_start();
    assert(argc == 3);
    char production[4096];
    assert(realpath(argv[2], production));
    char executable[4096];
    assert(realpath(argv[1], executable));
    char root[] = "/tmp/oca-cli-test-XXXXXX";
    assert(mkdtemp(root));
    char home[512], workspace[512], path[1024];
    snprintf(home, sizeof(home), "%s/home", root);
    snprintf(workspace, sizeof(workspace), "%s/work", root);
    assert(mkdir(home, 0700) == 0);
    assert(mkdir(workspace, 0700) == 0);
    fixture_write_config(home);

    char *help[] = { executable, "--help", NULL };
    result value = run(executable, help, home, workspace);
    assert(value.status == 0 && strstr(value.out, "An agent."));
    assert(strstr(value.out, "--diagnose"));
    assert(!strstr(value.out, "--debug") && !strstr(value.out, "--trace"));
    assert(!strstr(value.out, "  auth ") && !strstr(value.out, "  skills ") && !strstr(value.out, "  sandbox "));
    const char *commands[] = {"models", "sessions", "resume", "delete", "logout", "exec"};
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        char *args[] = {production, (char *)commands[i], NULL, NULL};
        value = run(production, args, home, workspace);
        assert(value.status == (i < 2 ? 0 : 2));
        args[2] = "-h";
        result short_help = run(production, args, home, workspace);
        args[2] = "--help";
        value = run(production, args, home, workspace);
        assert(!short_help.status && !value.status && !*value.err && !*short_help.err);
        assert(!strcmp(short_help.out, value.out) && strstr(value.out, "Usage:"));
        if (!strcmp(commands[i], "exec")) assert(!strstr(value.out, "--resume"));
        if (!strcmp(commands[i], "delete") || !strcmp(commands[i], "logout"))
            assert(strstr(value.out, "Options:\n") && strstr(value.out, "-y, --yes"));
    }
    const char *help_after[] = {"exec", "resume", "delete", "logout"};
    for (size_t i = 0; i < sizeof(help_after) / sizeof(help_after[0]); i++) {
        char *args[] = {production, (char *)help_after[i], "unused", "--help", NULL};
        value = run(production, args, home, workspace);
        assert(!value.status && strstr(value.out, "Usage:") && !*value.err);
    }
    char *run_help[] = {production, "-m", "missing-model", "--help", NULL};
    value = run(production, run_help, home, workspace);
    assert(!value.status && strstr(value.out, "An agent.") && !*value.err);
    const char *obsolete[][4] = {
        {"auth", "--login", "openai", NULL}, {"auth", "--api-key", "kimi", "--stdin"},
        {"auth", "--logout", "kimi", NULL}, {"auth", "--status", NULL, NULL},
        {"skills", "--list", NULL, NULL}, {"skills", "--show", "example", NULL},
        {"skills", "--check", NULL, NULL}, {"models", "--list", NULL, NULL},
        {"models", "--providers", NULL, NULL}, {"models", "--provider", "kimi", NULL},
        {"sessions", "--list", NULL, NULL}, {"sessions", "--resume", "missing", NULL},
        {"sessions", "--delete", "missing", NULL}, {"exec", "--trace", "unused", NULL},
        {"exec", "--debug", "unused", NULL},
        {"delete", "--yes", NULL, NULL}, {"delete", "x", "-y", "--yes"},
        {"logout", "kimi", "--unknown", NULL}, {"resume", "--unknown", NULL, NULL}
    };
    for (size_t i = 0; i < sizeof(obsolete) / sizeof(obsolete[0]); i++) {
        char *args[] = {production, (char *)obsolete[i][0], (char *)obsolete[i][1],
                        (char *)obsolete[i][2], (char *)obsolete[i][3], NULL};
        value = run(production, args, home, workspace);
        assert(value.status == 2 && !*value.out && !strstr(value.err, "API key:"));
        assert(strncmp(value.err, "session_id=", 11) && !strstr(value.err, "\nsession_id="));
    }

    char *bad[] = { executable, "--unknown", NULL };
    value = run(executable, bad, home, workspace);
    assert(value.status == 2 && strstr(value.err, "unknown option: --unknown"));

    char *normal[] = { executable, "exec", "hello", "-m", "kimi-k3",
                       "--reasoning", "low", NULL };
    value = run(executable, normal, home, workspace);
    assert(value.status == 0);
    assert(!strcmp(value.out, "Received: hello\n"));
    assert(strstr(value.err, "session_id="));
    test_alias_defaults(executable, home, workspace);
    test_exec(executable, home, workspace);
    snprintf(path, sizeof(path), "%s/.oca/config.json", home);
    struct stat state;
    assert(stat(path, &state) == 0 && (state.st_mode & 0777) == 0600);
    test_config_recovery(executable, home, workspace);

    const char *removed_flags[] = {"-n", "--tokens", "--temp", "--non-interactive", "-p", "--prompt"};
    for (size_t i = 0; i < sizeof(removed_flags) / sizeof(removed_flags[0]); i++) {
        char *args[] = {executable, "exec", "hello", (char *)removed_flags[i], "1", NULL};
        value = run(executable, args, home, workspace);
        assert(value.status == 2 && !*value.out && strncmp(value.err, "session_id=", 11));
        assert(!strstr(value.err, "\nsession_id="));
    }


    const char *removed[] = {"smoke-basic", "smoke-reasoning", "smoke", "basic", "sreasoning",
                            "ds4f", "ds4p", "deepseek-v4-pro", "deepseek-v4-flash"};
    for (size_t i = 0; i < sizeof(removed) / sizeof(removed[0]); i++) {
        char *selection[] = {production, "exec", "-m", (char *)removed[i], "x", NULL};
        value = run(production, selection, home, workspace);
        assert(value.status == 1 && strstr(value.err, "unknown model"));
    }
    assert(unsetenv("KIMI_API_KEY") == 0);
    value = run(executable, normal, home, workspace);
    assert(value.status == 1 && strstr(value.err, "needs credentials") && !*value.out);
    assert(setenv("KIMI_API_KEY", "fixture-api-key", 1) == 0);

    char *models[] = { executable, "models", NULL };
    value = run(production, models, home, workspace);
    assert(value.status == 0 && strstr(value.out, "k3") && !strstr(value.out, "kimi-k3") &&
           !strstr(value.out, "smoke"));
    assert(!strncmp(value.out, "MODEL", 5) && !strchr(value.out, '\t'));
    const char *model_header = strstr(value.out, "MODEL"), *provider_header = strstr(value.out, "PROVIDER");
    const char *auth_header = strstr(value.out, "AUTH");
    assert(model_header && provider_header && auth_header);
    size_t model_column = (size_t)(model_header - value.out);
    size_t provider_column = (size_t)(provider_header - value.out), auth_column = (size_t)(auth_header - value.out);
    for (const char *row = strchr(value.out, '\n') + 1; *row; row = strchr(row, '\n') + 1) {
        char id[64], provider[64], method[32];
        assert(sscanf(row, "%63s %63s %31[^\n]", id, provider, method) == 3);
        assert(!strncmp(row + model_column, id, strlen(id)));
        assert(!strncmp(row + provider_column, provider, strlen(provider)));
        assert(!strncmp(row + auth_column, method, strlen(method)));
    }
    size_t rows = 0;
    for (const char *at = value.out; *at; at++) if (*at == '\n') rows++;
    assert(rows == 14 && strstr(value.out, "signed out"));
    assert(has_model_auth(value.out, "gpt-6.1-sol", "openai", "signed out"));
    assert(has_model_auth(value.out, "gpt-6.1-terra", "openai", "signed out"));
    assert(has_model_auth(value.out, "k3.1-pro", "kimi", "api-key"));
    assert(!strstr(value.out, "context=") && !strstr(value.out, "reasoning:") && !strstr(value.out, "capabilities:"));
    assert(strstr(value.out, "deepseek-flash"));
    assert(!strstr(value.out, "ds4") && !strstr(value.out, "deepseek-v4-pro"));
    assert(!strstr(value.out, "output=") && !strstr(value.out, "temperature"));

    {
        char key_path[1024];
        snprintf(key_path, sizeof(key_path), "%s/.oca/auth.json", home);
        const char *seed = "{\"providers\":{"
            "\"kimi\":{\"auth_method\":\"api-key\",\"api_key\":\"fixture-retained-key\"},"
            "\"deepseek\":{\"auth_method\":\"api-key\",\"api_key\":\"fixture-logout-key\"},"
            "\"openai\":{\"auth_method\":\"oauth\",\"access_token\":\"fixture-access\","
            "\"refresh_token\":\"fixture-refresh\",\"account_id\":\"fixture-account\",\"expires_at\":4102444800}}}";
        const char *yes[] = {"-y", "--yes"};
        for (size_t i = 0; i < 2; i++) {
            write_file(key_path, seed);
            value = run(production, models, home, workspace);
            assert(!value.status && has_model_auth(value.out, "deepseek-flash", "deepseek", "api-key"));
            assert(has_model_auth(value.out, "gpt-6-astra", "openai", "oauth") && !strstr(value.out, "fixture-access"));
            assert(!strstr(value.out, "fixture-logout-key") && !strstr(value.err, "fixture-logout-key"));
            char *logout[] = {production, "logout", "deepseek", NULL};
            confirmation = "n\n";
            value = run(production, logout, home, workspace);
            confirmation = "";
            char *retained = snapshot_text(key_path);
            assert(!value.status && !strcmp(retained, seed));
            free(retained);
            char *remove[] = {production, "logout", i ? "deepseek" : (char *)yes[i],
                              i ? (char *)yes[i] : "deepseek", NULL};
            value = run(production, remove, home, workspace);
            assert(!value.status && !*value.err);
            retained = snapshot_text(key_path);
            assert(!strstr(retained, "fixture-logout-key") && strstr(retained, "fixture-retained-key"));
            free(retained);
            value = run(production, models, home, workspace);
            assert(!value.status && has_model_auth(value.out, "deepseek-flash", "deepseek", "signed out"));
        }
        assert(!unlink(key_path));
    }

    char trace[1024];
    char *traced[] = { executable, "exec", "diagnose",
                       "--diagnose", "-m", "kimi-k3", "--reasoning", "low", NULL };
    value = run(executable, traced, home, workspace);
    assert(value.status == 0);
    char trace_dir[1024];
    snprintf(trace_dir, sizeof(trace_dir), "%s/.oca/diagnose", home);
    DIR *directory = opendir(trace_dir);
    assert(directory);
    struct dirent *entry;
    int traces = 0;
    while ((entry = readdir(directory))) {
        if (!strstr(entry->d_name, ".log")) continue;
        assert(snprintf(trace, sizeof(trace), "%s/%s", trace_dir, entry->d_name) < (int)sizeof(trace));
        traces++;
    }
    assert(!closedir(directory) && traces == 1);
    FILE *trace_file = fopen(trace, "rb");
    assert(trace_file);
    char trace_text[8192] = {0};
    size_t trace_len = fread(trace_text, 1, sizeof(trace_text) - 1, trace_file);
    assert(!ferror(trace_file) && fclose(trace_file) == 0);
    assert(trace_len > 0 && strstr(trace_text, " core {") && strstr(trace_text, " llm {"));
    assert(!strstr(trace_text, "\n\n"));
    assert(stat(trace, &state) == 0 && (state.st_mode & 0777) == 0600);
    assert(!unlink(trace) && !rmdir(trace_dir));
    write_file(trace_dir, "blocked trace directory");
    value = run(executable, traced, home, workspace);
    assert(value.status == 1 && strstr(value.err, "diagnostic"));
    assert(!unlink(trace_dir));

    char saved_route[1024];
    snprintf(saved_route, sizeof(saved_route), "{\"last_used\":{\"model\":\"smoke-basic\",\"reasoning\":\"off\"}}\n");
    write_file(path, saved_route);
    value = run(production, models, home, workspace);
    assert(value.status == 0 && !*value.err);

    snprintf(saved_route, sizeof(saved_route), "{\"last_used\":{\"model\":\"retired-model\",\"reasoning\":\"off\"}}\n");
    write_file(path, saved_route);
    value = run(executable, models, home, workspace);
    assert(value.status == 0 && !*value.err);
    char auth_path[1024];
    snprintf(auth_path, sizeof(auth_path), "%s/.oca/auth.json", home);
    write_file(auth_path, "{\"providers\":{\"retired\":{\"token\":\"private-test-token\"}}}");
    value = run(executable, models, home, workspace);
    assert(value.status == 0 && strstr(value.err, "unsupported stored provider"));
    assert(!strstr(value.err, "private-test-token") && !strstr(value.out, "private-test-token"));
    value = run(executable, traced, home, workspace);
    assert(value.status == 0 && strstr(value.err, "saved route ignored"));
    assert(unlink(auth_path) == 0);

    write_file(path, "{\"unknown\":true}\n");
    value = run(executable, models, home, workspace);
    assert(value.status == 0 && !*value.err);

    assert(!nftw(root, remove_cli_fixture, 16, FTW_DEPTH | FTW_PHYS));
    fixture_server_stop(&server);
    puts("test_cli: passed");
    return 0;
}
