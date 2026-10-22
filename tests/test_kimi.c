/* Protocol and transport tests use only a loopback server and fake keys. */
#include "fixture_io.h"
#define write fixture_io_write
#define fchmod fixture_io_chmod
#include "../oca_llm.c"
#undef fchmod
#undef write
#include "fixture_models.h"

#include <assert.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>

typedef struct {
    int starts, done, errors, calls, ends, usage, cancel_text;
    char text[1024], reason[4096], args[1024], category[64];
    char call_id[257], call_name[65];
    oca_event counters;
} test_log;

static int collect_event(const oca_event *ev, void *user) {
    test_log *log = user;
    if (ev->kind == OCA_EV_RESPONSE_START) log->starts++;
    if (ev->kind == OCA_EV_RESPONSE_DONE) log->done++;
    if (ev->kind == OCA_EV_ERROR) {
        log->errors++;
        snprintf(log->category, sizeof(log->category), "%s", ev->category);
        assert(!strstr(ev->message, "test-api-key"));
    }
    if (ev->kind == OCA_EV_TOOL_CALL_START) {
        log->calls++;
        snprintf(log->call_id, sizeof(log->call_id), "%s", ev->call_id);
        snprintf(log->call_name, sizeof(log->call_name), "%s", ev->call_name);
    }
    if (ev->kind == OCA_EV_TOOL_CALL_END) {
        log->ends++;
        assert(strlen(log->args) + strlen(ev->arguments) < sizeof(log->args));
        strcat(log->args, ev->arguments);
    }
    if (ev->kind == OCA_EV_USAGE) {
        log->usage++;
        log->counters = *ev;
    }
    if (ev->kind == OCA_EV_TEXT_DELTA) {
        assert(strlen(log->text) + strlen(ev->text) < sizeof(log->text));
        strcat(log->text, ev->text);
        return log->cancel_text;
    }
    if (ev->kind == OCA_EV_REASONING_DELTA) {
        assert(strlen(log->reason) + strlen(ev->text) < sizeof(log->reason));
        strcat(log->reason, ev->text);
    }
    return 0;
}

static const char TEXT_STREAM[] =
    ": heartbeat\r\n\r\n"
    "data: {\"choices\":\r\n"
    "data: [{\"index\":0,\"delta\":{\"role\":\"assistant\",\"reasoning_content\":\"brief\"},\"finish_reason\":null}]}\r\n\r\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"OK \\ud83e\\udebf\"},\"finish_reason\":null}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\","
    "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":4,\"total_tokens\":14,\"cached_tokens\":6}}]}\n\n"
    "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":4,\"total_tokens\":14,"
    "\"prompt_tokens_details\":{\"cached_tokens\":6},\"completion_tokens_details\":{\"reasoning_tokens\":2}}}\n\n"
    "data: [DONE]\n\n";

static const char TOOL_STREAM[] =
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":["
    "{\"index\":1,\"id\":\"b\",\"type\":\"function\",\"function\":{\"name\":\"read\",\"arguments\":\"{\"}},"
    "{\"index\":0,\"id\":\"a\",\"type\":\"function\",\"function\":{\"name\":\"read\",\"arguments\":\"{\\\"pa\"}}]}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":["
    "{\"index\":0,\"function\":{\"arguments\":\"th\\\":\\\"a\\\"}\"}},"
    "{\"index\":1,\"function\":{\"arguments\":\"\\\"path\\\":\\\"b\\\"}\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\n"
    "data: [DONE]\n\n";

static oca_llm *test_llm;

static oca_request *request_new(oca_purpose purpose) {
    oca_request *req = oca_request_create(purpose, 42);
    assert(req);
    oca_request_set_route(req, oca_llm_model_resolve(test_llm, "kimi-k3"), 0);

    oca_request_push_msg(req, OCA_MSG_USER, "Reply OK.", NULL, NULL);
    return req;
}

static void add_read(oca_request *req) {
    oca_request_add_tool(req, "read", "Read a test file.",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}");
}

static void write_bytes(int fd, const char *data, size_t len, int fragment) {
    while (len) {
        size_t count = fragment && len > 7 ? 7 : len;
        ssize_t n = send(fd, data, count, 0);
        if (n <= 0) return;
        data += n;
        len -= (size_t)n;
    }
}

/* The child verifies the actual HTTP request before it serves a fixture. */
static pid_t mock_server(char *url, size_t url_size, int status, const char *body,
                          int inspect_history) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(listener, 1) == 0);
    socklen_t size = sizeof(addr);
    assert(getsockname(listener, (struct sockaddr *)&addr, &size) == 0);
    snprintf(url, url_size, "http://127.0.0.1:%u/chat/completions", ntohs(addr.sin_port));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        signal(SIGPIPE, SIG_IGN);
        alarm(10);
        int peer = accept(listener, NULL, NULL);
        assert(peer >= 0);
        char input[16384];
        size_t used = 0, body_size = 0;
        char *body_at = NULL;
        for (;;) {
            assert(used + 1 < sizeof(input));
            ssize_t n = read(peer, input + used, sizeof(input) - used - 1);
            assert(n > 0);
            used += (size_t)n;
            input[used] = 0;
            body_at = strstr(input, "\r\n\r\n");
            if (!body_at) continue;
            body_at += 4;
            char *length = strstr(input, "Content-Length: ");
            assert(length);
            body_size = strtoul(length + strlen("Content-Length: "), NULL, 10);
            if (used >= (size_t)(body_at - input) + body_size) break;
        }
        assert(strstr(input, "Authorization: Bearer test-api-key\r\n"));
        assert(strstr(input, "User-Agent: OCA/"));
        cJSON *request = json_parse(body_at, body_size);
        assert(request);
        assert(!strcmp(json_text(request, "model"), "kimi-k3"));
        assert(!strcmp(json_text(request, "reasoning_effort"), "low"));
        assert(!cJSON_GetObjectItemCaseSensitive(request, "temperature"));
        assert(!cJSON_GetObjectItemCaseSensitive(request, "max_completion_tokens") && !cJSON_GetObjectItemCaseSensitive(request, "max_tokens"));
        assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "stream")));
        if (inspect_history) {
            const cJSON *messages = cJSON_GetObjectItemCaseSensitive(request, "messages");
            const cJSON *assistant = cJSON_GetArrayItem(messages, 1);
            assert(!strcmp(json_text(assistant, "reasoning_content"), "prior reasoning"));
            assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(assistant, "tool_calls")) == 2);
            assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(messages, 3), "tool_call_id")), "b"));
            assert(!strcmp(json_text(request, "prompt_cache_key"), "test-session"));
        }
        cJSON_Delete(request);
        char header[256];
        int count = snprintf(header, sizeof(header),
            "HTTP/1.1 %d Test\r\nContent-Type: text/event-stream\r\n"
            "Content-Length: %zu\r\nConnection: close\r\n\r\n", status, strlen(body));
        write_bytes(peer, header, (size_t)count, 0);
        write_bytes(peer, body, strlen(body), 1);
        close(peer);
        close(listener);
        _exit(0);
    }
    close(listener);
    return child;
}

static test_log exchange(oca_llm *llm, oca_request *req, int status,
                          const char *body, int history, int cancel_text) {
    char url[128], err[256];
    pid_t child = mock_server(url, sizeof(url), status, body, history);
    test_log log = {.cancel_text = cancel_text};
    assert(validate_request(req, err, sizeof(err)) == 0);
    assert(chat_generate(llm, req, collect_event, &log, url, err, sizeof(err)) == 0);
    int result;
    assert(waitpid(child, &result, 0) == child);
    assert(WIFEXITED(result) && WEXITSTATUS(result) == 0);
    assert(log.done + log.errors == 1);
    return log;
}

static void test_parser(void) {
    const char *bad[] = {
        "{\"a\":1,\"a\":2}", "{\"x\":{\"a\":0,\"a\":1}}", "{\"x\":\"\\u0000\"}",
        "{\"x\":\"\\ud800\"}", "{\"x\":\"\xc0\x80\"}", "{\"a\":", "{} trailing",
        "{\"x\":1e999}", "{\"x\":01}", "{\"x\":1.}", "{\"x\":1e}"
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        assert(!json_parse(bad[i], strlen(bad[i])));
    cJSON *good = json_parse("{\"x\":\"\\\\u0000\"}", strlen("{\"x\":\"\\\\u0000\"}"));
    assert(good);
    cJSON_Delete(good);
    char nested[160];
    memset(nested, '[', 65);
    nested[65] = '0';
    memset(nested + 66, ']', 65);
    nested[131] = 0;
    assert(!json_parse(nested, 131));
}

static void test_requests(void) {
    char err[256];
    const oca_model *m = oca_llm_model_resolve(test_llm, "kimi-k3");
    assert(m && !strcmp(m->id, "kimi-k3"));
    assert(!strcmp(m->provider, "kimi"));
    assert(oca_model_min_reasoning(m) == 0);
    assert(oca_model_supports_reasoning(m, 2));
    assert(!oca_model_supports_reasoning(m, 3));
    assert(!strcmp(oca_model_reasoning_name(m, 2), "max"));
    assert(oca_model_reasoning_index(m, "max") == 2);
    oca_request *req = request_new(OCA_PURPOSE_COMPACTION);
    long tokens;
    assert(!oca_llm_estimate_input(req, &tokens, err, sizeof(err)));
    assert(tokens > (long)strlen("Reply OK."));
    oca_request_set_route(req, m, 1);
    assert(oca_llm_estimate_input(req, &tokens, err, sizeof(err)) < 0);
    oca_request_set_route(req, m, 0);
    assert(!oca_llm_estimate_input(req, &tokens, err, sizeof(err)));
    oca_request_destroy(req);
    req = request_new(OCA_PURPOSE_TURN);
    assert(!oca_llm_estimate_input(req, &tokens, err, sizeof(err)));
    oca_request_destroy(req);
}

static void test_transport(oca_llm *llm) {
    oca_request *req = request_new(OCA_PURPOSE_TURN);
    test_log log = exchange(llm, req, 200, TEXT_STREAM, 0, 0);
    assert(log.starts == 1 && log.done == 1 && !log.errors);
    assert(!strcmp(log.text, "OK 🪿") && !strcmp(log.reason, "brief"));
    assert(log.usage == 1 && log.counters.input_tokens == 10);
    assert(log.counters.cache_hit_tokens == 6 && log.counters.cache_miss_tokens == 4);
    assert(log.counters.reasoning_tokens == 2);
    log = exchange(llm, req, 200,
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"OK\"},\"finish_reason\":\"stop\","
        "\"usage\":{\"prompt_tokens\":10}}],\"usage\":null}\n\ndata: [DONE]\n\n", 0, 0);
    assert(log.usage == 1 && log.counters.has_input && !log.counters.has_output);
    log = exchange(llm, req, 200,
        "data: {\"model\":\"different-model\",\"choices\":[]}\n\n", 0, 0);
    assert(!strcmp(log.category, "model"));
    log = exchange(llm, req, 200, TEXT_STREAM, 0, 1);
    assert(log.errors == 1 && !strcmp(log.category, "cancelled"));
    log = exchange(llm, req, 401, "{\"error\":\"test-api-key\"}", 0, 0);
    assert(log.starts == 0 && !strcmp(log.category, "authentication"));
    log = exchange(llm, req, 429, "{\"error\":\"test-api-key\"}", 0, 0);
    assert(!strcmp(log.category, "rate_limit"));
    log = exchange(llm, req, 503, "{\"error\":\"test-api-key\"}", 0, 0);
    assert(!strcmp(log.category, "http"));
    const char *invalid[] = {
        "data: {bad}\n\n",
        "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n",
        "data: [DONE]\n\n",
        "data: {\"choices\":[],\"choices\":[]}\n\n",
        "data: {\"choices\":[{\"index\":0.5,\"delta\":{}}]}\n\n",
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"\\u0000\"}}]}\n\n",
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":-1}}\n\n",
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":10}}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":11}}\n\n"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        log = exchange(llm, req, 200, invalid[i], 0, 0);
        assert(log.errors == 1 && !log.done && !strcmp(log.category, "stream"));
    }
    log = exchange(llm, req, 200,
        "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"length\"}]}\n\n", 0, 0);
    assert(!strcmp(log.category, "output_limit"));
    char *large = malloc(CHAT_FRAME_MAX + 20);
    assert(large);
    memset(large, 'x', CHAT_FRAME_MAX + 19);
    memcpy(large, "data: ", 6);
    large[CHAT_FRAME_MAX + 19] = 0;
    log = exchange(llm, req, 200, large, 0, 0);
    assert(log.errors == 1 && !log.done);
    free(large);
    oca_request_destroy(req);

    req = request_new(OCA_PURPOSE_TURN);
    add_read(req);
    log = exchange(llm, req, 200, TOOL_STREAM, 0, 0);
    assert(log.calls == 2 && log.ends == 2 && log.done == 1);
    assert(!strcmp(log.args, "{\"path\":\"a\"}{\"path\":\"b\"}"));
    oca_request_push_msg(req, OCA_MSG_ASSISTANT, "", "prior reasoning", NULL);
    oca_request_msg_add_call(req, "a", "read", "{\"path\":\"a\"}");
    oca_request_msg_add_call(req, "b", "read", "{\"path\":\"b\"}");
    oca_request_push_msg(req, OCA_MSG_TOOL, "A", NULL, "a");
    oca_request_push_msg(req, OCA_MSG_TOOL, "B", NULL, "b");
    oca_request_set_cache_key(req, "test-session");
    log = exchange(llm, req, 200, TEXT_STREAM, 1, 0);
    assert(log.done == 1);

    const char *broken_tools[] = {
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":32}]}}]}\n\n",
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0},{\"index\":0}]}}]}\n\n",
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":1,\"id\":\"a\",\"function\":{\"name\":\"read\",\"arguments\":\"{}\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\ndata: [DONE]\n\n",
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"a\",\"function\":{\"name\":\"read\",\"arguments\":\"{\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\ndata: [DONE]\n\n",
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"a\",\"function\":{\"name\":\"read\",\"arguments\":\"{}\"}},{\"index\":1,\"id\":\"a\",\"function\":{\"name\":\"read\",\"arguments\":\"{}\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\ndata: [DONE]\n\n"
    };
    for (size_t i = 0; i < sizeof(broken_tools) / sizeof(broken_tools[0]); i++) {
        log = exchange(llm, req, 200, broken_tools[i], 0, 0);
        assert(log.errors == 1 && !log.calls);
    }
    oca_request_destroy(req);
}

static size_t allocation_at, allocation_fail, allocation_live;

static void *test_json_alloc(size_t size) {
    if (allocation_at++ == allocation_fail) return NULL;
    void *p = malloc(size);
    if (p) allocation_live++;
    return p;
}

static void test_json_free(void *p) {
    if (p) { assert(allocation_live); allocation_live--; }
    free(p);
}

static void test_allocation_failures(void) {
    oca_request *req = request_new(OCA_PURPOSE_TURN);
    add_read(req);
    oca_request_push_msg(req, OCA_MSG_ASSISTANT, "", "brief", NULL);
    oca_request_msg_add_call(req, "a", "read", "{}");
    oca_request_push_msg(req, OCA_MSG_TOOL, "OK", NULL, "a");
    for (size_t i = 0; i < 200; i++) {
        allocation_at = allocation_live = 0;
        allocation_fail = i;
        cJSON_Hooks hooks = {.malloc_fn = test_json_alloc, .free_fn = test_json_free};
        cJSON_InitHooks(&hooks);
        char *body = chat_serialize(req);
        cJSON_free(body);
        assert(allocation_live == 0);
        cJSON_InitHooks(NULL);
    }
    req->construction_failed = 1;
    char err[256];
    long count;
    assert(oca_llm_estimate_input(req, &count, err, sizeof(err)) < 0);
    oca_request_destroy(req);
}

/* Explicit opt-in: two small real requests, no files, tools, or saved state. */
static int live_test(void) {
    char key[4098], err[256];
    if (!fgets(key, sizeof(key), stdin)) return 1;
    key[strcspn(key, "\r\n")] = 0;
    if (setenv("KIMI_API_KEY", key, 1) != 0) return 1;
    memset(key, 0, sizeof(key));
    char directory[] = "/tmp/oca-kimi-live-XXXXXX";
    if (!mkdtemp(directory)) return 1;
    char auth[512];
    snprintf(auth, sizeof(auth), "%s/auth.json", directory);
    oca_llm *llm = fixture_llm_create(auth, err, sizeof(err));
    unsetenv("KIMI_API_KEY");
    if (!llm) { rmdir(directory); return 1; }
    oca_request *req = oca_request_create(OCA_PURPOSE_TURN, 1);
    if (!req) { oca_llm_destroy(llm); rmdir(directory); return 1; }
    oca_request_set_route(req, oca_llm_model_resolve(llm, "kimi-k3"), 0);

    oca_request_set_cache_key(req, "oca-live-validation");
    oca_request_push_msg(req, OCA_MSG_USER,
        "Call probe once with {}. After its result reply only OK.", NULL, NULL);
    oca_request_add_tool(req, "probe", "Return a test marker.",
        "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}");
    test_log first = {0}, second = {0};
    int rc = oca_llm_generate(llm, req, collect_event, &first, err, sizeof(err));
    int ok = rc == 0 && first.done == 1 && first.calls == 1 && first.ends == 1 &&
             !strcmp(first.call_name, "probe") && !strcmp(first.args, "{}");
    if (ok) {
        oca_request_push_msg(req, OCA_MSG_ASSISTANT, first.text, first.reason, NULL);
        oca_request_msg_add_call(req, first.call_id, first.call_name, first.args);
        oca_request_push_msg(req, OCA_MSG_TOOL, "OK", NULL, first.call_id);
        req->request_id++;
        rc = oca_llm_generate(llm, req, collect_event, &second, err, sizeof(err));
        ok = rc == 0 && second.done == 1 && !second.calls &&
             !strcmp(second.text, "OK") && first.usage == 1 && second.usage == 1;
    }
    printf("live_kimi: %s; calls=%d; usage_events=%d; output_tokens=%ld\n",
           ok ? "passed" : "failed", first.calls, first.usage + second.usage,
           first.counters.output_tokens + second.counters.output_tokens);
    if (!ok) printf("failure_category=%s%s\n", first.category, second.category);
    oca_request_destroy(req);
    oca_llm_destroy(llm);
    assert(access(auth, F_OK) != 0);
    assert(rmdir(directory) == 0);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--live")) return live_test();
    assert(argc == 1);
    alarm(30);
    assert(unsetenv("KIMI_API_KEY") == 0);
    assert(unsetenv("DEEPSEEK_API_KEY") == 0);
    char directory[] = "/tmp/oca-kimi-test-XXXXXX";
    assert(mkdtemp(directory));
    fixture_check_atomic_write(write_file_atomic_0600, directory, "tmp");
    char auth[512], err[256];
    snprintf(auth, sizeof(auth), "%s/auth.json", directory);
    oca_llm *llm = fixture_llm_create(auth, err, sizeof(err));
    assert(llm);
    test_llm = llm;
    const oca_model *m = oca_llm_model_resolve(llm, "kimi-k3");
    assert(!oca_llm_model_available(llm, m));
    oca_request *req = request_new(OCA_PURPOSE_TURN);
    test_log log = {0};
    assert(oca_llm_generate(llm, req, collect_event, &log, err, sizeof(err)) < 0);
    assert(!log.starts && !log.errors && !log.done);
    oca_request_destroy(req);
    assert(oca_llm_auth_set_api_key(llm, "kimi", "bad\nheader", err, sizeof(err)) < 0);
    assert(!oca_llm_auth_set_api_key(llm, "kimi", "test-api-key", err, sizeof(err)));
    oca_llm_destroy(llm);
    assert(setenv("KIMI_API_KEY", "environment-key", 1) == 0);
    llm = fixture_llm_create(auth, err, sizeof(err));
    assert(llm && oca_llm_model_available(llm, oca_llm_model_resolve(llm, "kimi-k3")));
    test_llm = llm;
    assert(!strcmp(provider_api_key(llm, "kimi"), "test-api-key"));
    test_parser();
    test_requests();
    test_allocation_failures();
    test_transport(llm);
    assert(!oca_llm_auth_logout(llm, "kimi", err, sizeof(err)));
    assert(!strcmp(provider_api_key(llm, "kimi"), "environment-key"));
    assert(!strcmp(oca_llm_auth_method(llm, "kimi"), "api-key"));
    oca_llm_destroy(llm);
    assert(unsetenv("KIMI_API_KEY") == 0);
    assert(unsetenv("DEEPSEEK_API_KEY") == 0);
    assert(unlink(auth) == 0);
    assert(rmdir(directory) == 0);
    puts("test_kimi: passed");
    return 0;
}
