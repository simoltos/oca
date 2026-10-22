#include "oca_llm.h"

#include "fixture_models.h"
#include "fixture_server.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    int started;
    int done;
    int errors;
    int reasoning;
    int usage;
    char text[1024];
    size_t text_len;
} event_log;

static int collect(const oca_event *event, void *user) {
    event_log *log = user;
    if (event->kind == OCA_EV_RESPONSE_START) log->started++;
    if (event->kind == OCA_EV_RESPONSE_DONE) log->done++;
    if (event->kind == OCA_EV_ERROR) log->errors++;
    if (event->kind == OCA_EV_REASONING_DELTA) log->reasoning++;
    if (event->kind == OCA_EV_USAGE) log->usage++;
    if (event->kind == OCA_EV_TEXT_DELTA && event->text) {
        size_t add = strlen(event->text);
        assert(log->text_len + add < sizeof(log->text));
        memcpy(log->text + log->text_len, event->text, add + 1);
        log->text_len += add;
    }
    return 0;
}

static void test_catalog(oca_llm *llm) {
    assert(oca_llm_model_count(llm) == 6);
    const oca_model *model = oca_llm_model_resolve(llm, "kimi-k3");
    assert(model && !strcmp(model->id, "kimi-k3") && !strcmp(model->name, "kimi-k3"));
    assert(model->context_limit == 262144);
    assert(model->reasoning_count == 3);
    assert(!strcmp(oca_model_reasoning_name(model, 0), "low"));
    assert(!strcmp(oca_model_reasoning_name(model, 2), "max"));
    assert(!oca_model_reasoning_name(model, 3));
    assert(oca_model_reasoning_index(model, "high") == 1);
    assert(oca_model_reasoning_index(model, "off") == OCA_REASONING_NONE);
    assert(oca_model_min_reasoning(model) == 0);
    assert(oca_model_supports_reasoning(model, 1));
    assert(!oca_model_supports_reasoning(model, 3));
    assert(!oca_llm_model_available(llm, model));
    assert(oca_provider_count() == 3);
    assert(!strcmp(oca_provider_id(0), "kimi"));
    assert(!strcmp(oca_provider_id(1), "openai"));
    assert(!strcmp(oca_provider_id(2), "deepseek"));
    assert(!oca_provider_id(3) && !oca_llm_model_at(llm, 6));
    const oca_model *flash = oca_llm_model_resolve(llm, "deepseek-flash");
    assert(flash && !strcmp(flash->id, "deepseek-flash"));
    const char *removed[] = {"smoke-basic", "smoke-reasoning", "smoke", "basic", "sreasoning",
                            "ds4f", "ds4p", "deepseek-v4-flash", "deepseek-v4-pro", "deepseek-v4.1-flash"};
    for (size_t i = 0; i < sizeof(removed) / sizeof(removed[0]); i++)
        assert(!oca_llm_model_resolve(llm, removed[i]));
    assert(!oca_provider_credential_kind("smoke"));
}

static void test_generation(oca_llm *llm) {
    char err[256];
    const oca_model *model = oca_llm_model_resolve(llm, "kimi-k3");
    oca_request *request = oca_request_create(OCA_PURPOSE_TURN, 7);
    assert(request);
    oca_request_set_route(request, model, 1);
    oca_request_push_msg(request, OCA_MSG_SYSTEM, "Test generation.", NULL, NULL);
    oca_request_push_msg(request, OCA_MSG_USER, "hello", NULL, NULL);
    long estimate = 0;
    assert(oca_llm_estimate_input(request, &estimate, err, sizeof(err)) == 0);
    assert(estimate > 0);
    event_log log = {0};
    assert(oca_llm_generate(llm, request, collect, &log, err, sizeof(err)) == 0);
    assert(log.started == 1 && log.done == 1 && log.errors == 0);
    assert(log.usage == 1 && log.reasoning > 0);
    assert(!strcmp(log.text, "Received: hello"));
    oca_request_destroy(request);
}

static void test_rejections(oca_llm *llm) {
    char err[256];
    const oca_model *basic = oca_llm_model_resolve(llm, "kimi-k3");
    oca_request *request = oca_request_create(OCA_PURPOSE_TURN, 8);
    assert(request);
    oca_request_set_route(request, basic, 5);
    oca_request_push_msg(request, OCA_MSG_USER, "test", NULL, NULL);
    event_log log = {0};
    assert(oca_llm_generate(llm, request, collect, &log, err, sizeof(err)) < 0);
    assert(strstr(err, "reasoning"));
    oca_request_destroy(request);
    assert(oca_llm_auth_set_api_key(llm, "smoke", "secret", err, sizeof(err)) < 0);
    assert(strstr(err, "unknown provider"));
    assert(oca_llm_auth_logout(llm, "smoke", err, sizeof(err)) < 0);
}


static void compatibility_write(const char *path, const char *data) {
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fputs(data, file) >= 0);
    assert(fclose(file) == 0);
}

static void test_catalog_compatibility(void) {
    assert(unsetenv("KIMI_API_KEY") == 0);
    char directory[] = "/tmp/oca-auth-compat-XXXXXX";
    assert(mkdtemp(directory));
    char path[512], err[256];
    snprintf(path, sizeof(path), "%s/auth.json", directory);
    const char *mixed =
        "{\"providers\":{\"retired\":{\"auth_method\":\"oauth\","
        "\"access_token\":\"private-test-token\",\"extra\":[true,null,{\"n\":9007199254740993}]},"
        "\"kimi\":{\"auth_method\":\"api-key\",\"api_key\":\"valid-test-key\"}}}";
    compatibility_write(path, mixed);
    oca_llm *llm = fixture_llm_create(path, err, sizeof(err));
    assert(llm && oca_llm_unsupported_provider_count(llm) == 1);
    assert(oca_llm_auth_member_count(llm) == 1);
    assert(oca_llm_model_available(llm, oca_llm_model_resolve(llm, "kimi-k3")));
    assert(oca_llm_auth_set_api_key(llm, "retired", "key", err, sizeof(err)) < 0);
    assert(oca_llm_auth_set_api_key(llm, "kimi", "replacement", err, sizeof(err)) == 0);
    char moved[512];
    snprintf(moved, sizeof(moved), "%s-moved", directory);
    assert(rename(directory, moved) == 0);
    compatibility_write(directory, "occupied");
    assert(oca_llm_auth_logout(llm, "kimi", err, sizeof(err)) < 0);
    assert(oca_llm_auth_set_api_key(llm, "kimi", "failed-update", err, sizeof(err)) < 0);
    assert(oca_llm_auth_member_count(llm) == 1);
    assert(oca_llm_unsupported_provider_count(llm) == 1);
    assert(unlink(directory) == 0);
    assert(rename(moved, directory) == 0);
    assert(oca_llm_auth_logout(llm, "kimi", err, sizeof(err)) == 0);
    oca_llm_destroy(llm);
    llm = fixture_llm_create(path, err, sizeof(err));
    assert(llm && oca_llm_unsupported_provider_count(llm) == 1);
    assert(oca_llm_auth_member_count(llm) == 0);
    oca_llm_destroy(llm);
    char text[2048] = {0};
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(fread(text, 1, sizeof(text) - 1, file) > 0);
    assert(fclose(file) == 0);
    assert(strstr(text, "private-test-token") && strstr(text, "\"extra\":[true,null,{\"n\":9007199254740993}]"));

    const char *invalid[] = {
        "{\"providers\":{\"retired\":{\"number\":01}}}",
        "{\"providers\":{\"retired\":{},\"kimi\":{\"auth_method\":\"api-key\",\"api_key\":\"valid\\u0000bad\"}}}",
        "{\"providers\":{\"retired\":{},\"retired\":{}}}",
        "{\"providers\":{\"retired\":{},\"kimi\":{\"auth_method\":\"api-key\"}}}",
        "{\"providers\":{\"retired\":[}}",
        "{\"providers\":{\"retired\":{}}} trailing",
        "{\"providers\":{},\"providers\":{}}"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        compatibility_write(path, invalid[i]);
        llm = fixture_llm_create(path, err, sizeof(err));
        assert(!llm && strstr(err, "auth file rejected"));
        assert(!strstr(err, "private-test-token"));
    }
    assert(unlink(path) == 0);
    assert(rmdir(directory) == 0);
}

static void test_catalog_ownership(void) {
    char err[256];
    char directory[] = "/tmp/oca-catalog-ownership-XXXXXX";
    assert(mkdtemp(directory));
    char auth[512];
    snprintf(auth, sizeof(auth), "%s/auth.json", directory);
    assert(setenv("KIMI_API_KEY", "fixture-test-key", 1) == 0);
    oca_llm *first = fixture_llm_create(auth, err, sizeof(err));
    oca_llm *second = fixture_llm_create(auth, err, sizeof(err));
    oca_llm *empty = oca_llm_create(auth, NULL, 0, err, sizeof(err));
    assert(first && second && empty);
    const oca_model *borrowed = oca_llm_model_at(first, 0);
    assert(borrowed && oca_llm_model_available(first, borrowed));
    assert(!oca_llm_model_available(second, borrowed));
    assert(!oca_llm_model_available(empty, borrowed));
    assert(!oca_llm_model_available(first, NULL));
    oca_llm_destroy(empty);
    oca_llm_destroy(second);
    oca_llm_destroy(first);

    const char *const invalid_levels[] = {"low", "bad level"};
    const oca_model_definition partial[] = {
        {"valid", "valid-remote", "kimi", 1000, FIXTURE_LEVELS_LOW_HIGH_MAX, 3, 0},
        {"invalid", "invalid-remote", "kimi", 1000, invalid_levels, 2, 0},
    };
    assert(!oca_llm_create(auth, partial, 2, err, sizeof(err)));
    assert(strstr(err, "invalid model definition"));
    oca_model_definition budget = {"budget", "budget", "kimi", 1000, NULL, 0, 800};
    oca_llm *limited = oca_llm_create(auth, &budget, 1, err, sizeof(err));
    assert(limited && oca_llm_model_at(limited, 0)->input_limit == 800);
    oca_llm_destroy(limited);
    budget.input_limit = 0;
    limited = oca_llm_create(auth, &budget, 1, err, sizeof(err));
    assert(limited && oca_llm_model_at(limited, 0)->input_limit == 750);
    oca_llm_destroy(limited);
    budget.input_limit = 1001;
    assert(!oca_llm_create(auth, &budget, 1, err, sizeof(err)));
    budget.input_limit = -1;
    assert(!oca_llm_create(auth, &budget, 1, err, sizeof(err)));
    assert(unsetenv("KIMI_API_KEY") == 0);
    assert(rmdir(directory) == 0);
}

int main(void) {
    test_catalog_compatibility();
    assert(unsetenv("KIMI_API_KEY") == 0);
    assert(unsetenv("DEEPSEEK_API_KEY") == 0);
    test_catalog_ownership();
    char directory[] = "/tmp/oca-llm-test-XXXXXX";
    assert(mkdtemp(directory));
    char auth[512];
    snprintf(auth, sizeof(auth), "%s/auth.json", directory);
    char err[256];
    oca_llm *llm = fixture_llm_create(auth, err, sizeof(err));
    assert(llm);
    test_catalog(llm);
    test_rejections(llm);
    oca_llm_destroy(llm);
    fixture_server server = fixture_server_start();
    llm = fixture_llm_create(auth, err, sizeof(err));
    assert(llm);
    assert(oca_llm_auth_member_count(llm) == 0);
    assert(oca_llm_model_available(llm, oca_llm_model_resolve(llm, "kimi-k3")));
    test_generation(llm);
    oca_llm_destroy(llm);
    fixture_server_stop(&server);
    assert(access(auth, F_OK) != 0);
    assert(rmdir(directory) == 0);
    puts("test_llm: passed");
    return 0;
}
