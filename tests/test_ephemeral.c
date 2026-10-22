#define oca_llm_generate fixture_real_generate
#include "../oca_llm.c"
#undef oca_llm_generate
#include "../oca_core.h"
#include "fixture_models.h"
#include "fixture_server.h"

#include <assert.h>
#include <stdatomic.h>

static atomic_int requests;
static atomic_int side_requests;
static atomic_int compactions;
static atomic_int trace_count;
static atomic_int checked_snapshot;
static atomic_int checked_pending;

static int request_contains(const oca_request *request, const char *text) {
    for (size_t i = 0; i < request->msg_count; i++)
        if (request->msgs[i].text && strstr(request->msgs[i].text, text)) return 1;
    return 0;
}

int oca_llm_generate(oca_llm *llm, const oca_request *request,
                     oca_event_fn *callback, void *user, char *err, size_t errsz) {
    atomic_fetch_add(&requests, 1);
    if (request->purpose == OCA_PURPOSE_COMPACTION) atomic_fetch_add(&compactions, 1);
    if (!request->tool_count && request->purpose != OCA_PURPOSE_COMPACTION) {
        atomic_fetch_add(&side_requests, 1);
        assert(request->purpose == OCA_PURPOSE_TURN);
        assert(request->msg_count > 1);
        assert(request_contains(request, "temporary text conversation"));
        const req_msg *last = &request->msgs[request->msg_count - 1];
        if (last->text && !strcmp(last->text, "inspect-pending-question")) {
            int question_seen = 0;
            for (size_t i = 0; i < request->call_count; i++)
                if (!strcmp(request->calls[i].name, "ask_user") &&
                    strstr(request->calls[i].args, "Which color?")) question_seen = 1;
            assert(question_seen);
            assert(request_contains(request, "This main-thread call is pending."));
            atomic_store(&checked_pending, 1);
        }
        if (last->text && !strcmp(last->text, "side-second")) {
            assert(request_contains(request, "main-seed"));
            assert(request_contains(request, "side-private-first"));
            assert(!request_contains(request, "main-after-fork"));
            atomic_store(&checked_snapshot, 1);
        }
    }
    return chat_generate(llm, request, callback, user, getenv("OCA_TEST_PROVIDER_URL"), err, errsz);
}

static int trace_write(void *user, const char *line) {
    (void)user;
    assert(!strstr(line, "side-private"));
    atomic_fetch_add(&trace_count, 1);
    return 0;
}

static uint64_t test_ms(void) {
    struct timespec value;
    assert(!clock_gettime(CLOCK_MONOTONIC, &value));
    return (uint64_t)value.tv_sec * 1000 + (uint64_t)value.tv_nsec / 1000000;
}

static oca_core_event wait_kind(oca_core *core, oca_core_event_kind kind) {
    uint64_t deadline = test_ms() + 8000;
    while (test_ms() < deadline) {
        oca_core_event event;
        while (oca_core_poll_event(core, &event)) {
            if (event.kind == kind) return event;
            assert(event.kind != OCA_CE_SAVE_ASK);
            oca_core_event_release(&event);
        }
        struct timespec pause = { .tv_nsec = 1000000 };
        nanosleep(&pause, NULL);
    }
    assert(!"event deadline");
    return (oca_core_event){0};
}

static void discard_kind(oca_core *core, oca_core_event_kind kind) {
    oca_core_event event = wait_kind(core, kind);
    oca_core_event_release(&event);
}

static void wait_state(oca_core *core, oca_core_state state) {
    static int invocation;
    int current = ++invocation;
    uint64_t deadline = test_ms() + 8000;
    while (test_ms() < deadline) {
        oca_core_status status;
        oca_core_get_status(core, &status);
        if (status.state == state) return;
        oca_core_event event;
        while (oca_core_poll_event(core, &event)) {
            if (event.kind == OCA_CE_ERROR)
                fprintf(stderr, "state wait error: %s\n", event.text ? event.text : "unknown");
            oca_core_event_release(&event);
        }
        struct timespec pause = { .tv_nsec = 1000000 };
        nanosleep(&pause, NULL);
    }
    oca_core_status status;
    oca_core_get_status(core, &status);
    fprintf(stderr, "state wait %d: wanted %d, got %d\n", current, state, status.state);
    assert(!"state deadline");
}

static void turn(oca_core *core, const char *prompt) {
    char err[256];
    assert(!oca_core_submit_prompt(core, prompt, err, sizeof(err)));
    for (;;) {
        oca_core_event event;
        assert(oca_core_wait_event(core, &event));
        if (event.kind == OCA_CE_ERROR)
            fprintf(stderr, "turn error: %s\n", event.text ? event.text : "unknown");
        assert(event.kind != OCA_CE_ERROR);
        int done = event.kind == OCA_CE_TURN_DONE;
        oca_core_event_release(&event);
        if (done) break;
    }
}

int main(void) {
    assert(!unsetenv("KIMI_API_KEY"));
    char root[] = "/tmp/oca-ephemeral-XXXXXX";
    assert(mkdtemp(root));
    assert(!setenv("HOME", root, 1));
    char err[256];
    fixture_server server = fixture_server_start();
    assert(!setenv("DEEPSEEK_API_KEY", "fixture-side-key", 1));
    oca_llm *llm = fixture_llm_create(NULL, err, sizeof(err));
    assert(llm);
    oca_trace_sink trace = { NULL, trace_write };
    oca_core *main_core = oca_core_create(llm, &(oca_core_options){
        .workspace = root,
        .flags = OCA_CORE_INTERACTIVE,
        .model = "kimi-k3",
        .reasoning_name = "low",
        .trace = &trace,
    }, err, sizeof(err));
    assert(main_core);
    turn(main_core, "main-seed");
    assert(!oca_core_goal(main_core, OCA_GOAL_CMD_SET, "fixture:hang", err, sizeof(err)));
    wait_state(main_core, OCA_S_STREAMING);
    int before = atomic_load(&requests);
    uint64_t start = test_ms();
    oca_core *side = oca_core_create_ephemeral(main_core, err, sizeof(err));
    assert(side && test_ms() - start < 1000);
    assert(atomic_load(&requests) == before);
    assert(!oca_core_create_ephemeral(main_core, err, sizeof(err)));
    assert(!oca_core_create_ephemeral(side, err, sizeof(err)));
    turn(side, "side-private-first");
    oca_core_status main_status;
    oca_core_get_status(main_core, &main_status);
    assert(main_status.state == OCA_S_STREAMING && main_status.goal == OCA_GOAL_ACTIVE);
    oca_core_cancel(main_core);
    discard_kind(main_core, OCA_CE_TURN_DONE);
    turn(main_core, "main-after-fork");
    before = atomic_load(&trace_count);
    turn(side, "side-second");
    assert(atomic_load(&checked_snapshot));
    assert(atomic_load(&trace_count) == before);
    assert(!oca_core_replay(side, err, sizeof(err)));
    oca_core_event replay = wait_kind(side, OCA_CE_TRANSCRIPT);
    int side_users = 0;
    for (size_t i = 0; i < replay.entry_count; i++) {
        const oca_transcript_entry *entry = &replay.entries[i];
        if (entry->text) assert(!strstr(entry->text, "main-seed"));
        if (entry->kind == OCA_CE_USER_TEXT) side_users++;
    }
    assert(side_users == 2);
    oca_core_event_release(&replay);

    char forbidden[512], prompt[1024];
    snprintf(forbidden, sizeof(forbidden), "%s/forbidden", root);
    snprintf(prompt, sizeof(prompt),
        "fixture:tool write {\"path\":\"%s\",\"content\":\"bad\"}", forbidden);
    assert(!oca_core_submit_prompt(side, prompt, err, sizeof(err)));
    oca_core_event failure = wait_kind(side, OCA_CE_ERROR);
    assert(failure.text && *failure.text);
    oca_core_event_release(&failure);
    discard_kind(side, OCA_CE_TURN_DONE);
    assert(access(forbidden, F_OK) != 0);
    assert(oca_core_save_session(side, err, sizeof(err)) < 0);
    assert(oca_core_new_session(side, err, sizeof(err)) < 0);
    assert(oca_core_goal(side, OCA_GOAL_CMD_SET, "bad", err, sizeof(err)) < 0);
    assert(oca_core_bash(side, "touch forbidden", err, sizeof(err)) < 0);
    assert(!oca_core_set_route(side, "deepseek-flash", "low", err, sizeof(err)));
    discard_kind(side, OCA_CE_ROUTE);
    oca_core_get_status(main_core, &main_status);
    assert(!strcmp(main_status.model_id, "kimi-k3"));

    assert(!oca_core_set_route(side, "kimi-k3", "low", err, sizeof(err)));
    discard_kind(side, OCA_CE_ROUTE);
    char *large = malloc(24001);
    assert(large);
    memset(large, 'x', 24000);
    large[24000] = 0;
    for (int i = 0; i < 16; i++) turn(side, large);
    assert(atomic_load(&compactions) > 0);
    free(large);
    assert(!oca_core_save_session(main_core, err, sizeof(err)));
    discard_kind(main_core, OCA_CE_SESSION);
    oca_core_get_status(main_core, &main_status);
    char path[1024];
    snprintf(path, sizeof(path), "%s/.oca/sessions/%s.oca", root, main_status.session_id);
    FILE *saved = fopen(path, "r");
    assert(saved);
    char content[16384];
    size_t length = fread(content, 1, sizeof(content) - 1, saved);
    assert(feof(saved) && !ferror(saved));
    content[length] = 0;
    fclose(saved);
    assert(!strstr(content, "side-private") && !strstr(content, "side-second"));

    assert(!oca_core_goal(main_core, OCA_GOAL_CMD_SET, "fixture:hang", err, sizeof(err)));
    wait_state(main_core, OCA_S_STREAMING);
    assert(!oca_core_submit_prompt(side, "fixture:hang", err, sizeof(err)));
    wait_state(side, OCA_S_STREAMING);
    oca_core_stop(side);
    wait_state(side, OCA_S_STOPPED);
    oca_core_destroy(side);
    oca_core_get_status(main_core, &main_status);
    assert(main_status.state == OCA_S_STREAMING && main_status.goal == OCA_GOAL_ACTIVE);
    side = oca_core_create_ephemeral(main_core, err, sizeof(err));
    assert(side);
    assert(!oca_core_replay(side, err, sizeof(err)));
    replay = wait_kind(side, OCA_CE_TRANSCRIPT);
    assert(!replay.entry_count);
    oca_core_event_release(&replay);
    char *budget_prompt = malloc(OCA_PROMPT_MAX_BYTES + 1);
    oca_core_cancel(main_core);
    discard_kind(main_core, OCA_CE_TURN_DONE);
    assert(budget_prompt);
    memset(budget_prompt, 'b', OCA_PROMPT_MAX_BYTES);
    budget_prompt[OCA_PROMPT_MAX_BYTES] = 0;
    size_t accepted = 0;
    while (!oca_core_submit_prompt(side, budget_prompt, err, sizeof(err))) {
        accepted++;
        discard_kind(side, OCA_CE_TURN_DONE);
    }
    assert(strstr(err, "history limit"));
    assert(accepted == OCA_EPHEMERAL_HISTORY_MAX_BYTES /
           (OCA_PROMPT_MAX_BYTES + OCA_TOOL_RESULT_MAX_BYTES));
    free(budget_prompt);
    oca_core_stop(side);
    oca_core_destroy(side);
    assert(!oca_core_goal(main_core, OCA_GOAL_CMD_CLEAR, NULL, err, sizeof(err)));
    discard_kind(main_core, OCA_CE_GOAL);
    assert(!oca_core_submit_prompt(main_core, "fixture:ask", err, sizeof(err)));
    discard_kind(main_core, OCA_CE_INTERACTION);
    side = oca_core_create_ephemeral(main_core, err, sizeof(err));
    assert(side);
    turn(side, "inspect-pending-question");
    assert(atomic_load(&checked_pending));
    oca_core_destroy(side);
    assert(!oca_core_submit_answers(main_core,
        "[{\"question\":\"Which color?\",\"answer\":\"red\"},"
        "{\"question\":\"Which size?\",\"answer\":\"small\"}]", err, sizeof(err)));
    discard_kind(main_core, OCA_CE_TURN_DONE);
    assert(!oca_core_replay(main_core, err, sizeof(err)));
    replay = wait_kind(main_core, OCA_CE_TRANSCRIPT);
    for (size_t i = 0; i < replay.entry_count; i++)
        if (replay.entries[i].text)
            assert(!strstr(replay.entries[i].text, "outside this frozen snapshot"));
    oca_core_event_release(&replay);
    oca_core_stop(main_core);
    oca_core_destroy(main_core);
    oca_llm_destroy(llm);
    fixture_server_stop(&server);
    assert(!unsetenv("DEEPSEEK_API_KEY"));
    assert(!unlink(path));
    snprintf(path, sizeof(path), "%s/.oca/sessions", root);
    assert(!rmdir(path));
    snprintf(path, sizeof(path), "%s/.oca", root);
    assert(!rmdir(path) && !rmdir(root));
    puts("test_ephemeral: passed");
    return 0;
}
