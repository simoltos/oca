/* OCA agent core. */
#ifndef OCA_CORE_H
#define OCA_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "oca_llm.h"

/* ==========================================================================
 * Public resource bounds
 * ============================================================================
 * The TUI enforces the prompt limit for editing convenience. The core
 * validates every limit again when it accepts an action.
 * ========================================================================== */

#define OCA_PROMPT_MAX_BYTES           (256 * 1024)
#define OCA_PROJECT_CONTEXT_MAX_BYTES  (64 * 1024)
#define OCA_FOLLOWUP_MAX               64
#define OCA_ACTION_QUEUE_MAX           256
#define OCA_EVENT_QUEUE_MAX            1024
#define OCA_TOOL_CALLS_MAX             32
#define OCA_TOOL_RESULT_MAX_BYTES      (64 * 1024)
#define OCA_TOOL_OUTPUT_MAX_BYTES      (32 * 1024 * 1024)
#define OCA_READ_OUTPUT_MAX_BYTES      (64 * 1024)
#define OCA_SKILL_NAME_MAX             64
#define OCA_SKILL_DESC_MAX_BYTES       1024
#define OCA_SKILL_FILE_MAX_BYTES       OCA_READ_OUTPUT_MAX_BYTES
#define OCA_GOAL_MAX_CHARS             4000
#define OCA_MESSAGES_MAX               65536
#define OCA_SNAPSHOT_MAX_BYTES         (64 * 1024 * 1024)
#define OCA_MODEL_STATE_MAX_BYTES      OCA_LLM_STATE_MAX_BYTES
#define OCA_INTERACTION_MAX_QUESTIONS  3
#define OCA_BASH_TIMEOUT_SECONDS       120
#define OCA_BASH_GRACE_SECONDS         5
#define OCA_EPHEMERAL_HISTORY_MAX_BYTES (8 * 1024 * 1024)

/* ==========================================================================
 * States
 * ========================================================================== */

typedef enum {
    OCA_S_IDLE = 0,
    OCA_S_REQUESTING,
    OCA_S_STREAMING,
    OCA_S_RUNNING_TOOL,
    OCA_S_WAITING_USER,
    OCA_S_COMPACTING,
    OCA_S_CANCELLING,
    OCA_S_ERROR,
    OCA_S_STOPPED
} oca_core_state;

typedef enum {
    OCA_GOAL_NONE = 0,
    OCA_GOAL_ACTIVE,
    OCA_GOAL_PAUSED,
    OCA_GOAL_COMPLETED,
    OCA_GOAL_BLOCKED
} oca_goal_state;

/* Goal commands. */
enum {
    OCA_GOAL_CMD_SHOW = 0,
    OCA_GOAL_CMD_SET,
    OCA_GOAL_CMD_EDIT,
    OCA_GOAL_CMD_PAUSE,
    OCA_GOAL_CMD_RESUME,
    OCA_GOAL_CMD_CLEAR
};

/* Core runtime flags for creation. */
enum {
    OCA_CORE_COMMAND = 0,      /* Process command: no worker, no session. */
    OCA_CORE_RUNTIME = 1,      /* One worker and one live session. */
    OCA_CORE_INTERACTIVE = 3  /* Runtime with user interaction support. */
};

typedef struct oca_core oca_core;

/* ============================================================================
 * Tool executor
 * ============================================================================
 * OCA runs every tool call in a fresh copy of this executable. Workers
 * inherit the environment and ordinary filesystem and network access. They
 * keep bounded output, cancellation, process-group cleanup, and independent
 * process state. One executor serves one runtime core.
 * Browser workers attach to Firefox DevTools. The dedicated browser and its
 * profile persist across executor destruction and OCA invocations.
 * ========================================================================== */

#define OCA_TOOL_PATH_MAX 4096
#define OCA_TOOL_WORKER_FLAG "--oca-internal-tool-worker"

typedef struct oca_tool_executor oca_tool_executor;

/* Copies home and workspace and verifies startup with a probe worker. The
 * caller owns the executor. Home and workspace must name existing
 * directories. Failure never falls back to in-process execution. */
oca_tool_executor *oca_tool_executor_create(const char *home, const char *workspace,
    char *err, size_t errsz);

/* Internal executable entry. Consumes a private request on descriptor 3.
 * Writes only to that channel. The process must exit after this returns. */
int oca_tool_worker_main(void);

/* Stop the executor after destroying every core that borrows it. */
void oca_tool_executor_destroy(oca_tool_executor *executor);

/* ==========================================================================
 * Creation and lifecycle
 * ============================================================================
 * The entry point owns process lifecycle and the config store. It supplies
 * a route persistence callback. The core publishes a route only after
 * persistence succeeds.
 * ========================================================================== */

typedef struct {
    void *user;
    /* Persists last_used route data. Returns zero on success. */
    int (*persist)(void *user, const char *model_id, const char *reasoning_name,
                   char *err, size_t errsz);
} oca_core_route_cbs;

/* Trace sink contract: write one complete record line. Return zero on
 * success. A failure stops the invocation at the next safe boundary. */
typedef struct {
    void *user;
    int (*write_record)(void *user, const char *line);
} oca_trace_sink;

/* Inputs are borrowed for the creation call. The core copies strings and
 * callback records. Callback user data, llm, and executor must outlive the core. */
typedef struct {
    const char *workspace;
    int flags;
    const char *resume_id;
    const char *model; /* explicit ID or alias; NULL restores saved selection */
    /* Overrides the saved level, or the round(N / 2) default of an explicit model. */
    const char *reasoning_name;
    const char *last_used_model;
    const char *last_used_reasoning;
    const oca_core_route_cbs *route;
    const oca_trace_sink *trace;
    oca_tool_executor *executor;
} oca_core_options;

/* Returns an owned core or NULL, with an error in the caller's buffer.
 * The options and llm are required. Workspace must name an existing directory.
 * RUNTIME flags create one worker and one session. COMMAND only discovers skills.
 * A NULL model selects the resumed route or saved preference; with no usable
 * selection the core starts without a route.
 * A NULL route skips preference persistence. An executor can serve only one core.
 * Runtime cores without an executor reject filesystem, Bash, and browser tools.
 * Session selection rebinds the executor to the saved workspace.
 * Destroy any ephemeral child first. Polled events retain their own data until release. */
oca_core *oca_core_create(oca_llm *llm, const oca_core_options *options,
                          char *err, size_t errsz);

/* Creates the main core's single ephemeral text conversation from its latest
 * stable conversation boundary. The copy excludes an unfinished response.
 * Pending tool calls receive snapshot-only descriptions of the missing result.
 * No model request or session write occurs. The result has its own worker,
 * context, route, event queue, and cancellation state. The caller owns it.
 * Destroy the result before main_core and its borrowed LLM layer. A second
 * live child or an ephemeral parent is rejected. */
oca_core *oca_core_create_ephemeral(oca_core *main_core, char *err, size_t errsz);

/* Requests worker shutdown without a save handshake. Safe from the owning
 * consumer thread. Poll status for STOPPED before nonblocking destruction. */
void oca_core_stop(oca_core *core);

/* Stops the worker and releases all owned state. */
void oca_core_destroy(oca_core *core);

/* ==========================================================================
 * Status snapshot
 * ============================================================================
 * The snapshot is a consistent copy for rendering. It never borrows core
 * memory.
 * ========================================================================== */

typedef struct {
    oca_core_state state;
    oca_goal_state goal;
    char model_id[OCA_LLM_MODEL_ID_MAX + 1];
    char reasoning_name[16];
    int route_available;
    int model_supports_reasoning;
    int has_used_tokens;        /* latest completed request reported input usage */
    long used_tokens;           /* provider input count when has_used_tokens */
    long context_limit;
    int has_cache;
    long cache_hit_tokens;
    int has_cache_input;       /* total input for the same completed request */
    long cache_input_tokens;
    int working;               /* one accepted activity, including user waits */
    int working_paused;
    int cancelling;
    uint64_t working_ms;        /* monotonic elapsed time, excluding user waits */
    uint64_t goal_elapsed_ms;   /* active goal time, excluding paused/blocked states */
    int conversation_dirty;    /* unsaved prompts, responses, or tool results */
    size_t followup_count;
    char followup_preview[1024];
    int session_dirty;
    int has_session;
    char session_id[40];
    char session_title[96];
    int has_project_context;
    size_t skill_count;
} oca_core_status;

void oca_core_get_status(const oca_core *core, oca_core_status *out);

/* Catalog view for the TUI. The catalog is immutable after creation and
 * these calls never touch an adapter. */
size_t oca_core_model_count(const oca_core *core);
const oca_model *oca_core_model_at(const oca_core *core, size_t index);
int oca_core_model_available(const oca_core *core, const oca_model *model);

/* ==========================================================================
 * Actions
 * ============================================================================
 * Action functions copy their string arguments. The caller keeps ownership
 * of every input. Each function returns zero on success and writes a
 * stable error for an invalid input or a full queue.
 * ========================================================================== */

/* During work, keeps input queued until the active turn ends. Tool results
 * continue that turn; cancellation ends it and allows queued input to run. */
int oca_core_submit_prompt(oca_core *core, const char *text, char *err, size_t errsz);

/* Sets the cancellation latch. Safe from any thread while work runs. */
void oca_core_cancel(oca_core *core);

/* Removes the oldest queued prompt immediately. The caller frees *text; an
 * empty queue returns NULL. This never interrupts the current activity. */
int oca_core_pop_followup(oca_core *core, char **text, char *err, size_t errsz);

/* Interrupts active work only while messages remain queued for delivery. */
void oca_core_send_queued(oca_core *core);

/* Sets a runtime preference for this interactive or ephemeral conversation.
 * Applies at the next model-request boundary. No request or save is started.
 * New and resumed sessions start disabled; an ephemeral child inherits once. */
int oca_core_set_hints(oca_core *core, int enabled, char *err, size_t errsz);
int oca_core_hints_enabled(oca_core *core);
/* Selects a model by local name or remote id. reasoning_name NULL keeps the
 * current level when it is valid for the new model, otherwise the selection
 * fails and the caller must request a level. */
int oca_core_set_route(oca_core *core, const char *model_id_or_alias,
                       const char *reasoning_name, char *err, size_t errsz);

/* answers_json: one array of {"question","answer"} objects in order. */
int oca_core_submit_answers(oca_core *core, const char *answers_json,
                            char *err, size_t errsz);

/* Answer the pending visible-browser startup prompt (0=no, 1=yes). */
int oca_core_browser_answer(oca_core *core, int allow, char *err, size_t errsz);

/* SHOW immediately queues a snapshot event without entering model context or
 * waiting for the worker. Mutating goal commands run through the action queue. */
int oca_core_goal(oca_core *core, int goal_cmd, const char *objective,
                  char *err, size_t errsz);

int oca_core_new_session(oca_core *core, char *err, size_t errsz);
/* Queues an atomic save in a main conversation runtime, including execution
 * without the TUI. The worker owns session I/O. Success emits OCA_CE_SESSION;
 * failure emits OCA_CE_ERROR. The caller retains the core until completion. */
int oca_core_save_session(oca_core *core, char *err, size_t errsz);
int oca_core_list_sessions(oca_core *core, char *err, size_t errsz);
int oca_core_resume_session(oca_core *core, const char *session_id,
                            char *err, size_t errsz);
int oca_core_list_skills(oca_core *core, char *err, size_t errsz);

/* Queues one explicit Bash command in the interactive runtime. The core
 * copies command until execution at an action boundary. Tool events carry
 * the result; no model request, message, session, or prompt history is added.
 * Empty or invalid UTF-8 input fails without starting a process. */
int oca_core_bash(oca_core *core, const char *command, char *err, size_t errsz);

/* Queues a worker-owned transcript copy at the next action boundary.
 * The caller releases the returned TRANSCRIPT event with event_release.
 * Replay does not change messages, session state, usage, or model context. */
int oca_core_replay(oca_core *core, char *err, size_t errsz);
int oca_core_quit(oca_core *core, char *err, size_t errsz);

/* Answers for a pending save or continue prompt. Nonzero selects yes. */
int oca_core_save_answer(oca_core *core, int yes, char *err, size_t errsz);

/* ==========================================================================
 * Events
 * ============================================================================
 * Every event owns its variable data and borrows no core memory. The consumer
 * releases it with oca_core_event_release, including after core destruction.
 * ========================================================================== */

typedef enum {
    OCA_TOOL_COMPLETE,
    OCA_TOOL_INTERRUPTED,
    OCA_TOOL_ERROR,
} oca_tool_state;

typedef enum {
    OCA_CE_STATUS = 0,       /* state, route, goal, token snapshot */
    OCA_CE_WARN,             /* text: startup or discovery warning */
    OCA_CE_ASSISTANT_TEXT,   /* text: one assistant text delta */
    OCA_CE_ASSISTANT_REASON, /* text: one reasoning delta */
    OCA_CE_TOOL_START,       /* name, detail: call id, text: args json */
    OCA_CE_TOOL_OUTPUT,      /* text: live UTF-8 output from the active Bash call */
    OCA_CE_TOOL_RESULT,      /* name, detail: call id, text: result, state */
    OCA_CE_INTERACTION,      /* questions: owned question and option arrays */
    OCA_CE_GOAL,             /* goal state, text: objective */
    OCA_CE_SESSIONS,         /* sessions array */
    OCA_CE_SKILLS,           /* skills array */
    OCA_CE_SAVE_ASK,         /* dirty session: save prompt */
    OCA_CE_SAVE_ASK_CONT,    /* save failed: continue-anyway prompt */
    OCA_CE_ERROR,            /* name: stable code, text: message */
    OCA_CE_ROUTE,            /* route applied */
    OCA_CE_TURN_DONE,        /* text: final text */
    OCA_CE_SESSION,          /* session replaced, resumed, or saved */
    OCA_CE_QUIT_OK,          /* worker completed shutdown */
    OCA_CE_USER_TEXT,        /* text: a delivered or replayed user prompt */
    OCA_CE_TRANSCRIPT,       /* entries: one owned, ordered replay batch */
    OCA_CE_BROWSER_APPROVAL  /* text: prompt; NULL dismisses the prompt */
} oca_core_event_kind;

typedef struct {
    char *id;
    char *title;
    char *model;
    char *workspace;
    long long last_used;
} oca_session_summary;

typedef struct {
    char *name;
    char *description;
    char *path;
} oca_skill_info;

typedef struct {
    char *question;
    char **options;
    size_t option_count;
} oca_interaction_question;

/* Each entry owns copied text and tool identity. The batch has no nested
 * events and never borrows the live conversation or session buffers. */
typedef struct {
    oca_core_event_kind kind;
    char *text;
    char *name;
    char *detail;
    oca_tool_state tool_state;
} oca_transcript_entry;

typedef struct {
    oca_core_event_kind kind;
    /* Status snapshot fields for OCA_CE_STATUS. */
    oca_core_state state;
    oca_goal_state goal;
    char model_id[OCA_LLM_MODEL_ID_MAX + 1];
    char reasoning_name[16];
    int route_available;
    int has_used_tokens;
    long used_tokens;
    long context_limit;
    /* Variable data. The meaning follows the event kind. */
    char *text;
    char *name;
    char *detail;
    oca_tool_state tool_state;
    int session_replaced;    /* SESSION: new or loaded; name owns workspace */
    int session_new;         /* SESSION: a successful new-session action */
    oca_transcript_entry *entries;
    size_t entry_count;
    oca_session_summary *sessions;
    size_t session_count;
    oca_skill_info *skills;
    size_t skill_count;
    oca_interaction_question *questions;
    size_t question_count;
    uint64_t goal_elapsed_ms; /* Active goal time, excluding explicit pauses. */
} oca_core_event;

/* Polls one queued event. Returns one when the event is valid. */
int oca_core_poll_event(oca_core *core, oca_core_event *out);

/* Blocks until one event exists or the worker stops. Returns one when the
 * event is valid. */
int oca_core_wait_event(oca_core *core, oca_core_event *out);

/* Frees owned event data and clears the caller's record. Accepts NULL. */
void oca_core_event_release(oca_core_event *event);

/* Borrows the nonblocking wake descriptor until core destruction. Do not close it. */
int oca_core_wake_fd(const oca_core *core);

/* ==========================================================================
 * Skill catalog and session files
 * ============================================================================
 * Catalog queries borrow strings until core destruction.
 * Use these borrowed views with a quiescent worker; list_skills emits an owned
 * snapshot during work. File operations run on the caller thread and never
 * change the active conversation.
 * ========================================================================== */

size_t oca_core_skill_count(const oca_core *core);
const char *oca_core_skill_name(const oca_core *core, size_t index);
const char *oca_core_skill_description(const oca_core *core, size_t index);
size_t oca_core_skill_warning_count(const oca_core *core);

/* Returns an owned array and strings through out. Release with oca_core_free_summaries. */
int oca_core_scan_sessions(const char *dir, oca_session_summary **out,
                           size_t *out_count, char *err, size_t errsz);
int oca_core_delete_session(const char *dir, const char *session_id,
                            char *err, size_t errsz);
void oca_core_free_summaries(oca_session_summary *sessions, size_t count);

#endif
