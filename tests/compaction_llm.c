/* This fixture preserves the production catalog and request estimator. */
#define oca_llm_generate fixture_network_generate
#include "../oca_llm.c"
#undef oca_llm_generate

#include <assert.h>

static atomic_int summaries;
static atomic_int hints_expected;
static atomic_int restored_hints;

void fixture_expect_hints(int enabled) {
    atomic_store(&hints_expected, enabled);
}

int fixture_restored_hints(void) {
    return atomic_load(&restored_hints);
}

int fixture_summary_count(void) {
    return atomic_load(&summaries);
}

int oca_llm_generate(oca_llm *llm, const oca_request *request,
                     oca_event_fn *callback, void *user, char *err, size_t errsz) {
    (void)llm;
    if (validate_request(request, err, errsz)) return -1;
    assert(callback && !strcmp(request->model->provider, "openai"));
    int summary = request->purpose == OCA_PURPOSE_COMPACTION;
    if (summary) atomic_fetch_add(&summaries, 1);
    else {
        int enabled = 0, full = 0;
        for (size_t i = 0; i < request->msg_count; i++) {
            const req_msg *message = &request->msgs[i];
            if (message->role != OCA_MSG_SYSTEM) continue;
            if (strstr(message->text, "The user enabled programming hints.")) enabled = 1;
            if (strstr(message->text, "Programming hints are disabled.")) enabled = 0;
            if (strstr(message->text, "Occasionally explain a useful concept")) full++;
        }
        assert(enabled == atomic_load(&hints_expected));
        if (enabled) {
            assert(full > 0);
            if (atomic_load(&summaries)) atomic_fetch_add(&restored_hints, 1);
        }
    }
    oca_event event = {.request_id = request->request_id, .model_id = request->model->id,
                       .kind = OCA_EV_RESPONSE_START};
    assert(!callback(&event, user));
    event.kind = OCA_EV_TEXT_DELTA;
    event.text = summary ? "Earlier turns completed. Continue the work." : "OK";
    assert(!callback(&event, user));
    if (!summary) {
        char state[128 * 1024 + 256];
        int prefix = snprintf(state, sizeof(state),
            "[{\"type\":\"reasoning\",\"id\":\"fixture-%llu\",\"encrypted_content\":\"",
            (unsigned long long)request->request_id);
        assert(prefix > 0 && prefix < 256);
        memset(state + prefix, 'a', 128 * 1024);
        strcpy(state + prefix + 128 * 1024, "\",\"summary\":[]}]");
        event.kind = OCA_EV_MODEL_STATE;
        event.state = state;
        assert(!callback(&event, user));
    }
    event.kind = OCA_EV_RESPONSE_DONE;
    event.finish = "stop";
    assert(!callback(&event, user));
    return 0;
}
