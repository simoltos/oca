/* Give the browser TUI fixture a private port without adding a production
 * configuration option or reserving the user's real DevTools endpoint. */
#define oca_tool_executor_create fixture_executor_create
#include "../oca_core.c"
#undef oca_tool_executor_create

oca_tool_executor *oca_tool_executor_create(const char *home, const char *workspace,
                                            char *err, size_t errsz) {
    oca_tool_executor *executor = fixture_executor_create(home, workspace, err, errsz);
    const char *port = getenv("OCA_TEST_BROWSER_PORT");
    if (executor && port) {
        char endpoint[96];
        snprintf(endpoint, sizeof(endpoint), "127.0.0.1:%s", port);
        if (!browser_endpoint_port(endpoint)) {
            oca_tool_executor_destroy(executor);
            err_set(err, errsz, "invalid fixture browser port");
            return NULL;
        }
        snprintf(executor->browser_endpoint, sizeof(executor->browser_endpoint), "%s", endpoint);
    }
    return executor;
}
