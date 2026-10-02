/* OCA LLM boundary. */
#ifndef OCA_LLM_H
#define OCA_LLM_H

#include <stddef.h>
#include <stdint.h>

/* ==========================================================================
 * Reasoning levels
 * ============================================================================
 * A model's reasoning levels are configured, ordered lowest to highest.
 * oca_reasoning is an index into that list. OCA_REASONING_NONE marks a
 * model with no configurable reasoning.
 * ========================================================================== */

typedef int oca_reasoning;
#define OCA_REASONING_NONE (-1)
#define OCA_REASONING_NAME_MAX 15

/* ==========================================================================
 * Model catalog
 * ============================================================================
 * The LLM owns a dynamic immutable catalog copied from startup definitions.
 * Records and their strings live until oca_llm_destroy. Callers borrow them
 * without modifying or freeing them. Requests use models from this catalog.
 * ========================================================================== */

/* Bounds opaque provider continuation state in requests and events. */
#define OCA_LLM_STATE_MAX_BYTES (1024u * 1024u)
/* Shared bound for visible assistant text and provider-exposed reasoning. */
#define OCA_LLM_RESPONSE_MAX_BYTES (8u * 1024u * 1024u)

/* Bounds one configured catalog record. */
#define OCA_LLM_MODEL_NAME_MAX 63
#define OCA_LLM_MODEL_ID_MAX 255
#define OCA_LLM_REASONING_LEVELS_MAX 8
#define OCA_LLM_MODELS_MAX 256
#define OCA_LLM_CONTEXT_LIMIT_MAX 100000000L

typedef enum {
    OCA_CREDENTIAL_NONE,
    OCA_CREDENTIAL_API_KEY,
    OCA_CREDENTIAL_OAUTH,
} oca_credential_kind;

typedef struct oca_model {
    const char *id;            /* configured remote model identifier */
    const char *name;          /* configured local name */
    const char *provider;      /* fixed provider registry id */
    long context_limit;       /* OCA context cap for this provider route */
    long input_limit;         /* local input budget, including request overhead */
    const char **reasoning_levels; /* owned, ordered lowest to highest */
    size_t reasoning_count;
    int tools;
    int reasoning_stream;
    int usage;
    int cache_metrics;
    oca_credential_kind credential_kind;
} oca_model;

/* Returns one borrowed configured level name, or NULL outside the list. */
const char *oca_model_reasoning_name(const oca_model *model, oca_reasoning effort);
/* Returns the configured level index for name, or OCA_REASONING_NONE. */
oca_reasoning oca_model_reasoning_index(const oca_model *model, const char *name);
/* Formats an invalid level and its supported alternatives into caller storage. */
void oca_model_reasoning_error(const oca_model *model, const char *name,
                               char *error, size_t size);
int oca_model_supports_reasoning(const oca_model *model, oca_reasoning effort);
/* Returns the lowest configured level, or OCA_REASONING_NONE. */
oca_reasoning oca_model_min_reasoning(const oca_model *model);

/* One startup model definition. All strings are borrowed for the call and
 * copied into the catalog. reasoning_levels is ordered lowest to highest. */
typedef struct {
    const char *name;
    const char *id;
    const char *provider;
    long context_limit;
    const char *const *reasoning_levels;
    size_t reasoning_count;
    long input_limit;         /* zero derives the existing bounded output reserve */
} oca_model_definition;

/* Borrows the compiled model catalog. Records remain valid for the process lifetime. */
const oca_model_definition *oca_llm_builtin_models(size_t *count);

typedef struct oca_llm oca_llm;

/* Creates the layer from the supplied immutable catalog and loads the auth
 * store. A NULL auth_path selects $HOME/.oca/auth.json. A missing file means
 * an empty store. A malformed file fails creation and writes err. KIMI_API_KEY
 * and DEEPSEEK_API_KEY are copied as optional process-only fallbacks. Stored
 * credentials take precedence. The caller owns the result. Destroy borrowing
 * cores and finish all generations before calling oca_llm_destroy. */
/* Unsupported stored providers remain inactive and survive credential writes.
 * The caller reports their count before interactive terminal ownership begins. */
oca_llm *oca_llm_create(const char *auth_path, const oca_model_definition *definitions,
                        size_t definition_count, char *err, size_t errsz);
void oca_llm_destroy(oca_llm *llm);

/* Catalog queries allocate nothing. Indexed queries return NULL outside the catalog. */
size_t oca_llm_model_count(const oca_llm *llm);
const oca_model *oca_llm_model_at(const oca_llm *llm, size_t index);

/* Resolves one configured local name or remote identifier. Returns NULL when
 * unknown. The result belongs to llm and remains valid until destruction. */
const oca_model *oca_llm_model_resolve(const oca_llm *llm, const char *name_or_id);

/* A model is available when its credential passes local validation.
 * This query is thread-safe during worker-owned OAuth refresh. */
int oca_llm_model_available(const oca_llm *llm, const oca_model *model);

/* Fixed provider registry. Model count does not change these results. */
size_t oca_provider_count(void);
const char *oca_provider_id(size_t index);

/* Returns a borrowed static method name, or NULL for an unknown provider. */
const char *oca_provider_credential_kind(const char *provider);

/* ==========================================================================
 * Credentials
 * ============================================================================
 * Call credential operations when generation is idle.
 * String arguments are borrowed during the call. Setters copy stored credentials.
 * Set and logout write the complete auth file atomically before return.
 * Failed writes preserve the previous in-memory credentials.
 * ========================================================================== */

int oca_llm_auth_set_api_key(oca_llm *llm, const char *provider, const char *api_key,
                             char *err, size_t errsz);
int oca_llm_auth_logout(oca_llm *llm, const char *provider, char *err, size_t errsz);

/* Returns a borrowed static credential method, or NULL when unavailable.
 * Includes supported environment fallbacks without creating store members. */
const char *oca_llm_auth_method(const oca_llm *llm, const char *provider);
/* Returns OAuth expiration in Unix seconds, or zero without OAuth credentials. */
int64_t oca_llm_auth_expiry(const oca_llm *llm, const char *provider);
size_t oca_llm_auth_member_count(const oca_llm *llm);

/* Returns the inactive stored-provider count. Borrows llm; allocates nothing.
 * Unsupported records survive credential writes and never authorize requests. */
size_t oca_llm_unsupported_provider_count(const oca_llm *llm);
/* Callbacks run synchronously on the login caller thread. The URL is borrowed
 * only during open_browser. A nonzero callback result cancels login. */
typedef struct {
    void *user;
    int (*open_browser)(void *user, const char *url);
    int (*cancelled)(void *user);
} oca_auth_ui;

/* Runs one native browser login and atomically stores its credentials.
 * Borrows ui until return. Failure preserves the previous credential record.
 * The caller owns terminal presentation; this function never writes to it. */
int oca_llm_auth_login(oca_llm *llm, const char *provider, const oca_auth_ui *ui,
                       char *err, size_t errsz);

/* ==========================================================================
 * Normalized requests
 * ============================================================================
 * String and record pointers passed to builders are borrowed and must stay
 * valid until oca_request_destroy. The adapter never retains them.
 * ========================================================================== */

typedef enum {
    OCA_MSG_SYSTEM = 0,
    OCA_MSG_USER,
    OCA_MSG_ASSISTANT,
    OCA_MSG_TOOL
} oca_msg_role;

typedef enum {
    OCA_PURPOSE_TURN = 0,
    OCA_PURPOSE_GOAL_CONT,
    OCA_PURPOSE_COMPACTION
} oca_purpose;

typedef struct oca_request oca_request;

/* Returns an owned request. Release it with oca_request_destroy. */
oca_request *oca_request_create(oca_purpose purpose, uint64_t request_id);
void oca_request_destroy(oca_request *request);

void oca_request_set_route(oca_request *request, const oca_model *model,
                           oca_reasoning effort);
/* Borrows a session cache key until request destruction. NULL omits it. */
void oca_request_set_cache_key(oca_request *request, const char *key);
void oca_request_set_cancel(oca_request *request, int (*poll)(void *), void *user);

/* Adds one tool definition for the current mode. */
void oca_request_add_tool(oca_request *request, const char *name,
                          const char *description, const char *args_json);

/* Appends one message. The call attach function extends the last assistant
 * message. Reasoning and tool_call_id apply only to matching roles. */
void oca_request_push_msg(oca_request *request, oca_msg_role role,
                          const char *text, const char *reasoning,
                          const char *tool_call_id);
/* Attaches one opaque model state block to the last assistant message.
 * Only the owning model can read it in a later request. */
void oca_request_msg_set_state(oca_request *request, const char *state);
void oca_request_msg_add_call(oca_request *request, const char *call_id,
                              const char *name, const char *args_json);

/* ==========================================================================
 * Stream events
 * ============================================================================
 * Event strings point into adapter storage that stays valid only during the
 * callback. The callback returns nonzero to request cancellation.
 *
 * One stream uses this order on the caller thread:
 *   response_start
 *   (reasoning_delta | text_delta | tool_call_start tool_call_arguments*
 *    tool_call_end | model_state | usage)*
 *   response_done | error
 *
 * Rules:
 *   - response_start is first. An opening failure can emit error alone.
 *   - Exactly one terminal event ends the stream. No event follows it.
 *   - reasoning_delta appears only when the model has a reasoning stream.
 *   - usage and model_state each occur at most once.
 *   - tool_call_end requires its earlier tool_call_start and the adapter
 *     combines fragmented arguments before it.
 *   - A tool_calls finish requires at least one complete tool call.
 *   - The adapter fails a stream that ends without a terminal event.
 * ========================================================================== */

typedef enum {
    OCA_EV_RESPONSE_START = 0,
    OCA_EV_REASONING_DELTA,
    OCA_EV_TEXT_DELTA,
    OCA_EV_TOOL_CALL_START,
    OCA_EV_TOOL_CALL_ARGUMENTS,
    OCA_EV_TOOL_CALL_END,
    OCA_EV_MODEL_STATE,
    OCA_EV_USAGE,
    OCA_EV_RESPONSE_DONE,
    OCA_EV_ERROR
} oca_event_kind;

/* Error diagnostics contain bounded structural metadata, never response content
 * or credentials. Strings are NUL-terminated; long metadata uses a trailing ~.
 * Event numbers start at one. An incomplete frame identifies the next event.
 * Zero means no stream frame was inspected.
 * Size counters use bytes. Optional fields require their corresponding flags. */
#define OCA_LLM_DIAGNOSTIC_TEXT_MAX 128

/* Transport measurements describe one attempt. Negative measurements are
 * unavailable. Timing values use microseconds from the attempt start, except
 * body_idle_us, which measures time since the last body callback.
 * curl_message contains library text, never a raw transport error buffer. */
typedef struct {
    int curl_code;
    char curl_message[OCA_LLM_DIAGNOSTIC_TEXT_MAX];
    unsigned attempt;
    int body_received;
    size_t request_bytes;
    int64_t os_errno, uploaded_bytes, downloaded_bytes;
    int64_t dns_us, connect_us, tls_us, pretransfer_us, first_byte_us, total_us, body_idle_us;
    long connect_timeout_ms, total_timeout_ms, low_speed_seconds, low_speed_bytes_per_second;
} oca_llm_transport_diagnostic;

typedef struct {
    char provider_event[OCA_LLM_DIAGNOSTIC_TEXT_MAX];
    char check[OCA_LLM_DIAGNOSTIC_TEXT_MAX];
    char field[OCA_LLM_DIAGNOSTIC_TEXT_MAX];
    char expected[OCA_LLM_DIAGNOSTIC_TEXT_MAX];
    char received[OCA_LLM_DIAGNOSTIC_TEXT_MAX];
    size_t event_index, frame_bytes, wire_bytes, response_bytes;
    int has_http_status, has_item_index, has_limit;
    long http_status, item_index;
    size_t actual, limit;
    int has_transport;
    oca_llm_transport_diagnostic transport;
} oca_llm_diagnostic;

typedef struct {
    oca_event_kind kind;
    uint64_t request_id;
    const char *model_id;   /* response_start, response_done */
    const char *text;       /* reasoning_delta, text_delta */
    const char *call_id;    /* tool call events */
    const char *call_name;  /* tool_call_start */
    const char *arguments;  /* tool_call_arguments, tool_call_end */
    const char *state;      /* model_state */
    const char *finish;     /* response_done: "stop" or "tool_calls" */
    const char *category;   /* error: safe failure category */
    const char *message;    /* error: safe message */
    /* Optional error metadata. Borrowed only until the callback returns. */
    const oca_llm_diagnostic *diagnostic;
    long call_index;
    /* Usage counters. A flag of zero means the counter is unknown. */
    int has_input, has_output, has_reasoning_tokens, has_total;
    int has_cache_hit, has_cache_miss;
    long input_tokens, output_tokens, reasoning_tokens, total_tokens;
    long cache_hit_tokens, cache_miss_tokens;
} oca_event;

typedef int oca_event_fn(const oca_event *event, void *user);

/* Estimates input tokens for one complete request. It is synchronous and
 * has no provider side effect. Borrows request and writes the caller's tokens
 * value on success. Returns zero on success. */
int oca_llm_estimate_input(const oca_request *request, long *tokens, char *err, size_t errsz);

/* Runs one generation on the caller thread. Returns zero when the stream
 * terminated. A negative return rejects the request before generation and
 * writes err. The request keeps borrowed data valid until return.
 * Concurrent generations may share this layer. Credential refresh and header
 * snapshots are serialized; response streams remain independent. Administrative
 * credential operations still require all generations to be idle. */
int oca_llm_generate(oca_llm *llm, const oca_request *request,
                     oca_event_fn *callback, void *user,
                     char *err, size_t errsz);

#endif
