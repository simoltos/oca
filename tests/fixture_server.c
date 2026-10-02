#include "fixture_server.h"

#include <assert.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <cjson/cJSON.h>

/* ============================================================================
 * HTTP fixtures
 * ============================================================================ */

static const char *field_text(const cJSON *object, const char *name) {
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(field) ? field->valuestring : "";
}

static int send_text(int fd, const char *text) {
    size_t left = strlen(text);
    while (left) {
        ssize_t count = write(fd, text, left);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        text += count;
        left -= (size_t)count;
    }
    return 0;
}

/* Takes ownership of delta. JSON quoting preserves the exact test input. */
static int send_delta(int fd, cJSON *delta, const char *finish) {
    cJSON *root = cJSON_CreateObject();
    cJSON *choices = cJSON_AddArrayToObject(root, "choices");
    cJSON *choice = cJSON_CreateObject();
    assert(root && choices && choice && delta);
    assert(cJSON_AddItemToArray(choices, choice));
    assert(cJSON_AddNumberToObject(choice, "index", 0));
    assert(cJSON_AddItemToObject(choice, "delta", delta));
    if (finish) assert(cJSON_AddStringToObject(choice, "finish_reason", finish));
    char *json = cJSON_PrintUnformatted(root);
    assert(json);
    int rc = send_text(fd, "data: ") || send_text(fd, json) || send_text(fd, "\n\n");
    cJSON_free(json);
    cJSON_Delete(root);
    return rc;
}

static int send_content(int fd, const char *field, const char *text) {
    cJSON *delta = cJSON_CreateObject();
    assert(delta && cJSON_AddStringToObject(delta, field, text));
    return send_delta(fd, delta, NULL);
}

static void add_call(cJSON *calls, const char *name, const char *args) {
    int index = cJSON_GetArraySize(calls);
    static unsigned long sequence;
    char id[64];
    snprintf(id, sizeof(id), "fixture-%ld-%lu-%d", (long)getpid(), ++sequence, index);
    cJSON *call = cJSON_CreateObject();
    assert(call && cJSON_AddItemToArray(calls, call));
    assert(cJSON_AddNumberToObject(call, "index", index));
    assert(cJSON_AddStringToObject(call, "id", id));
    assert(cJSON_AddStringToObject(call, "type", "function"));
    cJSON *function = cJSON_AddObjectToObject(call, "function");
    assert(function && cJSON_AddStringToObject(function, "name", name));
    assert(cJSON_AddStringToObject(function, "arguments", args));
}

static void serve_request(int fd) {
    char *input = calloc(1024 * 1024, 1);
    assert(input);
    size_t used = 0;
    char *body = NULL;
    for (;;) {
        assert(used + 1 < 1024 * 1024);
        ssize_t count = read(fd, input + used, 1024 * 1024 - used - 1);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { free(input); return; }
        used += (size_t)count;
        body = strstr(input, "\r\n\r\n");
        if (!body) continue;
        body += 4;
        const char *length = strstr(input, "Content-Length: ");
        assert(length && length < body);
        size_t size = strtoul(length + strlen("Content-Length: "), NULL, 10);
        assert(size < 1024 * 1024 - (size_t)(body - input));
        if (used >= (size_t)(body - input) + size) break;
    }
    assert(strstr(input, "Authorization: Bearer fixture-api-key\r\n"));
    cJSON *request = cJSON_Parse(body);
    assert(request && !strcmp(field_text(request, "model"), "kimi-k3"));
    const cJSON *tool;
    cJSON_ArrayForEach(tool, cJSON_GetObjectItemCaseSensitive(request, "tools")) {
        const cJSON *function = cJSON_GetObjectItemCaseSensitive(tool, "function");
        const char *name = field_text(function, "name");
        if (strcmp(name, "ask_user") && strcmp(name, "bash")) continue;
        const cJSON *schema = cJSON_GetObjectItemCaseSensitive(function, "parameters");
        assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(schema, "additionalProperties")));
        const cJSON *properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
        if (!strcmp(name, "bash")) {
            assert(cJSON_GetArraySize(properties) == 1);
            const cJSON *command = cJSON_GetObjectItemCaseSensitive(properties, "command");
            assert(!strcmp(field_text(command, "type"), "string"));
            assert(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(command, "minLength")) == 1);
            continue;
        }
        const cJSON *questions = cJSON_GetObjectItemCaseSensitive(properties, "questions");
        assert(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(questions, "minItems")) == 1);
        assert(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(questions, "maxItems")) == 3);
        const cJSON *question = cJSON_GetObjectItemCaseSensitive(questions, "items");
        assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(question, "additionalProperties")));
        properties = cJSON_GetObjectItemCaseSensitive(question, "properties");
        const cJSON *options = cJSON_GetObjectItemCaseSensitive(properties, "options");
        assert(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(options, "minItems")) == 2);
        assert(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(options, "maxItems")) == 3);
        assert(!strcmp(field_text(cJSON_GetObjectItemCaseSensitive(options, "items"), "type"), "string"));
        assert(strstr(field_text(function, "description"), "None of the above"));
    }
    const cJSON *messages = cJSON_GetObjectItemCaseSensitive(request, "messages");
    int count = cJSON_GetArraySize(messages);
    assert(count > 0);
    const cJSON *last = cJSON_GetArrayItem(messages, count - 1);
    /* Runtime system notes can follow the user or tool message under test. */
    while (count > 1 && !strcmp(field_text(last, "role"), "system"))
        last = cJSON_GetArrayItem(messages, --count - 1);
    const char *prompt = field_text(last, "content");
    const char *scenario = prompt;
    for (int i = count - 1; i >= 0; i--) {
        const cJSON *message = cJSON_GetArrayItem(messages, i);
        if (!strcmp(field_text(message, "role"), "user")) {
            scenario = field_text(message, "content");
            break;
        }
    }
    if (strstr(scenario, "fixture:mode-staged")) {
        const char *system = field_text(cJSON_GetArrayItem(messages, 0), "content");
        assert(strstr(system, "You are OCA, a terminal agent.") && !strstr(system, "Mode:"));
        int writes = 0;
        const cJSON *tool;
        cJSON_ArrayForEach(tool, cJSON_GetObjectItemCaseSensitive(request, "tools")) {
            const char *name = field_text(cJSON_GetObjectItemCaseSensitive(tool, "function"), "name");
            if (!strcmp(name, "write") || !strcmp(name, "edit")) writes++;
        }
        assert(writes == 2);
    }
    if (strstr(prompt, "fixture:error")) {
        send_text(fd, "HTTP/1.1 500 Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        goto done;
    }
    if (send_text(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n")) goto done;
    if (send_content(fd, "role", "assistant")) goto done;
    if (send_content(fd, "reasoning_content", "Reviewing the fixture request.")) goto done;

    cJSON *delta = cJSON_CreateObject();
    cJSON *calls = cJSON_AddArrayToObject(delta, "tool_calls");
    assert(delta && calls);
    const char *cursor = prompt;
    while ((cursor = strstr(cursor, "fixture:tool "))) {
        cursor += strlen("fixture:tool ");
        const char *space = strchr(cursor, ' ');
        assert(space && (size_t)(space - cursor) < 64);
        char name[64];
        memcpy(name, cursor, (size_t)(space - cursor));
        name[space - cursor] = 0;
        const char *end = strchr(space + 1, '\n');
        size_t len = end ? (size_t)(end - space - 1) : strlen(space + 1);
        char *args = malloc(len + 1);
        assert(args);
        memcpy(args, space + 1, len);
        args[len] = 0;
        add_call(calls, name, args);
        free(args);
        cursor = space + 1 + len;
    }
    if (strstr(prompt, "fixture:ask")) {
        add_call(calls, "ask_user",
            "{\"questions\":[{\"question\":\"Which color?\",\"options\":[\"red\",\"blue\"]},"
            "{\"question\":\"Which size?\",\"options\":[\"small\",\"large\"]}]}");
    }
    if (strstr(prompt, "fixture:goal-complete"))
        add_call(calls, "goal_status", "{\"state\":\"complete\",\"message\":\"verified\"}");
    if (strstr(prompt, "fixture:goal-blocked"))
        add_call(calls, "goal_status", "{\"state\":\"blocked\",\"message\":\"fixture block\"}");

    int continuation = !strcmp(field_text(last, "role"), "tool");
    int summary = !strncmp(prompt, "Summarize this conversation", 27);
    int repair = strstr(scenario, "fixture:repair-") != NULL;
    if (repair && (!continuation || strstr(prompt, "Invalid tool arguments:"))) {
        if (strstr(scenario, "fixture:repair-ask")) {
            add_call(calls, "ask_user", continuation ?
                "{\"questions\":[{\"question\":\"Which color?\",\"options\":[\"red\",\"blue\"]},"
                "{\"question\":\"Which size?\",\"options\":[\"small\",\"large\"]}]}" :
                "{\"questions\":[{\"question\":\"Which color?\",\"options\":[\"red\",\"blue\",\"green\",\"yellow\"]}]}");
        } else {
            if (!continuation)
                add_call(calls, "write", "{\"path\":\"validation-peer.txt\",\"content\":\"must not run\"}");
            int repeat = strstr(scenario, "fixture:repair-repeat") != NULL;
            add_call(calls, "bash", continuation && !repeat ?
                "{\"command\":\"printf repaired\"}" :
                "{\"command\":\"touch validation-bash.txt\",\"timeout\":10}");
        }
    }
    if (cJSON_GetArraySize(calls) && (!continuation || repair) && !summary) {
        if (send_delta(fd, delta, "tool_calls")) goto done;
    } else {
        cJSON_Delete(delta);
        char text[1024];
        if (continuation) {
            int finished = 0;
            while (count > 0 && !strcmp(field_text(cJSON_GetArrayItem(messages, --count), "role"), "tool"))
                finished++;
            snprintf(text, sizeof(text), "All %d tool calls finished.", finished);
        } else if (summary) {
            assert(!strcmp(field_text(request, "reasoning_effort"), "low"));
            assert(!cJSON_GetObjectItemCaseSensitive(request, "tools"));
            snprintf(text, sizeof(text), "Fixture summary: Preserve the earlier conversation and continue the pending work.");
        } else if (strstr(prompt, "fixture:prompt-contract")) {
            const cJSON *first = cJSON_GetArrayItem(messages, 0);
            const cJSON *tools = cJSON_GetObjectItemCaseSensitive(request, "tools");
            const char *system = field_text(first, "content");
            const char *prefix = "You are OCA, a terminal agent.\n\n"
                "Help the user complete the requested task.\n";
            assert(!strcmp(field_text(first, "role"), "system") && !strncmp(system, prefix, strlen(prefix)));
            if (strstr(prompt, "require-project"))
                assert(strstr(system, "Project context from AGENTS.md:") && strstr(system, "Project marker."));
            assert(strstr(system, "Respect the user's scope and explicit constraints."));
            assert(strstr(system, "Be concise, omit filler"));
            assert(strstr(system, "Before declaring a task unsupported") && strstr(system, "installed programs"));
            assert(!strstr(system, "implementation-ready"));
            assert(!strstr(system, "Mode:"));
            int writes = 0, skills = 0, bash = 0;
            const cJSON *tool;
            cJSON_ArrayForEach(tool, tools) {
                const cJSON *function = cJSON_GetObjectItemCaseSensitive(tool, "function");
                const char *name = field_text(function, "name");
                if (!strcmp(name, "write")) {
                    writes++;
                    assert(strstr(field_text(function, "description"), "Read an existing file"));
                } else if (!strcmp(name, "edit")) {
                    writes++;
                    assert(strstr(field_text(function, "description"), "Read the file first"));
                } else if (!strcmp(name, "bash")) {
                    bash++;
                    assert(strstr(field_text(function, "description"), "network requests"));
                    assert(strstr(field_text(function, "description"), "only a nonempty command field"));
                } else if (!strcmp(name, "skill")) {
                    skills++;
                    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(function, "parameters");
                    const cJSON *properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
                    const cJSON *action = cJSON_GetObjectItemCaseSensitive(properties, "action");
                    const cJSON *values = cJSON_GetObjectItemCaseSensitive(action, "enum");
                    assert(cJSON_GetArraySize(values) == 3);
                    assert(!strcmp(cJSON_GetStringValue(cJSON_GetArrayItem(values, 0)), "load"));
                    assert(cJSON_GetObjectItemCaseSensitive(properties, "content"));
                }
            }
            assert(tools && skills == 0 && writes == 2 && bash == 1);
            if (strstr(prompt, "require-edit-history")) {
                int found = 0;
                const cJSON *message, *call;
                cJSON_ArrayForEach(message, messages) {
                    cJSON_ArrayForEach(call, cJSON_GetObjectItemCaseSensitive(message, "tool_calls")) {
                        const cJSON *function = cJSON_GetObjectItemCaseSensitive(call, "function");
                        if (strcmp(field_text(function, "name"), "edit") ||
                            !strstr(field_text(function, "arguments"), "history-marker")) continue;
                        const cJSON *result;
                        cJSON_ArrayForEach(result, messages) {
                            if (!strcmp(field_text(result, "role"), "tool") &&
                                !strcmp(field_text(result, "tool_call_id"), field_text(call, "id"))) found = 1;
                        }
                    }
                }
                assert(found);
            }
            snprintf(text, sizeof(text), "Prompt contract: normal.");
        } else if (strstr(prompt, "fixture:hints")) {
            int enabled = 0, instructions = 0;
            const cJSON *message;
            cJSON_ArrayForEach(message, messages) {
                if (strcmp(field_text(message, "role"), "system")) continue;
                const char *note = field_text(message, "content");
                if (strstr(note, "The user enabled programming hints.")) enabled = 1;
                if (strstr(note, "Programming hints are disabled.")) enabled = 0;
                if (strstr(note, "Occasionally explain a useful concept")) instructions++;
            }
            snprintf(text, sizeof(text), "Hints: %s. Full instructions: %d.", enabled ? "on" : "off", instructions);
        } else if (strstr(prompt, "fixture:timed-reasoning")) {
            struct timespec delay = {.tv_sec = 1, .tv_nsec = 300000000};
            while (nanosleep(&delay, &delay) && errno == EINTR) {}
            snprintf(text, sizeof(text), "Timed reasoning complete.");
        } else if (strstr(prompt, "fixture:long-reasoning")) {
            char reasoning[2049];
            memset(reasoning, 'r', sizeof(reasoning) - 1);
            reasoning[sizeof(reasoning) - 1] = 0;
            for (int i = 0; i < 40; i++) if (send_content(fd, "reasoning_content", reasoning)) goto done;
            snprintf(text, sizeof(text), "Long reasoning complete.");
        } else if (strstr(prompt, "fixture:hang")) {
            struct pollfd peer = {.fd = fd, .events = POLLIN};
            /* Leave time for slow sanitizer and terminal runs to cancel it. */
            assert(poll(&peer, 1, 30000) >= 0);
            goto done;
        } else if (strstr(prompt, "fixture:slow")) {
            for (int i = 0; i < 60; i++) {
                struct timespec delay = {.tv_nsec = 20000000};
                nanosleep(&delay, NULL);
                snprintf(text, sizeof(text), "tick %d ", i);
                if (send_content(fd, "content", text)) goto done;
            }
            text[0] = 0;
        } else {
            snprintf(text, sizeof(text), "%s: %.900s", count == 1 ? "RAW" : "Received", prompt);
        }
        if (*text && send_content(fd, "content", text)) goto done;
        if (send_delta(fd, cJSON_CreateObject(), "stop")) goto done;
    }
    send_text(fd, "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":20,"
                  "\"total_tokens\":120,\"prompt_tokens_details\":{\"cached_tokens\":60},"
                  "\"completion_tokens_details\":{\"reasoning_tokens\":5}}}\n\ndata: [DONE]\n\n");
done:
    cJSON_Delete(request);
    free(input);
}

/* ============================================================================
 * Server lifecycle
 * ============================================================================ */

fixture_server fixture_server_start(void) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    assert(fcntl(listener, F_SETFD, FD_CLOEXEC) == 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(listener, 8) == 0);
    socklen_t length = sizeof(address);
    assert(getsockname(listener, (struct sockaddr *)&address, &length) == 0);
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/chat/completions", ntohs(address.sin_port));
    assert(setenv("OCA_TEST_PROVIDER_URL", url, 1) == 0);
    assert(setenv("KIMI_API_KEY", "fixture-api-key", 1) == 0);
    int stop[2];
    assert(pipe(stop) == 0);
    assert(fcntl(stop[1], F_SETFD, FD_CLOEXEC) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(stop[1]);
        signal(SIGPIPE, SIG_IGN);
        alarm(120);
        pid_t handlers[16] = {0};
        struct pollfd ready[] = {{.fd = stop[0], .events = POLLIN},
                                 {.fd = listener, .events = POLLIN}};
        for (;;) {
            int rc = poll(ready, 2, -1);
            if (rc < 0 && errno == EINTR) continue;
            assert(rc > 0);
            if (ready[0].revents) break;
            if (ready[1].revents & POLLIN) {
                int peer = accept(listener, NULL, NULL);
                assert(peer >= 0);
                size_t slot;
                for (slot = 0; slot < 16; slot++) {
                    if (handlers[slot] && waitpid(handlers[slot], NULL, WNOHANG) == handlers[slot])
                        handlers[slot] = 0;
                    if (!handlers[slot]) break;
                }
                assert(slot < 16);
                handlers[slot] = fork();
                assert(handlers[slot] >= 0);
                if (!handlers[slot]) {
                    close(listener);
                    close(stop[0]);
                    alarm(30);
                    serve_request(peer);
                    close(peer);
                    _exit(0);
                }
                close(peer);
            }
        }
        close(stop[0]);
        close(listener);
        for (size_t i = 0; i < 16; i++) {
            if (!handlers[i]) continue;
            if (waitpid(handlers[i], NULL, WNOHANG) == 0) {
                kill(handlers[i], SIGTERM);
                assert(waitpid(handlers[i], NULL, 0) == handlers[i]);
            }
        }
        _exit(0);
    }
    close(listener);
    close(stop[0]);
    return (fixture_server){child, stop[1]};
}

void fixture_server_stop(fixture_server *server) {
    close(server->stop_fd);
    int status;
    assert(waitpid(server->pid, &status, 0) == server->pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    server->pid = -1;
    server->stop_fd = -1;
    assert(unsetenv("OCA_TEST_PROVIDER_URL") == 0);
    assert(unsetenv("KIMI_API_KEY") == 0);
}
