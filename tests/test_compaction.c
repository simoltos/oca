#include "oca_core.h"
#include "fixture_models.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int fixture_summary_count(void);
void fixture_expect_hints(int enabled);
int fixture_restored_hints(void);

int main(void) {
    char home[] = "/tmp/oca-compaction-test-XXXXXX";
    assert(mkdtemp(home));
    assert(setenv("HOME", home, 1) == 0);
    assert(unsetenv("KIMI_API_KEY") == 0 && unsetenv("DEEPSEEK_API_KEY") == 0);
    char directory[512], path[512], err[256];
    snprintf(directory, sizeof(directory), "%s/.oca", home);
    assert(mkdir(directory, 0700) == 0);
    snprintf(path, sizeof(path), "%s/.oca/auth.json", home);
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fputs("{\"providers\":{\"openai\":{\"auth_method\":\"oauth\","
        "\"access_token\":\"fixture-access\",\"refresh_token\":\"fixture-refresh\","
        "\"account_id\":\"fixture-account\",\"expires_at\":4102444800}}}", file) >= 0);
    assert(fclose(file) == 0);
    oca_llm *llm = fixture_llm_create(path, err, sizeof(err));
    assert(llm);
    oca_core *core = oca_core_create(llm, &(oca_core_options){
        .workspace = home,
        .flags = OCA_CORE_INTERACTIVE,
        .model = "gpt-6-astra",
        .reasoning_name = "low",
    }, err, sizeof(err));
    assert(core);
    fixture_expect_hints(1);
    assert(!oca_core_set_hints(core, 1, err, sizeof(err)));
    char prompt[12001];
    memset(prompt, 'p', sizeof(prompt) - 1);
    prompt[sizeof(prompt) - 1] = 0;
    for (int turn = 0; turn < 12; turn++) {
        if (turn == 10) {
            fixture_expect_hints(0);
            assert(!oca_core_set_hints(core, 0, err, sizeof(err)));
        }
        assert(oca_core_submit_prompt(core, prompt, err, sizeof(err)) == 0);
        int text = 0, error = 0;
        for (;;) {
            oca_core_event event;
            assert(oca_core_wait_event(core, &event));
            if (event.kind == OCA_CE_ASSISTANT_TEXT) text++;
            if (event.kind == OCA_CE_ERROR) error++;
            int done = event.kind == OCA_CE_TURN_DONE;
            oca_core_event_release(&event);
            if (done) break;
        }
        assert(text == 1 && !error);
    }
    assert(fixture_summary_count() > 0);
    assert(fixture_restored_hints() > 0);
    oca_core_status status;
    oca_core_get_status(core, &status);
    assert(status.used_tokens < status.context_limit);
    oca_core_destroy(core);
    oca_llm_destroy(llm);
    assert(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/.oca/sessions", home);
    if (rmdir(path)) assert(errno == ENOENT);
    assert(rmdir(directory) == 0 && rmdir(home) == 0);
    puts("test_compaction: passed");
    return 0;
}
