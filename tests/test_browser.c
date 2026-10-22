/* Offline RDP/core regressions; --firefox also checks the pinned upstream
 * extractors on local HTML. No model-provider calls are made. */
#define _GNU_SOURCE
#define BROWSER_APPROVAL_SECONDS 1
#include "../oca_core.c"
#include "fixture_models.h"
#include "fixture_server.h"
#include <assert.h>
#include <ftw.h>
#include <sys/ioctl.h>
#include <termios.h>

static char *output_paths[128];
static size_t output_count;
static volatile sig_atomic_t fixture_browser_pid, fixture_http_pid;

static void failed_test(int number) {
    if (fixture_browser_pid > 0) kill(-(pid_t)fixture_browser_pid, SIGTERM);
    if (fixture_http_pid > 0) kill((pid_t)fixture_http_pid, SIGTERM);
    signal(number, SIG_DFL);
    raise(number);
}

static void remember_output(const char *text) {
    const char *path = text ? strstr(text, "\noutput_path=") : NULL;
    if (!path) return;
    path += strlen("\noutput_path="); size_t n = strcspn(path, " (\n");
    assert(n && !strncmp(path, "/tmp/oca_agent_web_", 19));
    for (size_t i = 0; i < output_count; i++)
        if (strlen(output_paths[i]) == n && !memcmp(output_paths[i], path, n)) return;
    assert(output_count < 128);
    output_paths[output_count++] = xstrndup(path, n); assert(output_paths[output_count - 1]);
}

static int send_bytes(int fd, const void *data, size_t length) {
    const char *p = data;
    while (length) {
        ssize_t n = send(fd, p, length, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; length -= (size_t)n;
    }
    return 0;
}

static int receive_bytes(int fd, void *data, size_t length) {
    char *p = data;
    while (length) {
        ssize_t n = recv(fd, p, length, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; length -= (size_t)n;
    }
    return 0;
}

static int listen_local(unsigned *port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((unsigned short)*port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    int yes = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    assert(fd >= 0 && !bind(fd, (struct sockaddr *)&address, sizeof(address)) && !listen(fd, 8));
    socklen_t size = sizeof(address); assert(!getsockname(fd, (struct sockaddr *)&address, &size));
    *port = ntohs(address.sin_port); return fd;
}

static int packet(int fd, const cJSON *value) {
    char *json = cJSON_PrintUnformatted(value); assert(json);
    char header[32]; int n = snprintf(header, sizeof(header), "%zu:", strlen(json));
    int rc = send_bytes(fd, header, 1) || send_bytes(fd, header + 1, (size_t)n - 1) ||
        send_bytes(fd, json, strlen(json));
    cJSON_free(json); return rc;
}

static cJSON *get_packet(int fd) {
    size_t size = 0; char c = 0;
    while (!receive_bytes(fd, &c, 1)) {
        if (c == ':') break;
        assert(c >= '0' && c <= '9' && size < BROWSER_WIRE_MAX);
        size = size * 10 + (size_t)(c - '0');
    }
    if (!size || c != ':') return NULL;
    char *data = malloc(size + 1); assert(data);
    if (receive_bytes(fd, data, size)) { free(data); return NULL; }
    data[size] = 0; cJSON *request = cJSON_Parse(data); free(data); assert(request); return request;
}

typedef struct { int active, consent, scrolled; char url[65536]; } fake_tab;
static fake_tab tabs[128];
static int next_tab = 2;

static char *fake_expression(const char *actor, const char *expression) {
    if (!strcmp(actor, "parent-console")) {
        if (strstr(expression, "gBrowser.addTab")) {
            int id = next_tab++; assert(id < 128); tabs[id].active = 1;
            snprintf(tabs[id].url, sizeof(tabs[id].url), "about:blank");
            char number[32]; snprintf(number, sizeof(number), "%d", id); return xstrdup(number);
        }
        if (strstr(expression, "tabs.find")) {
            const char *number = strstr(expression, "browserId==="); assert(number);
            int id = atoi(number + strlen("browserId===")), count = 0;
            for (int i = 1; i < next_tab; i++) count += tabs[i].active;
            if (id > 0 && id < next_tab && count > 1) tabs[id].active = 0;
            return xstrdup("closed");
        }
        return xstrdup("prepared");
    }
    int id = atoi(actor + strlen("console-")); assert(id > 0 && id < next_tab && tabs[id].active);
    if (!strcmp(expression, "document.readyState")) return xstrdup("complete");
    if (strstr(expression, "location.href+'\\n'+document.readyState")) {
        sbuf probe; sb_init(&probe); sb_printf(&probe, "%s\ncomplete\n1000", tabs[id].url); return sb_take(&probe);
    }
    if (strstr(expression, "const pats=")) { tabs[id].consent = 1; return xstrdup(""); }
    int search = strstr(tabs[id].url, "https://www.google.com/search?q=") != NULL;
    if (strstr(expression, "const scrollOnce=")) {
        assert(tabs[id].consent && !search); tabs[id].scrolled = 1; return xstrdup("scroll skipped hooks=0");
    }
    assert(tabs[id].consent && (search ? !tabs[id].scrolled : tabs[id].scrolled));
    assert(strstr(expression, search ? "# Google search results" : "## Content"));
    sbuf body; sb_init(&body);
    sb_printf(&body, "# Fixture title\n\nURL: %s\n\nUnicode: micelio è utile.\n", tabs[id].url);
    if (strstr(tabs[id].url, "/wall")) sb_puts(&body, "Our systems have detected unusual traffic.\n");
    if (strstr(tabs[id].url, "/large")) {
        for (int i = 0; i < 12000; i++) sb_puts(&body, "test source line\n");
        sb_puts(&body, "END-OF-SOURCE\n");
    }
    return sb_take(&body);
}

static void fake_connection(int fd) {
    cJSON *hello = cJSON_CreateObject(); cJSON_AddStringToObject(hello, "from", "root");
    cJSON_AddStringToObject(hello, "applicationType", "browser"); packet(fd, hello); cJSON_Delete(hello);
    char *long_text = NULL;
    for (;;) {
        cJSON *request = get_packet(fd); if (!request) break;
        const char *actor = json_text(request, "to"), *method = json_text(request, "type"); assert(actor && method);
        cJSON *reply = cJSON_CreateObject(); cJSON_AddStringToObject(reply, "from", actor);
        if (!strcmp(method, "getProcess")) {
            cJSON_AddStringToObject(cJSON_AddObjectToObject(reply, "processDescriptor"), "actor", "parent-descriptor");
        } else if (!strcmp(method, "getTarget")) {
            int parent = !strcmp(actor, "parent-descriptor");
            cJSON *target = cJSON_AddObjectToObject(reply, parent ? "process" : "frame");
            char name[64]; snprintf(name, sizeof(name), "console-%d", atoi(actor + 4));
            cJSON_AddStringToObject(target, "consoleActor", parent ? "parent-console" : name);
        } else if (!strcmp(method, "getTab")) {
            long id; assert(json_integer(cJSON_GetObjectItemCaseSensitive(request, "browserId"), &id));
            char name[64]; snprintf(name, sizeof(name), "tab-%ld", id);
            cJSON_AddStringToObject(cJSON_AddObjectToObject(reply, "tab"), "actor", name);
        } else if (!strcmp(method, "navigateTo")) {
            int id = atoi(actor + 4); assert(id > 0 && id < next_tab);
            snprintf(tabs[id].url, sizeof(tabs[id].url), "%s", json_text(request, "url"));
            if (strstr(tabs[id].url, "/hang")) {
                const char *ready = getenv("OCA_BROWSER_TEST_READY"); assert(ready);
                FILE *marker = fopen(ready, "w"); assert(marker && !fclose(marker));
                char c; while (recv(fd, &c, 1, 0) > 0) {}
                cJSON_Delete(reply); cJSON_Delete(request); break;
            }
        } else if (!strcmp(method, "evaluateJSAsync")) {
            assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(request, "mapped"), "await")));
            cJSON_AddStringToObject(reply, "resultID", "evaluation-1"); packet(fd, reply); cJSON_Delete(reply);
            const char *expression = json_text(request, "text"); assert(expression);
            int id = !strncmp(actor, "console-", 8) ? atoi(actor + 8) : 0;
            if (id && strstr(tabs[id].url, "/malformed") && strstr(expression, "## Content")) {
                send_bytes(fd, "7:{broken", 9); cJSON_Delete(request); continue;
            }
            if (id && strstr(tabs[id].url, "/oversized") && strstr(expression, "## Content")) {
                send_bytes(fd, "4194305:", 8); cJSON_Delete(request); continue;
            }
            reply = cJSON_CreateObject(); cJSON_AddStringToObject(reply, "from", actor);
            cJSON_AddStringToObject(reply, "type", "evaluationResult"); cJSON_AddStringToObject(reply, "resultID", "evaluation-1");
            char *result = fake_expression(actor, expression); assert(result);
            if (strlen(result) > 10000) {
                free(long_text); long_text = result;
                cJSON *value = cJSON_AddObjectToObject(reply, "result");
                cJSON_AddStringToObject(value, "type", "longString"); cJSON_AddStringToObject(value, "actor", "long-text");
                cJSON_AddNumberToObject(value, "length", utf8_chars(result));
            } else { cJSON_AddStringToObject(reply, "result", result); free(result); }
        } else if (!strcmp(method, "substring")) {
            assert(long_text); cJSON_AddStringToObject(reply, "substring", long_text);
        }
        cJSON *event = cJSON_CreateObject(); cJSON_AddStringToObject(event, "from", "root");
        cJSON_AddStringToObject(event, "type", "tabListChanged"); packet(fd, event); cJSON_Delete(event);
        int failed = packet(fd, reply); cJSON_Delete(reply); cJSON_Delete(request);
        if (failed) break;
    }
    free(long_text);
}

static int fake_firefox(int argc, char **argv) {
    unsigned port = 0;
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--start-debugger-server")) port = (unsigned)atoi(argv[i + 1]);
    assert(port); int listener = listen_local(&port); tabs[1].active = 1;
    snprintf(tabs[1].url, sizeof(tabs[1].url), "about:blank");
    for (;;) { int fd = accept(listener, NULL, NULL); if (fd >= 0) { fake_connection(fd); close(fd); } }
    return 0;
}

static int remove_fixture(const char *path, const struct stat *state, int kind, struct FTW *walk) {
    (void)state; (void)walk; return kind == FTW_DP ? rmdir(path) : unlink(path);
}

static void stop_fixture(pid_t pid) {
    if (pid <= 0) return;
    kill(-pid, SIGTERM); kill(pid, SIGTERM);
    for (int i = 0; i < 100; i++) {
        pid_t rc = waitpid(pid, NULL, WNOHANG);
        if (rc == pid || (rc < 0 && errno == ECHILD)) return;
        struct timespec delay = {0, 20000000}; nanosleep(&delay, NULL);
    }
    kill(-pid, SIGKILL); kill(pid, SIGKILL); waitpid(pid, NULL, 0);
}

static oca_tool_executor *make_executor(const char *root, unsigned port) {
    char err[512]; oca_tool_executor *e = oca_tool_executor_create(root, root, err, sizeof(err));
    if (!e) { fprintf(stderr, "%s\n", err); abort(); }
    snprintf(e->browser_endpoint, sizeof(e->browser_endpoint), "127.0.0.1:%u", port); return e;
}

static tool_outcome call(oca_tool_executor *e, const char *tool, const char *value) {
    cJSON *args = browser_params(!strcmp(tool, "google_search") ? "query" : "url", value);
    char *json = cJSON_PrintUnformatted(args); assert(json); cJSON_Delete(args);
    tool_outcome out = {0}; executor_call(e, e->workspace, tool, json, NULL, &out);
    if (e->browser_pid > 0) fixture_browser_pid = e->browser_pid;
    cJSON_free(json); assert(out.text); remember_output(out.text); return out;
}

static void check(tool_outcome out, oca_tool_state state, const char *needle) {
    if (out.state != state || !out.text || !strstr(out.text, needle)) {
        fprintf(stderr, "browser state %d, expected %d: %s\n", out.state, state, out.text ? out.text : "NULL"); abort();
    }
    free(out.text);
}

typedef struct { atomic_int *cancel; const char *ready; } cancellation;

static void *cancel_when_navigating(void *data) {
    cancellation *request = data;
    for (int i = 0; i < 500 && access(request->ready, F_OK); i++) {
        struct timespec delay = {0, 20000000}; nanosleep(&delay, NULL);
    }
    assert(!access(request->ready, F_OK));
    atomic_store(request->cancel, 1); return NULL;
}

static void check_scripts(void) {
    const char *names[] = {"consent", "search", "page", "scroll"};
    const char *scripts[] = {BROWSER_CONSENT_JS, BROWSER_SEARCH_JS, BROWSER_PAGE_JS, BROWSER_SCROLL_JS};
    for (size_t i = 0; i < 4; i++) {
        char path[128], err[256]; size_t length;
        snprintf(path, sizeof(path), "tests/fixtures/ds4_web_%s.js", names[i]);
        char *reference = read_whole_file(path, 16384, &length, err, sizeof(err)); assert(reference);
        char *script = strchr(reference, '\n'); assert(script); script++;
        if (length && reference[length - 1] == '\n') reference[length - 1] = 0;
        assert(!strcmp(script, scripts[i])); free(reference);
    }
}

static void check_preview(void) {
    char err[256]; sbuf page; sb_init(&page);
    for (int i = 0; i < 130; i++) sb_printf(&page, "line %d\n", i);
    char *result = browser_page_result("https://example.org/", page.buf, err, sizeof(err)); assert(result);
    remember_output(result);
    assert(strstr(result, "<head -100 ") && strstr(result, "line 99\n</head>") && !strstr(result, "line 100\n"));
    free(result); sb_free(&page); sb_init(&page);
    for (int i = 0; i < 8191; i++) sb_puts(&page, "a");
    sb_puts(&page, "é remainder");
    result = browser_page_result("https://example.org/", page.buf, err, sizeof(err)); assert(result);
    remember_output(result); assert(utf8_text_valid(result, strlen(result)) && !strstr(result, "é"));
    free(result); sb_free(&page);
}

static void core_checks(oca_tool_executor *e, const char *root) {
    assert(!setenv("HOME", root, 1)); fixture_server server = fixture_server_start(); char err[512];
    oca_llm *llm = fixture_llm_create(NULL, err, sizeof(err)); assert(llm);
    oca_core_options options = {.workspace = root, .flags = OCA_CORE_INTERACTIVE, .model = "kimi-k3", .executor = e};
    oca_core *core = oca_core_create(llm, &options, err, sizeof(err)); assert(core);
    const int answers[] = {0, -1, 1, 1};
    for (size_t i = 0; i < 4; i++) {
        assert(!oca_core_submit_prompt(core, "fixture:tool visit_page {\"url\":\"https://example.org/core\"}", err, sizeof(err)));
        int prompts = 0, results = 0, done = 0;
        while (!done) {
            oca_core_event event; assert(oca_core_wait_event(core, &event)); assert(event.kind != OCA_CE_ERROR);
            if (event.kind == OCA_CE_BROWSER_APPROVAL && event.text) {
                prompts++;
                if (answers[i] >= 0) assert(!oca_core_browser_answer(core, answers[i], err, sizeof(err)));
            }
            if (event.kind == OCA_CE_TOOL_RESULT) {
                assert(event.tool_state == (i < 2 ? OCA_TOOL_ERROR : OCA_TOOL_COMPLETE));
                if (i == 1) assert(strstr(event.text, "timed out"));
                remember_output(event.text); results++;
            }
            done = event.kind == OCA_CE_TURN_DONE; oca_core_event_release(&event);
        }
        assert(results == 1 && prompts == (i < 3));
    }
    oca_core_destroy(core); oca_llm_destroy(llm); fixture_server_stop(&server);
}

static char *http_headers(int fd) {
    sbuf text; sb_init(&text); char c;
    while (text.len < 16384 && !receive_bytes(fd, &c, 1)) {
        sb_put(&text, &c, 1);
        if (text.len >= 4 && !memcmp(text.buf + text.len - 4, "\r\n\r\n", 4)) return sb_take(&text);
    }
    sb_free(&text); return NULL;
}

static pid_t http_fixture(unsigned *port) {
    int listener = listen_local(port); pid_t child = fork(); assert(child >= 0);
    if (child) { close(listener); fixture_http_pid = child; return child; }
    for (;;) {
        int fd = accept(listener, NULL, NULL); if (fd < 0) continue;
        char *headers = http_headers(fd); if (!headers) { close(fd); continue; }
        sbuf body; sb_init(&body);
        sb_puts(&body, "<!doctype html><html><head><meta charset='utf-8'><title>Mycelium fixture</title></head><body><p>Outside main</p><main><h1>Micelio è utile</h1>");
        if (strstr(headers, "GET /search ")) {
            sb_puts(&body, "<a style='display:block' href='https://www.google.com/url?q=https%3A%2Fexample.org%2Funwrapped'>Unwrapped link</a>"
                "<a style='display:block' href='https://www.google.com/goto?url=opaque'>Opaque redirect</a>");
            for (int i = 1; i <= 22; i++) sb_printf(&body,
                "<a style='display:block' href='https://example.org/result-%d'>Result %d</a>", i, i);
        } else if (strstr(headers, "GET /consent ")) {
            sb_puts(&body, "<button onclick=\"this.remove();document.getElementById('consent').textContent='Consent handled'\">Accept all</button><p id='consent'>Waiting</p>");
        } else if (strstr(headers, "GET /lazy ")) {
            sb_puts(&body, "<div class='lazy' style='height:2200px'>Scroll area</div><script>window.onscroll=()=>{if(window.loaded)return;window.loaded=true;const p=document.createElement('p');p.textContent='Lazy source loaded '+('x'.repeat(400));document.body.append(p);};</script>");
        } else if (strstr(headers, "GET /viewport ")) {
            sb_puts(&body, "<p id='size'></p><script>document.getElementById('size').textContent=innerWidth+'x'+innerHeight;</script>");
        } else if (strstr(headers, "GET /cookie ")) {
            sb_puts(&body, "<p id='cookie'></p><script>document.getElementById('cookie').textContent=document.cookie;</script>");
        } else {
            sb_puts(&body, "<p>First paragraph <a href='/source'>source link</a>.</p><table><tr><th>Species</th><th>Density</th></tr><tr><td>Ganoderma</td><td>120</td></tr></table><pre>line one\nline two</pre><p style='display:none'>HIDDEN TEXT</p><p id='dynamic'></p><script>setTimeout(()=>document.getElementById('dynamic').textContent='Dynamic source text',300)</script>");
        }
        sb_puts(&body, "</main></body></html>"); char response[512];
        int n = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n%sContent-Length: %zu\r\nConnection: close\r\n\r\n",
            strstr(headers, "GET /plain ") ? "Set-Cookie: parity=persisted; Max-Age=3600; Path=/\r\n" : "", body.len);
        send_bytes(fd, response, (size_t)n); send_bytes(fd, body.buf, body.len); sb_free(&body); free(headers); close(fd);
    }
}

static char *reference_extract(oca_tool_executor *e, const char *url, const char *reference_path) {
    browser_connection b = {.fd = -1}; assert(!browser_connect(&b, e->browser_endpoint));
    cJSON *list = browser_command(&b, "root", "listTabs", cJSON_CreateObject(), 2000); assert(list);
    const char *actor = json_text(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(list, "tabs"), 0), "actor");
    assert(actor); char *descriptor = xstrdup(actor); cJSON_Delete(list);
    cJSON *p = browser_params("url", url); cJSON_AddBoolToObject(p, "waitForLoad", 0);
    cJSON *reply = browser_command(&b, descriptor, "navigateTo", p, 2000); assert(reply); cJSON_Delete(reply);
    assert(!browser_wait_ready(&b, descriptor, 1)); char err[256]; size_t size;
    char *script = read_whole_file(reference_path, 16384, &size, err, sizeof(err)); assert(script);
    char *result = browser_page_eval(&b, descriptor, script);
    if (!result) fprintf(stderr, "%s\n", b.error);
    assert(result); free(script); free(descriptor); close(b.fd); return result;
}

static void close_firefox_normally(oca_tool_executor *e, pid_t pid) {
    browser_connection b = {.fd = -1}; assert(!browser_connect(&b, e->browser_endpoint));
    char *parent = browser_parent_console(&b); assert(parent);
    char *reply = browser_evaluate_actor(&b, parent,
        "(()=>{setTimeout(()=>Services.startup.quit(Ci.nsIAppStartup.eForceQuit),100);return 'closing';})()");
    assert(reply && !strcmp(reply, "closing")); free(reply); free(parent); close(b.fd);
    int exited = 0;
    for (int i = 0; i < 250; i++) {
        if (waitpid(pid, NULL, WNOHANG) == pid) { exited = 1; break; }
        struct timespec delay = {0, 20000000}; nanosleep(&delay, NULL);
    }
    assert(exited); fixture_browser_pid = 0;
}

static int tui_checks(const char *image) {
    /* The fixture core uses a private bound-but-not-listening port. Approval
     * tests never launch a real browser or touch the user's DevTools server. */
    int reserved = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0); assert(reserved >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!bind(reserved, (struct sockaddr *)&address, sizeof(address)));
    socklen_t address_size = sizeof(address); assert(!getsockname(reserved, (struct sockaddr *)&address, &address_size));
    char port[16]; snprintf(port, sizeof(port), "%u", ntohs(address.sin_port));
    char *executable = realpath(image, NULL); assert(executable);
    fixture_server server = fixture_server_start();
    const char *answers[] = {"n\r", " yes\r", NULL};
    const char *expected[] = {"user denied Firefox browser start", "Firefox exited before DevTools became ready", "approval timed out"};
    for (size_t test = 0; test < 3; test++) {
        char root[] = "/tmp/oca-browser-tui-XXXXXX", path[512], err[256]; assert(mkdtemp(root));
        snprintf(path, sizeof(path), "%s/.oca/config.json", root);
        const char *config = "{\"show\":{\"read\":true}}";
        assert(!write_file_atomic_0600(path, config, strlen(config), err, sizeof(err)));
        int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC); assert(master >= 0);
        assert(!grantpt(master) && !unlockpt(master)); char *slave_name = ptsname(master); assert(slave_name);
        int slave = open(slave_name, O_RDWR | O_NOCTTY); assert(slave >= 0);
        struct winsize size = {.ws_row = 30, .ws_col = 140}; assert(!ioctl(slave, TIOCSWINSZ, &size));
        pid_t child = fork(); assert(child >= 0);
        if (!child) {
            setsid(); ioctl(slave, TIOCSCTTY, 0);
            dup2(slave, 0); dup2(slave, 1); dup2(slave, 2); close(slave); close(master);
            assert(!setenv("HOME", root, 1) && !setenv("OCA_FIREFOX", "/oca-missing-test-firefox", 1));
            assert(!setenv("OCA_TEST_BROWSER_PORT", port, 1));
            assert(!chdir(root));
            execl(executable, executable, "-m", "kimi-k3",
                "fixture:tool visit_page {\"url\":\"https://example.org/\"}", (char *)NULL);
            _exit(127);
        }
        close(slave); sbuf output; sb_init(&output);
        int answered = 0, saw_result = 0, save_answered = 0, exited = 0, status = 0;
        long long deadline = executor_now_ms() + (test == 2 ? 45000 : 15000);
        while (executor_now_ms() < deadline) {
            struct pollfd wait = {master, POLLIN, 0};
            if (poll(&wait, 1, 100) > 0) {
                char buffer[8192]; ssize_t n = read(master, buffer, sizeof(buffer));
                if (n > 0) { assert(output.len + (size_t)n < 1024 * 1024); sb_put(&output, buffer, (size_t)n); }
            }
            if (!answered && output.buf && strstr(output.buf, "auto-no in 30s")) {
                if (answers[test]) assert(write(master, answers[test], strlen(answers[test])) == (ssize_t)strlen(answers[test]));
                answered = 1;
            }
            if (answered && !saw_result && strstr(output.buf, expected[test]) && strstr(output.buf, "All 1 tool calls finished.")) {
                saw_result = 1; assert(write(master, "/quit\r", 6) == 6);
            }
            if (saw_result && !save_answered && strstr(output.buf, "Save current session?")) {
                save_answered = 1; assert(write(master, "n\r", 2) == 2);
            }
            if (waitpid(child, &status, WNOHANG) == child) { exited = 1; break; }
        }
        close(master);
        if (!exited) { kill(child, SIGKILL); waitpid(child, &status, 0); }
        if (!exited || !answered || !saw_result || !WIFEXITED(status) || WEXITSTATUS(status)) {
            fprintf(stderr, "browser TUI test %zu failed: %s\n", test, output.buf ? output.buf : ""); abort();
        }
        sb_free(&output); assert(!nftw(root, remove_fixture, 32, FTW_DEPTH | FTW_PHYS));
    }
    fixture_server_stop(&server); free(executable); close(reserved);
    puts("test_browser: TUI approval checks passed"); return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], OCA_TOOL_WORKER_FLAG)) return oca_tool_worker_main();
    if (argc > 1 && !strcmp(argv[1], "--no-remote")) return fake_firefox(argc, argv);
    if (argc == 3 && !strcmp(argv[1], "--tui")) return tui_checks(argv[2]);
    signal(SIGABRT, failed_test);
    int real = argc > 1 && !strcmp(argv[1], "--firefox"), failed = 0;
    check_scripts(); check_preview(); char root[] = "/tmp/oca-browser-test-XXXXXX", err[512]; assert(mkdtemp(root));
    char cancellation_ready[512]; snprintf(cancellation_ready, sizeof(cancellation_ready), "%s/navigation-ready", root);
    assert(!setenv("OCA_BROWSER_TEST_READY", cancellation_ready, 1));
    if (!real) { char *exe = realpath(argv[0], NULL); assert(exe && !setenv("OCA_FIREFOX", exe, 1)); free(exe); }
    unsigned port = 0; int reservation = listen_local(&port); close(reservation);
    oca_tool_executor *e = make_executor(root, port); pid_t browser = 0;
    if (real) {
        e->browser_allowed = 1; /* The explicit --firefox test authorizes startup. */
        if (argc > 2) {
            tool_outcome out = !strcmp(argv[2], "--search") && argc > 3 ?
                call(e, "google_search", argv[3]) : call(e, "visit_page", argv[2]);
            printf("state=%d\n%s\n", out.state, out.text); failed = out.state != OCA_TOOL_COMPLETE; free(out.text);
        } else {
            unsigned http_port = 0; pid_t server = http_fixture(&http_port); char url[128];
            snprintf(url, sizeof(url), "http://127.0.0.1:%u/plain", http_port);
            check(call(e, "visit_page", url), OCA_TOOL_COMPLETE, "Outside main"); size_t length;
            char *saved = read_whole_file(output_paths[output_count - 1], BROWSER_WIRE_MAX, &length, err, sizeof(err)); assert(saved);
            char *reference = reference_extract(e, url, "tests/fixtures/ds4_web_page.js");
            assert(!strcmp(saved, reference) && strstr(saved, "Dynamic source text") && !strstr(saved, "HIDDEN TEXT"));
            free(saved); free(reference);
            snprintf(url, sizeof(url), "http://127.0.0.1:%u/search", http_port);
            reference = reference_extract(e, url, "tests/fixtures/ds4_web_search.js");
            assert(strstr(reference, "[Unwrapped link](https://example.org/unwrapped)") &&
                strstr(reference, "[Result 19]") && !strstr(reference, "- [Result 20]") &&
                !strstr(reference, "- [Opaque redirect]"));
            free(reference);
            snprintf(url, sizeof(url), "http://127.0.0.1:%u/consent", http_port);
            check(call(e, "visit_page", url), OCA_TOOL_COMPLETE, "Consent handled");
            snprintf(url, sizeof(url), "http://127.0.0.1:%u/lazy", http_port);
            tool_outcome lazy = call(e, "visit_page", url);
            if (!strstr(lazy.text, "Lazy source loaded")) {
                char *probe = reference_extract(e, url, "tests/fixtures/ds4_web_scroll.js");
                fprintf(stderr, "Reference scroll result: %s\n", probe); free(probe);
            }
            check(lazy, OCA_TOOL_COMPLETE, "Lazy source loaded");
            snprintf(url, sizeof(url), "http://127.0.0.1:%u/viewport", http_port);
            check(call(e, "visit_page", url), OCA_TOOL_COMPLETE, "1365x900");
            browser = e->browser_pid; assert(browser > 0); oca_tool_executor_destroy(e); assert(!kill(browser, 0));
            e = make_executor(root, port);
            snprintf(url, sizeof(url), "http://127.0.0.1:%u/cookie", http_port);
            check(call(e, "visit_page", url), OCA_TOOL_COMPLETE, "parity=persisted");
            close_firefox_normally(e, browser);
            e->browser_allowed = 1;
            check(call(e, "visit_page", url), OCA_TOOL_COMPLETE, "parity=persisted");
            browser = e->browser_pid; assert(browser > 0);
            stop_fixture(server); fixture_http_pid = 0;
        }
    } else {
        assert(browser_endpoint_port("127.0.0.1:1234") == 1234);
        assert(!browser_endpoint_port("127.0.0.1:1234@elsewhere")); oca_core core = {0};
        assert(!validate_call(&core, "visit_page", "{\"url\":\"file:///tmp/source.html\"}", err, sizeof(err)));
        assert(!validate_call(&core, "google_search", "{\"query\":\"   \",\"extra\":true}", err, sizeof(err)));
        assert(validate_call(&core, "google_search", "{\"query\":\"\"}", err, sizeof(err)));
        check(call(e, "visit_page", "https://example.org/"), OCA_TOOL_ERROR, "interactive approval"); assert(!e->browser_pid);
        core_checks(e, root); browser = e->browser_pid; assert(browser > 0);
        tool_outcome out = call(e, "visit_page", "https://example.org/large");
        assert(out.state == OCA_TOOL_COMPLETE && strstr(out.text, "<head -100 ") && !strstr(out.text, "END-OF-SOURCE"));
        size_t length; char *saved = read_whole_file(output_paths[output_count - 1], BROWSER_WIRE_MAX, &length, err, sizeof(err));
        assert(saved && strstr(saved, "END-OF-SOURCE")); free(saved); free(out.text);
        check(call(e, "visit_page", "https://example.org/wall"), OCA_TOOL_COMPLETE, "unusual traffic");
        check(call(e, "visit_page", "https://example.org/malformed"), OCA_TOOL_ERROR, "invalid Firefox DevTools JSON");
        check(call(e, "visit_page", "https://example.org/oversized"), OCA_TOOL_ERROR, "oversized");
        assert(e->browser_pid == browser && !kill(browser, 0));
        check(call(e, "google_search", "micelio & funghi?"), OCA_TOOL_COMPLETE, "micelio%20%26%20funghi%3F");
        atomic_int cancel; atomic_init(&cancel, 0); pthread_t thread;
        cancellation request = {&cancel, cancellation_ready};
        assert(!pthread_create(&thread, NULL, cancel_when_navigating, &request));
        memset(&out, 0, sizeof(out)); executor_call(e, root, "visit_page", "{\"url\":\"https://example.org/hang\"}", &cancel, &out);
        assert(!pthread_join(thread, NULL)); check(out, OCA_TOOL_INTERRUPTED, "canceled"); assert(!kill(browser, 0));
        check(call(e, "visit_page", "https://example.org/recovered"), OCA_TOOL_COMPLETE, "Fixture title");
        char *profile = xstrdup(e->browser_profile); assert(profile);
        oca_tool_executor_destroy(e); assert(!kill(browser, 0) && !access(profile, F_OK)); free(profile);
        e = make_executor(root, port);
        check(call(e, "visit_page", "https://example.org/reconnected"), OCA_TOOL_COMPLETE, "Fixture title");
    }
    if (!browser) browser = e->browser_pid;
    oca_tool_executor_destroy(e); if (browser > 0) assert(!kill(browser, 0));
    stop_fixture(browser); /* Test teardown only: OCA must leave Firefox running. */
    assert(!nftw(root, remove_fixture, 32, FTW_DEPTH | FTW_PHYS));
    for (size_t i = 0; i < output_count; i++) { assert(!unlink(output_paths[i])); free(output_paths[i]); }
    puts(real ? "test_browser: Firefox parity checks finished" : "test_browser: offline parity checks passed");
    return failed;
}
