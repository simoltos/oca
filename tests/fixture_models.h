#ifndef OCA_TEST_FIXTURE_MODELS_H
#define OCA_TEST_FIXTURE_MODELS_H

#include "../oca_llm.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Standard offline test catalog. Local names match the old identifiers so
 * tests can keep using readable names; the wire ids are the same strings. */
static const char *const FIXTURE_LEVELS_LOW_HIGH_MAX[] = {"low", "high", "max"};
static const char *const FIXTURE_LEVELS_FOUR[] = {"low", "medium", "high", "max"};
static const char *const FIXTURE_LEVELS_DEEPSEEK[] = {"off", "low", "high", "max"};

static const oca_model_definition FIXTURE_MODELS[] = {
    {"kimi-k3", "kimi-k3", "kimi", 262144, FIXTURE_LEVELS_LOW_HIGH_MAX, 3, 0},
    {"gpt-6-astra", "gpt-6-astra", "openai", 872000, FIXTURE_LEVELS_FOUR, 4, 0},
    {"gpt-5.6-sol", "gpt-5.6-sol", "openai", 872000, FIXTURE_LEVELS_FOUR, 4, 0},
    {"gpt-5.6-terra", "gpt-5.6-terra", "openai", 872000, FIXTURE_LEVELS_FOUR, 4, 0},
    {"gpt-5.6-luna", "gpt-5.6-luna", "openai", 872000, FIXTURE_LEVELS_FOUR, 4, 0},
    {"deepseek-flash", "deepseek-flash", "deepseek", 1000000, FIXTURE_LEVELS_DEEPSEEK, 4, 0},
};
#define FIXTURE_MODEL_COUNT (sizeof(FIXTURE_MODELS) / sizeof(FIXTURE_MODELS[0]))

/* Creates the test LLM with the standard catalog. The caller owns it. */
static inline oca_llm *fixture_llm_create(const char *auth_path, char *err, size_t errsz) {
    return oca_llm_create(auth_path, FIXTURE_MODELS, FIXTURE_MODEL_COUNT, err, errsz);
}

/* Creates isolated state for spawned binaries. The fixture binary supplies
 * its catalog at link time. */
static inline void fixture_write_config_impl(const char *home, int saved_selection) {
    char directory[4096], path[8192];
    snprintf(directory, sizeof(directory), "%s/.oca", home);
    if (mkdir(directory, 0700) && errno != EEXIST) abort();
    if (saved_selection) {
        snprintf(path, sizeof(path), "%s/config.json", directory);
        FILE *file = fopen(path, "wb");
        assert(file && fputs("{\"last_used\":{\"model\":\"kimi-k3\",\"reasoning\":\"low\"}}", file) >= 0);
        assert(fclose(file) == 0);
    }
}

static inline void fixture_write_config(const char *home) {
    fixture_write_config_impl(home, 0);
}

static inline void fixture_write_config_saved(const char *home) {
    fixture_write_config_impl(home, 1);
}

#endif
