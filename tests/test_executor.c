/* Ordinary tool executor checks. These run offline with a local worker. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../oca_core.h"
#undef OCA_BASH_TIMEOUT_SECONDS
#define OCA_BASH_TIMEOUT_SECONDS 1
#include "../oca_core.c"

#include <arpa/inet.h>
#include <assert.h>
#include <ftw.h>
#include <netinet/in.h>

static int remove_fixture(const char *path, const struct stat *state, int kind, struct FTW *walk) {
    (void)state; (void)walk;
    return kind == FTW_DP ? rmdir(path) : unlink(path);
}

static void write_fixture(const char *path, const char *value) {
    FILE *file = fopen(path, "wb");
    assert(file && fputs(value, file) >= 0 && fclose(file) == 0);
}

static tool_outcome call(oca_tool_executor *executor, const char *name, const char *json) {
    tool_outcome out = {0};
    executor_call(executor, executor->workspace, name, json, NULL, &out);
    assert(out.text);
    return out;
}

static void expect(oca_tool_executor *executor, const char *name, const char *json,
                    oca_tool_state state, const char *text) {
    tool_outcome out = call(executor, name, json);
    if (out.state != state || !strstr(out.text, text)) {
        fprintf(stderr, "tool %s: state %d, result %s\n", name, out.state, out.text);
        abort();
    }
    free(out.text);
}

static void *cancel_after_ready(void *data) {
    atomic_int *cancel = data;
    struct timespec delay = {0, 150 * 1000 * 1000};
    nanosleep(&delay, NULL);
    atomic_store(cancel, 1);
    return NULL;
}

typedef struct {
    oca_tool_executor *executor;
    tool_outcome result;
    atomic_int done;
} stream_check;

static void *stream_command(void *data) {
    stream_check *check = data;
    cJSON *args = cJSON_CreateObject();
    assert(args && cJSON_AddStringToObject(args, "command",
        "printf ready; while [ ! -e .stream-release ]; do sleep 0.01; done; "
        "printf '\\342'; sleep 0.02; printf '\\202\\254done'"));
    char *json = cJSON_PrintUnformatted(args);
    assert(json);
    check->result = call(check->executor, "bash", json);
    cJSON_free(json);
    cJSON_Delete(args);
    atomic_store(&check->done, 1);
    return NULL;
}

static void check_streaming(oca_tool_executor *executor, const char *workspace) {
    oca_core progress = {.mu = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER,
                         .wake_pipe = {-1, -1}};
    atomic_init(&progress.running, 1);
    stream_check check = {.executor = executor};
    atomic_init(&check.done, 0);
    executor->progress = &progress;
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, stream_command, &check));
    sbuf output;
    sb_init(&output);
    int released = 0;
    for (int i = 0; i < 5000 && !atomic_load(&check.done); i++) {
        oca_core_event event;
        while (oca_core_poll_event(&progress, &event)) {
            assert(event.kind == OCA_CE_TOOL_OUTPUT && utf8_text_valid(event.text, strlen(event.text)));
            sb_puts(&output, event.text);
            oca_core_event_release(&event);
        }
        if (!released && output.buf && strstr(output.buf, "ready")) {
            assert(!atomic_load(&check.done));
            char path[1024];
            snprintf(path, sizeof(path), "%s/.stream-release", workspace);
            write_fixture(path, "release");
            released = 1;
        }
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    assert(!pthread_join(thread, NULL) && released && check.result.state == OCA_TOOL_COMPLETE);
    oca_core_event event;
    while (oca_core_poll_event(&progress, &event)) {
        assert(event.kind == OCA_CE_TOOL_OUTPUT && utf8_text_valid(event.text, strlen(event.text)));
        sb_puts(&output, event.text);
        oca_core_event_release(&event);
    }
    assert(output.buf && !strcmp(output.buf, "ready€done"));
    assert(strstr(check.result.text, "ready€done") && strstr(check.result.text, "[exit 0]"));
    free(check.result.text);
    sb_free(&output);
    executor->progress = NULL;
    pthread_cond_destroy(&progress.cond);
    pthread_mutex_destroy(&progress.mu);
}

static int offline_checks(const char *image) {
    (void)image;
    char root[] = "/tmp/oca-executor-test-XXXXXX";
    assert(mkdtemp(root));
    char home[512], work[512], nested[1024], path[1024], json[8192], err[512];
    snprintf(home, sizeof(home), "%s/home", root);
    snprintf(work, sizeof(work), "%s/work", root);
    snprintf(nested, sizeof(nested), "%s/private", root);
    assert(!mkdir(home, 0700) && !mkdir(work, 0700) && !mkdir(nested, 0700));
    assert(!setenv("HOME", home, 1) && !setenv("OCA_TEST_ENV", "ordinary-environment", 1));
    snprintf(path, sizeof(path), "%s/.oca", home); assert(!mkdir(path, 0700));
    snprintf(path, sizeof(path), "%s/.oca/auth.json", home); write_fixture(path, "private test data");

    oca_tool_executor *executor = oca_tool_executor_create(home, work, err, sizeof(err));
    if (!executor) { fprintf(stderr, "executor startup: %s\n", err); abort(); }
    assert(executor->ready && !strcmp(executor->workspace, work));
    check_streaming(executor, work);
    snprintf(json, sizeof(json), "{\"path\":\"%s\"}", path);
    expect(executor, "read", json, OCA_TOOL_COMPLETE, "private test data");
    assert(snprintf(path, sizeof(path), "%s/new-file", nested) < (int)sizeof(path));
    snprintf(json, sizeof(json), "{\"path\":\"%s\",\"content\":\"outside workspace\"}", path);
    expect(executor, "write", json, OCA_TOOL_COMPLETE, "File written.");
    assert(!access(path, F_OK));
    /* The write summary counts only the lines the diff actually changes. */
    expect(executor, "write", "{\"path\":\"diff.txt\",\"content\":\"one\\ntwo\\nthree\\n\"}",
           OCA_TOOL_COMPLETE, "File written. (+3 -0)");
    expect(executor, "write", "{\"path\":\"diff.txt\",\"content\":\"one\\ntwo\\nthree\\n\"}",
           OCA_TOOL_COMPLETE, "File written. (+0 -0)");
    expect(executor, "write", "{\"path\":\"diff.txt\",\"content\":\"one\\nTWO\\nthree\\nfour\\n\"}",
           OCA_TOOL_COMPLETE, "File written. (+2 -1)");
    expect(executor, "write", "{\"path\":\"diff.txt\",\"content\":\"a\\nb\\nc\\nd\\ne\\n\"}",
           OCA_TOOL_COMPLETE, "File written. (+5 -4)");
    expect(executor, "write", "{\"path\":\"diff.txt\",\"content\":\"a\\nB\\nc\\nD\\ne\\n\"}",
           OCA_TOOL_COMPLETE, "File written. (+2 -2)");
    snprintf(path, sizeof(path), "%s/large-diff.txt", work);
    FILE *large = fopen(path, "w");
    assert(large);
    for (int i = 1; i <= 9000; i++) assert(fprintf(large, "original line %04d\n", i) > 0);
    assert(!fclose(large));
    tool_outcome full = call(executor, "write", "{\"path\":\"large-diff.txt\",\"content\":\"replacement\\n\"}");
    assert(full.state == OCA_TOOL_COMPLETE && strlen(full.text) > OCA_TOOL_RESULT_MAX_BYTES);
    assert(strstr(full.text, "9000 - original line 9000") && strstr(full.text, "1 + replacement"));
    assert(!strstr(full.text, "[truncated]"));
    char *context_only = tool_context_text(full.text);
    assert(context_only && strlen(context_only) <= OCA_TOOL_RESULT_MAX_BYTES);
    assert(strstr(context_only, "shortened for model context") && strstr(context_only, "1 + replacement"));
    free(context_only);
    free(full.text);
    expect(executor, "write", "{\"path\":\"newline.txt\",\"content\":\"line\\n\"}", OCA_TOOL_COMPLETE, "File written.");
    full = call(executor, "write", "{\"path\":\"newline.txt\",\"content\":\"line\"}");
    assert(full.state == OCA_TOOL_COMPLETE && strstr(full.text, "(+1 -1)"));
    assert(strstr(full.text, "no newline at end of file"));
    free(full.text);
    expect(executor, "bash", "{\"command\":\"printf '%s' \\\"$OCA_TEST_ENV\\\"\"}", OCA_TOOL_COMPLETE, "ordinary-environment");
    int secret = open(path, O_RDONLY);
    assert(secret >= 0 && dup2(secret, 200) == 200); close(secret);
    expect(executor, "bash", "{\"command\":\"if (: <&200) 2>/dev/null; then printf leaked; else printf clean; fi\"}", OCA_TOOL_COMPLETE, "clean");
    close(200);
    atomic_int cancel;
    atomic_init(&cancel, 0);
    pthread_t canceler;
    assert(!pthread_create(&canceler, NULL, cancel_after_ready, &cancel));
    tool_outcome out = {0};
    executor_call(executor, work, "bash", "{\"command\":\"sleep 30\"}", &cancel, &out);
    assert(!pthread_join(canceler, NULL) && out.state == OCA_TOOL_INTERRUPTED);
    free(out.text);
    expect(executor, "bash", "{\"command\":\"sleep 30\"}", OCA_TOOL_ERROR, "runtime limit");
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(listener >= 0 && !bind(listener, (struct sockaddr *)&address, sizeof(address)) && !listen(listener, 1));
    socklen_t address_size = sizeof(address);
    assert(!getsockname(listener, (struct sockaddr *)&address, &address_size));
    snprintf(json, sizeof(json), "{\"command\":\"if exec 7<>/dev/tcp/127.0.0.1/%u; then printf connected; fi\"}", ntohs(address.sin_port));
    expect(executor, "bash", json, OCA_TOOL_COMPLETE, "connected");
    close(listener);
    assert(!executor_rebind_workspace(executor, home, err, sizeof(err)) && !strcmp(executor->workspace, home));
    expect(executor, "read", "{\"path\":\".oca/auth.json\"}", OCA_TOOL_COMPLETE, "private test data");

    /* A bad worker request is rejected instead of executed. */
    const char *bad_workspace = NULL, *bad_name = NULL, *bad_args = NULL;
    const char *bad = "{\"workspace\":\"/tmp\"}";
    cJSON *document = executor_bootstrap_decode(bad, strlen(bad),
        &bad_workspace, &bad_name, &bad_args, err, sizeof(err));
    assert(!document && strstr(err, "tool request rejected"));

    /* Background children in the tool process group are cleaned up. */
    snprintf(path, sizeof(path), "%s/group-survivor", work);
    snprintf(json, sizeof(json),
        "{\"command\":\"(sleep 1; printf survivor > '%s') & printf started; sleep 0.1\"}", path);
    expect(executor, "bash", json, OCA_TOOL_COMPLETE, "started");
    struct timespec delay = {1, 200 * 1000 * 1000};
    nanosleep(&delay, NULL);
    assert(access(path, F_OK) && errno == ENOENT);

    oca_tool_executor_destroy(executor);
    assert(!oca_tool_executor_create(home, "/oca-nonexistent-workspace", err, sizeof(err)));
    assert(!nftw(root, remove_fixture, 32, FTW_DEPTH | FTW_PHYS));
    puts("test_executor: offline checks passed");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], OCA_TOOL_WORKER_FLAG)) return oca_tool_worker_main();
    assert(argc == 2);
    return offline_checks(argv[0]);
}
