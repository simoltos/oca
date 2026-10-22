#include "oca_core.h"
#include "fixture_models.h"

#include "fixture_server.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
    char model[64];
    char reasoning[16];
    int calls;
} route_log;

static int persist_route(void *user, const char *model, const char *reasoning,
                         char *err, size_t errsz) {
    (void)err;
    (void)errsz;
    route_log *log = user;
    snprintf(log->model, sizeof(log->model), "%s", model);
    snprintf(log->reasoning, sizeof(log->reasoning), "%s", reasoning);
    log->calls++;
    return 0;
}

static void write_text_file(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(text, 1, strlen(text), file) == strlen(text));
    assert(fclose(file) == 0);
}

static int compactions;

static char *collect_turn(oca_core *core, int cancel_after_text, int *saw_error) {
    char *text = calloc(1, 1);
    size_t len = 0;
    assert(text);
    for (;;) {
        oca_core_event event;
        assert(oca_core_wait_event(core, &event));
        if (event.kind == OCA_CE_ASSISTANT_TEXT && event.text) {
            size_t add = strlen(event.text);
            text = realloc(text, len + add + 1);
            assert(text);
            memcpy(text + len, event.text, add + 1);
            len += add;
            if (cancel_after_text) {
                oca_core_cancel(core);
                cancel_after_text = 0;
            }
        }
        if (event.kind == OCA_CE_ERROR && saw_error) *saw_error = 1;
        if (event.kind == OCA_CE_STATUS && event.state == OCA_S_COMPACTING) compactions++;
        int done = event.kind == OCA_CE_TURN_DONE;
        oca_core_event_release(&event);
        if (done) return text;
    }
}

static void wait_for_session_event(oca_core *core) {
    for (;;) {
        oca_core_event event;
        assert(oca_core_wait_event(core, &event));
        int done = event.kind == OCA_CE_SESSION;
        oca_core_event_release(&event);
        if (done) return;
    }
}

static void answer_interaction(oca_core *core) {
    for (;;) {
        oca_core_event event;
        assert(oca_core_wait_event(core, &event));
        assert(event.kind != OCA_CE_ERROR && event.kind != OCA_CE_TURN_DONE);
        if (event.kind == OCA_CE_INTERACTION) {
            assert(event.question_count == 2);
            char err[256];
            assert(oca_core_submit_answers(core,
                "[{\"question\":\"Which color?\",\"answer\":\"red\"},"
                "{\"question\":\"Which size?\",\"answer\":\"large\"}]",
                err, sizeof(err)) == 0);
            oca_core_event_release(&event);
            return;
        }
        oca_core_event_release(&event);
    }
}

static void wait_for_goal(oca_core *core, oca_goal_state expected) {
    for (;;) {
        oca_core_event event;
        assert(oca_core_wait_event(core, &event));
        int done = event.kind == OCA_CE_GOAL && event.goal == expected;
        oca_core_event_release(&event);
        if (done) return;
    }
}

static void test_queued_turns(oca_core *core, const char *workspace) {
    char err[256], release[1024];
    snprintf(release, sizeof(release), "%s/queue-release", workspace);
    for (int interrupt = 0; interrupt < 2; interrupt++) {
        assert(access(release, F_OK) != 0 && errno == ENOENT);
        assert(!oca_core_submit_prompt(core,
            "fixture:tool bash {\"command\":\"echo queue-tool-ready; "
            "while [ ! -f queue-release ]; do sleep 1; done; echo queue-tool-finished\"}",
            err, sizeof(err)));
        for (;;) {
            oca_core_event event;
            assert(oca_core_wait_event(core, &event));
            assert(event.kind != OCA_CE_ERROR && event.kind != OCA_CE_TURN_DONE);
            int ready = event.kind == OCA_CE_TOOL_OUTPUT && event.text &&
                strstr(event.text, "queue-tool-ready");
            oca_core_event_release(&event);
            if (ready) break;
        }
        assert(!oca_core_submit_prompt(core, "queued-first", err, sizeof(err)));
        assert(!oca_core_submit_prompt(core, "queued-second", err, sizeof(err)));
        oca_core_status status;
        oca_core_get_status(core, &status);
        assert(status.state == OCA_S_RUNNING_TOOL && !status.cancelling);
        assert(status.followup_count == 2);
        if (interrupt) oca_core_send_queued(core);
        else write_text_file(release, "continue\n");

        int turns = 0, results = 0, announced = 0;
        while (turns < 2) {
            oca_core_event event;
            assert(oca_core_wait_event(core, &event));
            assert(event.kind != OCA_CE_ERROR);
            if (event.kind == OCA_CE_USER_TEXT) {
                /* Delivery must follow the original turn's final response,
                 * including the model request that consumes the tool result. */
                assert(turns == 1);
                assert(!strcmp(event.text, announced ? "queued-second" : "queued-first"));
                announced++;
            } else if (event.kind == OCA_CE_TOOL_RESULT) {
                assert(!turns && !announced);
                assert(event.tool_state == (interrupt ? OCA_TOOL_INTERRUPTED : OCA_TOOL_COMPLETE));
                results++;
            } else if (event.kind == OCA_CE_TURN_DONE) {
                if (!turns) {
                    assert(results == 1 && !announced);
                    assert(interrupt || !strcmp(event.text, "All 1 tool calls finished."));
                } else {
                    const char *first = strstr(event.text, "queued-first");
                    const char *second = strstr(event.text, "queued-second");
                    assert(announced == 2 && first && second && first < second);
                }
                turns++;
            }
            oca_core_event_release(&event);
        }
        oca_core_get_status(core, &status);
        assert(!status.followup_count);
        if (!interrupt) assert(!unlink(release));
    }
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], OCA_TOOL_WORKER_FLAG)) return oca_tool_worker_main();
    assert(unsetenv("KIMI_API_KEY") == 0);
    fixture_server server = fixture_server_start();
    assert(unsetenv("DEEPSEEK_API_KEY") == 0);
    char root[] = "/tmp/oca-core-test-XXXXXX";
    assert(mkdtemp(root));
    char home[512], workspace[512], path[1024];
    snprintf(home, sizeof(home), "%s/home", root);
    snprintf(workspace, sizeof(workspace), "%s/work", root);
    assert(mkdir(home, 0700) == 0);
    assert(mkdir(workspace, 0700) == 0);
    snprintf(path, sizeof(path), "%s/.oca", home);
    assert(mkdir(path, 0700) == 0);
    snprintf(path, sizeof(path), "%s/.oca/skills", home);
    assert(mkdir(path, 0700) == 0);
    snprintf(path, sizeof(path), "%s/.oca/skills/good-skill", home);
    assert(mkdir(path, 0700) == 0);
    snprintf(path, sizeof(path), "%s/.oca/skills/good-skill/SKILL.md", home);
    write_text_file(path, "---\nname: good-skill\ndescription: Test one skill.\n---\n\nFollow it.\n");
    snprintf(path, sizeof(path), "%s/.oca/skills/bad--skill", home);
    assert(mkdir(path, 0700) == 0);
    snprintf(path, sizeof(path), "%s/.oca/skills/bad--skill/SKILL.md", home);
    write_text_file(path, "---\nname: bad--skill\ndescription: Invalid name.\n---\n\nNo.\n");
    snprintf(path, sizeof(path), "%s/AGENTS.md", workspace);
    write_text_file(path, "# Test context\n\nUse short answers.\n");
    assert(setenv("HOME", home, 1) == 0);
    char err[256];
    oca_tool_executor *executor = oca_tool_executor_create(home, workspace, err, sizeof(err));
    assert(executor);

    oca_llm *llm = fixture_llm_create(NULL, err, sizeof(err));
    assert(llm);
    route_log log = {0};
    oca_core_route_cbs callbacks = { &log, persist_route };
    oca_core *core = oca_core_create(llm, &(oca_core_options){
        .workspace = workspace,
        .flags = OCA_CORE_RUNTIME,
        .model = "kimi-k3",
        .reasoning_name = "low",
        .executor = executor,
        .route = &callbacks,
    }, err, sizeof(err));
    assert(core);
    assert(oca_core_skill_count(core) == 1);
    assert(oca_core_skill_warning_count(core) == 1);
    assert(!strcmp(oca_core_skill_name(core, 0), "good-skill"));
    assert(log.calls == 1 && !strcmp(log.model, "kimi-k3"));
    oca_core_status status;
    int saw_error = 0;
    oca_core_get_status(core, &status);
    assert(status.has_project_context && status.has_session);
    char *too_large_prompt = malloc(OCA_PROMPT_MAX_BYTES + 2);
    assert(too_large_prompt);
    memset(too_large_prompt, 'p', OCA_PROMPT_MAX_BYTES + 1);
    too_large_prompt[OCA_PROMPT_MAX_BYTES + 1] = '\0';
    assert(oca_core_submit_prompt(core, too_large_prompt, err, sizeof(err)) < 0);
    free(too_large_prompt);

    assert(oca_core_submit_prompt(core, "hello", err, sizeof(err)) == 0);
    char *answer = collect_turn(core, 0, NULL);
    assert(!strcmp(answer, "Received: hello"));
    free(answer);

    char *large = malloc(110001);
    assert(large);
    memset(large, 'x', 110000);
    large[110000] = '\0';
    assert(oca_core_submit_prompt(core, large, err, sizeof(err)) == 0);
    answer = collect_turn(core, 0, NULL);
    free(answer);
    assert(oca_core_submit_prompt(core, large, err, sizeof(err)) == 0);
    answer = collect_turn(core, 0, NULL);
    free(answer);
    assert(oca_core_submit_prompt(core, large, err, sizeof(err)) == 0);
    saw_error = 0;
    answer = collect_turn(core, 0, &saw_error);
    assert(!saw_error && *answer && compactions > 0);
    free(answer);
    free(large);
    oca_core_get_status(core, &status);
    assert(status.used_tokens < status.context_limit);

    assert(chdir(workspace) == 0);
    assert(oca_core_submit_prompt(core,
        "fixture:tool write {\"path\":\"made.txt\",\"content\":\"made\"}",
        err, sizeof(err)) == 0);
    answer = collect_turn(core, 0, NULL);
    free(answer);
    write_text_file(path, "# Test context\n\nChanged after session creation.\n");
    FILE *made = fopen("made.txt", "rb");
    assert(made);
    char made_text[8] = {0};
    assert(fread(made_text, 1, 4, made) == 4);
    fclose(made);
    assert(!strcmp(made_text, "made"));

    assert(oca_core_submit_prompt(core,
        "fixture:tool edit {\"path\":\"made.txt\",\"old_text\":\"made\","
        "\"new_text\":\"edited\"}", err, sizeof(err)) == 0);
    answer = collect_turn(core, 0, NULL);
    free(answer);
    made = fopen("made.txt", "rb");
    assert(made);
    memset(made_text, 0, sizeof(made_text));
    assert(fread(made_text, 1, 6, made) == 6);
    fclose(made);
    assert(!strcmp(made_text, "edited"));

    assert(oca_core_submit_prompt(core,
        "fixture:tool write {\"path\":\"should-not-exist\",\"content\":\"bad\"}\n"
        "fixture:tool read {}", err, sizeof(err)) == 0);
    saw_error = 0;
    answer = collect_turn(core, 0, &saw_error);
    free(answer);
    assert(saw_error);
    assert(access("should-not-exist", F_OK) != 0 && errno == ENOENT);

    assert(oca_core_submit_prompt(core, "fixture:slow", err, sizeof(err)) == 0);
    saw_error = 0;
    answer = collect_turn(core, 1, &saw_error);
    assert(*answer);
    free(answer);
    (void)saw_error;
    oca_core_destroy(core);

    core = oca_core_create(llm, &(oca_core_options){
        .workspace = workspace,
        .flags = OCA_CORE_INTERACTIVE,
        .executor = executor,
        .model = "kimi-k3",
        .reasoning_name = "low",
        .route = &callbacks,
    }, err, sizeof(err));
    assert(core);
    assert(!oca_core_submit_prompt(core, "fixture:long-reasoning", err, sizeof(err)));
    size_t reasoning_bytes = 0;
    for (;;) {
        oca_core_event event;
        assert(oca_core_wait_event(core, &event));
        assert(event.kind != OCA_CE_ERROR);
        if (event.kind == OCA_CE_ASSISTANT_REASON) reasoning_bytes += strlen(event.text);
        int done = event.kind == OCA_CE_TURN_DONE;
        oca_core_event_release(&event);
        if (done) break;
    }
    assert(reasoning_bytes == 40 * 2048 + strlen("Reviewing the fixture request."));
    test_queued_turns(core, workspace);
    char *too_large_goal = malloc(OCA_GOAL_MAX_CHARS + 2);
    assert(too_large_goal);
    memset(too_large_goal, 'g', OCA_GOAL_MAX_CHARS + 1);
    too_large_goal[OCA_GOAL_MAX_CHARS + 1] = '\0';
    assert(oca_core_goal(core, OCA_GOAL_CMD_SET, too_large_goal,
                         err, sizeof(err)) < 0);
    free(too_large_goal);
    for (int send_asap = 0; send_asap < 2; send_asap++) {
        assert(!oca_core_submit_prompt(core, "fixture:hang", err, sizeof(err)));
        for (;;) {
            oca_core_event event;
            assert(oca_core_wait_event(core, &event));
            int active = event.kind == OCA_CE_STATUS && event.state == OCA_S_REQUESTING;
            oca_core_event_release(&event);
            if (active) break;
        }
        assert(!oca_core_submit_prompt(core, "first queued", err, sizeof(err)));
        assert(!oca_core_submit_prompt(core, "second queued", err, sizeof(err)));
        assert(!oca_core_submit_prompt(core, "third queued", err, sizeof(err)));
        char *recalled = NULL;
        assert(!oca_core_pop_followup(core, &recalled, err, sizeof(err)));
        assert(recalled && !strcmp(recalled, "first queued"));
        free(recalled);
        oca_core_get_status(core, &status);
        assert(status.working && !status.cancelling && status.followup_count == 2);
        assert(!strcmp(status.followup_preview, "second queued"));
        if (send_asap) oca_core_send_queued(core);
        else oca_core_cancel(core);
        free(collect_turn(core, 0, NULL));
        answer = collect_turn(core, 0, NULL);
        const char *second = strstr(answer, "second queued"), *third = strstr(answer, "third queued");
        assert(second && third && second < third && !strstr(answer, "first queued"));
        free(answer);
        oca_core_get_status(core, &status);
        assert(!status.followup_count);
    }
    snprintf(path, sizeof(path), "%s/AGENTS.md", workspace);
    write_text_file(path, "# Test context\n\nNew disk context.\n");
    assert(oca_core_submit_answers(core, "[]", err, sizeof(err)) < 0);
    assert(oca_core_submit_prompt(core, "fixture:ask", err, sizeof(err)) == 0);
    answer_interaction(core);
    answer = collect_turn(core, 0, NULL);
    assert(strstr(answer, "tool calls finished"));
    free(answer);

    assert(!oca_core_submit_prompt(core,
        "fixture:tool edit {\"path\":\"made.txt\",\"old_text\":\"edited\","
        "\"new_text\":\"history-marker\"}", err, sizeof(err)));
    saw_error = 0;
    free(collect_turn(core, 0, &saw_error));
    assert(!saw_error);
    assert(!oca_core_submit_prompt(core, "fixture:prompt-contract require-edit-history", err, sizeof(err)));
    saw_error = 0;
    answer = collect_turn(core, 0, &saw_error);
    assert(!saw_error && strstr(answer, "Prompt contract: normal."));
    free(answer);
    const char *fatal[] = {
        "fixture:tool write [}",
        "fixture:tool edit []",
        "fixture:tool write {\"path\":\"invalid-peer.txt\",\"content\":\"blocked\"}\nfixture:tool read {}",
        "fixture:tool edit {}\nfixture:tool bash {"
    };
    for (size_t j = 0; j < sizeof(fatal) / sizeof(fatal[0]); j++) {
        assert(!oca_core_submit_prompt(core, fatal[j], err, sizeof(err)));
        int errors = 0, done = 0;
        while (!done) {
            oca_core_event event;
            assert(oca_core_wait_event(core, &event));
            assert(event.kind != OCA_CE_TOOL_START && event.kind != OCA_CE_TOOL_RESULT);
            if (event.kind == OCA_CE_ERROR) {
                assert(!strcmp(event.name, j == 2 ? "tool_validation_failed" : "adapter_failed"));
                errors++;
            }
            done = event.kind == OCA_CE_TURN_DONE;
            oca_core_event_release(&event);
        }
        assert(errors == 1);
    }

    assert(!oca_core_submit_prompt(core, "fixture:repair-ask", err, sizeof(err)));
    answer_interaction(core);
    saw_error = 0;
    answer = collect_turn(core, 0, &saw_error);
    assert(!saw_error && strstr(answer, "tool calls finished"));
    free(answer);

    const char *repairs[] = {"fixture:repair-bash", "fixture:repair-repeat"};
    for (size_t i = 0; i < sizeof(repairs) / sizeof(repairs[0]); i++) {
        assert(!oca_core_submit_prompt(core, repairs[i], err, sizeof(err)));
        int errors = 0, results = 0, done = 0;
        while (!done) {
            oca_core_event event;
            assert(oca_core_wait_event(core, &event));
            if (event.kind == OCA_CE_TOOL_RESULT) {
                if (results < 2) assert(event.tool_state == OCA_TOOL_ERROR);
                else assert(event.tool_state == OCA_TOOL_COMPLETE && strstr(event.text, "repaired"));
                results++;
            }
            if (event.kind == OCA_CE_ERROR) {
                assert(!strcmp(event.name, "tool_validation_failed"));
                errors++;
            }
            done = event.kind == OCA_CE_TURN_DONE;
            oca_core_event_release(&event);
        }
        assert(errors == (int)i && results == (i ? 2 : 3));
        assert(access("validation-peer.txt", F_OK) && errno == ENOENT);
        assert(access("validation-bash.txt", F_OK) && errno == ENOENT);
    }

    const char *malformed[] = {"fixture:tool bash []", "fixture:tool bash {"};
    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        assert(!oca_core_submit_prompt(core, malformed[i], err, sizeof(err)));
        saw_error = 0;
        answer = collect_turn(core, 0, &saw_error);
        assert(saw_error);
        free(answer);
        assert(!oca_core_submit_prompt(core, "After malformed arguments", err, sizeof(err)));
        saw_error = 0;
        answer = collect_turn(core, 0, &saw_error);
        assert(!saw_error && strstr(answer, "Received:"));
        free(answer);
    }

    assert(oca_core_goal(core, OCA_GOAL_CMD_SET, "fixture:slow",
                         err, sizeof(err)) == 0);
    for (;;) {
        oca_core_event event;
        assert(oca_core_wait_event(core, &event));
        int streamed = event.kind == OCA_CE_ASSISTANT_TEXT;
        oca_core_event_release(&event);
        if (streamed) break;
    }
    assert(oca_core_goal(core, OCA_GOAL_CMD_PAUSE, NULL, err, sizeof(err)) == 0);
    wait_for_goal(core, OCA_GOAL_PAUSED);
    assert(oca_core_goal(core, OCA_GOAL_CMD_SET,
                         "fixture:goal-complete verified", err, sizeof(err)) == 0);
    answer = collect_turn(core, 0, NULL);
    free(answer);
    oca_core_get_status(core, &status);
    assert(status.goal == OCA_GOAL_COMPLETED);
    assert(oca_core_save_session(core, err, sizeof(err)) == 0);
    wait_for_session_event(core);
    oca_core_get_status(core, &status);
    assert(!status.session_dirty);
    char saved_id[40];
    snprintf(saved_id, sizeof(saved_id), "%s", status.session_id);
    oca_core_destroy(core);

    snprintf(path, sizeof(path), "%s/.oca/sessions/%s.oca", home, saved_id);
    FILE *snapshot = fopen(path, "rb");
    assert(snapshot);
    char *snapshot_text = malloc(1024 * 1024);
    assert(snapshot_text);
    size_t snapshot_len = fread(snapshot_text, 1, 1024 * 1024 - 1, snapshot);
    assert(!ferror(snapshot) && fclose(snapshot) == 0);
    snapshot_text[snapshot_len] = '\0';
    assert(strstr(snapshot_text, "Changed after session creation."));
    assert(!strstr(snapshot_text, "New disk context."));
    snprintf(path, sizeof(path), "%s/.oca/sessions", home);
    oca_session_summary *sessions = NULL;
    size_t session_count = 0;
    assert(oca_core_scan_sessions(path, &sessions, &session_count,
                                  err, sizeof(err)) == 0);
    assert(session_count == 1);
    assert(!strcmp(sessions[0].id, saved_id));
    oca_core_free_summaries(sessions, session_count);

    core = oca_core_create(llm, &(oca_core_options){
        .workspace = workspace,
        .flags = OCA_CORE_INTERACTIVE,
        .executor = executor,
        .resume_id = saved_id,
        .route = &callbacks,
    }, err, sizeof(err));
    assert(core);
    oca_core_get_status(core, &status);
    assert(status.goal == OCA_GOAL_COMPLETED);
    assert(status.has_project_context);
    oca_core_destroy(core);

    snprintf(path, sizeof(path), "%s/.oca/sessions/%s.oca", home, saved_id);
    const char *old_route = "model 7\nkimi-k3\nreasoning 3\nlow\n";
    char *route_at = strstr(snapshot_text, old_route);
    assert(route_at);
    size_t prefix = (size_t)(route_at - snapshot_text);
    snapshot = fopen(path, "wb");
    assert(snapshot && fwrite(snapshot_text, 1, prefix, snapshot) == prefix);
    assert(fputs("model 14\ndeepseek-flash\nreasoning 4\nhigh\n", snapshot) >= 0);
    assert(fputs(route_at + strlen(old_route), snapshot) >= 0 && !fclose(snapshot));
    core = oca_core_create(llm, &(oca_core_options){
        .workspace = workspace,
        .flags = OCA_CORE_INTERACTIVE,
        .executor = executor,
        .resume_id = saved_id,
        .route = &callbacks,
    }, err, sizeof(err));
    assert(core);
    oca_core_get_status(core, &status);
    assert(!strcmp(status.model_id, "deepseek-flash") && !strcmp(status.reasoning_name, "high"));
    oca_core_destroy(core);
    snapshot = fopen(path, "wb");
    assert(snapshot);
    assert(fwrite(snapshot_text, 1, prefix, snapshot) == prefix);
    assert(fputs("model 11\nsmoke-basic\nreasoning 3\noff\n", snapshot) >= 0);
    assert(fputs(route_at + strlen(old_route), snapshot) >= 0);
    assert(fclose(snapshot) == 0);
    free(snapshot_text);
    int persisted = log.calls;
    core = oca_core_create(llm, &(oca_core_options){
        .workspace = workspace,
        .flags = OCA_CORE_INTERACTIVE,
        .executor = executor,
        .resume_id = saved_id,
        .route = &callbacks,
    }, err, sizeof(err));
    assert(core && log.calls == persisted);
    oca_core_get_status(core, &status);
    assert(!strcmp(status.model_id, "smoke-basic") && !status.route_available);
    /* An unusable route rejects the prompt before consuming it. */
    oca_core_get_status(core, &status);
    assert(!status.conversation_dirty && !status.followup_count);
    assert(oca_core_submit_prompt(core, "unavailable", err, sizeof(err)) != 0);
    assert(strstr(err, "select a model"));
    oca_core_get_status(core, &status);
    assert(!status.conversation_dirty && !status.followup_count);
    assert(oca_core_set_route(core, "kimi-k3", "low", err, sizeof(err)) == 0);
    /* Route changes are queued actions. Wait until the usable route is applied. */
    for (;;) {
        oca_core_event event;
        assert(oca_core_wait_event(core, &event));
        int applied = event.kind == OCA_CE_ROUTE && event.route_available &&
            !strcmp(event.model_id, "kimi-k3");
        oca_core_event_release(&event);
        if (applied) break;
    }
    oca_core_get_status(core, &status);
    assert(status.route_available);
    assert(oca_core_submit_prompt(core, "recovered", err, sizeof(err)) == 0);
    saw_error = 0;
    answer = collect_turn(core, 0, &saw_error);
    assert(!saw_error && strstr(answer, "recovered"));
    free(answer);
    oca_core_get_status(core, &status);
    assert(status.route_available && !strcmp(status.model_id, "kimi-k3"));
    assert(oca_core_goal(core, OCA_GOAL_CMD_SET, "fixture:goal-blocked",
                         err, sizeof(err)) == 0);
    answer = collect_turn(core, 0, NULL);
    free(answer);
    oca_core_get_status(core, &status);
    assert(status.goal == OCA_GOAL_BLOCKED);
    oca_core_destroy(core);

    core = oca_core_create(llm, &(oca_core_options){
        .workspace = workspace,
        .flags = OCA_CORE_INTERACTIVE,
        .executor = executor,
        .resume_id = saved_id,
        .model = "kimi-k3",
        .reasoning_name = "low",
        .route = &callbacks,
    }, err, sizeof(err));
    assert(core);
    oca_core_get_status(core, &status);
    assert(status.route_available && !strcmp(status.model_id, "kimi-k3"));
    oca_core_destroy(core);

    char *large_context = malloc(OCA_PROJECT_CONTEXT_MAX_BYTES + 2);
    assert(large_context);
    memset(large_context, 'a', OCA_PROJECT_CONTEXT_MAX_BYTES + 1);
    large_context[OCA_PROJECT_CONTEXT_MAX_BYTES + 1] = '\0';
    snprintf(path, sizeof(path), "%s/AGENTS.md", workspace);
    write_text_file(path, large_context);
    free(large_context);
    core = oca_core_create(llm, &(oca_core_options){
        .workspace = workspace,
        .flags = OCA_CORE_RUNTIME,
        .executor = executor,
        .route = &callbacks,
    }, err, sizeof(err));
    assert(!core && strstr(err, "size limit"));
    oca_llm_destroy(llm);

    snprintf(path, sizeof(path), "%s/made.txt", workspace);
    unlink(path);
    snprintf(path, sizeof(path), "%s/AGENTS.md", workspace);
    unlink(path);
    snprintf(path, sizeof(path), "%s/.oca/sessions/%s.oca", home, saved_id);
    unlink(path);
    snprintf(path, sizeof(path), "%s/.oca/sessions", home);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/.oca/skills/good-skill/SKILL.md", home);
    unlink(path);
    snprintf(path, sizeof(path), "%s/.oca/skills/good-skill", home);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/.oca/skills/bad--skill/SKILL.md", home);
    unlink(path);
    snprintf(path, sizeof(path), "%s/.oca/skills/bad--skill", home);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/.oca/skills", home);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/.oca", home);
    rmdir(path);
    assert(rmdir(workspace) == 0);
    assert(rmdir(home) == 0);
    assert(rmdir(root) == 0);
    oca_tool_executor_destroy(executor);
    fixture_server_stop(&server);
    puts("test_core: passed");
    return 0;
}
