#define main oca_entry_main
#include "../oca.c"
#undef main

#include <assert.h>

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    assert(file && fwrite(text, 1, strlen(text), file) == strlen(text));
    assert(fclose(file) == 0);
}

static char *read_text(const char *path) {
    FILE *file = fopen(path, "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    assert(length >= 0 && fseek(file, 0, SEEK_SET) == 0);
    char *text = malloc((size_t)length + 1);
    assert(text && fread(text, 1, (size_t)length, file) == (size_t)length);
    text[length] = 0;
    assert(fclose(file) == 0);
    return text;
}

static void check_directory_lock(const char *home) {
    char err[256], directory[512];
    assert(snprintf(directory, sizeof(directory), "%s/.oca", home) < (int)sizeof(directory));
    int held = config_lock(err, sizeof(err)), ready[2];
    assert(held >= 0 && pipe(ready) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(held);
        close(ready[0]);
        int probe = open(directory, O_RDONLY | O_DIRECTORY);
        assert(probe >= 0 && flock(probe, LOCK_EX | LOCK_NB) < 0 && errno == EWOULDBLOCK);
        close(probe);
        assert(write(ready[1], "x", 1) == 1);
        close(ready[1]);
        int acquired = config_lock(err, sizeof(err));
        assert(acquired >= 0);
        close(acquired);
        _exit(0);
    }
    close(ready[1]);
    char byte;
    assert(read(ready[0], &byte, 1) == 1);
    close(ready[0]);
    close(held);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void check_config(const char *path, const char *home) {
    char err[256];
    config_store cfg;
    assert(load_config(&cfg, err, sizeof(err)) == 0);
    assert(!cfg.has_route && !cfg.model_aliases);
    assert(cfg.display.show_reasoning && cfg.display.show_write && cfg.display.show_edit &&
           cfg.display.show_bash && !cfg.display.show_read);
    free_config(&cfg);
    assert(access(path, F_OK) < 0 && errno == ENOENT);
    assert(save_config(NULL, "k3", "high", err, sizeof(err)) == 0);
    check_directory_lock(home);
    struct stat before, after;
    assert(stat(path, &before) == 0 && (before.st_mode & 0777) == 0600);
    assert(save_config(NULL, "k3", "high", err, sizeof(err)) == 0);
    assert(stat(path, &after) == 0 && before.st_ino == after.st_ino);
    write_text(path, "{\"last_used\":{\"model\":\"k3\",\"reasoning\":\"low\"},"
               "\"show\":{\"reasoning\":true,\"bash\":true}}");
    assert(load_config(&cfg, err, sizeof(err)) == 0);
    assert(cfg.has_route && !strcmp(cfg.model, "k3") && !strcmp(cfg.reasoning, "low"));
    assert(cfg.display.show_reasoning && cfg.display.show_bash && cfg.display.show_write && cfg.display.show_edit && !cfg.display.show_read);
    free_config(&cfg);
    assert(save_config(NULL, "deepseek-flash", "high", err, sizeof(err)) == 0);
    char *text = read_text(path);
    assert(strstr(text, "deepseek-flash") && strstr(text, "show") && strstr(text, "reasoning"));
    free(text);
    const char *invalid[] = {
        "{\"models\":{}}", "{\"last_used\":{}}", "{\"show\":1}",
        "{\"show\":{\"bash\":1}}", "{\"unknown\":true}",
        "{\"show\":{\"reasoning\":true,\"reasoning\":false}}",
        "{\"show\":{\"skill\":false}}", "{\"show_reasoning\":true}",
        "{\"tool_output\":{}}", "{\"show\":{\"unknown\":true}}"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        write_text(path, invalid[i]);
        assert(load_config(&cfg, err, sizeof(err)) != 0);
        assert(save_config(NULL, "k3", "low", err, sizeof(err)) != 0);
        text = read_text(path);
        assert(!strcmp(text, invalid[i]));
        free(text);
    }
    write_text(path, "{\"show\":{\"reasoning\":false,\"read\":true,\"bash\":false}}");
    assert(!load_config(&cfg, err, sizeof(err)));
    assert(!cfg.display.show_reasoning && cfg.display.show_read && !cfg.display.show_bash);
    assert(cfg.display.show_write && cfg.display.show_edit);
    free_config(&cfg);
    assert(!save_config(NULL, "k3", "high", err, sizeof(err)));
    assert(!load_config(&cfg, err, sizeof(err)));
    assert(!cfg.display.show_reasoning && cfg.display.show_read && !cfg.display.show_bash);
    free_config(&cfg);
    write_text(path, "{\"show\":{\"reasoning\":true}}");
}

static void check_saved_routes(const char *path) {
    char err[256];
    config_store cfg;
    const char *const levels[] = {"low", "high"};
    oca_model_definition definitions[] = {
        {"short", "k3", "kimi", 262144, levels, 2, 0},
        {"plain", "plain", "kimi", 128000, NULL, 0, 0}
    };
    oca_llm *llm = oca_llm_create(NULL, definitions, 2, err, sizeof(err));
    assert(llm);

    /* A null effort remains null; a model with levels still needs one. */
    assert(save_config(NULL, "k3", NULL, err, sizeof(err)) == 0);
    assert(load_config(&cfg, err, sizeof(err)) == 0 && cfg.has_route && !cfg.reasoning[0]);
    assert(validate_saved_config(llm, &cfg, err, sizeof(err)) != 0);
    free_config(&cfg);

    const struct { const char *model, *reasoning; int valid; } routes[] = {
        {"short", "high", 1}, {"k3", "bogus", 0}, {"retired", "high", 0},
        {"plain", NULL, 1}, {"plain", "low", 0}
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        cJSON *document = cJSON_CreateObject();
        cJSON *route = cJSON_AddObjectToObject(document, "last_used");
        assert(route && cJSON_AddStringToObject(route, "model", routes[i].model));
        if (routes[i].reasoning)
            assert(cJSON_AddStringToObject(route, "reasoning", routes[i].reasoning));
        else assert(cJSON_AddNullToObject(route, "reasoning"));
        char *text = cJSON_PrintUnformatted(document);
        assert(text);
        write_text(path, text);
        cJSON_free(text);
        cJSON_Delete(document);
        assert(load_config(&cfg, err, sizeof(err)) == 0);
        assert((validate_saved_config(llm, &cfg, err, sizeof(err)) == 0) == routes[i].valid);
        free_config(&cfg);
    }
    oca_llm_destroy(llm);

    /* A failed atomic replacement preserves the complete previous bytes. */
    const char *original = "{\"last_used\":{\"model\":\"k3\",\"reasoning\":\"low\"},"
                           "\"show\":{\"reasoning\":true,\"bash\":true}}";
    write_text(path, original);
    char blocked[1100];
    assert(snprintf(blocked, sizeof(blocked), "%s.tmp.%ld", path, (long)getpid()) < (int)sizeof(blocked));
    assert(mkdir(blocked, 0700) == 0);
    assert(save_config(NULL, "plain", NULL, err, sizeof(err)) != 0);
    char *after = read_text(path);
    assert(!strcmp(after, original));
    free(after);
    assert(rmdir(blocked) == 0);
    write_text(path, "{\"show\":{\"reasoning\":true}}");
}

static void check_aliases(const char *config, const char *models) {
    char err[256];
    config_store cfg;
    write_text(config, "{\"model_aliases\":{\"gpt-6-astra\":\"astra\","
                       "\"gpt-6.1-sol\":\"sol61\",\"gpt-6.1-terra\":\"terra61\",\"k3.1-pro\":\"kimi31\",\"k3\":\"mykimi\"},"
                       "\"show\":{\"reasoning\":true}}");
    assert(!load_config(&cfg, err, sizeof(err)));
    assert(cfg.model_aliases && cfg.display.show_reasoning);
    size_t count = 0;
    const oca_model_definition *builtins = oca_llm_builtin_models(&count);
    assert(count == 13);
    oca_model_definition *definitions = calloc(count, sizeof(*definitions));
    assert(definitions);
    for (size_t i = 0; i < count; i++) {
        definitions[i] = builtins[i];
        if (!strcmp(builtins[i].provider, "openai")) {
            assert(builtins[i].reasoning_count == 5);
            assert(!strcmp(builtins[i].reasoning_levels[0], "low"));
            assert(!strcmp(builtins[i].reasoning_levels[2], "high"));
            assert(!strcmp(builtins[i].reasoning_levels[4], "max"));
        }
        const cJSON *alias = cJSON_GetObjectItemCaseSensitive(cfg.model_aliases, builtins[i].id);
        if (alias) definitions[i].name = alias->valuestring;
    }
    oca_llm *llm = oca_llm_create(NULL, definitions, count, err, sizeof(err));
    assert(llm && !strcmp(oca_llm_model_resolve(llm, "astra")->id, "gpt-6-astra"));
    const oca_model *sol = oca_llm_model_resolve(llm, "sol61");
    assert(sol && sol == oca_llm_model_resolve(llm, "gpt-6.1-sol"));
    assert(!strcmp(sol->provider, "openai") && sol->credential_kind == OCA_CREDENTIAL_OAUTH);
    assert(sol->context_limit == 872000 && sol->input_limit == 828400);
    assert(oca_llm_model_resolve(llm, "gpt-6-astra") &&
           !strcmp(oca_llm_model_resolve(llm, "mykimi")->id, "k3"));
    const oca_model *terra = oca_llm_model_resolve(llm, "terra61");
    const oca_model *kimi = oca_llm_model_resolve(llm, "kimi31");
    assert(terra && terra == oca_llm_model_resolve(llm, "gpt-6.1-terra"));
    assert(!strcmp(terra->provider, "openai") && terra->credential_kind == OCA_CREDENTIAL_OAUTH);
    assert(terra->context_limit == 872000 && terra->input_limit == 828400);
    assert(kimi && kimi == oca_llm_model_resolve(llm, "k3.1-pro"));
    assert(!strcmp(kimi->provider, "kimi") && kimi->credential_kind == OCA_CREDENTIAL_API_KEY);
    assert(kimi->context_limit == 262144 && kimi->input_limit == 245760);
    assert(oca_model_reasoning_index(kimi, "off") == OCA_REASONING_NONE);
    assert(oca_llm_model_resolve(llm, "deepseek-flash")->context_limit == 1048576);
    oca_llm_destroy(llm);
    free(definitions);
    free_config(&cfg);
    assert(!save_config(NULL, "gpt-6-astra", "high", err, sizeof(err)));
    char *saved = read_text(config);
    assert(strstr(saved, "\"model_aliases\"") && strstr(saved, "\"astra\"") &&
           strstr(saved, "\"show\""));
    free(saved);

    const char *invalid[] = {
        "{\"model_aliases\":null}",
        "{\"model_aliases\":{\"k3\":1}}",
        "{\"model_aliases\":{\"k3\":\"bad alias\"}}",
        "{\"model_aliases\":{\"k3\":\"a\",\"k3\":\"b\"}}",
        "{\"model_aliases\":{\"k3\":\"a\",\"gpt-6-astra\":\"a\"}}",
        "{\"model_aliases\":{\"gpt-6-astra\":\"k3\"}}",
        "{\"model_aliases\":{\"gpt-6-astra\":\"gpt-6.1-sol\"}}"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        write_text(config, invalid[i]);
        assert(load_config(&cfg, err, sizeof(err)) != 0);
        assert(save_config(NULL, "k3", "high", err, sizeof(err)) != 0);
        saved = read_text(config);
        assert(!strcmp(saved, invalid[i]));
        free(saved);
    }

    const char *legacy = "{\"gpt-6-astra\":{\"alias\":\"astra\",\"provider\":\"openai\","
                         "\"context_limit\":872000,\"reasoning_levels\":[\"high\"]},"
                         "\"k3\":{\"alias\":\"mykimi\"},"
                         "\"personal-only\":{\"provider\":\"kimi\"}}";
    write_text(models, legacy);
    write_text(config, "{\"show\":{\"reasoning\":true}}");
    assert(!load_config(&cfg, err, sizeof(err)));
    assert(!migrate_model_aliases(&cfg, err, sizeof(err)));
    assert(cfg.model_aliases && cfg.display.show_reasoning);
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(cfg.model_aliases, "gpt-6-astra")->valuestring, "astra"));
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(cfg.model_aliases, "k3")->valuestring, "mykimi"));
    struct stat before, after;
    assert(!stat(config, &before));
    assert(!migrate_model_aliases(&cfg, err, sizeof(err)));
    assert(!stat(config, &after) && before.st_ino == after.st_ino);
    free_config(&cfg);
    saved = read_text(models);
    assert(!strcmp(saved, legacy));
    free(saved);

    write_text(config, "{\"model_aliases\":{\"gpt-6-astra\":\"chosen\"}}");
    assert(!load_config(&cfg, err, sizeof(err)));
    assert(!migrate_model_aliases(&cfg, err, sizeof(err)));
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(cfg.model_aliases, "gpt-6-astra")->valuestring, "chosen"));
    free_config(&cfg);

    write_text(models, "{broken");
    write_text(config, "{\"show\":{\"reasoning\":true}}");
    assert(!load_config(&cfg, err, sizeof(err)));
    assert(migrate_model_aliases(&cfg, err, sizeof(err)) != 0);
    saved = read_text(config);
    assert(!strcmp(saved, "{\"show\":{\"reasoning\":true}}"));
    free(saved);
    free_config(&cfg);

    write_text(models, legacy);
    write_text(config, "{\"show\":{\"reasoning\":true}}");
    char blocked[1100];
    assert(snprintf(blocked, sizeof(blocked), "%s.tmp.%ld", config, (long)getpid()) < (int)sizeof(blocked));
    assert(!mkdir(blocked, 0700));
    assert(!load_config(&cfg, err, sizeof(err)));
    assert(migrate_model_aliases(&cfg, err, sizeof(err)) != 0);
    saved = read_text(config);
    assert(!strcmp(saved, "{\"show\":{\"reasoning\":true}}"));
    free(saved);
    free_config(&cfg);
    assert(!rmdir(blocked));
    assert(!unlink(models));
}

int main(void) {
    char home[] = "/tmp/oca-config-test-XXXXXX", directory[512], config[1024], models[1024];
    assert(mkdtemp(home) && setenv("HOME", home, 1) == 0);
    snprintf(directory, sizeof(directory), "%s/.oca", home);
    snprintf(config, sizeof(config), "%s/config.json", directory);
    snprintf(models, sizeof(models), "%s/models.json", directory);
    check_config(config, home);
    check_saved_routes(config);
    check_aliases(config, models);
    puts("test_config: passed");
    return 0;
}
