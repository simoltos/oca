#define _GNU_SOURCE
#include "../oca_core.h"
#include "fixture_models.h"
#include "fixture_server.h"
#include "fixture_io.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <unistd.h>

enum { FAIL_NONE, FAIL_MUTEX, FAIL_CONDITION, FAIL_PIPE, FAIL_PIPE_FLAGS };
static int creation_failure;
static int creation_pipe[2];

static int creation_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attributes) {
    if (creation_failure == FAIL_MUTEX) return EAGAIN;
    return pthread_mutex_init(mutex, attributes);
}

static int creation_condition_init(pthread_cond_t *condition, const pthread_condattr_t *attributes) {
    if (creation_failure == FAIL_CONDITION) return EAGAIN;
    return pthread_cond_init(condition, attributes);
}

static int creation_pipe_open(int descriptors[2]) {
    if (creation_failure == FAIL_PIPE) {
        errno = EMFILE;
        return -1;
    }
    int rc = pipe(descriptors);
    if (!rc && creation_failure == FAIL_PIPE_FLAGS) {
        creation_pipe[0] = descriptors[0];
        creation_pipe[1] = descriptors[1];
    }
    return rc;
}

static int creation_fcntl(int descriptor, int command, ...) {
    if (creation_failure == FAIL_PIPE_FLAGS && command == F_SETFL) {
        errno = EIO;
        return -1;
    }
    if (command == F_SETFL || command == F_SETFD || command == F_DUPFD || command == F_DUPFD_CLOEXEC) {
        va_list args;
        va_start(args, command);
        int value = va_arg(args, int);
        va_end(args);
        return fcntl(descriptor, command, value);
    }
    return fcntl(descriptor, command);
}

static int fixture_generate(oca_llm *, const oca_request *, oca_event_fn *,
                            void *, char *, size_t);
static int interaction_broadcast(pthread_cond_t *condition);
static int interaction_wait(pthread_cond_t *condition, pthread_mutex_t *mutex);
#define oca_llm_generate fixture_generate
#define pthread_cond_broadcast interaction_broadcast
#define pthread_cond_wait interaction_wait
#define pthread_mutex_init creation_mutex_init
#define pthread_cond_init creation_condition_init
#define pipe creation_pipe_open
#define fcntl creation_fcntl
#define write fixture_io_write
#define fchmod fixture_io_chmod
#include "../oca_core.c"
#undef fchmod
#undef write
#undef fcntl
#undef pipe
#undef pthread_cond_init
#undef pthread_mutex_init
#undef pthread_cond_wait
#undef pthread_cond_broadcast
#undef oca_llm_generate

#include <assert.h>
#include <ftw.h>

static oca_core *fixture_core;
static int fixture_enabled;
static int fixture_error;
static int fixture_burst;
static int fixture_requests;
static oca_event fixture_usage;
static oca_llm_diagnostic fixture_diagnostic;
static sbuf fixture_trace;
static oca_core *immediate_interaction;
static int forbid_interaction_wait;

static int interaction_broadcast(pthread_cond_t *condition) {
    oca_core *core = immediate_interaction;
    if (core && condition == &core->cond && core->eq_head != core->eq_tail &&
        core->eq[(core->eq_tail + EVENT_RING_SIZE - 1) % EVENT_RING_SIZE].kind == OCA_CE_INTERACTION) {
        immediate_interaction = NULL;
        char error[256];
        const oca_core_event *event = &core->eq[(core->eq_tail + EVENT_RING_SIZE - 1) % EVENT_RING_SIZE];
        const char *answer = "None of the above\n\nDetails: amber";
        sbuf json_buffer;
        sb_init(&json_buffer);
        sb_puts(&json_buffer, "[{\"question\":");
        sb_json_str(&json_buffer, event->questions[0].question);
        sb_puts(&json_buffer, ",\"answer\":");
        sb_json_str(&json_buffer, answer);
        sb_puts(&json_buffer, "}]");
        int submit = oca_core_submit_answers(core, json_buffer.buf, error, sizeof(error));
        if (submit) fprintf(stderr, "interaction answer failed: %s JSON=%s\n", error, json_buffer.buf);
        sb_free(&json_buffer);
        assert(!submit);
    }
    return pthread_cond_broadcast(condition);
}

static int interaction_wait(pthread_cond_t *condition, pthread_mutex_t *mutex) {
    assert(!forbid_interaction_wait && "An answer submitted during event publication was lost");
    return pthread_cond_wait(condition, mutex);
}

static void test_interaction_handoff(void) {
    oca_core core = {.interactive = 1, .running = 1, .wake_pending = 1};
    assert(!pthread_mutex_init(&core.mu, NULL));
    assert(!pthread_cond_init(&core.cond, NULL));
    atomic_init(&core.cancel_latch, 0);
    cJSON *args = cJSON_Parse("{\"questions\":[{\"question\":\"Which color?\",\"options\":[\"red\",\"blue\"]}]}");
    assert(args);
    tool_outcome outcome = {0};
    immediate_interaction = &core;
    forbid_interaction_wait = 1;
    tool_ask_user(&core, args, &outcome);
    forbid_interaction_wait = 0;
    assert(!immediate_interaction);
    assert(outcome.state == OCA_TOOL_COMPLETE);
    assert(!strcmp(outcome.text, "Q: Which color?\nA: None of the above\n\nDetails: amber\n"));
    free(outcome.text);
    cJSON_Delete(args);
    for (size_t i = core.eq_head; i != core.eq_tail; i = (i + 1) % EVENT_RING_SIZE)
        oca_core_event_release(&core.eq[i]);
    assert(!pthread_cond_destroy(&core.cond));
    assert(!pthread_mutex_destroy(&core.mu));
}

static int collect_trace(void *user, const char *line) {
    sb_puts(user, line);
    return ((sbuf *)user)->oom ? -1 : 0;
}

static int fixture_generate(oca_llm *llm, const oca_request *request,
                            oca_event_fn *callback, void *user, char *err, size_t errsz) {
    fixture_requests++;
    if (!fixture_enabled)
        return oca_llm_generate(llm, request, callback, user, err, errsz);
    oca_event event = { .kind = OCA_EV_RESPONSE_START,
        .request_id = fixture_core->request_seq, .model_id = fixture_core->route_model };
    assert(!callback(&event, user));
    if (fixture_burst) {
        event.kind = OCA_EV_REASONING_DELTA;
        event.text = "row\n";
        for (int i = 0; i < 1300; i++) assert(!callback(&event, user));
    }
    event.kind = OCA_EV_TEXT_DELTA;
    event.text = "fixture answer";
    assert(!callback(&event, user));
    fixture_usage.kind = OCA_EV_USAGE;
    assert(!callback(&fixture_usage, user));
    if (fixture_error) {
        event.kind = OCA_EV_ERROR;
        event.category = "provider";
        event.message = "fixture failure";
        event.diagnostic = &fixture_diagnostic;
    } else {
        event.kind = OCA_EV_RESPONSE_DONE;
        event.finish = "stop";
    }
    assert(!callback(&event, user));
    return 0;
}

static oca_core_event wait_kind(oca_core *core, oca_core_event_kind kind) {
    for (;;) {
        oca_core_event event;
        assert(oca_core_wait_event(core, &event));
        if (event.kind == kind) return event;
        assert(event.kind != OCA_CE_SAVE_ASK && event.kind != OCA_CE_SAVE_ASK_CONT);
        assert(event.kind != OCA_CE_QUIT_OK);
        oca_core_event_release(&event);
    }
}

static void discard_kind(oca_core *core, oca_core_event_kind kind) {
    oca_core_event event = wait_kind(core, kind);
    oca_core_event_release(&event);
}

static void turn(oca_core *core, const char *prompt) {
    char err[256];
    assert(!oca_core_submit_prompt(core, prompt, err, sizeof(err)));
    oca_core_status status;
    oca_core_get_status(core, &status);
    discard_kind(core, OCA_CE_TURN_DONE);
    oca_core_get_status(core, &status);
    assert(!status.working);
}

static size_t json_allocations, json_fail_after;

static void *json_test_malloc(size_t size) {
    if (json_allocations++ == json_fail_after) return NULL;
    return malloc(size);
}

static void test_tool_json(void) {
    const char *source = "{\"path\":\"a\",\"text\":\"\\u00e9\\n\",\"number\":1.5,"
                         "\"yes\":true,\"no\":false,\"nil\":null,\"items\":[1,2]}";
    const char *why = NULL;
    cJSON *document = json_parse(source, strlen(source), &why);
    assert(document && cJSON_GetArraySize(document) == 7);
    assert(!strcmp(json_text(document, "text"), "\xc3\xa9\n"));
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(document, "yes")));
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(document, "no")));
    assert(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(document, "nil")));
    long value = 0;
    assert(!json_integer(cJSON_GetObjectItemCaseSensitive(document, "number"), &value));
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(document, "items");
    assert(cJSON_GetArraySize(items) == 2);
    assert(json_integer(cJSON_GetArrayItem(items, 1), &value) && value == 2);
    cJSON_Delete(document);

    const char *invalid[] = {
        "{\"x\":1,\"x\":2}", "{\"x\":1,\"\\u0078\":2}",
        "{\"x\":\"\\u0000\"}", "{\"x\":\"\\ud83d\\ude00\"}",
        "{\"x\":\"\xc0\xaf\"}", "{\"x\":\"\n\"}",
        "[1,]", "{\"x\":}", "{} trailing", "1e9999", "\"\\q\""
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        why = NULL;
        assert(!json_parse(invalid[i], strlen(invalid[i]), &why) && why);
    }
    const char embedded_nul[] = {'[', '0', 0, ']'};
    assert(!json_parse(embedded_nul, sizeof(embedded_nul), &why));

    for (size_t depth = 16; depth <= 17; depth++) {
        char text[40];
        memset(text, '[', depth);
        text[depth] = '0';
        memset(text + depth + 1, ']', depth);
        text[depth * 2 + 1] = 0;
        document = json_parse(text, strlen(text), &why);
        assert((document != NULL) == (depth == 16));
        cJSON_Delete(document);
    }
    for (size_t count = 4095; count <= 4096; count++) {
        sbuf array;
        sb_init(&array);
        sb_puts(&array, "[");
        for (size_t i = 0; i < count; i++) sb_puts(&array, i ? ",0" : "0");
        sb_puts(&array, "]");
        document = json_parse(array.buf, array.len, &why);
        assert((document != NULL) == (count == 4095));
        cJSON_Delete(document);
        sb_free(&array);
    }
    size_t limit = 1u << 20;
    char *text = malloc(limit + 4);
    assert(text);
    text[0] = '"';
    memset(text + 1, 'a', limit);
    text[limit + 1] = '"';
    text[limit + 2] = 0;
    document = json_parse(text, limit + 2, &why);
    assert(document && strlen(cJSON_GetStringValue(document)) == limit);
    cJSON_Delete(document);
    text[limit + 1] = 'a';
    text[limit + 2] = '"';
    text[limit + 3] = 0;
    assert(!json_parse(text, limit + 3, &why));
    assert(!strcmp(why, "string too long"));
    free(text);

    cJSON_Hooks hooks = {.malloc_fn = json_test_malloc, .free_fn = free};
    int succeeded = 0;
    cJSON_InitHooks(&hooks);
    for (json_fail_after = 0; json_fail_after < 32; json_fail_after++) {
        json_allocations = 0;
        why = NULL;
        document = json_parse(source, strlen(source), &why);
        if (document) {
            cJSON_Delete(document);
            succeeded = 1;
            break;
        }
        assert(why && !strcmp(why, "out of memory"));
    }
    cJSON_InitHooks(NULL);
    assert(succeeded);
}

static void test_session_exchange_bounds(void) {
    char error[256];
    call_rec calls[] = {{.id = "one"}, {.id = "two"}};
    msg_rec messages[] = {
        {.role = OCA_MSG_USER},
        {.role = OCA_MSG_ASSISTANT, .calls = calls, .n_calls = 2},
        {.role = OCA_MSG_TOOL, .call_id = "two"},
        {.role = OCA_MSG_TOOL, .call_id = "one"},
    };
    assert(!validate_log_exchanges(messages, 4, error, sizeof(error)));
    assert(validate_log_exchanges(messages, 3, error, sizeof(error)) < 0);
    assert(strstr(error, "missing tool results"));
    messages[1].n_calls = 1;
    assert(validate_log_exchanges(messages, 2, error, sizeof(error)) < 0);
    assert(strstr(error, "missing tool results"));
    messages[1].n_calls = 2;
    messages[3].call_id = "two";
    assert(validate_log_exchanges(messages, 4, error, sizeof(error)) < 0);
    assert(strstr(error, "duplicate tool result"));

    msg_rec *partial = calloc(1, sizeof(*partial));
    assert(partial);
    partial->n_calls = 2;
    partial->text = strdup("partially constructed message");
    assert(partial->text);
    msg_free_all(partial, 1);

    oca_core_event event = {
        .kind = OCA_CE_INTERACTION, .question_count = 2,
        .entry_count = 1, .session_count = 1, .skill_count = 1,
    };
    event.questions = calloc(event.question_count, sizeof(*event.questions));
    assert(event.questions);
    event.questions[0].question = strdup("partial question");
    assert(event.questions[0].question);
    event.questions[0].option_count = 2;
    event.questions[1].option_count = 2;
    event.questions[1].options = calloc(2, sizeof(char *));
    assert(event.questions[1].options);
    event.questions[1].options[0] = strdup("partial option");
    assert(event.questions[1].options[0]);
    oca_core_event_release(&event);
    assert(!event.questions && !event.question_count);
    oca_core_event_release(&event);
}

static void test_replay_interrupted(void) {
    msg_rec messages[] = {
        {.role = OCA_MSG_ASSISTANT, .state = MS_INTERRUPTED, .reasoning = "partial reasoning"},
        {.role = OCA_MSG_ASSISTANT, .state = MS_INTERRUPTED, .reasoning = "finished reasoning", .text = "partial answer"},
        {.role = OCA_MSG_ASSISTANT, .state = MS_COMPLETE, .reasoning = "complete reasoning", .text = "complete answer"},
    };
    oca_core core = {.running = 1, .wake_pending = 1, .msgs = messages, .n_msgs = 3};
    assert(!pthread_mutex_init(&core.mu, NULL));
    assert(!pthread_cond_init(&core.cond, NULL));
    push_transcript(&core);
    assert(core.eq_tail == 1);
    oca_core_event *replay = &core.eq[0];
    assert(replay->kind == OCA_CE_TRANSCRIPT && replay->entry_count == 8);
    assert(replay->entries[0].kind == OCA_CE_ASSISTANT_REASON);
    assert(!strcmp(replay->entries[0].text, "partial reasoning"));
    assert(replay->entries[1].kind == OCA_CE_TURN_DONE && !replay->entries[1].name);
    assert(replay->entries[3].kind == OCA_CE_ASSISTANT_TEXT && !strcmp(replay->entries[3].text, "partial answer"));
    assert(replay->entries[4].kind == OCA_CE_TURN_DONE && !replay->entries[4].name);
    assert(replay->entries[7].kind == OCA_CE_TURN_DONE && !replay->entries[7].name);
    oca_core_event_release(replay);
    assert(!pthread_cond_destroy(&core.cond));
    assert(!pthread_mutex_destroy(&core.mu));
}

/* A goal read is synchronous with respect to the event queue, even when the
 * worker cannot consume actions. It must never append a conversation item. */
static void test_goal_read(void) {
    oca_core core = {.running = 1, .interactive = 1, .wake_pending = 1,
                     .state = OCA_S_IDLE};
    assert(!pthread_mutex_init(&core.mu, NULL));
    assert(!pthread_cond_init(&core.cond, NULL));
    char error[256];
    core.goal_state = OCA_GOAL_ACTIVE;
    core.goal_text = strdup("Finish the test");
    assert(core.goal_text);
    assert(!oca_core_goal(&core, OCA_GOAL_CMD_SHOW, NULL, error, sizeof(error)));
    assert(core.aq_head == core.aq_tail && core.eq_tail == 1 && core.n_msgs == 0);
    assert(!core.dirty && core.goal_state == OCA_GOAL_ACTIVE);
    assert(core.eq[0].kind == OCA_CE_GOAL && !strcmp(core.eq[0].name, "show"));
    assert(!strcmp(core.eq[0].text, "Finish the test"));
    oca_core_event_release(&core.eq[0]);
    core.eq_head = core.eq_tail = 0;
    core.goal_state = OCA_GOAL_NONE;
    free(core.goal_text);
    core.goal_text = NULL;
    assert(!oca_core_goal(&core, OCA_GOAL_CMD_SHOW, NULL, error, sizeof(error)));
    assert(core.eq[0].goal == OCA_GOAL_NONE && !core.eq[0].text);
    oca_core_event_release(&core.eq[0]);
    core.eq_tail = EVENT_RING_SIZE - 1;
    assert(oca_core_goal(&core, OCA_GOAL_CMD_SHOW, NULL, error, sizeof(error)) < 0);
    assert(strstr(error, "event queue is full"));
    assert(!pthread_cond_destroy(&core.cond));
    assert(!pthread_mutex_destroy(&core.mu));
}

static void test_startup_reasoning(void) {
    const char *const levels[] = {"a", "b", "c", "d", "e", "f"};
    const oca_reasoning expected[] = {OCA_REASONING_NONE, 0, 0, 1, 1, 2, 2};
    for (size_t n = 0; n <= 6; n++) {
        oca_model_definition definition = {"alias", "canonical", "kimi", 262144, levels, n, 0};
        char error[256];
        oca_llm *llm = oca_llm_create(NULL, &definition, 1, error, sizeof(error));
        assert(llm);
        oca_core core = {.llm = llm};
        const char *identifiers[] = {"alias", "canonical"};
        for (size_t i = 0; i < 2; i++) {
            oca_core_options options = {.model = identifiers[i],
                                        .last_used_model = "canonical", .last_used_reasoning = "a"};
            assert(!select_initial_route(&core, &options, error, sizeof(error)));
            assert(!strcmp(core.route_model, "canonical") && core.route_effort == expected[n]);
            if (n) {
                options.reasoning_name = levels[n - 1];
                assert(!select_initial_route(&core, &options, error, sizeof(error)));
                assert(core.route_effort == (oca_reasoning)n - 1);
                options.reasoning_name = "invalid";
                assert(select_initial_route(&core, &options, error, sizeof(error)) < 0);
                assert(strstr(error, "Available levels:") && strstr(error, levels[n - 1]));
            }
        }
        if (n) {
            assert(!select_initial_route(&core, &(oca_core_options){
                .last_used_model = "canonical", .last_used_reasoning = levels[n - 1]
            }, error, sizeof(error)));
            assert(core.route_effort == (oca_reasoning)n - 1);
        }
        assert(!select_initial_route(&core, &(oca_core_options){0}, error, sizeof(error)));
        assert(!core.route_model[0] && core.route_effort == OCA_REASONING_NONE);
        oca_llm_destroy(llm);
    }
}

static void test_goal_duration(void) {
    oca_core core = {0};
    assert(!pthread_mutex_init(&core.mu, NULL));
    core.goal_elapsed_ms = 3723000;
    set_goal_state(&core, OCA_GOAL_ACTIVE, 0);
    core.goal_started_ms -= 1250;
    set_goal_state(&core, OCA_GOAL_PAUSED, 0);
    uint64_t paused = core.goal_elapsed_ms;
    assert(paused >= 3724250 && paused < 3725250);
    assert(!core.goal_started_ms && goal_elapsed(&core) == paused);
    set_goal_state(&core, OCA_GOAL_ACTIVE, 0);
    core.goal_started_ms -= 1000;
    set_goal_state(&core, OCA_GOAL_COMPLETED, 0);
    assert(core.goal_elapsed_ms >= paused + 1000 && !core.goal_started_ms);

    session_data saved, loaded;
    session_data_init(&saved);
    strcpy(saved.id, "0123456789abcdef0123456789abcdef");
    saved.created = saved.last_used = 1;
    saved.workspace = "/tmp";
    saved.model = "kimi-k3";
    saved.effort = "low";
    saved.goal = "Check duration";
    saved.goal_state = OCA_GOAL_COMPLETED;
    saved.goal_elapsed_ms = core.goal_elapsed_ms;
    sbuf encoded;
    sb_init(&encoded);
    char error[256];
    assert(!session_serialize(&saved, &encoded));
    assert(!session_deserialize(encoded.buf, encoded.len, &loaded, error, sizeof(error)));
    assert(loaded.goal_elapsed_ms == saved.goal_elapsed_ms);
    session_data_free(&loaded);
    char *duration = strstr(encoded.buf, "goal-elapsed ");
    assert(duration);
    duration[13] = '-';
    assert(session_deserialize(encoded.buf, encoded.len, &loaded, error, sizeof(error)));
    assert(strstr(error, "goal duration"));
    sb_free(&encoded);
    saved.goal_elapsed_ms = 0;
    sb_init(&encoded);
    assert(!session_serialize(&saved, &encoded));
    assert(!strstr(encoded.buf, "goal-elapsed"));
    assert(!session_deserialize(encoded.buf, encoded.len, &loaded, error, sizeof(error)));
    assert(!loaded.goal_elapsed_ms);
    session_data_free(&loaded);
    sb_free(&encoded);
    set_goal_state(&core, OCA_GOAL_NONE, 0);
    assert(!core.goal_elapsed_ms && !core.goal_started_ms);
    assert(!pthread_mutex_destroy(&core.mu));
}

static void test_legacy_mode(oca_llm *llm, const char *workspace) {
    const char *id = "abcdef0123456789abcdef0123456789";
    sbuf encoded;
    sb_init(&encoded);
    sb_printf(&encoded, "oca-session 1\nid 32\n%s\ncreated 1\nlast-used 1\n"
        "title 6\nLegacy\nworkspace %zu\n%s\nagents 0\n\nmodel 7\nkimi-k3\n"
        "reasoning 3\nlow\nmode plan\ngoal active 8\nContinue\nmessages 2\n"
        "message user 2\nHi\nmessage assistant complete 7 3 4 5 6 0 0\n"
        "kimi-k3\nlow\nplan\nHello\nThink.\n\ncompaction none\nend\n",
        id, strlen(workspace), workspace);
    char path[1024], err[256];
    snprintf(path, sizeof(path), "%s/.oca", workspace);
    assert(!mkdir(path, 0700) || errno == EEXIST);
    snprintf(path, sizeof(path), "%s/.oca/sessions", workspace);
    assert(!mkdir(path, 0700) || errno == EEXIST);
    snprintf(path, sizeof(path), "%s/.oca/sessions/%s.oca", workspace, id);
    assert(!write_file_atomic_0600(path, encoded.buf, encoded.len, err, sizeof(err)));
    oca_core *core = oca_core_create(llm, &(oca_core_options){
        .workspace = workspace, .flags = OCA_CORE_COMMAND}, err, sizeof(err));
    assert(core && !load_session(core, id, err, sizeof(err)));
    assert(!core->dirty && !core->conversation_dirty && core->goal_state == OCA_GOAL_ACTIVE);
    assert(core->n_msgs == 2 && !strcmp(core->msgs[1].reasoning, "Think."));
    sbuf prompt;
    sb_init(&prompt);
    render_system_text(core, &prompt);
    assert(strstr(prompt.buf, "You are OCA, a terminal agent.") && !strstr(prompt.buf, "Mode:"));
    assert(strstr(prompt.buf, "Active goal:\nContinue"));
    sb_free(&prompt);
    assert(!validate_call(core, "write", "{\"path\":\"new.txt\",\"content\":\"ok\"}", err, sizeof(err)));
    assert(!validate_call(core, "edit", "{\"path\":\"new.txt\",\"old_text\":\"old\",\"new_text\":\"new\"}", err, sizeof(err)));
    session_data saved;
    session_borrow(core, &saved);
    sbuf normalized;
    sb_init(&normalized);
    assert(!session_serialize(&saved, &normalized));
    assert(strstr(normalized.buf, "mode normal\n") && !strstr(normalized.buf, "\nplan\n"));
    sb_free(&normalized);
    oca_core_destroy(core);
    size_t length = 0;
    char *unchanged = read_whole_file(path, OCA_SNAPSHOT_MAX_BYTES, &length, err, sizeof(err));
    assert(unchanged && length == encoded.len && !memcmp(unchanged, encoded.buf, length));
    free(unchanged);
    sb_free(&encoded);
    assert(!unlink(path));
}

static void test_core_creation(oca_llm *llm, const char *workspace) {
    char error[256];
    oca_core_options options = {.workspace = workspace, .flags = OCA_CORE_COMMAND};
    assert(!oca_core_create(llm, NULL, error, sizeof(error)));
    assert(strstr(error, "options are required"));
    assert(!oca_core_create(NULL, &options, error, sizeof(error)));
    options.flags = -1;
    assert(!oca_core_create(llm, &options, error, sizeof(error)));
    assert(strstr(error, "invalid core flags"));
    options.flags = OCA_CORE_COMMAND;
    for (creation_failure = FAIL_MUTEX; creation_failure <= FAIL_PIPE_FLAGS; creation_failure++) {
        creation_pipe[0] = creation_pipe[1] = -1;
        assert(!oca_core_create(llm, &options, error, sizeof(error)));
        assert(strstr(error, "cannot initialize the core"));
        for (int i = 0; i < 2; i++) {
            if (creation_pipe[i] < 0) continue;
            errno = 0;
            assert(fcntl(creation_pipe[i], F_GETFD) == -1 && errno == EBADF);
        }
    }
    creation_failure = FAIL_NONE;
    char *owned_workspace = strdup(workspace);
    assert(owned_workspace);
    options.workspace = owned_workspace;
    oca_core *core = oca_core_create(llm, &options, error, sizeof(error));
    assert(core);
    memset(&options, 0, sizeof(options));
    free(owned_workspace);
    oca_core_status status;
    oca_core_get_status(core, &status);
    assert(status.state == OCA_S_IDLE);
    snprintf(core->route_model, sizeof(core->route_model), "kimi-k3");
    core->request_seq = 1;
    for (int interrupted = 0; interrupted < 2; interrupted++) {
        stream_ctx stream = {.c = core, .quiet = 1};
        oca_event event = {
            .kind = OCA_EV_RESPONSE_START, .request_id = 1, .model_id = "kimi-k3",
        };
        assert(!stream_cb(&event, &stream));
        event.kind = OCA_EV_TOOL_CALL_START;
        event.call_id = "pending";
        event.call_name = "read";
        assert(!stream_cb(&event, &stream));
        if (interrupted) {
            event.kind = OCA_EV_ERROR;
            event.category = "cancelled";
            event.message = "Cancelled by user.";
            assert(!stream_cb(&event, &stream));
        } else {
            event.kind = OCA_EV_TOOL_CALL_ARGUMENTS;
            event.call_id = "wrong-id";
            event.arguments = "{}";
            assert(stream_cb(&event, &stream));
        }
        stream_free(&stream);
    }
    oca_core_destroy(core);
}

static void expect_hints(oca_core *core, const char *expected) {
    char err[256];
    assert(!oca_core_submit_prompt(core, "fixture:hints", err, sizeof(err)));
    for (;;) {
        oca_core_event event;
        assert(oca_core_wait_event(core, &event));
        assert(event.kind != OCA_CE_ERROR);
        int done = event.kind == OCA_CE_TURN_DONE;
        if (done) assert(event.text && !strcmp(event.text, expected));
        oca_core_event_release(&event);
        if (done) break;
    }
}

static void test_hints_lifetime(oca_llm *llm, const char *workspace) {
    char err[256];
    oca_core *core = oca_core_create(llm, &(oca_core_options){
        .workspace = workspace, .flags = OCA_CORE_INTERACTIVE, .model = "kimi-k3",
        .reasoning_name = "low"
    }, err, sizeof(err));
    assert(core);
    assert(oca_core_set_hints(NULL, 1, err, sizeof(err)) < 0);
    assert(oca_core_set_hints(core, 2, err, sizeof(err)) < 0);
    assert(!oca_core_set_hints(core, 1, err, sizeof(err)));
    oca_core_status status;
    oca_core_get_status(core, &status);
    assert(!status.conversation_dirty && !status.session_dirty);
    expect_hints(core, "Hints: on. Full instructions: 1.");
    expect_hints(core, "Hints: on. Full instructions: 1.");
    assert(!oca_core_set_hints(core, 0, err, sizeof(err)));
    expect_hints(core, "Hints: off. Full instructions: 1.");
    assert(!oca_core_set_hints(core, 1, err, sizeof(err)));
    expect_hints(core, "Hints: on. Full instructions: 1.");
    assert(!oca_core_save_session(core, err, sizeof(err)));
    discard_kind(core, OCA_CE_SESSION);
    oca_core_get_status(core, &status);
    char id[40];
    snprintf(id, sizeof(id), "%s", status.session_id);
    oca_core_destroy(core);
    core = oca_core_create(llm, &(oca_core_options){
        .workspace = workspace, .flags = OCA_CORE_INTERACTIVE, .resume_id = id
    }, err, sizeof(err));
    assert(core);
    expect_hints(core, "Hints: off. Full instructions: 1.");
    oca_core_destroy(core);
    char path[4096];
    snprintf(path, sizeof(path), "%s/.oca/sessions/%s.oca", workspace, id);
    assert(!unlink(path));
}

static int skill_remove_entry(const char *path, const struct stat *st, int kind, struct FTW *walk) {
    (void)st; (void)walk;
    return kind == FTW_DP ? rmdir(path) : unlink(path);
}

static void skill_write_fixture(const char *path, const char *text) {
    FILE *file = fopen(path, "w");
    assert(file && fputs(text, file) >= 0 && !fclose(file));
}

static void test_skill_files(void) {
    const char *keys[] = {"HOME", "KIMI_API_KEY", "DEEPSEEK_API_KEY"};
    char *saved[3];
    for (size_t i = 0; i < 3; i++) saved[i] = getenv(keys[i]) ? xstrdup(getenv(keys[i])) : NULL;
    char root[] = "/tmp/oca-skills-test-XXXXXX", store[512], file[1024], other[1024], reference[1024], workspace[512], err[256];
    assert(mkdtemp(root));
    assert(!setenv("HOME", root, 1));
    assert(!unsetenv("KIMI_API_KEY") && !unsetenv("DEEPSEEK_API_KEY"));
    snprintf(store, sizeof(store), "%s/.oca", root);
    assert(!mkdir(store, 0700));
    snprintf(store, sizeof(store), "%s/.oca/skills", root);
    assert(!mkdir(store, 0700));
    snprintf(file, sizeof(file), "%s/review", store);
    assert(!mkdir(file, 0700));
    snprintf(file, sizeof(file), "%s/review/references", store);
    assert(!mkdir(file, 0700));
    snprintf(file, sizeof(file), "%s/review/SKILL.md", store);
    snprintf(reference, sizeof(reference), "%s/review/references/checks.md", store);
    skill_write_fixture(file, "---\nname: review\ndescription: Review C changes.\n---\n\nRead [checks](references/checks.md).\n");
    skill_write_fixture(reference, "Check ownership without a final newline");
    snprintf(other, sizeof(other), "%s/another", store);
    assert(!mkdir(other, 0700));
    snprintf(other, sizeof(other), "%s/another/SKILL.md", store);
    skill_write_fixture(other, "---\nname: another\ndescription: Another skill.\n---\n\nDifferent instructions.\n");
    snprintf(workspace, sizeof(workspace), "%s/work", root);
    assert(!mkdir(workspace, 0700));
    oca_llm *llm = fixture_llm_create(NULL, err, sizeof(err));
    assert(llm);
    oca_core *core = oca_core_create(llm, &(oca_core_options){.workspace = workspace, .flags = OCA_CORE_COMMAND}, err, sizeof(err));
    assert(core && oca_core_skill_count(core) == 2);
    sbuf prompt;
    sb_init(&prompt);
    render_system_text(core, &prompt);
    assert(strstr(prompt.buf, file) && strstr(prompt.buf, other));
    assert(strstr(prompt.buf, "Resolve relative references") && !strstr(prompt.buf, "Use the skill tool"));
    assert(!strstr(prompt.buf, "Different instructions."));
    sb_free(&prompt);
    const char *paths[] = {file, other, reference};
    const char *expected[] = {"references/checks.md", "Different instructions.", "Check ownership"};
    for (size_t i = 0; i < 3; i++) {
        cJSON *args = cJSON_CreateObject();
        assert(args && cJSON_AddStringToObject(args, "path", paths[i]));
        tool_outcome out = {0};
        tool_read(workspace, args, &out);
        assert(out.state == OCA_TOOL_COMPLETE && strstr(out.text, expected[i]));
        free(out.text);
        cJSON_Delete(args);
    }
    cJSON *args = cJSON_CreateObject();
    assert(args && cJSON_AddStringToObject(args, "path", reference));
    assert(cJSON_AddStringToObject(args, "content", "Updated checks"));
    tool_outcome out = {0};
    tool_write(workspace, args, &out);
    assert(out.state == OCA_TOOL_COMPLETE);
    free(out.text);
    cJSON_Delete(args);
    args = cJSON_CreateObject();
    assert(args && cJSON_AddStringToObject(args, "path", reference));
    assert(cJSON_AddStringToObject(args, "old_text", "Updated") && cJSON_AddStringToObject(args, "new_text", "Edited"));
    tool_edit(workspace, args, &out);
    assert(out.state == OCA_TOOL_COMPLETE);
    free(out.text);
    cJSON_Delete(args);
    snprintf(other, sizeof(other), "%s/new", store);
    assert(!mkdir(other, 0700));
    snprintf(other, sizeof(other), "%s/new/SKILL.md", store);
    skill_write_fixture(other, "---\nname: new\ndescription: New skill.\n---\n\nNew instructions.\n");
    assert(oca_core_skill_count(core) == 2);
    oca_core_destroy(core);
    core = oca_core_create(llm, &(oca_core_options){.workspace = workspace, .flags = OCA_CORE_COMMAND}, err, sizeof(err));
    assert(core && oca_core_skill_count(core) == 3);
    oca_core_destroy(core);
    oca_llm_destroy(llm);
    assert(!nftw(root, skill_remove_entry, 32, FTW_DEPTH | FTW_PHYS));
    for (size_t i = 0; i < 3; i++) {
        assert(!(saved[i] ? setenv(keys[i], saved[i], 1) : unsetenv(keys[i])));
        free(saved[i]);
    }
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], OCA_TOOL_WORKER_FLAG)) return oca_tool_worker_main();
    test_skill_files();
    test_interaction_handoff();
    test_tool_json();
    test_session_exchange_bounds();
    test_replay_interrupted();
    test_goal_duration();
    test_goal_read();
    assert(!unsetenv("KIMI_API_KEY"));
    char root[] = "/tmp/oca-tui-core-XXXXXX";
    assert(mkdtemp(root));
    assert(!setenv("HOME", root, 1));
    char err[256];
    fixture_server server = fixture_server_start();
    test_startup_reasoning();
    oca_llm *llm = fixture_llm_create(NULL, err, sizeof(err));
    assert(llm);
    oca_tool_executor *executor = oca_tool_executor_create(root, root, err, sizeof(err));
    assert(executor);
    test_core_creation(llm, root);
    test_legacy_mode(llm, root);
    test_hints_lifetime(llm, root);
    fixture_check_atomic_write(write_file_atomic_0600, root, "tmp");
    fixture_check_atomic_write(atomic_write_text, root, "ocatmp");
    oca_trace_sink trace = {&fixture_trace, collect_trace};
    oca_core *core = oca_core_create(llm, &(oca_core_options){
        .workspace = root,
        .flags = OCA_CORE_INTERACTIVE,
        .model = "kimi-k3",
        .reasoning_name = "low",
        .trace = &trace,
        .executor = executor,
    }, err, sizeof(err));
    assert(core);
    fixture_core = core;
    oca_core_status status;
    oca_core_get_status(core, &status);
    assert(!status.conversation_dirty);
    oca_core_get_status(core, &status);
    assert(!status.session_dirty && !status.conversation_dirty);
    assert(!oca_core_new_session(core, err, sizeof(err)));
    discard_kind(core, OCA_CE_SESSION);

    assert(oca_core_bash(core, "", err, sizeof(err)) < 0);
    assert(oca_core_bash(core, " \n\t", err, sizeof(err)) < 0);
    assert(oca_core_bash(core, NULL, err, sizeof(err)) < 0);
    int requests_before_bash = fixture_requests;
    assert(!oca_core_bash(core, "printf manual-unit; exit 7", err, sizeof(err)));
    oca_core_event bash_result = wait_kind(core, OCA_CE_TOOL_RESULT);
    assert(!strcmp(bash_result.name, "bash"));
    assert(strstr(bash_result.text, "manual-unit") && strstr(bash_result.text, "[exit 7]"));
    oca_core_event_release(&bash_result);
    discard_kind(core, OCA_CE_TURN_DONE);
    oca_core_get_status(core, &status);
    assert(!status.conversation_dirty && !status.working);
    assert(fixture_requests == requests_before_bash);
    assert(!oca_core_replay(core, err, sizeof(err)));
    oca_core_event empty_history = wait_kind(core, OCA_CE_TRANSCRIPT);
    assert(!empty_history.entry_count);
    oca_core_event_release(&empty_history);

    oca_core_cancel(core);
    assert(!oca_core_submit_prompt(core, "fixture:hang", err, sizeof(err)));
    oca_core_cancel(core);
    discard_kind(core, OCA_CE_TURN_DONE);
    oca_core_get_status(core, &status);
    assert(status.conversation_dirty && !status.working);
    fixture_enabled = 1;
    fixture_usage = (oca_event){ .has_input = 1, .input_tokens = 2000,
        .has_cache_hit = 1, .cache_hit_tokens = 1600 };
    turn(core, "known counters");
    oca_core_get_status(core, &status);
    assert(status.has_cache && status.has_cache_input);
    assert(status.has_used_tokens && status.used_tokens == 2000);
    assert(status.cache_hit_tokens == 1600 && status.cache_input_tokens == 2000);
    fixture_usage = (oca_event){ .has_cache_hit = 1, .cache_hit_tokens = 2,
        .has_cache_miss = 1, .cache_miss_tokens = 1 };
    turn(core, "derive total");
    oca_core_get_status(core, &status);
    assert(status.has_cache_input && status.cache_input_tokens == 3);
    assert(status.has_used_tokens && status.used_tokens == 3);
    fixture_usage = (oca_event){0};
    turn(core, "missing counters");
    oca_core_get_status(core, &status);
    assert(!status.has_cache && !status.has_cache_input);
    assert(!status.has_used_tokens && status.used_tokens == 0);
    fixture_usage = (oca_event){ .has_cache_hit = 1, .cache_hit_tokens = LONG_MAX,
        .has_cache_miss = 1, .cache_miss_tokens = 1 };
    turn(core, "overflowing total");
    oca_core_get_status(core, &status);
    assert(!status.has_cache_input);
    fixture_error = 1;
    fixture_diagnostic = (oca_llm_diagnostic){
        .provider_event = "response.output_text.delta", .check = "size_limit",
        .field = "response_bytes", .expected = "at most 65536 bytes", .received = "65537 bytes",
        .event_index = 4, .frame_bytes = 65000, .wire_bytes = 66000, .response_bytes = 1000,
        .has_http_status = 1, .http_status = 200, .has_item_index = 1, .item_index = 0,
        .has_limit = 1, .actual = 65537, .limit = 65536,
        .has_transport = 1, .transport = {
            .curl_code = 23, .curl_message = "Failed writing received data",
            .attempt = 2, .body_received = 1, .request_bytes = 4096,
            .uploaded_bytes = 4096, .downloaded_bytes = 66000, .tls_us = -1,
            .first_byte_us = 500, .total_us = 10000, .body_idle_us = 1000,
            .connect_timeout_ms = 15000, .total_timeout_ms = 180000,
            .low_speed_seconds = 45, .low_speed_bytes_per_second = 1
        }
    };
    size_t trace_at = fixture_trace.len;
    turn(core, "failed generation");
    const char *error_line = strstr(fixture_trace.buf + trace_at, "llm {\"event\":\"error\"");
    assert(error_line && strstr(fixture_trace.buf, "\"request_id\":"));
    char *record = xstrndup(strchr(error_line, '{'), strcspn(strchr(error_line, '{'), "\n"));
    assert(record);
    const char *why = NULL;
    cJSON *parsed = json_parse(record, strlen(record), &why);
    assert(parsed);
    const cJSON *detail = cJSON_GetObjectItemCaseSensitive(parsed, "diagnostic");
    assert(detail && !strcmp(json_text(detail, "check"), "size_limit"));
    assert(!strcmp(json_text(detail, "provider_event"), "response.output_text.delta"));
    assert(strstr(record, "\"actual\":65537,\"limit\":65536") && strstr(record, "\"http_status\":200"));
    assert(strstr(record, "\"event_index\":4") && strstr(record, "\"item_index\":0"));
    const cJSON *transport = cJSON_GetObjectItemCaseSensitive(detail, "transport");
    assert(transport && !strcmp(json_text(transport, "curl_message"), "Failed writing received data"));
    assert(strstr(record, "\"attempt\":2,\"curl_code\":23") && strstr(record, "\"tls_us\":null"));
    assert(strstr(record, "\"downloaded_bytes\":66000") && strstr(record, "\"body_received\":true"));
    assert(strstr(record, "\"first_byte_us\":500,\"total_us\":10000,\"body_idle_us\":1000"));
    assert(!strstr(record, "fixture answer") && !strstr(record, "failed generation"));
    cJSON_Delete(parsed);
    free(record);
    oca_core_get_status(core, &status);
    assert(!status.has_cache && !status.has_cache_input && status.conversation_dirty);
    fixture_error = 0;
    fixture_burst = 1;
    assert(!oca_core_submit_prompt(core, "burst", err, sizeof(err)));
    struct timespec lag = { .tv_nsec = 100000000 };
    nanosleep(&lag, NULL);
    size_t received = 0;
    for (;;) {
        oca_core_event event;
        assert(oca_core_wait_event(core, &event));
        if (event.kind == OCA_CE_ASSISTANT_REASON) received += strlen(event.text);
        int done = event.kind == OCA_CE_TURN_DONE;
        oca_core_event_release(&event);
        if (done) break;
    }
    assert(received == 1300 * strlen("row\n"));
    fixture_burst = 0;
    fixture_error = fixture_enabled = 0;

    assert(!oca_core_submit_prompt(core, "fixture:ask", err, sizeof(err)));
    discard_kind(core, OCA_CE_INTERACTION);
    oca_core_get_status(core, &status);
    assert(status.working && status.working_paused);
    uint64_t paused_ms = status.working_ms;
    struct timespec pause = { .tv_nsec = 50000000 };
    nanosleep(&pause, NULL);
    oca_core_get_status(core, &status);
    assert(status.working_ms == paused_ms);
    assert(!oca_core_submit_answers(core,
        "[{\"question\":\"Which color?\",\"answer\":\"red\"},"
        "{\"question\":\"Which size?\",\"answer\":\"small\"}]", err, sizeof(err)));
    discard_kind(core, OCA_CE_TURN_DONE);
    assert(!oca_core_save_session(core, err, sizeof(err)));
    discard_kind(core, OCA_CE_SESSION);
    oca_core_get_status(core, &status);
    assert(!status.conversation_dirty && !status.session_dirty);
    char saved_id[40];
    snprintf(saved_id, sizeof(saved_id), "%s", status.session_id);

    assert(!oca_core_replay(core, err, sizeof(err)));
    oca_core_event replay = wait_kind(core, OCA_CE_TRANSCRIPT);
    int users = 0, questions = 0;
    for (size_t i = 0; i < replay.entry_count; i++) {
        oca_transcript_entry *entry = &replay.entries[i];
        if (entry->kind == OCA_CE_USER_TEXT) users++;
        if (entry->kind == OCA_CE_TOOL_RESULT && !strcmp(entry->name, "ask_user")) {
            assert(strstr(entry->text, "Q: Which color?\nA: red"));
            assert(i && replay.entries[i - 1].kind == OCA_CE_TOOL_START);
            assert(!strcmp(entry->detail, replay.entries[i - 1].detail));
            assert(strstr(replay.entries[i - 1].text, "options"));
            questions++;
        }
    }
    assert(users == 8 && questions == 1);
    oca_core_event_release(&replay);
    oca_core_get_status(core, &status);
    assert(!status.conversation_dirty && !status.working);

    assert(!oca_core_quit(core, err, sizeof(err)));
    discard_kind(core, OCA_CE_QUIT_OK);
    oca_core_destroy(core);
    sb_free(&fixture_trace);
    core = oca_core_create(llm, &(oca_core_options){
        .workspace = root,
        .flags = OCA_CORE_INTERACTIVE,
        .resume_id = saved_id,
        .executor = executor,
    }, err, sizeof(err));
    assert(core);
    fixture_core = core;
    oca_core_get_status(core, &status);
    assert(!status.conversation_dirty);
    turn(core, "unsaved change");

    char sessions[1024], retained[1024];
    snprintf(sessions, sizeof(sessions), "%s/.oca/sessions", root);
    snprintf(retained, sizeof(retained), "%s/.oca/sessions-retained", root);
    assert(!rename(sessions, retained));
    FILE *blocker = fopen(sessions, "w");
    assert(blocker && !fclose(blocker));
    assert(!oca_core_new_session(core, err, sizeof(err)));
    discard_kind(core, OCA_CE_SAVE_ASK);
    assert(!oca_core_save_answer(core, 1, err, sizeof(err)));
    discard_kind(core, OCA_CE_SAVE_ASK_CONT);
    assert(!oca_core_save_answer(core, 0, err, sizeof(err)));
    discard_kind(core, OCA_CE_STATUS);
    oca_core_get_status(core, &status);
    assert(status.conversation_dirty && !strcmp(status.session_id, saved_id));
    assert(!unlink(sessions) && !rename(retained, sessions));
    assert(!oca_core_new_session(core, err, sizeof(err)));
    discard_kind(core, OCA_CE_SAVE_ASK);
    assert(!oca_core_save_answer(core, 0, err, sizeof(err)));
    discard_kind(core, OCA_CE_SESSION);
    oca_core_get_status(core, &status);
    assert(!status.conversation_dirty);
    oca_core_destroy(core);
    oca_tool_executor_destroy(executor);
    oca_llm_destroy(llm);
    fixture_server_stop(&server);
    char snapshot[2048];
    snprintf(snapshot, sizeof(snapshot), "%s/%s.oca", sessions, saved_id);
    assert(!unlink(snapshot));
    assert(!rmdir(sessions));
    snprintf(sessions, sizeof(sessions), "%s/.oca", root);
    assert(!rmdir(sessions) && !rmdir(root));
    puts("test_tui_core: passed");
    return 0;
}
