/* Provider and OAuth fixtures use loopback sockets and synthetic credentials. */
#include "../oca_llm.c"
#include "fixture_models.h"

#include <assert.h>
#include <signal.h>
#include <sys/wait.h>

typedef struct {
    pid_t pid;
    int capture;
    char url[128];
} server;

static long response_delay_ns;
static long body_pause_ns;
static size_t response_missing_bytes;

static void send_all(int fd, const char *text, size_t length) {
    while (length) {
        ssize_t sent = write(fd, text, length);
        if (sent < 0 && errno == EINTR) continue;
        assert(sent > 0);
        text += sent;
        length -= (size_t)sent;
    }
}

static server serve_sequence(int status, const char *body, const char *next_body) {
    unsigned short port = 0;
    int listener = oauth_listener(&port);
    assert(listener >= 0 && fcntl(listener, F_SETFL, 0) == 0);
    int capture[2];
    assert(pipe(capture) == 0);
    assert(fcntl(capture[0], F_SETFD, FD_CLOEXEC) == 0);
    server result = {.pid = fork(), .capture = capture[0]};
    assert(result.pid >= 0);
    snprintf(result.url, sizeof(result.url), "http://127.0.0.1:%u/test", port);
    if (!result.pid) {
        alarm(10);
        signal(SIGPIPE, SIG_IGN);
        close(capture[0]);
        for (int stage = 0; stage < (next_body ? 2 : 1); stage++) {
            int response_status = stage ? 200 : status;
            const char *response_body = stage ? next_body : body;
            size_t body_length = strlen(response_body);
            int peer = accept(listener, NULL, NULL);
            assert(peer >= 0);
            char *request = calloc(2 * 1024 * 1024, 1);
            assert(request);
            size_t used = 0;
            for (;;) {
                assert(used + 1 < 2 * 1024 * 1024);
                ssize_t count = read(peer, request + used, 2 * 1024 * 1024 - used - 1);
                if (count < 0 && errno == EINTR) continue;
                assert(count > 0);
                used += (size_t)count;
                char *body_at = strstr(request, "\r\n\r\n");
                if (!body_at) continue;
                char *length = strstr(request, "Content-Length: ");
                assert(length && length < body_at);
                size_t expected = strtoul(length + strlen("Content-Length: "), NULL, 10);
                if (used >= (size_t)(body_at + 4 - request) + expected) break;
            }
            char header[256];
            if (response_delay_ns) {
                struct timespec delay = {.tv_nsec = response_delay_ns};
                nanosleep(&delay, NULL);
            }
            snprintf(header, sizeof(header), "HTTP/1.1 %d Fixture\r\nContent-Type: text/event-stream\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", response_status, body_length + response_missing_bytes);
            send_all(peer, header, strlen(header));
            size_t prefix = 0;
            if (body_pause_ns && body_length) {
                send_all(peer, response_body, 1);
                prefix = 1;
                struct timespec delay = {.tv_nsec = body_pause_ns};
                nanosleep(&delay, NULL);
            }
            /* Small writes exercise fragmented transport independently of JSON. */
            for (size_t i = prefix; i < body_length;) {
                size_t length = body_length - i;
                size_t fragment = body_length > 128 * 1024 ? 4096 : 11;
                if (length > fragment) length = fragment;
                ssize_t sent = write(peer, response_body + i, length);
                if (sent <= 0) break;
                i += (size_t)sent;
            }
            close(peer);
            send_all(capture[1], request, used);
            free(request);
        }
        close(listener);
        close(capture[1]);
        _exit(0);
    }
    close(capture[1]);
    close(listener);
    return result;
}

static server serve(int status, const char *body) {
    return serve_sequence(status, body, NULL);
}

static char *capture_request(server *server) {
    byte_buffer text = {0};
    char bytes[4096];
    ssize_t count;
    while ((count = read(server->capture, bytes, sizeof(bytes))) > 0)
        assert(!buffer_append(&text, bytes, (size_t)count, 2 * 1024 * 1024));
    assert(count == 0);
    close(server->capture);
    int status;
    assert(waitpid(server->pid, &status, 0) == server->pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return text.data;
}

static char *fake_access(time_t expiry, const char *account) {
    char payload[512], encoded[1024];
    snprintf(payload, sizeof(payload), "{\"exp\":%lld,\"https://api.openai.com/auth\":{\"chatgpt_account_id\":\"%s\"}}",
             (long long)expiry, account);
    assert(!base64url((const unsigned char *)payload, strlen(payload), encoded, sizeof(encoded)));
    char *token = malloc(strlen(encoded) + 32);
    assert(token);
    sprintf(token, "e30.%s.fixture-signature", encoded);
    return token;
}

static char *token_response(time_t expiry, const char *refresh) {
    cJSON *object = cJSON_CreateObject();
    char *access = fake_access(expiry, "fixture-account");
    assert(object && cJSON_AddStringToObject(object, "access_token", access));
    if (refresh) assert(cJSON_AddStringToObject(object, "refresh_token", refresh));
    char *text = cJSON_PrintUnformatted(object);
    assert(text);
    secret_free(access);
    cJSON_Delete(object);
    return text;
}

static char *query_value(const char *query, const char *name) {
    size_t length = strlen(name);
    for (const char *part = query; part && *part;) {
        const char *end = strchr(part, '&');
        size_t size = end ? (size_t)(end - part) : strlen(part);
        if (size > length && !memcmp(part, name, length) && part[length] == '=')
            return oauth_unescape(part + length + 1, size - length - 1);
        part = end ? end + 1 : NULL;
    }
    return NULL;
}

typedef struct {
    char challenge[65];
    int opened, cancel;
} browser_state;

static int browser(void *user, const char *url) {
    browser_state *state = user;
    state->opened++;
    const char *query = strchr(url, '?');
    assert(query && strstr(url, "code_challenge_method=S256") && strstr(url, "originator=oca"));
    char *challenge = query_value(query + 1, "code_challenge");
    char *nonce = query_value(query + 1, "state");
    char *redirect = query_value(query + 1, "redirect_uri");
    assert(challenge && nonce && redirect && strlen(challenge) == 43 && strlen(nonce) == 43);
    snprintf(state->challenge, sizeof(state->challenge), "%s", challenge);
    unsigned short port = 0;
    assert(sscanf(redirect, "http://localhost:%hu/auth/callback", &port) == 1 && port);
    int peer = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(peer >= 0 && connect(peer, (struct sockaddr *)&address, sizeof(address)) == 0);
    char request[512];
    snprintf(request, sizeof(request), "GET /auth/callback?state=%s&code=fixture-code HTTP/1.1\r\nHost: localhost:%u\r\n\r\n", nonce, port);
    send_all(peer, request, strlen(request));
    close(peer);
    free(challenge); free(nonce); free(redirect);
    return state->cancel;
}

static int cancel_login(void *user) {
    return ((browser_state *)user)->cancel;
}

static void test_oauth(oca_llm *llm, const char *directory) {
    char err[256];
    char *response = token_response(time(NULL) + 3600, "fixture-refresh-one");
    server service = serve(200, response);
    browser_state state = {0};
    oca_auth_ui ui = {.user = &state, .open_browser = browser, .cancelled = cancel_login};
    assert(!openai_login(llm, &ui, "http://127.0.0.1", service.url, 0, err, sizeof(err)));
    assert(state.opened == 1 && !strcmp(oca_llm_auth_method(llm, "openai"), "oauth"));
    assert(oca_llm_auth_expiry(llm, "openai") > (int64_t)time(NULL));
    char *request = capture_request(&service);
    char *body = strstr(request, "\r\n\r\n");
    assert(body);
    char *verifier = query_value(body + 4, "code_verifier");
    assert(verifier && strlen(verifier) == 43);
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length;
    char challenge[65];
    assert(EVP_Digest(verifier, strlen(verifier), digest, &length, EVP_sha256(), NULL) == 1);
    assert(!base64url(digest, length, challenge, sizeof(challenge)) && !strcmp(challenge, state.challenge));
    assert(strstr(body, "grant_type=authorization_code") && strstr(body, "client_id=" OPENAI_CLIENT_ID));
    assert(strstr(body, "code=fixture-code"));
    secret_free(verifier); free(request); cJSON_free(response);

    assert(!openai_refresh(llm, 0, &ui, "http://127.0.0.1:1/unused", err, sizeof(err)));
    response = token_response(time(NULL) + 7200, "fixture-refresh-two");
    service = serve(200, response);
    assert(!openai_refresh(llm, 1, &ui, service.url, err, sizeof(err)));
    assert(!strcmp(find_member(llm, "openai")->refresh_token, "fixture-refresh-two"));
    request = capture_request(&service);
    assert(strstr(request, "fixture-refresh-one") && strstr(request, "refresh_token"));
    free(request); cJSON_free(response);

    service = serve(400, "{\"error\":\"fixture-secret-must-not-escape\"}");
    assert(openai_refresh(llm, 1, &ui, service.url, err, sizeof(err)) < 0);
    assert(!strstr(err, "fixture-secret") && !strcmp(find_member(llm, "openai")->refresh_token, "fixture-refresh-two"));
    request = capture_request(&service);
    free(request);

    find_member(llm, "openai")->expires_at = (int64_t)time(NULL) + 15;
    response = token_response(time(NULL) + 7200, NULL);
    service = serve(200, response);
    assert(!openai_refresh(llm, 0, &ui, service.url, err, sizeof(err)));
    assert(!strcmp(find_member(llm, "openai")->refresh_token, "fixture-refresh-two"));
    request = capture_request(&service);
    assert(strstr(request, "fixture-refresh-two"));
    free(request); cJSON_free(response);

    state.cancel = 1;
    assert(openai_login(llm, &ui, "http://127.0.0.1", "http://127.0.0.1:1/unused", 0, err, sizeof(err)) < 0);
    assert(strstr(err, "canceled") && state.opened == 1);
    state.cancel = 0;

    int denied;
    char wrong[] = "GET /auth/callback?state=wrong&code=fixture HTTP/1.1\r\n\r\n";
    assert(!oauth_callback(wrong, "expected", &denied) && !denied);
    char duplicate[] = "GET /auth/callback?state=expected&state=expected&code=fixture HTTP/1.1\r\n\r\n";
    assert(!oauth_callback(duplicate, "expected", &denied) && !denied);
    char denied_request[] = "GET /auth/callback?state=expected&error=access_denied HTTP/1.1\r\n\r\n";
    assert(!oauth_callback(denied_request, "expected", &denied) && denied);
    char nul[] = "GET /auth/callback?state=expected&code=abc%00def HTTP/1.1\r\n\r\n";
    assert(!oauth_callback(nul, "expected", &denied));

    char path[512], old_path[4096];
    snprintf(path, sizeof(path), "%s/no-replace", directory);
    assert(mkdir(path, 0700) == 0);
    snprintf(old_path, sizeof(old_path), "%s", llm->auth_path);
    snprintf(llm->auth_path, sizeof(llm->auth_path), "%s", path);
    response = token_response(time(NULL) + 7200, "fixture-refresh-three");
    cJSON *parsed = json_parse(response, strlen(response));
    assert(parsed && oauth_store_response(llm, parsed, find_member(llm, "openai"), err, sizeof(err)) < 0);
    assert(!strcmp(find_member(llm, "openai")->refresh_token, "fixture-refresh-two"));
    assert(oca_llm_auth_logout(llm, "openai", err, sizeof(err)) < 0 && find_member(llm, "openai"));
    snprintf(llm->auth_path, sizeof(llm->auth_path), "%s", old_path);
    cJSON_Delete(parsed); cJSON_free(response);
    assert(rmdir(path) == 0);
}

typedef struct {
    int starts, done, errors, calls, ends, usage, states, reasoning, cancel;
    char text[1024], category[64], message[256];
    char call_id[257], call_name[65], arguments[1024];
    char *state;
    oca_event counters;
    int has_diagnostic;
    oca_llm_diagnostic diagnostic;
} event_log;

static int collect(const oca_event *event, void *user) {
    event_log *log = user;
    if (event->kind == OCA_EV_RESPONSE_START) log->starts++;
    if (event->kind == OCA_EV_RESPONSE_DONE) log->done++;
    if (event->kind == OCA_EV_ERROR) {
        log->errors++;
        snprintf(log->category, sizeof(log->category), "%s", event->category);
        snprintf(log->message, sizeof(log->message), "%s", event->message);
        assert(!strstr(event->message, "fixture-refresh") && !strstr(event->message, "fixture-secret"));
        if (event->diagnostic) {
            log->has_diagnostic = 1;
            log->diagnostic = *event->diagnostic;
        }
    }
    if (event->kind == OCA_EV_TOOL_CALL_START) {
        log->calls++;
        snprintf(log->call_id, sizeof(log->call_id), "%s", event->call_id);
        snprintf(log->call_name, sizeof(log->call_name), "%s", event->call_name);
    }
    if (event->kind == OCA_EV_TOOL_CALL_END) {
        log->ends++;
        snprintf(log->arguments, sizeof(log->arguments), "%s", event->arguments);
    }
    if (event->kind == OCA_EV_USAGE) { log->usage++; log->counters = *event; }
    if (event->kind == OCA_EV_REASONING_DELTA) log->reasoning++;
    if (event->kind == OCA_EV_MODEL_STATE) { log->states++; log->state = strdup(event->state); assert(log->state); }
    if (event->kind == OCA_EV_TEXT_DELTA) {
        assert(strlen(log->text) + strlen(event->text) < sizeof(log->text));
        strcat(log->text, event->text);
        return log->cancel;
    }
    return 0;
}

static oca_llm *test_llm;

static oca_request *request_for(const char *name) {
    oca_request *request = oca_request_create(OCA_PURPOSE_TURN, 17);
    const oca_model *model = oca_llm_model_resolve(test_llm, name);
    assert(request && model);
    oca_request_set_route(request, model, model->reasoning_count > 2 ? 2 : oca_model_min_reasoning(model));

    oca_request_set_cache_key(request, "fixture-conversation");
    oca_request_push_msg(request, OCA_MSG_SYSTEM, "Keep the fixture short.", NULL, NULL);
    oca_request_push_msg(request, OCA_MSG_USER, "Reply OK.", NULL, NULL);
    oca_request_add_tool(request, "read", "Read a fixture.", "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}");
    return request;
}

static const char OPENAI_TEXT[] =
    "data: {\"type\":\"response.created\",\"response\":{\"model\":\"gpt-6-astra\"}}\n\n"
    "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"reason-1\",\"type\":\"reasoning\"}}\n\n"
    "data: {\"type\":\"response.reasoning_summary_text.delta\",\"output_index\":0,\"item_id\":\"reason-1\",\"delta\":\"Reviewing.\"}\n\n"
    "data: {\"type\":\"response.output_item.added\",\"output_index\":1,\"item\":{\"id\":\"message-1\",\"type\":\"message\",\"role\":\"assistant\",\"content\":[]}}\n\n"
    "data: {\"type\":\"response.output_text.delta\",\"output_index\":1,\"item_id\":\"message-1\",\"delta\":\"OK\"}\n\n"
    "data: {\"type\":\"response.completed\",\"response\":{\"model\":\"gpt-6-astra\",\"status\":\"completed\",\"output\":["
    "{\"id\":\"reason-1\",\"type\":\"reasoning\",\"encrypted_content\":\"fixture-ciphertext\",\"summary\":[]},"
    "{\"id\":\"message-1\",\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"OK\"}]}],"
    "\"usage\":{\"input_tokens\":100,\"output_tokens\":20,\"total_tokens\":120,\"input_tokens_details\":{\"cached_tokens\":60},\"output_tokens_details\":{\"reasoning_tokens\":10}}}}\n\n";

static const char OPENAI_TOOLS[] =
    "data: {\"type\":\"response.created\",\"response\":{\"model\":\"gpt-6-astra\"}}\n\n"
    "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"function-1\",\"type\":\"function_call\",\"call_id\":\"call-1\",\"name\":\"read\",\"arguments\":\"\"}}\n\n"
    "data: {\"type\":\"response.function_call_arguments.delta\",\"output_index\":0,\"item_id\":\"function-1\",\"delta\":\"{\\\"path\\\":\"}\n\n"
    "data: {\"type\":\"response.function_call_arguments.delta\",\"output_index\":0,\"item_id\":\"function-1\",\"delta\":\"\\\"fixture\\\"}\"}\n\n"
    "data: {\"type\":\"response.completed\",\"response\":{\"model\":\"gpt-6-astra\",\"status\":\"completed\",\"output\":["
    "{\"id\":\"function-1\",\"type\":\"function_call\",\"call_id\":\"call-1\",\"name\":\"read\",\"arguments\":\"{\\\"path\\\":\\\"fixture\\\"}\"}]}}\n\n";

static char *replace_all(const char *text, const char *before, const char *after) {
    byte_buffer result = {0};
    const char *at;
    while ((at = strstr(text, before))) {
        assert(!buffer_append(&result, text, (size_t)(at - text), OPENAI_FRAME_MAX));
        assert(!buffer_append(&result, after, strlen(after), OPENAI_FRAME_MAX));
        text = at + strlen(before);
    }
    assert(!buffer_append(&result, text, strlen(text), OPENAI_FRAME_MAX));
    return result.data;
}

static int stop_request(void *user) { (void)user; return 1; }

static void append_event(byte_buffer *stream, const cJSON *event) {
    char *json = cJSON_PrintUnformatted(event);
    assert(json);
    assert(!buffer_append(stream, "data: ", 6, OPENAI_FRAME_MAX));
    assert(!buffer_append(stream, json, strlen(json), OPENAI_FRAME_MAX));
    assert(!buffer_append(stream, "\n\n", 2, OPENAI_FRAME_MAX));
    cJSON_free(json);
}

/* Close selected items before completion. Negative final counts preserve the
 * full snapshot (-1) or omit the completion event (-2). */
static char *completion_fixture(const char *source, size_t closed_items, int final_count) {
    const char *final = strstr(source, "data: {\"type\":\"response.completed\"");
    assert(final);
    cJSON *event = cJSON_Parse(final + 6);
    cJSON *response = cJSON_GetObjectItemCaseSensitive(event, "response");
    cJSON *output = cJSON_GetObjectItemCaseSensitive(response, "output");
    assert(event && response && cJSON_IsArray(output));
    byte_buffer stream = {0};
    assert(!buffer_append(&stream, source, (size_t)(final - source), OPENAI_FRAME_MAX));
    for (size_t i = 0; i < closed_items && i < (size_t)cJSON_GetArraySize(output); i++) {
        cJSON *done = cJSON_CreateObject();
        assert(done && cJSON_AddStringToObject(done, "type", "response.output_item.done"));
        assert(cJSON_AddNumberToObject(done, "output_index", (double)i));
        assert(cJSON_AddItemToObject(done, "item", cJSON_Duplicate(cJSON_GetArrayItem(output, (int)i), 1)));
        append_event(&stream, done);
        cJSON_Delete(done);
    }
    if (final_count >= 0) {
        while (cJSON_GetArraySize(output) > final_count)
            cJSON_DeleteItemFromArray(output, cJSON_GetArraySize(output) - 1);
    }
    if (final_count != -2) append_event(&stream, event);
    cJSON_Delete(event);
    return stream.data;
}

static event_log diagnostic_exchange(oca_llm *llm, oca_request *request, const char *body,
                                      const char *check, const char *field) {
    char err[256];
    server service = serve(200, body);
    event_log log = {0};
    assert(!openai_generate(llm, request, collect, &log, service.url,
                           "http://127.0.0.1:1/unused", err, sizeof(err)));
    free(capture_request(&service));
    assert(log.errors == 1 && !log.done && log.has_diagnostic);
    assert(!strcmp(log.diagnostic.check, check) && !strcmp(log.diagnostic.field, field));
    assert(log.diagnostic.has_http_status && log.diagnostic.http_status == 200);
    assert(log.diagnostic.has_transport);
    assert(!strstr(log.diagnostic.transport.curl_message, "fixture-secret"));
    assert(log.diagnostic.wire_bytes && log.diagnostic.frame_bytes && log.diagnostic.event_index);
    assert(!strstr(log.diagnostic.received, "fixture-private") && !strstr(log.diagnostic.expected, "fixture-private"));
    assert(!log.state);
    return log;
}

static void test_empty_completion(oca_llm *llm) {
    const char *fixtures[] = {OPENAI_TEXT, OPENAI_TOOLS};
    char err[256];
    for (size_t i = 0; i < 2; i++) {
        oca_request *request = request_for("gpt-5.6-luna");
        char *source = replace_all(fixtures[i], "gpt-6-astra", "gpt-5.6-luna");
        char *body = completion_fixture(source, SIZE_MAX, 0);
        server service = serve(200, body);
        event_log log = {0};
        assert(!openai_generate(llm, request, collect, &log, service.url,
                               "http://127.0.0.1:1/unused", err, sizeof(err)));
        free(capture_request(&service));
        assert(log.starts == 1 && log.done == 1 && !log.errors);
        if (!i) {
            assert(!strcmp(log.text, "OK") && log.reasoning == 1 && log.states == 1 && log.usage == 1);
            assert(log.counters.input_tokens == 100 && log.counters.cache_hit_tokens == 60);
            assert(log.state && strstr(log.state, "fixture-ciphertext"));
            oca_request_push_msg(request, OCA_MSG_ASSISTANT, log.text, NULL, NULL);
            oca_request_msg_set_state(request, log.state);
            char *replay = openai_serialize(request);
            assert(replay && strstr(replay, "fixture-ciphertext"));
            cJSON_free(replay);
        } else {
            assert(log.calls == 1 && log.ends == 1 && !log.states);
            assert(!strcmp(log.call_id, "call-1") && !strcmp(log.call_name, "read"));
            assert(!strcmp(log.arguments, "{\"path\":\"fixture\"}"));
        }
        free(body);
        oca_request_destroy(request);
        free(log.state);
        request = request_for("gpt-5.6-luna");

        body = completion_fixture(source, 0, 0);
        log = diagnostic_exchange(llm, request, body, "item_order", "response.output");
        assert(!log.calls && !log.ends && !log.states && !log.usage);
        assert(!strcmp(log.diagnostic.received, "unfinished item"));
        free(body);
        body = completion_fixture(source, SIZE_MAX, -2);
        log = diagnostic_exchange(llm, request, body, "stream_end", "response.completed");
        assert(!log.calls && !log.ends && !log.states && !log.usage);
        free(body);

        body = completion_fixture(source, SIZE_MAX, 0);
        const char *invalid[] = {"null", "{}"};
        for (size_t k = 0; k < 2; k++) {
            char replacement[32];
            snprintf(replacement, sizeof(replacement), "\"output\":%s", invalid[k]);
            char *bad = replace_all(body, "\"output\":[]", replacement);
            log = diagnostic_exchange(llm, request, bad, "field_type", "response.output");
            assert(!log.calls && !log.ends && !log.states);
            free(bad);
        }
        if (!i) {
            char *partial = completion_fixture(source, SIZE_MAX, 1);
            log = diagnostic_exchange(llm, request, partial, "item_count", "response.output");
            assert(!strcmp(log.diagnostic.received, "1 items"));
            free(partial);
            partial = completion_fixture(source, 1, 0);
            log = diagnostic_exchange(llm, request, partial, "item_order", "response.output");
            assert(log.diagnostic.has_item_index && log.diagnostic.item_index == 1);
            free(partial);
        } else {
            char *gap = replace_all(body, "\"output_index\":0", "\"output_index\":1");
            log = diagnostic_exchange(llm, request, gap, "item_order", "response.output");
            assert(!strcmp(log.diagnostic.received, "missing item") && !log.calls && !log.ends);
            free(gap);
        }
        free(body);
        free(source);
        oca_request_destroy(request);
    }
}

static void test_openai_metadata(oca_llm *llm) {
    const char metadata[] = "data: {\"type\":\"response.metadata\",\"metadata\":{\"fixture\":true},"
        "\"delta\":\"ignored\",\"usage\":{\"input_tokens\":999},"
        "\"item\":{\"type\":\"function_call\",\"name\":\"unrequested\"}}\n\n";
    const char *fixtures[] = {OPENAI_TEXT, OPENAI_TOOLS};
    const char *markers[] = {"data: {\"type\":\"response.output_item.added\"",
                             "data: {\"type\":\"response.completed\""};
    oca_request *request = request_for("gpt-6-astra");
    char err[256];
    for (size_t i = 0; i < 2; i++) {
        byte_buffer body = {0};
        const char *start = fixtures[i];
        for (size_t k = 0; k < 2; k++) {
            const char *at = strstr(start, markers[k]);
            assert(at && !buffer_append(&body, start, (size_t)(at - start), OPENAI_FRAME_MAX));
            assert(!buffer_append(&body, metadata, strlen(metadata), OPENAI_FRAME_MAX));
            start = at;
        }
        assert(!buffer_append(&body, start, strlen(start), OPENAI_FRAME_MAX));
        server service = serve(200, body.data);
        event_log log = {0};
        assert(!openai_generate(llm, request, collect, &log, service.url,
                               "http://127.0.0.1:1/unused", err, sizeof(err)));
        free(capture_request(&service));
        assert(log.starts == 1 && log.done == 1 && !log.errors);
        if (!i) {
            assert(!strcmp(log.text, "OK") && !log.calls && !log.ends);
            assert(log.reasoning == 1 && log.states == 1 && log.usage == 1);
            assert(log.counters.input_tokens == 100 && log.counters.cache_hit_tokens == 60);
            assert(log.state && strstr(log.state, "fixture-ciphertext"));
        } else {
            assert(!*log.text && log.calls == 1 && log.ends == 1 && !log.usage && !log.states);
            assert(!strcmp(log.call_name, "read") && !strcmp(log.call_id, "call-1"));
            assert(!strcmp(log.arguments, "{\"path\":\"fixture\"}"));
        }
        free(log.state);
        const char *completion = strstr(body.data, markers[1]);
        assert(completion);
        body.data[completion - body.data] = 0;
        log = diagnostic_exchange(llm, request, body.data, "stream_end", "response.completed");
        assert(!log.calls && !log.ends);
        buffer_free(&body);
    }
    diagnostic_exchange(llm, request, metadata, "event_order", "type");
    byte_buffer after = {0};
    assert(!buffer_append(&after, OPENAI_TEXT, strlen(OPENAI_TEXT), OPENAI_FRAME_MAX));
    assert(!buffer_append(&after, metadata, strlen(metadata), OPENAI_FRAME_MAX));
    diagnostic_exchange(llm, request, after.data, "event_order", "type");
    buffer_free(&after);
    diagnostic_exchange(llm, request, "data: {\"type\":\"response.metadata\",broken}\n\n",
                        "frame_json", "frame");
    oca_request_destroy(request);
}

static void test_stream_diagnostics(oca_llm *llm) {
    oca_request *request = request_for("gpt-6-astra");
    event_log log = diagnostic_exchange(llm, request, "data: {broken}\n\n", "frame_json", "frame");
    assert(log.diagnostic.event_index == 1 && log.diagnostic.frame_bytes == strlen("{broken}"));
    log = diagnostic_exchange(llm, request,
        "data: {\"type\":\"response.created\",\"response\":{\"model\":\"other-model\"}}\n\n", "model_mismatch", "response.model");
    assert(!strcmp(log.diagnostic.expected, "gpt-6-astra") && !strcmp(log.diagnostic.received, "other-model"));
    assert(!strcmp(log.diagnostic.provider_event, "response.created"));
    const struct { const char *before, *after, *check, *field; } cases[] = {
        {"response.reasoning_summary_text.delta", "response.future_event", "event_type", "type"},
        {"\"output_index\":0", "\"output_index\":-1", "item_index", "output_index"},
        {"\"type\":\"message\"", "\"type\":\"future_item\"", "item_type", "item.type"},
        {"\"item_id\":\"message-1\"", "\"item_id\":\"wrong-id\"", "item_identity", "item_id"},
        {"\"delta\":\"OK\"", "\"delta\":null", "field_type", "delta"},
        {"\"text\":\"OK\"", "\"text\":42", "field_type", "content[].text"},
        {"\"text\":\"OK\"", "\"text\":\"fixture-private-content\"", "text_mismatch", "item.content"},
        {"\"output_tokens\":20", "\"output_tokens\":-1", "usage_counter", "output_tokens"},
        {"\"cached_tokens\":60", "\"cached_tokens\":101", "usage_counter", "cached_tokens"},
        {"\"status\":\"completed\"", "\"status\":\"in_progress\"", "response_status", "response.status"},
        {"\"response.created\"", "\"response.queued\"", "event_order", "type"},
        {"\"response.reasoning_summary_text.delta\"", "\"fixture-private\\nvalue\"", "event_type", "type"}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *body = replace_all(OPENAI_TEXT, cases[i].before, cases[i].after);
        log = diagnostic_exchange(llm, request, body, cases[i].check, cases[i].field);
        if (i == 0) {
            assert(log.diagnostic.event_index == 3);
            assert(!strcmp(log.diagnostic.provider_event, "response.future_event"));
        }
        if (i == 3) {
            assert(log.diagnostic.has_item_index && log.diagnostic.item_index == 1);
            assert(!strcmp(log.diagnostic.expected, "message-1") && !strcmp(log.diagnostic.received, "wrong-id"));
        }
        free(body);
    }
    log = diagnostic_exchange(llm, request, "data: [DONE]\n\n", "event_order", "response.completed");
    assert(!strcmp(log.diagnostic.provider_event, "[DONE]"));

    /* Cross the response limit with legal frames, so the cumulative limit is
     * exercised independently of the per-frame limit. */
    size_t chunk = 1024 * 1024;
    char *large = malloc(chunk + 1);
    assert(large);
    memset(large, 'x', chunk);
    large[chunk] = 0;
    const char *delta = strstr(OPENAI_TEXT, "data: {\"type\":\"response.reasoning_summary_text.delta\"");
    const char *prefix = "data: {\"type\":\"response.reasoning_summary_text.delta\",\"output_index\":0,\"item_id\":\"reason-1\",\"delta\":\"";
    byte_buffer stream = {0};
    assert(delta && !buffer_append(&stream, OPENAI_TEXT, (size_t)(delta - OPENAI_TEXT), OPENAI_WIRE_MAX));
    for (size_t sent = 0; sent <= CHAT_RESPONSE_MAX; sent += chunk) {
        assert(!buffer_append(&stream, prefix, strlen(prefix), OPENAI_WIRE_MAX));
        size_t n = sent == CHAT_RESPONSE_MAX ? 1 : chunk;
        assert(!buffer_append(&stream, large, n, OPENAI_WIRE_MAX));
        assert(!buffer_append(&stream, "\"}\n\n", 4, OPENAI_WIRE_MAX));
    }
    log = diagnostic_exchange(llm, request, stream.data, "size_limit", "response_bytes");
    assert(log.diagnostic.has_limit && log.diagnostic.limit == CHAT_RESPONSE_MAX);
    assert(log.diagnostic.actual == CHAT_RESPONSE_MAX + 1 && log.diagnostic.response_bytes == CHAT_RESPONSE_MAX);
    free(stream.data);
    free(large);

    large = malloc(OPENAI_FRAME_MAX + 2);
    assert(large);
    memset(large, 'x', OPENAI_FRAME_MAX + 1);
    large[OPENAI_FRAME_MAX + 1] = 0;
    log = diagnostic_exchange(llm, request, large, "size_limit", "line_bytes");
    assert(log.diagnostic.limit == OPENAI_FRAME_MAX && log.diagnostic.actual == OPENAI_FRAME_MAX + 1);
    assert(!strcmp(log.diagnostic.provider_event, "<undecoded>"));
    free(large);

    openai_run run = {.request = request, .callback = collect, .user = &log, .wire_bytes = OPENAI_WIRE_MAX};
    assert(!openai_write("x", 1, 1, &run));
    assert(!strcmp(run.diagnostic.field, "wire_bytes") && run.diagnostic.actual == OPENAI_WIRE_MAX + 1);
    char label[OCA_LLM_DIAGNOSTIC_TEXT_MAX + 32], bounded[OCA_LLM_DIAGNOSTIC_TEXT_MAX];
    memset(label, 'a', sizeof(label) - 1); label[sizeof(label) - 1] = 0;
    diagnostic_token(bounded, label);
    assert(strlen(bounded) == sizeof(bounded) - 1 && bounded[sizeof(bounded) - 2] == '~');
    oca_request_destroy(request);
}

static void test_compaction_settings(void) {
    for (size_t i = 0; i < oca_llm_model_count(test_llm); i++) {
        const oca_model *model = oca_llm_model_at(test_llm, i);
        oca_request *request = oca_request_create(OCA_PURPOSE_COMPACTION, 19);
        assert(request);
        oca_request_set_route(request, model, oca_model_min_reasoning(model));
        oca_request_push_msg(request, OCA_MSG_USER, "Summarize this fixture.", NULL, NULL);
        char *serialized = !strcmp(model->provider, "openai") ?
                           openai_serialize(request) : chat_serialize(request);
        assert(serialized);
        cJSON *body = json_parse(serialized, strlen(serialized));
        assert(body && !cJSON_GetObjectItemCaseSensitive(body, "max_tokens") && !cJSON_GetObjectItemCaseSensitive(body, "max_output_tokens") &&
               !cJSON_GetObjectItemCaseSensitive(body, "max_completion_tokens") && !cJSON_GetObjectItemCaseSensitive(body, "temperature"));
        assert(!cJSON_GetObjectItemCaseSensitive(body, "tools"));
        cJSON_Delete(body);
        cJSON_free(serialized);
        oca_request_destroy(request);
    }
}

static void test_openai(oca_llm *llm) {
    char err[256];
    oca_request *request = request_for("gpt-6-astra");
    const char *fixtures[] = {OPENAI_TEXT, OPENAI_TOOLS};
    for (size_t i = 0; i < 2; i++) {
        server service = serve(200, fixtures[i]);
        event_log log = {0};
        assert(!openai_generate(llm, request, collect, &log, service.url, "http://127.0.0.1:1/unused", err, sizeof(err)));
        assert(log.starts == 1 && log.done == 1 && !log.errors);
        if (!i) {
            assert(!strcmp(log.text, "OK") && log.states == 1 && log.reasoning == 1 && log.usage == 1);
            assert(log.counters.input_tokens == 100 && log.counters.cache_hit_tokens == 60 && log.counters.cache_miss_tokens == 40);
            oca_request_push_msg(request, OCA_MSG_ASSISTANT, "OK", "Reviewing.", NULL);
            oca_request_msg_set_state(request, log.state);
            char *body = openai_serialize(request);
            assert(body && strstr(body, "fixture-ciphertext"));
            cJSON_free(body);
            oca_request_destroy(request);
            request = request_for("gpt-6-astra");
            free(log.state);
        } else assert(log.calls == 1 && log.ends == 1 && !log.states);
        char *wire = capture_request(&service);
        assert(strstr(wire, "ChatGPT-Account-Id: fixture-account") && strstr(wire, "originator: oca"));
        cJSON *body = json_parse(strstr(wire, "\r\n\r\n") + 4, strlen(strstr(wire, "\r\n\r\n") + 4));
        assert(body && !strcmp(json_text(body, "model"), "gpt-6-astra"));
        assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(body, "store")) && cJSON_GetObjectItemCaseSensitive(body, "input"));
        assert(!cJSON_GetObjectItemCaseSensitive(body, "max_output_tokens") && !cJSON_GetObjectItemCaseSensitive(body, "temperature"));
        cJSON_Delete(body); free(wire);
    }
    char *large_usage = replace_all(OPENAI_TEXT, "\"output_tokens\":20,\"total_tokens\":120",
                                   "\"output_tokens\":20000,\"total_tokens\":20100");
    server service = serve(200, large_usage);
    event_log above_old_cap = {0};
    assert(!openai_generate(llm, request, collect, &above_old_cap, service.url,
                           "http://127.0.0.1:1/unused", err, sizeof(err)));
    assert(above_old_cap.done == 1 && !above_old_cap.errors && above_old_cap.usage == 1);
    assert(above_old_cap.counters.output_tokens == 20000);
    free(above_old_cap.state);
    free(capture_request(&service));
    free(large_usage);
    service = serve(200, OPENAI_TEXT);
    event_log log = {.cancel = 1};
    assert(!openai_generate(llm, request, collect, &log, service.url, "http://127.0.0.1:1/unused", err, sizeof(err)));
    assert(log.errors == 1 && !log.done && !strcmp(log.category, "cancelled"));
    free(capture_request(&service));

    const char *bad[] = {"data: {broken}\n\n", "data: [DONE]\n\n",
        "data: {\"type\":\"response.created\",\"response\":{\"model\":\"wrong-model\"}}\n\n"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        service = serve(200, bad[i]);
        memset(&log, 0, sizeof(log));
        assert(!openai_generate(llm, request, collect, &log, service.url, "http://127.0.0.1:1/unused", err, sizeof(err)));
        assert(log.errors == 1 && !log.done);
        free(capture_request(&service));
    }
    service = serve(400, "{\"detail\":\"fixture-secret\"}");
    memset(&log, 0, sizeof(log));
    assert(!openai_generate(llm, request, collect, &log, service.url, "http://127.0.0.1:1/unused", err, sizeof(err)));
    assert(log.errors == 1 && !strcmp(log.category, "http"));
    assert(log.diagnostic.has_transport && log.diagnostic.transport.curl_code == CURLE_OK);
    assert(log.diagnostic.http_status == 400);
    char *wire = capture_request(&service);
    assert(!strstr(wire, "max_output_tokens"));
    free(wire);
    service = serve(400, "{\"detail\":\"Unsupported parameter: max_output_tokens\"}");
    memset(&log, 0, sizeof(log));
    assert(!openai_generate(llm, request, collect, &log, service.url, "http://127.0.0.1:1/unused", err, sizeof(err)));
    assert(log.errors == 1 && !log.done && !strcmp(log.category, "http"));
    assert(strstr(log.message, "max_output_tokens"));
    free(capture_request(&service));
    service = serve(400, "{\"error\":{\"param\":\"instructions\",\"message\":\"fixture-secret\"}}");
    memset(&log, 0, sizeof(log));
    assert(!openai_generate(llm, request, collect, &log, service.url, "http://127.0.0.1:1/unused", err, sizeof(err)));
    assert(log.errors == 1 && strstr(log.message, "instructions"));
    free(capture_request(&service));
    char *refreshed = token_response(time(NULL) + 10800, "fixture-refresh-after-401");
    server token_service = serve(200, refreshed);
    service = serve_sequence(401, OPENAI_TEXT, OPENAI_TEXT);
    memset(&log, 0, sizeof(log));
    assert(!openai_generate(llm, request, collect, &log, service.url, token_service.url, err, sizeof(err)));
    assert(log.starts == 1 && log.done == 1 && !log.errors && !strcmp(log.text, "OK"));
    free(log.state);
    free(capture_request(&token_service));
    wire = capture_request(&service);
    char *second_request = strstr(wire + 1, "POST /test HTTP/");
    assert(second_request && !strcmp(find_member(llm, "openai")->refresh_token, "fixture-refresh-after-401"));
    free(wire); cJSON_free(refreshed);

    oca_request_set_cancel(request, stop_request, NULL);
    memset(&log, 0, sizeof(log));
    assert(!openai_generate(llm, request, collect, &log, "http://127.0.0.1:1/unused", "http://127.0.0.1:1/unused", err, sizeof(err)));
    assert(log.errors == 1 && !log.done && !strcmp(log.category, "cancelled"));
    oca_request_destroy(request);

    const char *models[] = {"gpt-5.6-sol", "gpt-5.6-terra", "gpt-5.6-luna"};
    for (size_t i = 0; i < sizeof(models) / sizeof(models[0]); i++) {
        request = request_for(models[i]);
        char *fixture = replace_all(OPENAI_TEXT, "gpt-6-astra", request->model->id);
        service = serve(200, fixture);
        memset(&log, 0, sizeof(log));
        assert(!openai_generate(llm, request, collect, &log, service.url, "http://127.0.0.1:1/unused", err, sizeof(err)));
        assert(log.done == 1 && !log.errors && !strcmp(log.text, "OK"));
        free(log.state);
        wire = capture_request(&service);
        assert(strstr(wire, request->model->id));
        free(wire); free(fixture);
        oca_request_destroy(request);
    }

    request = oca_request_create(OCA_PURPOSE_TURN, 18);
    assert(request);
    oca_request_set_route(request, oca_llm_model_resolve(test_llm, "gpt-6-astra"), 0);
    oca_request_push_msg(request, OCA_MSG_USER, "One raw message.", NULL, NULL);
    char *serialized = openai_serialize(request);
    cJSON *raw = json_parse(serialized, strlen(serialized));
    assert(raw && cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(raw, "input")) == 1 && !cJSON_GetObjectItemCaseSensitive(raw, "tools"));
    assert(!strcmp(json_text(raw, "instructions"), ""));
    cJSON_Delete(raw); cJSON_free(serialized);
    oca_request_destroy(request);
}

/* Exercise the production catalog through OAuth transport, not fixture-only IDs. */
static void test_builtin_openai(const char *auth) {
    char err[256];
    size_t count;
    const oca_model_definition *models = oca_llm_builtin_models(&count);
    oca_llm *llm = oca_llm_create(auth, models, count, err, sizeof(err));
    assert(llm);
    for (size_t i = 0; i < count; i++) {
        const oca_model *model = oca_llm_model_at(llm, i);
        if (strcmp(model->provider, "openai")) continue;
        oca_request *request = oca_request_create(OCA_PURPOSE_TURN, 19);
        assert(request && oca_llm_model_available(llm, model));
        oca_request_set_route(request, model, oca_model_reasoning_index(model, "high"));
        oca_request_push_msg(request, OCA_MSG_USER, "Read a fixture.", NULL, NULL);
        oca_request_add_tool(request, "read", "Read a fixture.", "{\"type\":\"object\"}");
        const char *fixtures[] = {OPENAI_TEXT, OPENAI_TOOLS};
        for (size_t j = 0; j < sizeof(fixtures) / sizeof(fixtures[0]); j++) {
            char *fixture = replace_all(fixtures[j], "gpt-6-astra", model->id);
            server service = serve(200, fixture);
            event_log log = {0};
            assert(!openai_generate(llm, request, collect, &log, service.url,
                                   "http://127.0.0.1:1/unused", err, sizeof(err)));
            assert(log.done == 1 && !log.errors);
            if (j) assert(log.calls == 1 && log.ends == 1);
            else assert(!strcmp(log.text, "OK") && log.reasoning == 1 && log.usage == 1);
            char *wire = capture_request(&service);
            const char *body_at = strstr(wire, "\r\n\r\n");
            assert(body_at);
            cJSON *body = json_parse(body_at + 4, strlen(body_at + 4));
            assert(body && !strcmp(json_text(body, "model"), model->id));
            assert(!strcmp(json_text(cJSON_GetObjectItemCaseSensitive(body, "reasoning"), "effort"), "high"));
            assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(body, "tools")) == 1);
            assert(!cJSON_GetObjectItemCaseSensitive(body, "max_output_tokens"));
            cJSON_Delete(body);
            free(wire);
            free(log.state);
            free(fixture);
        }
        oca_request_destroy(request);
    }
    oca_llm_destroy(llm);
}

static void test_chat_rejection_details(void) {
    const struct { const char *frame, *check; int finished, done; } cases[] = {
        {"{\"choices\":[],\"usage\":{}}", "after_done", 1, 1},
        {"[DONE]", "missing_finish", 0, 0},
        {"{\"choices\":\"private fixture text\"}", "choices_shape", 0, 0},
        {"{\"choices\":[{\"index\":0,\"delta\":{}}]}", "after_finish", 1, 0},
        {"{\"choices\":[{\"index\":1,\"delta\":{}}]}", "choice_index", 0, 0},
        {"{\"choices\":[{\"index\":0,\"delta\":null}]}", "delta_shape", 0, 0},
        {"{\"choices\":[],\"usage\":{\"prompt_tokens\":11}}", "usage_conflict", 1, 0}
    };
    oca_request *request = request_for("deepseek-flash");
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        chat_run run = {.req = request, .started = 1, .finished = cases[i].finished, .done = cases[i].done};
        run.usage.has_input = 1;
        run.usage.input_tokens = 10;
        assert(chat_chunk(&run, cases[i].frame, strlen(cases[i].frame)) != 0);
        assert(!strcmp(run.diagnostic.check, cases[i].check));
        assert(run.diagnostic.event_index == 1 && run.diagnostic.frame_bytes == strlen(cases[i].frame));
        assert(!strstr(run.diagnostic.received, "private fixture"));
    }
    oca_request_destroy(request);
}


static void test_deepseek(oca_llm *llm) {
    char err[256];
    assert(!oca_llm_auth_set_api_key(llm, "deepseek", "fixture-deepseek-key", err, sizeof(err)));
    const char *returned_models[] = {"deepseek-flash", "deepseek-v4.1-flash",
                                    "deepseek-v4-pro", "unrelated-model", "deepseek-flash-unknown"};
    for (size_t i = 0; i < sizeof(returned_models) / sizeof(returned_models[0]); i++) {
        char response[1024];
        snprintf(response, sizeof(response),
            "data: {\"model\":\"%s\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"OK\","
            "\"reasoning_content\":\"Reviewing.\"},\"finish_reason\":\"stop\"}],"
            "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":2,"
            "\"prompt_cache_hit_tokens\":6,\"prompt_cache_miss_tokens\":4}}\n\ndata: [DONE]\n\n",
            returned_models[i]);
        oca_request *request = request_for("deepseek-flash");
        server service = serve(200, response);
        event_log log = {0};
        assert(!chat_generate(llm, request, collect, &log, service.url, err, sizeof(err)));
        if (i > 0) {
            assert(!log.done && log.errors == 1 && !*log.text && !strcmp(log.category, "model"));
            free(capture_request(&service));
            oca_request_destroy(request);
            continue;
        }
        assert(log.done == 1 && !log.errors && log.reasoning && log.usage == 1 && !strcmp(log.text, "OK"));
        char *wire = capture_request(&service);
        assert(strstr(wire, "Authorization: Bearer fixture-deepseek-key"));
        char *body_at = strstr(wire, "\r\n\r\n");
        cJSON *body = json_parse(body_at + 4, strlen(body_at + 4));
        assert(body && !strcmp(json_text(body, "model"), request->model->id));
        assert(!cJSON_GetObjectItemCaseSensitive(body, "max_tokens") && !cJSON_GetObjectItemCaseSensitive(body, "max_completion_tokens"));
        assert(!cJSON_GetObjectItemCaseSensitive(body, "prompt_cache_key"));
        assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(body, "thinking"), "type")), "enabled"));
        assert(!strcmp(json_text(body, "reasoning_effort"), "high"));
        cJSON_Delete(body); free(wire);
        oca_request_destroy(request);

        request = request_for("deepseek-flash");
        oca_request_set_route(request, request->model, 0);
        service = serve(200, response);
        memset(&log, 0, sizeof(log));
        assert(!chat_generate(llm, request, collect, &log, service.url, err, sizeof(err)));
        assert(log.done == 1 && !log.errors);
        wire = capture_request(&service);
        body_at = strstr(wire, "\r\n\r\n");
        body = json_parse(body_at + 4, strlen(body_at + 4));
        assert(body && !cJSON_GetObjectItemCaseSensitive(body, "reasoning_effort"));
        assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(body, "thinking"), "type")), "disabled"));
        cJSON_Delete(body); free(wire);
        oca_request_destroy(request);
    }
}

typedef struct {
    oca_llm *llm;
    const char *url;
    atomic_int *ready;
    atomic_int *go;
    int result;
} refresh_worker;

static void *refresh_concurrently(void *arg) {
    refresh_worker *worker = arg;
    atomic_fetch_add(worker->ready, 1);
    while (!atomic_load(worker->go)) {
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }
    char err[256];
    worker->result = openai_refresh(worker->llm, 0, NULL, worker->url, err, sizeof(err));
    return NULL;
}

static void test_concurrent_refresh(oca_llm *llm) {
    find_member(llm, "openai")->expires_at = 1;
    char *response = token_response(time(NULL) + 7200, "fixture-shared-refresh");
    response_delay_ns = 100000000;
    server service = serve(200, response);
    response_delay_ns = 0;
    atomic_int ready = 0, go = 0;
    refresh_worker workers[2] = {{llm, service.url, &ready, &go, -1},
                                 {llm, service.url, &ready, &go, -1}};
    pthread_t threads[2];
    for (size_t i = 0; i < 2; i++) assert(!pthread_create(&threads[i], NULL, refresh_concurrently, &workers[i]));
    while (atomic_load(&ready) != 2) {
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }
    atomic_store(&go, 1);
    for (size_t i = 0; i < 2; i++) {
        assert(!pthread_join(threads[i], NULL));
        assert(workers[i].result == 0);
    }
    char *request = capture_request(&service);
    assert(request && strstr(request, "refresh_token"));
    assert(!strcmp(find_member(llm, "openai")->refresh_token, "fixture-shared-refresh"));
    free(request);
    cJSON_free(response);
}

static event_log transport_failure(oca_llm *llm, const char *model, const char *url, CURLcode expected) {
    oca_request *request = request_for(model);
    char err[256];
    event_log log = {0};
    int rc = !strcmp(request->model->provider, "openai") ?
        openai_generate(llm, request, collect, &log, url, "http://127.0.0.1:1/unused", err, sizeof(err)) :
        chat_generate(llm, request, collect, &log, url, err, sizeof(err));
    assert(!rc && log.errors == 1 && !log.done && !strcmp(log.category, "transport"));
    assert(log.has_diagnostic && log.diagnostic.has_transport);
    assert(!strcmp(log.diagnostic.check, "transport") && !strcmp(log.diagnostic.field, "curl_code"));
    const oca_llm_transport_diagnostic *t = &log.diagnostic.transport;
    assert(t->curl_code == (int)expected && !strcmp(t->curl_message, curl_easy_strerror(expected)));
    assert(t->attempt == 1 && t->request_bytes > 0 && t->total_us >= 0);
    assert(t->connect_timeout_ms == 15000 && t->total_timeout_ms == 180000);
    assert(t->low_speed_seconds == 45 && t->low_speed_bytes_per_second == 1);
    assert(!log.state && !log.usage && !log.calls && !log.ends);
    oca_request_destroy(request);
    return log;
}

static size_t receive_transport_body(char *data, size_t size, size_t count, void *user) {
    (void)data;
    assert(!size || count <= SIZE_MAX / size);
    size_t length = size * count;
    if (length) transport_received(user);
    return length;
}

static void test_transport_diagnostics(oca_llm *llm) {
    char err[256];
    assert(!oca_llm_auth_set_api_key(llm, "kimi", "fixture-kimi-key", err, sizeof(err)));
    const char *models[] = {"kimi-k3", "deepseek-flash", "gpt-5.6-luna"};
    int refused = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(refused >= 0 && !bind(refused, (struct sockaddr *)&address, sizeof(address)));
    socklen_t address_length = sizeof(address);
    assert(!getsockname(refused, (struct sockaddr *)&address, &address_length));
    char refused_url[128];
    snprintf(refused_url, sizeof(refused_url), "http://127.0.0.1:%u/test", ntohs(address.sin_port));
    const char *chat = "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"OK\"},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
    for (size_t i = 0; i < sizeof(models) / sizeof(models[0]); i++) {
        event_log log = transport_failure(llm, models[i], refused_url, CURLE_COULDNT_CONNECT);
        assert(!log.diagnostic.has_http_status && !log.diagnostic.transport.body_received);
        assert(log.diagnostic.transport.os_errno == ECONNREFUSED);
        assert(log.diagnostic.transport.uploaded_bytes == 0 && log.diagnostic.transport.downloaded_bytes == 0);
        assert(log.diagnostic.transport.body_idle_us == -1);
        char *body = !strcmp(models[i], "gpt-5.6-luna") ?
            replace_all(OPENAI_TEXT, "gpt-6-astra", "gpt-5.6-luna") : strdup(chat);
        response_missing_bytes = 17;
        server service = serve(200, body);
        response_missing_bytes = 0;
        log = transport_failure(llm, models[i], service.url, CURLE_PARTIAL_FILE);
        free(capture_request(&service));
        assert(log.diagnostic.has_http_status && log.diagnostic.http_status == 200);
        assert(log.diagnostic.transport.body_received && log.diagnostic.transport.body_idle_us >= 0);
        assert(log.diagnostic.transport.downloaded_bytes == (int64_t)strlen(body));
        assert(log.diagnostic.transport.uploaded_bytes == (int64_t)log.diagnostic.transport.request_bytes);
        assert(log.diagnostic.wire_bytes == strlen(body) && log.diagnostic.event_index > 0);
        free(body);
    }
    assert(!close(refused));
    assert(!oca_llm_auth_logout(llm, "kimi", err, sizeof(err)));

    body_pause_ns = 500000000;
    server service = serve(200, "delayed fixture body");
    body_pause_ns = 0;
    transport_limits limits = {1000, 250, 45, 1};
    transport_watch watch = {.limits = &limits, .request_bytes = 2, .attempt = 1};
    CURL *curl = curl_easy_init();
    assert(curl);
    assert(curl_easy_setopt(curl, CURLOPT_URL, service.url) == CURLE_OK);
    assert(curl_easy_setopt(curl, CURLOPT_NOPROXY, "*") == CURLE_OK);
    assert(curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "{}") == CURLE_OK);
    assert(curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, limits.connect_ms) == CURLE_OK);
    assert(curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, limits.total_ms) == CURLE_OK);
    assert(curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, limits.low_speed_bytes_per_second) == CURLE_OK);
    assert(curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, limits.low_speed_seconds) == CURLE_OK);
    assert(curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_transport_body) == CURLE_OK);
    assert(curl_easy_setopt(curl, CURLOPT_WRITEDATA, &watch) == CURLE_OK);
    CURLcode result = curl_easy_perform(curl);
    assert(result == CURLE_OPERATION_TIMEDOUT);
    long status;
    assert(curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK && status == 200);
    oca_llm_diagnostic d = {0};
    diagnostic_transport(&d, curl, result, status, &watch);
    assert(d.transport.curl_code == CURLE_OPERATION_TIMEDOUT && d.transport.downloaded_bytes == 1);
    assert(d.transport.uploaded_bytes == 2 && d.transport.total_us >= 200000);
    assert(d.transport.body_received && d.transport.body_idle_us >= 100000);
    assert(d.transport.first_byte_us > 0 && d.transport.total_timeout_ms == 250);
    curl_easy_cleanup(curl);
    free(capture_request(&service));

    const char *bad = "data: {broken}\n\n";
    service = serve_sequence(401, "{}", bad);
    char *refreshed = token_response(time(NULL) + 10800, "fixture-transport-refresh");
    server token_service = serve(200, refreshed);
    oca_request *request = request_for("gpt-5.6-luna");
    event_log log = {0};
    assert(!openai_generate(llm, request, collect, &log, service.url, token_service.url, err, sizeof(err)));
    assert(log.errors == 1 && !strcmp(log.diagnostic.check, "frame_json"));
    assert(log.diagnostic.has_transport && log.diagnostic.transport.attempt == 2);
    assert(log.diagnostic.transport.curl_code == CURLE_WRITE_ERROR && log.diagnostic.http_status == 200);
    assert(log.diagnostic.wire_bytes == strlen(bad));
    free(capture_request(&service));
    free(capture_request(&token_service));
    cJSON_free(refreshed);
    oca_request_destroy(request);
}

static void test_input_budget(oca_llm *llm) {
    const char *ids[] = {"gpt-6-astra", "deepseek-flash"};
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        oca_request *request = request_for(ids[i]);
        long estimate = 0;
        char err[256];
        assert(!oca_llm_estimate_input(request, &estimate, err, sizeof(err)));
        oca_model model = *request->model;
        model.input_limit = estimate - 1;
        request->model = &model;
        event_log log = {0};
        const char *unused = "http://127.0.0.1:1/unused";
        int result = i ? chat_generate(llm, request, collect, &log, unused, err, sizeof(err)) :
            openai_generate(llm, request, collect, &log, unused, unused, err, sizeof(err));
        assert(result < 0 && strstr(err, "input exceeds the local context limit"));
        oca_request_destroy(request);
    }
}

int main(void) {
    alarm(90);
    assert(!unsetenv("KIMI_API_KEY") && !unsetenv("DEEPSEEK_API_KEY"));
    char directory[] = "/tmp/oca-provider-test-XXXXXX";
    assert(mkdtemp(directory));
    char auth[512], err[256];
    snprintf(auth, sizeof(auth), "%s/auth.json", directory);
    oca_llm *llm = fixture_llm_create(auth, err, sizeof(err));
    assert(llm && oca_llm_model_count(llm) == 6 && oca_provider_count() == 3);
    test_llm = llm;
    const char *ids[] = {"gpt-6-astra", "gpt-5.6-sol", "gpt-5.6-terra", "gpt-5.6-luna",
                         "deepseek-flash"};
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        const oca_model *model = oca_llm_model_resolve(llm, ids[i]);
        assert(model && !strcmp(model->id, ids[i]));
        assert(!oca_llm_model_available(llm, model));
        assert(!strcmp(model->provider, i < 4 ? "openai" : "deepseek"));
    }
    assert(oca_llm_auth_set_api_key(llm, "openai", "fixture-key", err, sizeof(err)) < 0);
    test_oauth(llm, directory);
    test_builtin_openai(auth);
    test_compaction_settings();
    test_openai(llm);
    test_empty_completion(llm);
    test_openai_metadata(llm);
    test_stream_diagnostics(llm);
    test_chat_rejection_details();
    test_deepseek(llm);
    test_input_budget(llm);
    test_transport_diagnostics(llm);
    test_concurrent_refresh(llm);
    oca_llm_destroy(llm);
    llm = fixture_llm_create(auth, err, sizeof(err));
    assert(llm && !strcmp(oca_llm_auth_method(llm, "openai"), "oauth") && oca_llm_auth_member_count(llm) == 2);
    test_llm = llm;
    assert(oca_llm_model_available(llm, oca_llm_model_resolve(test_llm, "gpt-5.6-sol")));
    assert(oca_llm_model_available(llm, oca_llm_model_resolve(test_llm, "deepseek-flash")));
    assert(!oca_llm_auth_logout(llm, "openai", err, sizeof(err)));
    assert(!oca_llm_auth_method(llm, "openai"));
    assert(oca_llm_auth_method(llm, "deepseek") && oca_llm_model_available(llm, oca_llm_model_resolve(test_llm, "deepseek-flash")));
    oca_llm_destroy(llm);
    assert(setenv("DEEPSEEK_API_KEY", "environment-deepseek-key", 1) == 0);
    llm = fixture_llm_create(auth, err, sizeof(err));
    assert(llm && oca_llm_auth_member_count(llm) == 1);
    test_llm = llm;
    assert(!strcmp(oca_llm_auth_method(llm, "deepseek"), "api-key"));
    assert(oca_llm_model_available(llm, oca_llm_model_resolve(test_llm, "deepseek-flash")));
    oca_llm_destroy(llm);
    assert(unsetenv("DEEPSEEK_API_KEY") == 0);
    assert(unlink(auth) == 0 && rmdir(directory) == 0);
    puts("test_providers: passed");
    return 0;
}
