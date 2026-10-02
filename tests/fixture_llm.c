/* Use the production adapter with a loopback transport in test binaries.
 * The installed executable contains neither this shim nor the fixture URL. */
#define oca_llm_generate fixture_default_generate
#define oca_llm_builtin_models fixture_default_builtin_models
#include "../oca_llm.c"
#undef oca_llm_generate
#undef oca_llm_builtin_models

#include "fixture_models.h"

const oca_model_definition *oca_llm_builtin_models(size_t *count) {
    if (count) *count = FIXTURE_MODEL_COUNT;
    return FIXTURE_MODELS;
}

int oca_llm_generate(oca_llm *llm, const oca_request *request,
                     oca_event_fn *callback, void *user,
                     char *err, size_t errsz) {
    if (validate_request(request, err, errsz) != 0) return -1;
    if (!callback) {
        err_set(err, errsz, "generation requires an event callback");
        return -1;
    }
    const char *url = getenv("OCA_TEST_PROVIDER_URL");
    if (!url || strncmp(url, "http://127.0.0.1:", 17)) {
        err_set(err, errsz, "test generation requires a loopback fixture");
        return -1;
    }
    return chat_generate(llm, request, callback, user, url, err, errsz);
}
