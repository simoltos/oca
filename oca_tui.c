/* OCA terminal interface implementation. */
#define _XOPEN_SOURCE 700
#include "oca_tui.h"
#include <cjson/cJSON.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#define ESCAPE_MAX 32
#define HISTORY_MAX 512
#define MENU_ROWS_MAX 8
#define UTF8_PENDING_MAX 4
#define REDRAW_INTERVAL_MS 33
#ifndef VIEW_HISTORY_MAX_BYTES
#define VIEW_HISTORY_MAX_BYTES OCA_SNAPSHOT_MAX_BYTES
#endif
#ifndef VIEW_HISTORY_MAX_ROWS
#define VIEW_HISTORY_MAX_ROWS (OCA_TOOL_OUTPUT_MAX_BYTES / 4)
#endif

#define STYLE_RESET   "\x1b[0m"
#define STYLE_BOLD    "\x1b[1m"
#define STYLE_MUTED   "\x1b[38;2;141;149;166m"
#define STYLE_REASONING STYLE_MUTED "\x1b[3m"
#define STYLE_SUBTLE  "\x1b[38;2;94;102;115m"
#define STYLE_ACCENT  "\x1b[38;2;125;161;222m"
#define STYLE_BRIGHT  "\x1b[1;38;2;171;194;236m"
#define STYLE_SUCCESS "\x1b[38;2;130;184;157m"
#define STYLE_WARNING "\x1b[38;2;216;178;112m"
#define STYLE_DANGER  "\x1b[38;2;218;138;147m"
/* Match ds4-agent's terminal palette. */
#define STYLE_SELECTED "\x1b[32m"
#define STYLE_TOOL_BASH STYLE_BOLD STYLE_SELECTED
#define STYLE_TOOL_FILE STYLE_BOLD STYLE_ACCENT
#define STYLE_INLINE   "\x1b[36m"
#define STYLE_HINT_TEXT "\x1b[38;5;30m"
#define STYLE_HINT      "\x1b[1;97;48;5;23m"
#define STYLE_EXIT     STYLE_BOLD "\x1b[33m"
#define STYLE_USER     "\x1b[38;2;73;155;91m"
#define STYLE_DIFF_ADD "\x1b[97;48;2;24;55;35m"
#define STYLE_DIFF_DELETE "\x1b[97;48;2;65;28;32m"
#define STYLE_DIFF_REMOVE "\x1b[31m"
#define STYLE_PURPLE    "\x1b[38;5;177m"
#define STYLE_LILAC     "\x1b[38;5;183m"
#define STYLE_BADGE    STYLE_BOLD STYLE_WARNING     /* startup badge row */

enum { TOOL_RUNNING = -1 };

typedef enum {
    MENU_NONE = 0,
    MENU_COMMANDS,
    MENU_MODEL,
    MENU_REASONING,
    MENU_SESSIONS,
    MENU_SKILLS,
    MENU_HELP,
    MENU_INTERACTION,
    MENU_SAVE,
    MENU_SAVE_CONTINUE
} menu_kind;

typedef enum {
    CONVERSATION_NONE,
    CONVERSATION_ASSISTANT,
    CONVERSATION_REASONING,
    CONVERSATION_BODY,
    CONVERSATION_BASH,
    CONVERSATION_LEGEND,
    CONVERSATION_USER,
    CONVERSATION_DIFF,
    CONVERSATION_ACTION,
    CONVERSATION_ANSWER,
} conversation_kind;

typedef struct {
    struct termios saved;
    int saved_flags;
    int active;
    int title_saved;
    int alternate_screen;
} terminal_state;

typedef struct {
    char *label;
    char *value;
    int number;
} menu_item;

typedef struct {
    char *text;
    char *source;             /* Owned unmodified body for retained reflow. */
    const char *style;
    int fill;
    size_t source_end;
    size_t accent_start, accent_end;
    const char *accent_style;
    size_t accent2_start, accent2_end;
    const char *accent2_style;
    size_t accent3_start, accent3_end;
    const char *accent3_style;
    size_t accent4_start, accent4_end;
    const char *accent4_style;
    conversation_kind conversation_kind;
    int continued;
    int soft_wrap;
    size_t prefix_bytes;
    int card_role;                 /* top, content, bottom of a question card */
} display_line;

typedef struct {
    display_line *items;
    size_t count;
    size_t capacity;
} display_lines;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} text_buffer;

#define MARKDOWN_INLINE_MAX 4096
typedef struct {
    int mode;
    int hint;
    char fence;
    size_t fence_length;
    text_buffer source_line, table;
    int table_rows, columns;
} markdown_state;

typedef struct tui_app tui_app;
struct tui_app {
    oca_core *core;
    oca_tui_options display;
    tui_app *main_view;
    tui_app *side_view;          /* owned only by main_view */
    tui_app *active_view;        /* selected by main_view */
    int side_chat, hidden, closing;
    int restore_pending;
    int welcome_pending;
    int overlay;
    int history_omitted;
    display_lines retained;
    size_t retained_bytes;
    size_t flushed_rows;
    size_t view_end;
    const char *workspace;
    char *owned_workspace;
    char *tool_id;
    char *tool_name;
    char *tool_arguments;
    size_t tool_streamed_bytes;
    int replaying;
    terminal_state terminal;
    int signal_pipe[2];
    char *input;
    size_t input_len;
    size_t cursor;
    int vertical_column;
    int vertical_move;
    char save_input[16];
    size_t save_input_len;
    char escape[ESCAPE_MAX];
    size_t escape_len;
    long long escape_deadline_ms;
    int paste;
    size_t paste_end_len;
    int paste_saw_cr;
    char *paste_text;
    size_t paste_text_len;
    size_t paste_text_capacity;
    int paste_rejected;
    unsigned char utf8_pending[UTF8_PENDING_MAX];
    size_t utf8_pending_len;
    size_t utf8_pending_need;
    int running;
    char notice[256];
    long long notice_deadline_ms;
    int exit_hint;
    long long exit_hint_restore_ms;
    long long timer_tick_ms;
    int invalid_input;
    int dirty;
    int redraw_urgent;
    int events_pending;
    long long last_render_ms;
    int failed;
    int rows;
    int cols;
    int block_lines;
    int block_cursor_row;
    char *stream_text;
    size_t stream_text_len;
    conversation_kind stream_kind;
    int stream_continued;
    markdown_state markdown;
    menu_kind menu;
    menu_item *menu_items;
    size_t menu_count;
    size_t menu_selection;
    size_t menu_first;
    size_t interaction_question;
    oca_interaction_question *questions;
    size_t question_count;
    char **answers;
    char *interaction_draft;
    size_t interaction_cursor;
    int interaction_details;
    char **history;
    size_t history_count;
    size_t history_at;
    char *history_draft;
    char *pending_model;
    char **deferred_inputs;
    size_t deferred_input_count;
};

typedef struct {
    const char *name;
    const char *description;
    const char *syntax;
} command_info;

static const command_info COMMANDS[] = {
    { "/help", "Show keyboard shortcuts", "" },
    { "/goal", "Show or manage the active goal", " [objective | edit | pause | resume | clear]" },
    { "/new", "Start a new session", "" },
    { "/resume", "Resume a saved session", "" },
    { "/model", "Select a model", "" },
    { "/skills", "Inspect available skills", "" },
    { "/hints", "Toggle programming hints", "" },
    { "/btw", "Open temporary chat", " [message]" },
    { "/quit", "Exit OCA", "" }
};

static const struct {
    const char *name;
    const char *description;
    int command;
} GOAL_ACTIONS[] = {
    { "edit", "Pause and edit the current objective", OCA_GOAL_CMD_EDIT },
    { "pause", "Pause automatic goal continuations", OCA_GOAL_CMD_PAUSE },
    { "resume", "Resume the current goal", OCA_GOAL_CMD_RESUME },
    { "clear", "Remove the current goal", OCA_GOAL_CMD_CLEAR }
};

static volatile sig_atomic_t signal_exit;
static volatile sig_atomic_t signal_resize;
static volatile sig_atomic_t signal_wake_write = -1;
static terminal_state *active_terminal;

static void command_menu(tui_app *app);
static void menu_clear(tui_app *app);
static void stream_commit(tui_app *app, int finish);
static void history_emit(tui_app *app, const display_lines *lines, size_t count, int gap);
static void side_open(tui_app *app, const char *message);
static void side_close(tui_app *app);
static void view_switch(tui_app *app);
static void view_restore(tui_app *app);
static void app_free(tui_app *app);
static void tool_action_lines(tui_app *app, display_lines *lines, int state, const char *result);
static void conversation_wrap(display_lines *lines, conversation_kind kind, int continued,
                              const char *text, int cols);
static void input_clear(tui_app *app);
static void overlay_leave(tui_app *main);
static size_t sgr_length(const char *text);
static void interaction_restore(tui_app *app);
static void question_lines(tui_app *app, display_lines *lines, int *cursor_row, int *cursor_col);
static void question_heading(display_lines *lines, const char *question, int cols);
static void question_field(display_lines *lines, const char *label, const char *value, int cols);
static void question_answer_lines(display_lines *lines, const oca_interaction_question *question,
                                  const char *answer, int cols);
static int question_width(int cols);
static void question_wrap(display_lines *lines, const char *style, const char *text, int cols);
static void question_card(display_lines *lines, const display_lines *body, int cols, int active);
static void buffer_json_string(text_buffer *buffer, const char *text);

static int command_visible(const tui_app *app, const command_info *command) {
    return !app->side_chat || !strcmp(command->name, "/btw") ||
           !strcmp(command->name, "/help") || !strcmp(command->name, "/model") ||
           !strcmp(command->name, "/hints");
}

static int command_accepts_arguments(const command_info *command) {
    return command->syntax[0] != '\0';
}

/* ============================================================================
 * Terminal and text helpers
 * ========================================================================== */

static void write_all(const char *data, size_t len) {
    while (len) {
        ssize_t n = write(STDOUT_FILENO, data, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd descriptor = { .fd = STDOUT_FILENO, .events = POLLOUT };
                int ready;
                do {
                    ready = poll(&descriptor, 1, -1);
                } while (ready < 0 && errno == EINTR);
                if (ready <= 0) return;
                continue;
            }
            return;
        }
        if (n == 0) return;
        data += n;
        len -= (size_t)n;
    }
}

static void write_text(const char *text) {
    write_all(text, strlen(text));
}

static long long monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void terminal_restore(void) {
    terminal_state *term = active_terminal;
    if (!term || !term->active) return;
    if (term->alternate_screen) {
        write_text("\x1b[?1049l");
        term->alternate_screen = 0;
    }
    if (term->title_saved) write_text("\x1b[23;2t");
    write_text(STYLE_RESET "\x1b[?7h\x1b[?2004l\x1b[?25h\r\n");
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &term->saved);
    fcntl(STDIN_FILENO, F_SETFL, term->saved_flags);
    term->active = 0;
    active_terminal = NULL;
}

static void signal_handler(int number) {
    int saved = errno;
    if (number == SIGWINCH) signal_resize = 1;
    else signal_exit = 1;
    if (signal_wake_write >= 0) {
        unsigned char byte = 1;
        (void)write((int)signal_wake_write, &byte, 1);
    }
    errno = saved;
}

static int install_signals(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    const int signals[] = { SIGWINCH, SIGHUP, SIGINT, SIGTERM, SIGQUIT };
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        if (sigaction(signals[i], &action, NULL) != 0) return -1;
    }
    return 0;
}

static int signal_pipe_open(int descriptors[2]) {
    if (pipe(descriptors) != 0) return -1;
    for (int i = 0; i < 2; i++) {
        int flags = fcntl(descriptors[i], F_GETFL);
        int fdflags = fcntl(descriptors[i], F_GETFD);
        if (flags < 0 || fdflags < 0 ||
            fcntl(descriptors[i], F_SETFL, flags | O_NONBLOCK) != 0 ||
            fcntl(descriptors[i], F_SETFD, fdflags | FD_CLOEXEC) != 0) {
            close(descriptors[0]);
            close(descriptors[1]);
            descriptors[0] = descriptors[1] = -1;
            return -1;
        }
    }
    return 0;
}

static void drain_descriptor(int descriptor) {
    unsigned char bytes[64];
    while (read(descriptor, bytes, sizeof(bytes)) > 0) {}
}

static int terminal_enter(terminal_state *term) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fputs("oca: stdin and stdout must be terminals\n", stderr);
        return -1;
    }
    if (tcgetattr(STDIN_FILENO, &term->saved) != 0) {
        fprintf(stderr, "oca: cannot read terminal state: %s\n", strerror(errno));
        return -1;
    }
    term->saved_flags = fcntl(STDIN_FILENO, F_GETFL);
    if (term->saved_flags < 0) return -1;
    struct termios raw = term->saved;
    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= (tcflag_t)~OPOST;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &term->saved);
        fputs("oca: cannot enter terminal mode\n", stderr);
        return -1;
    }
    term->active = 1;
    active_terminal = term;
    atexit(terminal_restore);
    write_text(STYLE_RESET "\x1b[?7h\x1b[?2004h\x1b[?25h");
    return 0;
}

static void terminal_size(tui_app *app) {
    struct winsize size;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_row && size.ws_col) {
        app->rows = size.ws_row;
        app->cols = size.ws_col;
    } else {
        app->rows = 24;
        app->cols = 80;
    }
}

static void *checked_realloc(void *old, size_t bytes) {
    void *next = realloc(old, bytes ? bytes : 1);
    if (!next) {
        terminal_restore();
        fputs("oca: out of memory\n", stderr);
        exit(1);
    }
    return next;
}

static char *copy_text(const char *text) {
    size_t len = strlen(text) + 1;
    char *copy = checked_realloc(NULL, len);
    memcpy(copy, text, len);
    return copy;
}

static void append_text(char **buffer, size_t *length, const char *text) {
    size_t add = strlen(text);
    if (add > SIZE_MAX - *length - 1) return;
    *buffer = checked_realloc(*buffer, *length + add + 1);
    memcpy(*buffer + *length, text, add + 1);
    *length += add;
}

static size_t utf8_decode(const char *text, size_t remaining, uint32_t *value,
                          int *width, int *valid) {
    const unsigned char *s = (const unsigned char *)text;
    uint32_t code = 0;
    size_t length = 1;
    int ok = 1;

    if (!remaining || s[0] == 0) {
        ok = 0;
    } else if (s[0] < 0x80) {
        code = s[0];
    } else if (s[0] >= 0xC2 && s[0] <= 0xDF && remaining >= 2 &&
               (s[1] & 0xC0) == 0x80) {
        code = ((uint32_t)(s[0] & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
        length = 2;
    } else if (s[0] >= 0xE0 && s[0] <= 0xEF && remaining >= 3 &&
               (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
               !(s[0] == 0xE0 && s[1] < 0xA0) &&
               !(s[0] == 0xED && s[1] >= 0xA0)) {
        code = ((uint32_t)(s[0] & 0x0F) << 12) |
               ((uint32_t)(s[1] & 0x3F) << 6) | (uint32_t)(s[2] & 0x3F);
        length = 3;
    } else if (s[0] >= 0xF0 && s[0] <= 0xF4 && remaining >= 4 &&
               (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
               (s[3] & 0xC0) == 0x80 && !(s[0] == 0xF0 && s[1] < 0x90) &&
               !(s[0] == 0xF4 && s[1] >= 0x90)) {
        code = ((uint32_t)(s[0] & 0x07) << 18) |
               ((uint32_t)(s[1] & 0x3F) << 12) |
               ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
        length = 4;
    } else {
        ok = 0;
    }

    if (!ok) code = 0xFFFD;
    int columns = 1;
    if (ok && code != '\n' && code != '\r' && code != '\t') {
#if WCHAR_MAX >= 0x10FFFF
        columns = wcwidth((wchar_t)code);
#else
        columns = code <= (uint32_t)WCHAR_MAX ? wcwidth((wchar_t)code) : 1;
#endif
        if (columns < 0) columns = 1;
    }
    *value = code;
    *width = columns;
    *valid = ok;
    return length;
}

static int utf8_bytes_valid(const char *text, size_t len) {
    size_t at = 0;
    while (at < len) {
        uint32_t value;
        int width, valid;
        size_t bytes = utf8_decode(text + at, len - at, &value, &width, &valid);
        (void)value;
        (void)width;
        if (!valid || text[at] == '\0') return 0;
        at += bytes;
    }
    return 1;
}

static int regional_indicator(uint32_t value) {
    return value >= 0x1F1E6 && value <= 0x1F1FF;
}

static int grapheme_extend(uint32_t value, int width) {
    if (value == 0x200D) return 0;
    if (width == 0 || (value >= 0xFE00 && value <= 0xFE0F) ||
        (value >= 0xE0100 && value <= 0xE01EF) ||
        (value >= 0x1F3FB && value <= 0x1F3FF) ||
        (value >= 0xE0020 && value <= 0xE007F)) return 1;
    return 0;
}

static size_t next_grapheme(const char *text, size_t len, size_t at) {
    if (at >= len) return len;
    uint32_t value;
    int width, valid;
    size_t bytes = utf8_decode(text + at, len - at, &value, &width, &valid);
    int first_regional = valid && regional_indicator(value);
    at += bytes;

    if (first_regional && at < len) {
        bytes = utf8_decode(text + at, len - at, &value, &width, &valid);
        if (valid && regional_indicator(value)) at += bytes;
    }
    while (at < len) {
        bytes = utf8_decode(text + at, len - at, &value, &width, &valid);
        if (!valid) break;
        if (grapheme_extend(value, width)) {
            at += bytes;
            continue;
        }
        if (value != 0x200D) break;
        at += bytes;
        if (at >= len) break;
        bytes = utf8_decode(text + at, len - at, &value, &width, &valid);
        if (!valid) break;
        at += bytes;
    }
    return at;
}

static size_t previous_codepoint(const char *text, size_t at) {
    if (!at) return 0;
    size_t start = at - 1;
    while (start > 0 && ((unsigned char)text[start] & 0xC0) == 0x80) start--;
    return start;
}

static int grapheme_columns(const char *text, size_t len) {
    size_t at = 0;
    int columns = 0;
    while (at < len) {
        uint32_t value;
        int width, valid;
        size_t bytes = utf8_decode(text + at, len - at, &value, &width, &valid);
        if (!valid) return 1;
        if (value != 0x200D && !grapheme_extend(value, width) && width > columns)
            columns = width;
        at += bytes;
    }
    return columns;
}

static size_t previous_grapheme(const char *text, size_t at) {
    if (!at) return 0;
    size_t start = previous_codepoint(text, at);
    uint32_t value;
    int width, valid;
    (void)utf8_decode(text + start, at - start, &value, &width, &valid);

    if (valid && regional_indicator(value)) {
        size_t count = 1;
        size_t scan = start;
        while (scan > 0) {
            size_t previous = previous_codepoint(text, scan);
            (void)utf8_decode(text + previous, scan - previous,
                              &value, &width, &valid);
            if (!valid || !regional_indicator(value)) break;
            count++;
            scan = previous;
        }
        return count % 2 == 0 ? previous_codepoint(text, start) : start;
    }

    while (start > 0 && valid && grapheme_extend(value, width)) {
        start = previous_codepoint(text, start);
        (void)utf8_decode(text + start, at - start, &value, &width, &valid);
    }
    while (start > 0) {
        size_t joiner = previous_codepoint(text, start);
        (void)utf8_decode(text + joiner, start - joiner,
                          &value, &width, &valid);
        if (!valid || value != 0x200D) break;
        start = joiner;
        while (start > 0) {
            start = previous_codepoint(text, start);
            (void)utf8_decode(text + start, at - start,
                              &value, &width, &valid);
            if (!valid || !grapheme_extend(value, width)) break;
        }
    }
    return start;
}

/* ============================================================================
 * Live block
 * ========================================================================== */

static void buffer_reserve(text_buffer *buffer, size_t extra) {
    if (extra > SIZE_MAX - buffer->length - 1) {
        terminal_restore();
        fputs("oca: text is too large\n", stderr);
        exit(1);
    }
    size_t required = buffer->length + extra + 1;
    if (required <= buffer->capacity) return;
    size_t capacity = buffer->capacity ? buffer->capacity : 64;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) capacity = required;
        else capacity *= 2;
    }
    buffer->data = checked_realloc(buffer->data, capacity);
    buffer->capacity = capacity;
}

static void buffer_append(text_buffer *buffer, const char *text, size_t len) {
    buffer_reserve(buffer, len);
    memcpy(buffer->data + buffer->length, text, len);
    buffer->length += len;
    buffer->data[buffer->length] = '\0';
}

static void buffer_clear(text_buffer *buffer) {
    buffer->length = 0;
    if (buffer->data) buffer->data[0] = '\0';
}

static void buffer_free(text_buffer *buffer) {
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static void lines_add_full(display_lines *lines, const char *style,
                           const char *text, size_t len, int fill) {
    if (lines->count == lines->capacity) {
        size_t next = lines->capacity ? lines->capacity * 2 : 16;
        lines->items = checked_realloc(lines->items, next * sizeof(*lines->items));
        lines->capacity = next;
    }
    char *copy = checked_realloc(NULL, len + 1);
    memcpy(copy, text, len);
    copy[len] = '\0';
    lines->items[lines->count++] = (display_line){ .text = copy, .style = style, .fill = fill };
}

static void lines_add(display_lines *lines, const char *style,
                      const char *text, size_t len) {
    lines_add_full(lines, style, text, len, 0);
}

static void wrap_flush(display_lines *lines, const char *style,
                       text_buffer *line, int *column) {
    lines_add(lines, style, line->data ? line->data : "", line->length);
    buffer_clear(line);
    *column = 0;
}

static void wrap_ascii(display_lines *lines, const char *style,
                       text_buffer *line, int *column, int cols,
                       const char *text, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (*column >= cols) wrap_flush(lines, style, line, column);
        buffer_append(line, text + i, 1);
        (*column)++;
    }
}

static void wrap_safe_prefix(display_lines *lines, const char *style,
                             text_buffer *line, int *column, int cols,
                             const char *prefix) {
    size_t length = strlen(prefix);
    size_t at = 0;
    while (at < length) {
        size_t end = next_grapheme(prefix, length, at);
        int width = grapheme_columns(prefix + at, end - at);
        if (width > cols) {
            wrap_ascii(lines, style, line, column, cols, "?", 1);
        } else {
            if (*column > 0 && *column + width > cols)
                wrap_flush(lines, style, line, column);
            buffer_append(line, prefix + at, end - at);
            *column += width;
        }
        at = end;
    }
}

static void lines_wrap(display_lines *lines, const char *style, const char *prefix,
                       const char *text, int cols, size_t cursor_byte,
                       int *cursor_row, int *cursor_col) {
    if (cols < 1) cols = 1;
    size_t text_len = strlen(text);
    text_buffer line = {0};
    int column = 0;
    wrap_safe_prefix(lines, style, &line, &column, cols, prefix);
    size_t at = 0;
    if (cursor_row && cursor_byte == 0) {
        if (column >= cols) wrap_flush(lines, style, &line, &column);
        *cursor_row = (int)lines->count;
        *cursor_col = column;
    }
    while (at < text_len) {
        if (column >= cols) wrap_flush(lines, style, &line, &column);
        if (cursor_row && cursor_byte == at) {
            *cursor_row = (int)lines->count;
            *cursor_col = column;
        }
        if (text[at] == '\n' ||
            (text[at] == '\r' && at + 1 < text_len && text[at + 1] == '\n')) {
            wrap_flush(lines, style, &line, &column);
            at += text[at] == '\r' ? 2 : 1;
            if (cursor_row && cursor_byte == at) {
                *cursor_row = (int)lines->count;
                *cursor_col = 0;
            }
            continue;
        }
        if (text[at] == '\t') {
            int spaces = 4 - column % 4;
            while (spaces-- > 0)
                wrap_ascii(lines, style, &line, &column, cols, " ", 1);
            at++;
        } else if ((unsigned char)text[at] < 0x20 ||
                   (unsigned char)text[at] == 0x7F) {
            char escaped[5];
            snprintf(escaped, sizeof(escaped), "\\x%02X", (unsigned char)text[at]);
            wrap_ascii(lines, style, &line, &column, cols, escaped, 4);
            at++;
        } else {
            uint32_t value;
            int width, valid;
            size_t first = utf8_decode(text + at, text_len - at,
                                       &value, &width, &valid);
            size_t end = valid ? next_grapheme(text, text_len, at) : at + first;
            width = valid ? grapheme_columns(text + at, end - at) : 1;
            if (width > cols) {
                wrap_ascii(lines, style, &line, &column, cols, "?", 1);
            } else {
                if (column > 0 && column + width > cols)
                    wrap_flush(lines, style, &line, &column);
                if (cursor_row && cursor_byte == at) {
                    *cursor_row = (int)lines->count;
                    *cursor_col = column;
                }
                if (valid) buffer_append(&line, text + at, end - at);
                else buffer_append(&line, "\xEF\xBF\xBD", 3);
                column += width;
            }
            at = end;
        }
        if (cursor_row && cursor_byte == at) {
            if (column >= cols && at == text_len)
                wrap_flush(lines, style, &line, &column);
            *cursor_row = (int)lines->count;
            *cursor_col = column;
        }
    }
    lines_add(lines, style, line.data ? line.data : "", line.length);
    buffer_free(&line);
}

static char *safe_clip(const char *text, int columns) {
    display_lines lines = {0};
    lines_wrap(&lines, STYLE_RESET, "", text, columns, SIZE_MAX, NULL, NULL);
    char *result = copy_text(lines.count ? lines.items[0].text : "");
    for (size_t i = 0; i < lines.count; i++) free(lines.items[i].text);
    free(lines.items);
    return result;
}

static void lines_free(display_lines *lines) {
    for (size_t i = 0; i < lines->count; i++) {
        free(lines->items[i].text);
        free(lines->items[i].source);
    }
    free(lines->items);
    memset(lines, 0, sizeof(*lines));
}

/* Composer rendering and vertical movement use the same grapheme layout. */
static void composer_wrap(tui_app *app, display_lines *lines, const char *style,
                           int cols, int *cursor_row, int *cursor_col) {
    if (cols < 1) cols = 1;
    int padding = cols >= 3 ? 2 : cols - 1;
    int manual = !app->side_chat && app->menu != MENU_INTERACTION &&
                 app->input_len && app->input[0] == '!';
    size_t skip = manual ? 1 + (app->input_len > 1 && app->input[1] == ' ') : 0;
    const char *text = app->input_len ? app->input + skip :
        app->menu == MENU_INTERACTION ? "Add details (optional)" : "Ask OCA anything";
    size_t cursor = app->cursor > skip ? app->cursor - skip : 0;
    lines_wrap(lines, style, "", text, cols - padding, cursor, cursor_row, cursor_col);
    for (size_t i = 0; i < lines->count; i++) {
        display_line *line = &lines->items[i];
        text_buffer padded = {0};
        int spaces = padding;
        if (!i && padding) {
            const char *marker = manual ? STYLE_DANGER "!" : STYLE_USER "›";
            buffer_append(&padded, marker, strlen(marker));
            buffer_append(&padded, style, strlen(style));
            spaces--;
        }
        while (spaces-- > 0) buffer_append(&padded, " ", 1);
        buffer_append(&padded, line->text, strlen(line->text));
        free(line->text);
        line->text = padded.data ? padded.data : copy_text("");
    }
    *cursor_col += padding;
}

static int text_columns(const char *text) {
    size_t at = 0, len = strlen(text);
    int columns = 0;
    while (at < len) {
        size_t style = sgr_length(text + at);
        if (style) { at += style; continue; }
        size_t end = next_grapheme(text, len, at);
        int width = grapheme_columns(text + at, end - at);
        if (columns > INT_MAX - width) return INT_MAX;
        columns += width;
        at = end;
    }
    return columns;
}

static void refresh_geometry(tui_app *app) {
    terminal_size(app);
    if (app->block_lines > app->rows) app->block_lines = app->rows;
    /* A shorter viewport clips rows below the cursor first. It does not
     * necessarily remove rows above it within the live block. */
    if (app->block_cursor_row >= app->block_lines && app->block_lines > 0)
        app->block_cursor_row = app->block_lines - 1;
}

static void terminal_update_begin(void) {
    write_text("\x1b[?25l\x1b[?7l");
}

static void terminal_update_end(void) {
    write_text(STYLE_RESET "\x1b[?7h\x1b[?25h");
}

/* Clears the known live rows and leaves the cursor at their first row. */
static void clear_block(tui_app *app) {
    if (!app->block_lines) return;
    char move[32];
    write_text("\r");
    if (app->block_cursor_row > 0) {
        snprintf(move, sizeof(move), "\x1b[%dA", app->block_cursor_row);
        write_text(move);
    }
    /* Erasing must not scroll after the terminal clips the old block. */
    write_text("\x1b[J");
    app->block_lines = 0;
    app->block_cursor_row = 0;
}

static void erase_block(tui_app *app) {
    if (!app->block_lines) return;
    refresh_geometry(app);
    terminal_update_begin();
    clear_block(app);
    terminal_update_end();
}

static const char *goal_name(oca_goal_state goal) {
    switch (goal) {
    case OCA_GOAL_ACTIVE:
        return "goal active";
    case OCA_GOAL_PAUSED:
        return "goal paused";
    case OCA_GOAL_BLOCKED:
        return "goal blocked";
    case OCA_GOAL_COMPLETED:
        return "goal completed";
    case OCA_GOAL_NONE:
        return NULL;
    }
    return NULL;
}

static void lines_copy_range(display_lines *target, const display_lines *source,
                             size_t start, size_t count) {
    for (size_t i = 0; i < count; i++) {
        const display_line *line = &source->items[start + i];
        lines_add_full(target, line->style, line->text, strlen(line->text), line->fill);
        display_line *copy = &target->items[target->count - 1];
        char *text = copy->text;
        *copy = *line;
        copy->text = text;
        copy->source = line->source ? copy_text(line->source) : NULL;
    }
}

static void lines_add_menu(display_lines *lines, const char *style,
                           const char *prefix, const char *label, int cols) {
    size_t length = strlen(prefix) + strlen(label) + 1;
    char *combined = checked_realloc(NULL, length);
    snprintf(combined, length, "%s%s", prefix, label);
    char *clipped = safe_clip(combined, cols);
    lines_add(lines, style, clipped, strlen(clipped));
    free(clipped);
    free(combined);
}

static int status_join(char **out, const char *left, const char *right, int cols) {
    char *safe_left = safe_clip(left, INT_MAX);
    char *safe_right = safe_clip(right, INT_MAX);
    int left_width = text_columns(safe_left);
    int right_width = text_columns(safe_right);
    int gap = *safe_left && *safe_right ? 1 : 0;
    if (left_width > cols - right_width - gap) {
        free(safe_left);
        free(safe_right);
        return 0;
    }
    size_t spaces = (size_t)(cols - left_width - right_width);
    size_t size = strlen(safe_left) + spaces + strlen(safe_right) + 1;
    char *line = checked_realloc(NULL, size);
    char *at = line;
    size_t left_len = strlen(safe_left);
    memcpy(at, safe_left, left_len);
    at += left_len;
    memset(at, ' ', spaces);
    at += spaces;
    size_t right_len = strlen(safe_right);
    memcpy(at, safe_right, right_len);
    at += right_len;
    *at = '\0';
    free(safe_left);
    free(safe_right);
    *out = line;
    return 1;
}

static void notice_set(tui_app *app, const char *text) {
    snprintf(app->notice, sizeof(app->notice), "%s", text);
    app->notice_deadline_ms = monotonic_ms() + 3000;
    app->dirty = 1;
    app->redraw_urgent = 1;
}

static void elapsed_text(const char *label, uint64_t milliseconds, char *out, size_t size) {
    uint64_t seconds = milliseconds / 1000;
    if (seconds < 60)
        snprintf(out, size, "%s %llus", label, (unsigned long long)seconds);
    else if (seconds < 3600)
        snprintf(out, size, "%s %llum %llus", label, (unsigned long long)(seconds / 60),
                 (unsigned long long)(seconds % 60));
    else
        snprintf(out, size, "%s %lluh %02llum %02llus", label,
                 (unsigned long long)(seconds / 3600),
                 (unsigned long long)(seconds / 60 % 60),
                 (unsigned long long)(seconds % 60));
}

static void working_text(const oca_core_status *status, char *out, size_t size) {
    elapsed_text("Working", status->working_ms, out, size);
}

static void goal_time(uint64_t milliseconds, char *out, size_t size) {
    unsigned long long seconds = milliseconds / 1000;
    unsigned long long minutes = seconds / 60;
    if (!minutes) snprintf(out, size, "%llus", seconds);
    else if (minutes < 60) snprintf(out, size, "%llum", minutes);
    else snprintf(out, size, "%lluh %llum", minutes / 60, minutes % 60);
}

static char *shared_row(tui_app *app) {
    oca_core_status status;
    oca_core_get_status(app->core, &status);
    const char *text = NULL;
    char working[96];
    if (status.cancelling) text = "Canceling…";
    else if (app->exit_hint) text = app->exit_hint_restore_ms ? "" : "Press Ctrl+D to exit";
    else if (app->notice[0]) text = app->notice;
    else if (status.working && app->menu != MENU_INTERACTION) {
        working_text(&status, working, sizeof(working));
        if (!status.followup_count) {
            size_t used = strlen(working);
            snprintf(working + used, sizeof(working) - used, " (Esc to interrupt)");
        }
        text = working;
    }
    if (!text) return NULL;
    char row[320];
    snprintf(row, sizeof(row), " %s", text);
    return safe_clip(row, app->cols);
}

static void cache_text(const oca_core_status *status, char *out, size_t size) {
    if (!status->has_cache || !status->has_cache_input ||
        status->cache_input_tokens <= 0 || status->cache_hit_tokens < 0 ||
        status->cache_hit_tokens > status->cache_input_tokens) {
        snprintf(out, size, "cache hit —");
        return;
    }
    long double percent = 100.0L * status->cache_hit_tokens / status->cache_input_tokens;
    snprintf(out, size, "cache hit %u%%", (unsigned)(percent + 0.5L));
}

static char *status_text(tui_app *app, size_t *right_start) {
    oca_core_status status;
    oca_core_get_status(app->core, &status);
    *right_start = SIZE_MAX;
    char right[160] = "";
    const char *view = "";
    if (app->side_chat) view = "Temporary chat (Ctrl+/ to switch · Ctrl+C to close)";
    else if (app->side_view) view = "Main chat (Ctrl+/ to switch)";
    const char *goal = goal_name(status.goal);
    if (oca_core_hints_enabled(app->core)) snprintf(right, sizeof(right), "hints");
    if (goal) {
        char duration[48];
        goal_time(status.goal_elapsed_ms, duration, sizeof(duration));
        size_t used = strlen(right);
        snprintf(right + used, sizeof(right) - used, "%s%s %s",
                 used ? " · " : "", goal, duration);
    }
    if (*right) {
        size_t used = strlen(right);
        if (used + 1 < sizeof(right)) {
            right[used] = ' ';
            right[used + 1] = '\0';
        }
    }
    char tokens[96], cache[64];
    if (status.has_used_tokens)
        snprintf(tokens, sizeof(tokens), "%ld/%ld", status.used_tokens, status.context_limit);
    else snprintf(tokens, sizeof(tokens), "—/%ld", status.context_limit);
    cache_text(&status, cache, sizeof(cache));
    char candidates[4][512];
    snprintf(candidates[0], sizeof(candidates[0]), " %s %s · %s · %s",
             status.model_id, status.reasoning_name, tokens, cache);
    snprintf(candidates[1], sizeof(candidates[1]), " %s %s · %s",
             status.model_id, status.reasoning_name, cache);
    snprintf(candidates[2], sizeof(candidates[2]), " %s %s", status.model_id, status.reasoning_name);
    snprintf(candidates[3], sizeof(candidates[3]), " %s", status.model_id);
    if (*view) {
        for (size_t i = 0; i < 4; i++) {
            size_t used = strlen(candidates[i]);
            snprintf(candidates[i] + used, sizeof(candidates[i]) - used, " · %s", view);
        }
    }
    char *line = NULL;
    for (size_t i = 0; i < 4; i++) {
        if (status_join(&line, candidates[i], right, app->cols)) {
            if (*right) *right_start = strlen(line) - strlen(right);
            return line;
        }
    }
    char *safe_right = safe_clip(right, app->cols);
    int right_width = text_columns(safe_right);
    if (*safe_right && right_width < app->cols) {
        char *left = safe_clip(*view ? view : candidates[3], app->cols - right_width - 1);
        size_t left_len = strlen(left), right_len = strlen(safe_right);
        line = checked_realloc(NULL, left_len + right_len + 2);
        memcpy(line, left, left_len);
        line[left_len] = ' ';
        memcpy(line + left_len + 1, safe_right, right_len + 1);
        *right_start = left_len + 1;
        free(left);
    } else {
        const char *text = *safe_right ? safe_right : status.model_id;
        if (*view) text = view;
        line = safe_clip(text, app->cols);
        if (text == safe_right) *right_start = 0;
    }
    free(safe_right);
    return line;
}

static void write_display_line(const display_line *line, int cols) {
    write_text("\x1b[2K");
    write_text(line->style);
    size_t prefix = line->prefix_bytes;
    if (prefix) {
        write_all(line->text, prefix);
        write_text(line->style);
    }
    size_t at = prefix, length = strlen(line->text);
    const size_t starts[] = {line->accent_start, line->accent2_start,
                             line->accent3_start, line->accent4_start};
    const size_t ends[] = {line->accent_end, line->accent2_end,
                           line->accent3_end, line->accent4_end};
    const char *styles[] = {line->accent_style, line->accent2_style,
                            line->accent3_style, line->accent4_style};
    for (size_t i = 0; i < 4; i++) {
        if (!styles[i] || starts[i] < at || ends[i] < starts[i] || ends[i] > length) continue;
        write_all(line->text + at, starts[i] - at);
        write_text(styles[i]);
        write_all(line->text + starts[i], ends[i] - starts[i]);
        if (strcmp(line->style, STYLE_RESET)) write_text(STYLE_RESET);
        write_text(line->style);
        at = ends[i];
    }
    write_text(line->text + at);
    if (line->fill) {
        int spaces = cols - text_columns(line->text);
        while (spaces-- > 0) write_text(" ");
    }
    write_text(STYLE_RESET);
}

/* ============================================================================
 * Conversation presentation
 * ========================================================================== */

static char *conversation_safe(const char *text) {
    text_buffer safe = {0};
    size_t len = strlen(text);
    for (size_t at = 0; at < len;) {
        unsigned char byte = (unsigned char)text[at];
        if (byte == '\r' && at + 1 < len && text[at + 1] == '\n') {
            buffer_append(&safe, "\n", 1);
            at += 2;
        } else if (byte == '\t') {
            buffer_append(&safe, "    ", 4);
            at++;
        } else if ((byte < 0x20 && byte != '\n') || byte == 0x7f) {
            char escaped[5];
            snprintf(escaped, sizeof(escaped), "\\x%02X", byte);
            buffer_append(&safe, escaped, 4);
            at++;
        } else {
            uint32_t value;
            int width, valid;
            size_t size = utf8_decode(text + at, len - at, &value, &width, &valid);
            buffer_append(&safe, valid ? text + at : "\xEF\xBF\xBD", valid ? size : 3);
            at += size;
        }
    }
    char *result = safe.data ? safe.data : copy_text("");
    return result;
}

enum { MD_PROSE, MD_FENCED };
enum { MD_BOLD = 1, MD_ITALIC = 2, MD_INLINE = 4 };

/* Formatting sequences originate here, after model text has been escaped. */
static size_t sgr_length(const char *text) {
    if (text[0] != '\x1b' || text[1] != '[') return 0;
    for (size_t i = 2; i < 47 && text[i]; i++) {
        if (text[i] == 'm') return i + 1;
        if (!isdigit((unsigned char)text[i]) && text[i] != ';') return 0;
    }
    return 0;
}

static void markdown_attrs(text_buffer *out, unsigned attrs) {
    if (attrs == MD_INLINE) {
        buffer_append(out, STYLE_INLINE, strlen(STYLE_INLINE));
        return;
    }
    char style[32];
    snprintf(style, sizeof(style), "\x1b[0%s%s%sm",
             attrs & MD_BOLD ? ";1" : "", attrs & MD_ITALIC ? ";3" : "",
             attrs & MD_INLINE ? ";36" : "");
    buffer_append(out, style, strlen(style));
}

static size_t markdown_run(const char *text, size_t length, char marker) {
    size_t count = 0;
    while (count < length && text[count] == marker) count++;
    return count;
}

static int markdown_escaped(const char *text, size_t at) {
    size_t count = 0;
    while (at && text[--at] == '\\') count++;
    return count & 1;
}

static int markdown_link_end(const char *text, size_t length, size_t *end) {
    if (length < 5 || text[0] != '[') return 0;
    if (length > MARKDOWN_INLINE_MAX) length = MARKDOWN_INLINE_MAX;
    size_t at = 1;
    while (at + 1 < length && (text[at] != ']' || text[at + 1] != '(' ||
           markdown_escaped(text, at))) at++;
    if (at + 1 >= length) return 0;
    size_t depth = 1;
    for (at += 2; at < length; at++) {
        if (markdown_escaped(text, at)) continue;
        if (text[at] == '(') depth++;
        if (text[at] == ')' && --depth == 0) {
            *end = at + 1;
            return 1;
        }
    }
    return 0;
}

static void markdown_inline(text_buffer *out, const char *text, size_t length,
                            unsigned attrs, unsigned depth) {
    for (size_t at = 0; at < length;) {
        if (text[at] == '\\' && at + 1 < length &&
            ispunct((unsigned char)text[at + 1])) {
            buffer_append(out, text + at + 1, 1);
            at += 2;
            continue;
        }
        size_t link_end;
        if (markdown_link_end(text + at, length - at, &link_end)) {
            buffer_append(out, text + at, link_end);
            at += link_end;
            continue;
        }
        char marker = text[at];
        int delimiter = strchr("*_", marker) || marker == 0x60;
        size_t run = delimiter ? markdown_run(text + at, length - at, marker) : 1;
        if (depth < 8 && delimiter &&
            (marker == 0x60 || run <= 3) &&
            !(marker == '_' && at && isalnum((unsigned char)text[at - 1]))) {
            size_t limit = length;
            if (limit - at > MARKDOWN_INLINE_MAX)
                limit = at + MARKDOWN_INLINE_MAX;
            size_t end = at + run;
            for (; end < limit; end++) {
                if (text[end] != marker || (marker != 0x60 &&
                    markdown_escaped(text, end))) continue;
                size_t close = markdown_run(text + end, length - end, marker);
                if (close == run && end > at + run &&
                    (marker == 0x60 || (!isspace((unsigned char)text[at + run]) &&
                                       !isspace((unsigned char)text[end - 1])))) break;
                end += close - 1;
            }
            if (end < limit) {
                unsigned style = attrs | (marker == 0x60 ? MD_INLINE :
                    run == 1 ? MD_ITALIC : run == 2 ? MD_BOLD :
                    MD_BOLD | MD_ITALIC);
                markdown_attrs(out, style);
                if (marker == 0x60) buffer_append(out, text + at + run,
                                                   end - at - run);
                else markdown_inline(out, text + at + run, end - at - run,
                                     style, depth + 1);
                markdown_attrs(out, attrs);
                at = end + run;
                continue;
            }
        }
        if (delimiter && run > 1) {
            buffer_append(out, text + at, run);
            at += run;
            continue;
        }
        buffer_append(out, text + at++, 1);
    }
}

static void markdown_prose(text_buffer *out, const char *text, size_t length) {
    markdown_inline(out, text, length, 0, 0);
}

/* Match ds4-agent's hint marker only at a prose line boundary. */
static int markdown_hint(markdown_state *md, text_buffer *out, const char *line,
                         size_t length, int preview) {
    const char *prefix = md->hint ? "> " : "> **Hint:**";
    size_t size = strlen(prefix);
    if (preview && length < size && !memcmp(line, prefix, length)) return 1;
    int continuation = md->hint;
    int match = length >= size && !memcmp(line, prefix, size);
    if (!preview) md->hint = match;
    if (!match) return 0;
    if (!continuation)
        buffer_append(out, STYLE_HINT " Hint " STYLE_RESET,
                      strlen(STYLE_HINT " Hint " STYLE_RESET));
    else buffer_append(out, "  ", 2);
    markdown_prose(out, line + size, length - size);
    return 1;
}

static int markdown_fence(const char *line, size_t length, char *marker,
                          size_t *run) {
    size_t at = 0;
    while (at < length && at < 3 && line[at] == ' ') at++;
    if (at == length || (line[at] != 0x60 && line[at] != '~')) return 0;
    size_t count = markdown_run(line + at, length - at, line[at]);
    if (count < 3) return 0;
    *marker = line[at];
    *run = count;
    return 1;
}

static int markdown_closing_fence(const markdown_state *md, const char *line,
                                  size_t length) {
    char marker;
    size_t run, at = 0;
    if (!markdown_fence(line, length, &marker, &run) || marker != md->fence ||
        run < md->fence_length) return 0;
    while (at < length && at < 3 && line[at] == ' ') at++;
    at += run;
    while (at < length && isspace((unsigned char)line[at])) at++;
    return at == length;
}

/* Table cells share the owned source buffer until inline formatting. */
static size_t table_cells(char *line, char **cells, size_t capacity, int *piped) {
    char *start = line + strspn(line, " \t");
    char *end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) *--end = 0;
    if (*start == '|') { start++; if (piped) *piped = 1; }
    if (end > start && end[-1] == '|' && !markdown_escaped(start, (size_t)(end - start - 1))) {
        *--end = 0;
        if (piped) *piped = 1;
    }
    size_t count = 0;
    char *cell = start, *write = start;
    for (char *read = start;; read++) {
        if (*read == '\\' && read[1]) {
            if (read[1] != '|') *write++ = *read;
            *write++ = *++read;
            continue;
        }
        if (*read == '|' || !*read) {
            int done = !*read;
            *write = 0;
            char *value = cell + strspn(cell, " \t");
            char *last = write;
            while (last > value && (last[-1] == ' ' || last[-1] == '\t')) *--last = 0;
            if (count < capacity) cells[count] = value;
            count++;
            if (done) break;
            if (piped) *piped = 1;
            cell = ++write;
        } else *write++ = *read;
    }
    return count;
}

static void table_border(text_buffer *out, const int *widths, size_t columns,
                         const char *left, const char *middle, const char *right) {
    buffer_append(out, STYLE_MUTED, strlen(STYLE_MUTED));
    buffer_append(out, left, strlen(left));
    for (size_t c = 0; c < columns; c++) {
        for (int i = 0; i < widths[c] + 2; i++) buffer_append(out, "─", strlen("─"));
        const char *edge = c + 1 < columns ? middle : right;
        buffer_append(out, edge, strlen(edge));
    }
    buffer_append(out, STYLE_RESET "\n", strlen(STYLE_RESET "\n"));
}

static char *table_inline(const char *text, int heading) {
    text_buffer out = {0};
    if (heading) markdown_attrs(&out, MD_BOLD);
    markdown_inline(&out, text, strlen(text), heading ? MD_BOLD : 0, 0);
    buffer_append(&out, STYLE_RESET, strlen(STYLE_RESET));
    return out.data;
}

/* Return zero for invalid or oversized tables. The caller renders plain text. */
static int markdown_table(const char *source, int columns, text_buffer *out) {
    size_t lines = 0, max_columns = 1, formatted = 0;
    for (const char *p = source; *p; p++) lines += *p == '\n';
    size_t length = strlen(source);
    if (length && source[length - 1] != '\n') lines++;
    for (const char *p = source; *p && *p != '\n'; p++) max_columns += *p == '|';
    if (lines < 2 || max_columns > 65536) return 0;
    char *data = copy_text(source), *save = NULL;
    char *line = strtok_r(data, "\n", &save);
    char **head = checked_realloc(NULL, max_columns * sizeof(*head));
    int piped = 0;
    size_t count = line ? table_cells(line, head, max_columns, &piped) : 0;
    if (!piped || !count || lines - 1 > 65536 / count) { free(data); free(head); return 0; }
    char **cells = calloc((lines - 1) * count, sizeof(*cells));
    int *align = calloc(count, sizeof(*align)), *width = calloc(count, sizeof(*width)), *wanted = calloc(count, sizeof(*wanted));
    display_lines *wrapped = calloc(count, sizeof(*wrapped));
    text_buffer result = {0};
    int valid = 0;
    if (!cells || !align || !width || !wanted || !wrapped) goto cleanup;
    memcpy(cells, head, count * sizeof(*cells));
    line = strtok_r(NULL, "\n", &save);
    if (!line || table_cells(line, head, count, NULL) != count) goto cleanup;
    for (size_t c = 0; c < count; c++) {
        const char *p = head[c];
        int left = *p == ':';
        if (left) p++;
        size_t dashes = strspn(p, "-");
        if (!dashes) goto cleanup;
        p += dashes;
        int right = *p == ':';
        if (right) p++;
        if (*p) goto cleanup;
        align[c] = left && right ? 0 : right ? 1 : -1;
    }
    size_t rows = 1;
    while ((line = strtok_r(NULL, "\n", &save))) {
        table_cells(line, cells + rows * count, count, NULL);
        rows++;
    }
    for (size_t i = 0; i < rows * count; i++) {
        cells[i] = table_inline(cells[i] ? cells[i] : "", i < count);
        formatted++;
        size_t c = i % count;
        int n = text_columns(cells[i]);
        if (n > wanted[c]) wanted[c] = n;
        if (!width[c]) width[c] = 1;
        size_t cell_length = strlen(cells[i]);
        for (size_t at = 0; at < cell_length;) {
            size_t sgr = sgr_length(cells[i] + at);
            if (sgr) { at += sgr; continue; }
            size_t end = next_grapheme(cells[i], cell_length, at);
            int wide = grapheme_columns(cells[i] + at, end - at);
            if (wide > width[c]) width[c] = wide;
            at = end;
        }
    }
    int available = columns - (int)(3 * count + 1);
    for (size_t c = 0; c < count; c++) available -= width[c];
    if (available < 0) {
        /* Very narrow terminals cannot fit even one glyph per column. */
        for (size_t r = rows > 1 ? 1 : 0; r < rows; r++) {
            for (size_t c = 0; c < count; c++) {
                text_buffer field = {0};
                buffer_append(&field, cells[c], strlen(cells[c]));
                if (r) {
                    buffer_append(&field, ": ", 2);
                    buffer_append(&field, cells[r * count + c], strlen(cells[r * count + c]));
                }
                display_lines parts = {0};
                conversation_wrap(&parts, CONVERSATION_NONE, 0, field.data, columns);
                for (size_t i = 0; i < parts.count; i++) {
                    buffer_append(&result, parts.items[i].text, strlen(parts.items[i].text));
                    buffer_append(&result, "\n", 1);
                }
                lines_free(&parts);
                buffer_free(&field);
            }
            if (r + 1 < rows) buffer_append(&result, "\n", 1);
        }
    } else {
        while (available > 0) {
            size_t needing = 0;
            for (size_t c = 0; c < count; c++) needing += wanted[c] > width[c];
            if (!needing) break;
            int share = available / (int)needing;
            if (!share) share = 1;
            for (size_t c = 0; c < count && available; c++) {
                int add = wanted[c] - width[c];
                if (add <= 0) continue;
                if (add > share) add = share;
                if (add > available) add = available;
                width[c] += add;
                available -= add;
            }
        }
        table_border(&result, width, count, "┌", "┬", "┐");
        for (size_t r = 0; r < rows; r++) {
            size_t height = 0;
            for (size_t c = 0; c < count; c++) {
                conversation_wrap(&wrapped[c], CONVERSATION_NONE, 0, cells[r * count + c], width[c]);
                if (wrapped[c].count > height) height = wrapped[c].count;
            }
            for (size_t y = 0; y < height; y++) {
                buffer_append(&result, STYLE_MUTED "│" STYLE_RESET, strlen(STYLE_MUTED "│" STYLE_RESET));
                for (size_t c = 0; c < count; c++) {
                    const char *text = y < wrapped[c].count ? wrapped[c].items[y].text : "";
                    int padding = width[c] - text_columns(text);
                    int left = align[c] > 0 ? padding : align[c] == 0 ? padding / 2 : 0;
                    for (int i = 0; i <= left; i++) buffer_append(&result, " ", 1);
                    buffer_append(&result, text, strlen(text));
                    for (int i = 0; i <= padding - left; i++) buffer_append(&result, " ", 1);
                    buffer_append(&result, STYLE_MUTED "│" STYLE_RESET, strlen(STYLE_MUTED "│" STYLE_RESET));
                }
                buffer_append(&result, "\n", 1);
                if (result.length > OCA_SNAPSHOT_MAX_BYTES) goto cleanup;
            }
            for (size_t c = 0; c < count; c++) lines_free(&wrapped[c]);
            if (!r && rows > 1) table_border(&result, width, count, "├", "┼", "┤");
        }
        table_border(&result, width, count, "└", "┴", "┘");
    }
    buffer_append(out, result.data, result.length);
    valid = 1;
cleanup:
    if (wrapped) for (size_t c = 0; c < count; c++) lines_free(&wrapped[c]);
    for (size_t i = 0; i < formatted; i++) free(cells[i]);
    free(data); free(head); free(cells); free(align); free(width); free(wanted); free(wrapped);
    buffer_free(&result);
    return valid;
}

static int table_block_start(const char *line) {
    size_t spaces = strspn(line, " \t");
    const char *p = line + spaces;
    if (spaces >= 4 || !*p || *p == '\n' || *p == '\r' || *p == '>') return 1;
    if (!strncmp(p, "```", 3) || !strncmp(p, "~~~", 3)) return 1;
    size_t hashes = strspn(p, "#");
    if (hashes && hashes <= 6 && (p[hashes] == ' ' || p[hashes] == '\t')) return 1;
    if (strchr("-+*", *p) && (p[1] == ' ' || p[1] == '\t')) return 1;
    size_t digits = strspn(p, "0123456789");
    return digits && (p[digits] == '.' || p[digits] == ')') && (p[digits + 1] == ' ' || p[digits + 1] == '\t');
}

static void markdown_flush_table(markdown_state *md, text_buffer *out) {
    if (!md->table.length) return;
    if (md->table_rows < 2 ||
        !markdown_table(md->table.data, md->columns > 0 ? md->columns : 80, out))
        markdown_prose(out, md->table.data, md->table.length);
    buffer_clear(&md->table);
    md->table_rows = 0;
}

static void markdown_line(markdown_state *md, text_buffer *out, const char *line,
                          size_t length) {
    char marker;
    size_t run;
    if (md->mode == MD_FENCED) {
        buffer_append(out, line, length);
        if (markdown_closing_fence(md, line, length)) {
            md->mode = MD_PROSE;
            md->fence = 0;
            md->fence_length = 0;
        }
        return;
    }
    if (markdown_fence(line, length, &marker, &run)) {
        markdown_flush_table(md, out);
        md->hint = 0;
        buffer_append(out, line, length);
        md->mode = MD_FENCED;
        md->fence = marker;
        md->fence_length = run;
        return;
    }
    if (md->table_rows == 1) {
        text_buffer candidate = {0}, preview = {0};
        buffer_append(&candidate, md->table.data, md->table.length);
        buffer_append(&candidate, line, length);
        int delimiter = markdown_table(candidate.data,
            md->columns > 0 ? md->columns : 80, &preview);
        buffer_free(&candidate);
        buffer_free(&preview);
        if (delimiter) {
            buffer_append(&md->table, line, length);
            md->table_rows = 2;
            return;
        }
        markdown_flush_table(md, out);
    }
    if (md->table_rows > 1) {
        if (!table_block_start(line)) {
            buffer_append(&md->table, line, length);
            md->table_rows++;
            return;
        }
        markdown_flush_table(md, out);
    }
    if (markdown_hint(md, out, line, length, 0)) return;
    int block_start = table_block_start(line);
    if (!block_start && memchr(line, '|', length)) {
        buffer_append(&md->table, line, length);
        md->table_rows = 1;
    } else markdown_prose(out, line, length);
}

static void markdown_feed(markdown_state *md, text_buffer *out, const char *text) {
    for (size_t i = 0; text[i]; i++) {
        buffer_append(&md->source_line, text + i, 1);
        if (text[i] == '\n') {
            markdown_line(md, out, md->source_line.data, md->source_line.length);
            buffer_clear(&md->source_line);
        }
    }
}

/* Preview pending source without changing the renderer state. */
static void markdown_preview(const markdown_state *md, text_buffer *out) {
    text_buffer source = {0};
    if (md->table.length) buffer_append(&source, md->table.data, md->table.length);
    if (md->source_line.length)
        buffer_append(&source, md->source_line.data, md->source_line.length);
    if (md->table_rows && source.length &&
        markdown_table(source.data, md->columns > 0 ? md->columns : 80, out)) {
        buffer_free(&source);
        return;
    }
    if (md->table.length)
        markdown_prose(out, md->table.data, md->table.length);
    if (md->source_line.length) {
        char marker;
        size_t run;
        if (md->mode == MD_FENCED ||
            markdown_fence(md->source_line.data, md->source_line.length,
                           &marker, &run))
            buffer_append(out, md->source_line.data, md->source_line.length);
        else {
            markdown_state state = *md;
            if (!markdown_hint(&state, out, md->source_line.data,
                               md->source_line.length, 1))
                markdown_prose(out, md->source_line.data, md->source_line.length);
        }
    }
    buffer_free(&source);
}

static void markdown_finish(markdown_state *md, text_buffer *out) {
    if (md->source_line.length)
        markdown_line(md, out, md->source_line.data, md->source_line.length);
    markdown_flush_table(md, out);
    buffer_free(&md->source_line);
    buffer_free(&md->table);
    memset(md, 0, sizeof(*md));
}

/* source_end counts normalized source bytes, including explicit newlines. */
static void conversation_wrap(display_lines *lines, conversation_kind kind, int continued,
                              const char *text, int cols) {
    if (cols < 1) cols = 1;
    int prefix_cols = kind == CONVERSATION_USER || kind == CONVERSATION_BASH ?
                      (cols >= 3 ? 2 : cols - 1) :
                      kind == CONVERSATION_LEGEND && cols > 1 ? 1 : 0;
    const char *style = kind == CONVERSATION_REASONING ? STYLE_REASONING : STYLE_RESET;
    size_t length = strlen(text), at = 0;
    char current_style[48] = "";
    int another = 1;
    while (another) {
        text_buffer line = {0};
        if (prefix_cols && kind == CONVERSATION_USER) {
            const char *mark = continued ? " " : STYLE_USER "›" STYLE_RESET;
            buffer_append(&line, mark, strlen(mark));
            if (prefix_cols == 2) buffer_append(&line, " ", 1);
        } else for (int i = 0; i < prefix_cols; i++) buffer_append(&line, " ", 1);
        size_t prefix_bytes = line.length;
        size_t source_start = at;
        size_t style_prefix = strlen(current_style);
        if (*current_style) buffer_append(&line, current_style, strlen(current_style));
        int row_continued = continued;
        int column = prefix_cols;
        while (at < length && text[at] != '\n') {
            size_t style_length = sgr_length(text + at);
            if (style_length) {
                memcpy(current_style, text + at, style_length);
                current_style[style_length] = 0;
                buffer_append(&line, text + at, style_length);
                at += style_length;
                continue;
            }
            size_t end = next_grapheme(text, length, at);
            int width = grapheme_columns(text + at, end - at);
            if (column > prefix_cols && column + width > cols) break;
            if (width > cols - prefix_cols) {
                buffer_append(&line, "?", 1);
                column++;
            } else {
                buffer_append(&line, text + at, end - at);
                column += width;
            }
            at = end;
        }
        size_t source_length = at - source_start;
        int soft_wrap = at < length && text[at] != '\n';
        another = at < length;
        if (at < length && text[at] == '\n') {
            at++;
            /* A final LF terminates Bash output; it does not add another row. */
            if (kind == CONVERSATION_BASH && at == length) another = 0;
        }
        lines_add(lines, style, line.data ? line.data : "", line.length);
        display_line *row = &lines->items[lines->count - 1];
        row->source_end = at;
        row->conversation_kind = kind;
        row->continued = row_continued;
        row->prefix_bytes = prefix_bytes;
        row->soft_wrap = soft_wrap;
        row->source = checked_realloc(NULL, style_prefix + source_length + 1);
        if (style_prefix) memcpy(row->source, line.data + prefix_bytes, style_prefix);
        memcpy(row->source + style_prefix, text + source_start, source_length);
        row->source[style_prefix + source_length] = 0;
        buffer_free(&line);
        continued = 1;
    }
}

static tui_app *main_view(tui_app *app) {
    return app->main_view ? app->main_view : app;
}

static void retained_wrap(display_lines *target, const display_line *row, int cols) {
    size_t first = target->count;
    if (row->conversation_kind)
        conversation_wrap(target, row->conversation_kind, row->continued,
                          row->text + row->prefix_bytes, cols);
    else lines_wrap(target, row->style, "", row->text, cols, SIZE_MAX, NULL, NULL);
    size_t consumed = row->prefix_bytes;
    for (size_t i = first; i < target->count; i++) {
        display_line *item = &target->items[i];
        item->style = row->style;
        item->fill = row->fill;
        size_t body = strlen(item->text) - item->prefix_bytes;
        if (row->accent_style && row->accent_end > consumed && row->accent_start < consumed + body) {
            item->accent_style = row->accent_style;
            item->accent_start = item->prefix_bytes + (row->accent_start > consumed ? row->accent_start - consumed : 0);
            item->accent_end = item->prefix_bytes + (row->accent_end < consumed + body ? row->accent_end - consumed : body);
        }
        consumed += body;
    }
}

static size_t retained_card(display_lines *target, const display_lines *history,
                            size_t first, size_t limit, int cols) {
    size_t end = first + 1;
    while (end < limit && history->items[end].card_role != 3) end++;
    if (end == limit) return first;
    display_lines body = {0};
    text_buffer logical = {0};
    const char *style = STYLE_RESET;
    conversation_kind kind = CONVERSATION_NONE;
    for (size_t i = first + 1; i < end; i++) {
        const display_line *row = &history->items[i];
        if (row->card_role != 2) break;
        if (!logical.length) {
            style = row->accent_style ? row->accent_style : STYLE_RESET;
            kind = row->conversation_kind;
        }
        const char *part = row->source ? row->source : row->text;
        buffer_append(&logical, part, strlen(part));
        if (!row->soft_wrap) {
            if (kind) conversation_wrap(&body, kind, 0, logical.data ? logical.data : "",
                                        question_width(cols));
            else question_wrap(&body, style, logical.data ? logical.data : "",
                               question_width(cols));
            buffer_clear(&logical);
        }
    }
    if (logical.length) {
        if (kind) conversation_wrap(&body, kind, 0, logical.data, question_width(cols));
        else question_wrap(&body, style, logical.data, question_width(cols));
    }
    question_card(target, &body, cols, 0);
    buffer_free(&logical);
    lines_free(&body);
    return end + 1;
}

static int retained_continues(const display_line *previous, const display_line *next) {
    return previous->soft_wrap && previous->conversation_kind && next->continued &&
           previous->conversation_kind == next->conversation_kind && !strcmp(previous->style, next->style);
}

/* Join physical slices before wrapping at the new width. Explicit newlines
 * and message boundaries remain separate records. */
static size_t retained_group(display_lines *target, const display_lines *history,
                             size_t first, size_t limit, int cols) {
    if (history->items[first].card_role == 1) {
        size_t end = retained_card(target, history, first, limit, cols);
        if (end != first) return end;
    }
    const display_line *row = &history->items[first];
    display_line joined = *row;
    text_buffer text = {0};
    buffer_append(&text, row->text, strlen(row->text));
    size_t end = first + 1;
    while (end < limit && retained_continues(&history->items[end - 1], &history->items[end])) {
        const display_line *next = &history->items[end++];
        size_t offset = text.length;
        if (next->accent_style && next->accent_end > next->prefix_bytes) {
            if (!joined.accent_style) {
                joined.accent_style = next->accent_style;
                joined.accent_start = offset + next->accent_start - next->prefix_bytes;
            }
            joined.accent_end = offset + next->accent_end - next->prefix_bytes;
        }
        buffer_append(&text, next->text + next->prefix_bytes, strlen(next->text + next->prefix_bytes));
    }
    joined.text = text.data;
    joined.source = NULL;
    retained_wrap(target, &joined, cols);
    buffer_free(&text);
    return end;
}

/* Only main-view output can enter the primary terminal scrollback. */
static void history_flush(tui_app *app) {
    tui_app *main = main_view(app);
    if (main->overlay || app->hidden || app->side_chat) return;
    if (app->flushed_rows == app->retained.count) return;
    tui_app *active = main->active_view ? main->active_view : main;
    erase_block(active);
    terminal_update_begin();
    if (app->history_omitted) {
        write_text("...[earlier display output omitted]\r\n");
        app->history_omitted = 0;
    }
    for (size_t i = app->flushed_rows; i < app->retained.count;) {
        display_lines lines = {0};
        i = retained_group(&lines, &app->retained, i, app->retained.count, active->cols);
        for (size_t k = 0; k < lines.count; k++) {
            write_display_line(&lines.items[k], active->cols);
            write_text("\r\n");
        }
        lines_free(&lines);
    }
    terminal_update_end();
    app->flushed_rows = app->retained.count;
    main->dirty = main->redraw_urgent = 1;
}

static size_t retained_row_bytes(const display_line *row) {
    return strlen(row->text) + 1 + (row->source ? strlen(row->source) + 1 : 0);
}

static void history_trim(tui_app *app, size_t bytes, size_t rows) {
    while (app->retained.count && (app->retained.count + rows > VIEW_HISTORY_MAX_ROWS ||
           app->retained_bytes > VIEW_HISTORY_MAX_BYTES ||
           bytes > VIEW_HISTORY_MAX_BYTES - app->retained_bytes)) {
        size_t first = 0, end = 0, count = app->retained.count / 4 + 1;
        while (end < app->retained.count && end < count) {
            app->retained_bytes -= retained_row_bytes(&app->retained.items[end]);
            free(app->retained.items[end].text);
            free(app->retained.items[end].source);
            end++;
        }
        while (end < app->retained.count && app->retained.items[end].card_role &&
               app->retained.items[end].card_role != 1) {
            int bottom = app->retained.items[end].card_role == 3;
            app->retained_bytes -= retained_row_bytes(&app->retained.items[end]);
            free(app->retained.items[end].text);
            free(app->retained.items[end].source);
            end++;
            if (bottom) break;
        }
        size_t drop = end - first;
        if (end > app->flushed_rows) app->history_omitted = 1;
        memmove(app->retained.items + first, app->retained.items + end,
                (app->retained.count - end) * sizeof(*app->retained.items));
        app->retained.count -= drop;
        if (app->flushed_rows > first) app->flushed_rows = app->flushed_rows >= end ? app->flushed_rows - drop : first;
        if (app->view_end > first) app->view_end = app->view_end >= end ? app->view_end - drop : first;
    }
}

static void history_retain(tui_app *app, const display_line *row) {
    text_buffer original = {0};
    if (row->source && !row->card_role) {
        buffer_append(&original, row->text, row->prefix_bytes);
        buffer_append(&original, row->source, strlen(row->source));
    }
    const char *text = original.data ? original.data : row->text;
    size_t text_bytes = strlen(text) + 1;
    size_t bytes = text_bytes +
        (row->card_role && row->source ? strlen(row->source) + 1 : 0);
    if (bytes > VIEW_HISTORY_MAX_BYTES) {
        app->history_omitted = 1;
        buffer_free(&original);
        return;
    }
    int following = app->view_end == app->retained.count;
    history_trim(app, bytes, 1);
    lines_add(&app->retained, row->style, text, text_bytes - 1);
    display_line *copy = &app->retained.items[app->retained.count - 1];
    copy->fill = row->fill;
    copy->conversation_kind = row->conversation_kind;
    copy->continued = row->continued;
    copy->soft_wrap = row->soft_wrap;
    copy->prefix_bytes = row->prefix_bytes;
    copy->accent_start = row->accent_start;
    copy->accent_end = row->accent_end;
    copy->accent_style = row->accent_style;
    copy->accent2_start = row->accent2_start;
    copy->accent2_end = row->accent2_end;
    copy->accent2_style = row->accent2_style;
    copy->accent3_start = row->accent3_start;
    copy->accent3_end = row->accent3_end;
    copy->accent3_style = row->accent3_style;
    copy->accent4_start = row->accent4_start;
    copy->accent4_end = row->accent4_end;
    copy->accent4_style = row->accent4_style;
    copy->card_role = row->card_role;
    if (row->card_role && row->source) copy->source = copy_text(row->source);
    app->retained_bytes += bytes;
    if (following) app->view_end = app->retained.count;
    buffer_free(&original);
}

static void history_emit(tui_app *app, const display_lines *lines, size_t count, int gap) {
    int visible = !app->hidden && !main_view(app)->overlay;
    int omit_card = 0;
    if (count && lines->items[0].card_role == 1) {
        size_t bytes = 0;
        for (size_t i = 0; i < count; i++) {
            size_t row_bytes = strlen(lines->items[i].text) + 1 +
                (lines->items[i].source ? strlen(lines->items[i].source) + 1 : 0);
            if (row_bytes > VIEW_HISTORY_MAX_BYTES - bytes) {
                omit_card = 1;
                break;
            }
            bytes += row_bytes;
        }
        if (count > VIEW_HISTORY_MAX_ROWS) omit_card = 1;
        if (omit_card) app->history_omitted = 1;
        else history_trim(app, bytes, count);
    }
    if (visible) {
        terminal_update_begin();
        clear_block(app);
    }
    for (size_t i = 0; i < count; i++) {
        if (!omit_card) history_retain(app, &lines->items[i]);
        if (visible) {
            write_display_line(&lines->items[i], app->cols);
            write_text("\r\n");
            app->flushed_rows = app->retained.count;
        }
    }
    while (gap-- > 0) {
        display_line blank = { .text = "", .style = STYLE_RESET };
        history_retain(app, &blank);
        if (visible) {
            write_text("\r\n");
            app->flushed_rows = app->retained.count;
        }
    }
    if (visible) terminal_update_end();
    app->dirty = app->redraw_urgent = 1;
}

/* Keep only the unfinished physical row in the live block. Completed rows
 * enter terminal history before another redraw can hide them. */
static void stream_commit(tui_app *app, int finish) {
    if (!app->stream_kind) return;
    if (finish && app->stream_kind == CONVERSATION_ASSISTANT) {
        text_buffer end = {0};
        markdown_finish(&app->markdown, &end);
        if (end.length) append_text(&app->stream_text, &app->stream_text_len, end.data);
        buffer_free(&end);
    }
    refresh_geometry(app);
    display_lines lines = {0};
    if (app->stream_text_len)
        conversation_wrap(&lines, app->stream_kind, app->stream_continued,
                          app->stream_text, app->cols);
    size_t count = lines.count;
    int bash_line_ended = app->stream_kind == CONVERSATION_BASH && app->stream_text_len &&
                          app->stream_text[app->stream_text_len - 1] == '\n';
    if (!finish && count && !bash_line_ended) count--;
    if (count || finish) {
        history_emit(app, &lines, count, finish && app->stream_kind != CONVERSATION_BASH ? 2 : 0);
        if (count) {
            size_t consumed = lines.items[count - 1].source_end;
            char style[48] = "";
            for (size_t at = 0; at < consumed;) {
                size_t length = sgr_length(app->stream_text + at);
                if (length) {
                    memcpy(style, app->stream_text + at, length);
                    style[length] = 0;
                }
                at += length ? length : 1;
            }
            if (!strcmp(style, STYLE_RESET)) style[0] = 0;
            size_t prefix = strlen(style), remaining = app->stream_text_len - consumed;
            char *next = checked_realloc(NULL, prefix + remaining + 1);
            memcpy(next, style, prefix);
            memcpy(next + prefix, app->stream_text + consumed, remaining + 1);
            free(app->stream_text);
            app->stream_text = next;
            app->stream_text_len = prefix + remaining;
            app->stream_continued = 1;
        }
        app->dirty = 1;
    }
    if (finish) {
        free(app->stream_text);
        app->stream_text = NULL;
        app->stream_text_len = 0;
        app->stream_kind = CONVERSATION_NONE;
        app->stream_continued = 0;
    }
    lines_free(&lines);
}

static void conversation_print(tui_app *app, conversation_kind kind, const char *text) {
    stream_commit(app, 1);
    refresh_geometry(app);
    char *safe = conversation_safe(text);
    if (kind == CONVERSATION_ASSISTANT) {
        markdown_state md = {.columns = app->cols};
        text_buffer formatted = {0};
        markdown_feed(&md, &formatted, safe);
        markdown_finish(&md, &formatted);
        free(safe);
        safe = formatted.data ? formatted.data : copy_text("");
    }
    display_lines lines = {0};
    conversation_wrap(&lines, kind, 0, safe, app->cols);
    history_emit(app, &lines, lines.count, 2);
    lines_free(&lines);
    free(safe);
    app->dirty = 1;
    app->redraw_urgent = 1;
}

static const command_info *command_for_input(tui_app *app) {
    const char *name = app->input;
    if (app->menu == MENU_COMMANDS && app->menu_selection < app->menu_count)
        name = app->menu_items[app->menu_selection].value;
    if (!name) return NULL;
    size_t length = strcspn(name, " \n");
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++)
        if (command_visible(app, &COMMANDS[i]) && strlen(COMMANDS[i].name) == length &&
            !strncmp(name, COMMANDS[i].name, length))
            return &COMMANDS[i];
    return NULL;
}

static char *command_hint(tui_app *app) {
    const command_info *command = command_for_input(app);
    if (!command || !app->input_len || app->input[0] != '/') return NULL;
    size_t name_length = strlen(command->name);
    if (app->input_len > name_length &&
        app->input[name_length + strspn(app->input + name_length, " \t\r\n")]) return NULL;
    const char *remaining = app->input_len < name_length ? command->name + app->input_len : "";
    const char *syntax = command->syntax;
    if (app->input_len > name_length && *syntax == ' ') syntax++;
    size_t length = strlen(remaining) + strlen(syntax) + 1;
    char *hint = checked_realloc(NULL, length);
    snprintf(hint, length, "%s%s", remaining, syntax);
    return hint;
}

static void frame_border(display_lines *lines, const char *left, const char *right, int cols) {
    text_buffer row = {0};
    buffer_append(&row, left, strlen(left));
    for (int i = 0; i < cols - 2; i++) buffer_append(&row, "─", strlen("─"));
    buffer_append(&row, right, strlen(right));
    lines_add(lines, STYLE_ACCENT, row.data, row.length);
    buffer_free(&row);
}

static void overlay_history(tui_app *app, display_lines *page, size_t rows) {
    size_t end = app->view_end < app->retained.count ? app->view_end : app->retained.count;
    size_t first = end, height = 0;
    while (first && height < rows) {
        display_lines wrapped = {0};
        size_t last = first--;
        while (first && app->retained.items[first].card_role &&
               app->retained.items[first].card_role != 1) first--;
        while (first && retained_continues(&app->retained.items[first - 1], &app->retained.items[first])) first--;
        retained_group(&wrapped, &app->retained, first, last, app->cols);
        height += wrapped.count;
        lines_free(&wrapped);
    }
    if (!first && app->history_omitted)
        lines_wrap(page, STYLE_MUTED, "", "...[earlier display output omitted]", app->cols, SIZE_MAX, NULL, NULL);
    for (size_t i = first; i < end;) i = retained_group(page, &app->retained, i, end, app->cols);
}

static void build_composer(tui_app *app, display_lines *composer, int content_cols,
                            int *cursor_row, int *cursor_col) {
    int save_question = app->menu == MENU_SAVE || app->menu == MENU_SAVE_CONTINUE;
    const char *input_style = app->invalid_input ? STYLE_DANGER :
                              app->input_len ? STYLE_RESET : STYLE_SUBTLE;
    if (save_question) {
        char question[96];
        snprintf(question, sizeof(question), "%s (y/n) %s",
                 app->menu == MENU_SAVE ? "Save current session?" : "Continue anyway?", app->save_input);
        lines_wrap(composer, STYLE_RESET, "", question, content_cols,
                   strlen(question), cursor_row, cursor_col);
    } else composer_wrap(app, composer, input_style, content_cols, cursor_row, cursor_col);
    if (!save_question && app->menu != MENU_INTERACTION &&
        app->input_len && app->input[0] == '/') {
        char *hint = command_hint(app);
        if (hint && *hint && composer->count) {
            display_line *row = &composer->items[composer->count - 1];
            int room = content_cols - text_columns(row->text);
            if (room > 0) {
                char *clipped = safe_clip(hint, room);
                size_t length = strlen(row->text), extra = strlen(clipped);
                row->text = checked_realloc(row->text, length + extra + 1);
                memcpy(row->text + length, clipped, extra + 1);
                row->accent_start = length;
                row->accent_end = length + extra;
                row->accent_style = app->invalid_input ? STYLE_DANGER : STYLE_SUBTLE;
                free(clipped);
            }
        }
        free(hint);
    }
}

static size_t menu_columns(const tui_app *app) {
    if (app->menu != MENU_HELP) return 1;
    int width = 0;
    for (size_t i = 0; i < app->menu_count; i++) {
        int columns = text_columns(app->menu_items[i].label);
        if (columns > width) width = columns;
    }
    return width && app->cols / 2 > width ? 2 : 1;
}

static size_t append_menu(tui_app *app, display_lines *lines, size_t menu_rows,
                          size_t menu_count, size_t columns) {
    if (app->menu == MENU_INTERACTION) return 0;
    if (app->menu == MENU_HELP && menu_count && app->menu_selection >= menu_count)
        app->menu_selection = menu_count - 1;
    size_t first_menu = app->menu_first;
    if (first_menu + menu_rows > menu_count) first_menu = menu_count - menu_rows;
    if (menu_rows) {
        if (app->menu_selection < first_menu) first_menu = app->menu_selection;
        else if (app->menu_selection >= first_menu + menu_rows)
            first_menu = app->menu_selection - menu_rows + 1;
        if (first_menu + menu_rows > menu_count) first_menu = menu_count - menu_rows;
        app->menu_first = first_menu;
    }
    for (size_t i = 0; i < menu_rows; i++) {
        size_t at = first_menu + i;
        if (app->menu == MENU_HELP) {
            text_buffer text = {0};
            size_t starts[2] = {0}, ends[2] = {0};
            for (size_t column = 0; column < columns; column++) {
                size_t item = at * columns + column;
                if (item >= app->menu_count) break;
                int width = app->cols / (int)columns;
                char *part = safe_clip(app->menu_items[item].label, width - (columns > 1));
                if (strlen(part) > 1) {
                    starts[column] = text.length + 1;
                    ends[column] = starts[column] + strcspn(part + 1, " ");
                }
                buffer_append(&text, part, strlen(part));
                if (column + 1 < columns) {
                    int spaces = width - text_columns(part);
                    while (spaces-- > 0) buffer_append(&text, " ", 1);
                }
                free(part);
            }
            lines_add(lines, STYLE_MUTED, text.data ? text.data : "", text.length);
            display_line *row = &lines->items[lines->count - 1];
            row->accent_start = starts[0]; row->accent_end = ends[0];
            row->accent2_start = starts[1]; row->accent2_end = ends[1];
            if (ends[0]) row->accent_style = STYLE_INLINE;
            if (ends[1]) row->accent2_style = STYLE_INLINE;
            buffer_free(&text);
        } else {
            const char *style = at == app->menu_selection ? STYLE_SELECTED : STYLE_MUTED;
            lines_add_menu(lines, style, at == app->menu_selection ? "› " : "  ",
                           app->menu_items[at].label, app->cols);
        }
    }
    return first_menu;
}

static void render_block(tui_app *app) {
    refresh_geometry(app);
    stream_commit(app, 0);
    display_lines stream = {0};
    text_buffer preview = {0};
    if (app->stream_text_len) buffer_append(&preview, app->stream_text, app->stream_text_len);
    if (app->stream_kind == CONVERSATION_ASSISTANT) {
        app->markdown.columns = app->cols;
        markdown_preview(&app->markdown, &preview);
    }
    if (preview.length)
        conversation_wrap(&stream, app->stream_kind, app->stream_continued,
                          preview.data, app->cols);
    buffer_free(&preview);
    if (app->tool_name && !app->tool_streamed_bytes && strcmp(app->tool_name, "ask_user")) {
        tool_action_lines(app, &stream, TOOL_RUNNING, NULL);
    }
    oca_core_status status;
    oca_core_get_status(app->core, &status);
    display_lines control = {0};
    size_t queue_legend_rows = 0;
    if (status.followup_count) {
        char queued[1200];
        snprintf(queued, sizeof(queued), "%s", status.followup_preview);
        for (char *at = queued; *at; at++) if (*at == '\n' || *at == '\r' || *at == '\t') *at = ' ';
        display_lines preview_rows = {0};
        char *safe = conversation_safe(queued);
        text_buffer labeled = {0};
        buffer_append(&labeled, " " STYLE_BOLD "queued:" STYLE_RESET " ",
                      strlen(" " STYLE_BOLD "queued:" STYLE_RESET " "));
        buffer_append(&labeled, safe, strlen(safe));
        conversation_wrap(&preview_rows, CONVERSATION_NONE, 0, labeled.data, app->cols);
        buffer_free(&labeled);
        free(safe);
        size_t preview_limit = app->cols < 40 ? 1 : 3;
        lines_copy_range(&control, &preview_rows, 0,
                         preview_rows.count < preview_limit ? preview_rows.count : preview_limit);
        lines_free(&preview_rows);
        char legend[192];
        int remove_available = !app->input_len && !app->side_chat &&
                               app->menu != MENU_SAVE && app->menu != MENU_SAVE_CONTINUE;
        snprintf(legend, sizeof(legend),
                 STYLE_INLINE "Esc" STYLE_MUTED ": %s%s"
                 STYLE_INLINE "Ctrl+X" STYLE_MUTED ": %s%s%s",
                 app->cols < 40 ? "interrupt/send" : "interrupt and send",
                 app->cols < 40 ? "\n" : " · ",
                 status.followup_count > 1 ? "edit first" : "edit",
                 !remove_available ? "" : app->cols < 40 ? "\n" : " · ",
                 remove_available ? STYLE_INLINE "Ctrl+C" STYLE_MUTED ": remove" : "");
        if (app->menu != MENU_INTERACTION) {
            size_t first_legend = control.count;
            conversation_wrap(&control, CONVERSATION_LEGEND, 0, legend, app->cols);
            queue_legend_rows = control.count - first_legend;
        }
    }
    int question = app->menu == MENU_INTERACTION;
    int choices = question && !app->interaction_details;
    int question_cursor_row = 0, question_cursor_col = 0;
    if (question) {
        if (stream.count) lines_add(&stream, STYLE_RESET, "", 0);
        question_lines(app, &stream, &question_cursor_row, &question_cursor_col);
    }
    if (!question && main_view(app)->overlay && app->view_end < app->retained.count)
        lines_free(&stream);
    char *notice = shared_row(app);
    int status_rows = app->rows >= 2 ? 1 : 0;
    int notice_rows = notice && app->rows >= 3 && (!question || app->notice[0]) ? 1 : 0;
    size_t available = (size_t)(app->rows - status_rows - notice_rows);
    int save_question = app->menu == MENU_SAVE || app->menu == MENU_SAVE_CONTINUE;
    int border = !question && !save_question && app->cols >= 8 && available >= 3;
    int content_cols = app->cols - (border ? 4 : 0);
    display_lines composer = {0};
    int composer_cursor_row = 0, cursor_col = 0;
    if (!question) build_composer(app, &composer, content_cols, &composer_cursor_row, &cursor_col);
    size_t content_available = available - (border ? 2 : 0);
    size_t composer_rows = composer.count < content_available ? composer.count : content_available;
    if (control.count && content_available > 3 && composer_rows > content_available - 3)
        composer_rows = content_available - 3;
    size_t columns = menu_columns(app);
    size_t menu_count = question ? 0 : (app->menu_count + columns - 1) / columns;
    size_t menu_rows = menu_count;
    if (app->menu != MENU_HELP && menu_rows > MENU_ROWS_MAX) menu_rows = MENU_ROWS_MAX;
    size_t menu_capacity = content_available - composer_rows;
    if (menu_rows > menu_capacity) menu_rows = menu_capacity;
    int menu_footer = menu_count > menu_rows && menu_rows > 0;
    if (menu_footer && menu_rows == menu_capacity) {
        if (menu_rows > 1) menu_rows--;
        else menu_footer = 0;
    }
    size_t remaining = menu_capacity - menu_rows - (size_t)menu_footer;
    size_t control_rows = control.count;
    if (control_rows > remaining - (remaining && stream.count ? 1u : 0u))
        control_rows = remaining - (remaining && stream.count ? 1u : 0u);
    size_t gap_rows = remaining > control_rows + (question && stream.count ? 1u : 0u) &&
        (stream.count || app->retained.count || control_rows) ? 1 : 0;
    size_t stream_rows = remaining - control_rows - gap_rows;
    if (stream_rows > stream.count) stream_rows = stream.count;
    size_t composer_first = 0;
    if (composer_rows < composer.count) {
        if ((size_t)composer_cursor_row >= composer_rows)
            composer_first = (size_t)composer_cursor_row - composer_rows + 1;
        if (composer_first + composer_rows > composer.count)
            composer_first = composer.count - composer_rows;
    }
    display_lines lines = {0};
    size_t stream_first = stream.count - stream_rows;
    if (question && !choices && stream_rows && (size_t)question_cursor_row < stream_first)
        stream_first = (size_t)question_cursor_row;
    if (question && choices && stream_rows) {
        size_t selected = stream_first;
        for (size_t i = 0; i < stream.count; i++) {
            if (stream.items[i].card_role == 2 && stream.items[i].accent_style &&
                !strcmp(stream.items[i].accent_style, STYLE_SELECTED)) {
                selected = i;
                break;
            }
        }
        stream_first = selected >= stream_rows ? selected - stream_rows + 1 : 0;
        if (stream_first + stream_rows > stream.count) stream_first = stream.count - stream_rows;
    }
    if (stream_rows) lines_copy_range(&lines, &stream, stream_first, stream_rows);
    if (gap_rows) lines_add(&lines, STYLE_RESET, "", 0);
    if (control_rows) {
        if (control_rows < control.count && queue_legend_rows && control_rows >= queue_legend_rows) {
            lines_copy_range(&lines, &control, 0, control_rows - queue_legend_rows);
            lines_copy_range(&lines, &control, control.count - queue_legend_rows, queue_legend_rows);
        } else lines_copy_range(&lines, &control, 0, control_rows);
    }
    if (notice_rows) {
        lines_add(&lines, app->exit_hint ? STYLE_EXIT : STYLE_MUTED, notice, strlen(notice));
        char *key = strstr(notice, "(Esc to interrupt)");
        if (key) {
            display_line *row = &lines.items[lines.count - 1];
            row->accent_start = (size_t)(key - notice) + 1;
            row->accent_end = row->accent_start + 3;
            row->accent_style = STYLE_INLINE;
        }
    }
    if (border) frame_border(&lines, "╭", "╮", app->cols);
    int desired = (int)lines.count + composer_cursor_row - (int)composer_first;
    if (question) {
        desired = choices ? 0 : question_cursor_row - (int)stream_first;
        if (desired < 0) desired = 0;
        if ((size_t)desired >= stream_rows) desired = stream_rows ? (int)stream_rows - 1 : 0;
        cursor_col = question_cursor_col;
    }
    for (size_t i = 0; i < composer_rows; i++) {
        const display_line *source = &composer.items[composer_first + i];
        size_t prefix = 0;
        if (border) {
            text_buffer row = {0};
            buffer_append(&row, STYLE_ACCENT "│ ", strlen(STYLE_ACCENT "│ "));
            buffer_append(&row, source->style, strlen(source->style));
            prefix = row.length;
            buffer_append(&row, source->text, strlen(source->text));
            int padding = content_cols - text_columns(source->text);
            while (padding-- > 0) buffer_append(&row, " ", 1);
            buffer_append(&row, STYLE_ACCENT " │" STYLE_RESET, strlen(STYLE_ACCENT " │" STYLE_RESET));
            lines_add(&lines, source->style, row.data, row.length);
            buffer_free(&row);
        } else lines_add(&lines, source->style, source->text, strlen(source->text));
        display_line *target = &lines.items[lines.count - 1];
        target->accent_style = source->accent_style;
        target->accent_start = source->accent_start + prefix;
        target->accent_end = source->accent_end + prefix;
    }
    if (border) {
        frame_border(&lines, "╰", "╯", app->cols);
        cursor_col += 2;
    }
    size_t first_menu = append_menu(app, &lines, menu_rows, menu_count, columns);
    if (menu_footer) {
        text_buffer footer = {0};
        int below = first_menu + menu_rows < menu_count;
        buffer_append(&footer, "  ", 2);
        if (first_menu) buffer_append(&footer, "↑", strlen("↑"));
        if (below) buffer_append(&footer, "↓", strlen("↓"));
        buffer_append(&footer, " ", 1);
        size_t from = first_menu * columns + 1;
        size_t to = (first_menu + menu_rows) * columns;
        if (to > app->menu_count) to = app->menu_count;
        char count[80];
        snprintf(count, sizeof(count), "%zu–%zu of %zu", from, to, app->menu_count);
        buffer_append(&footer, count, strlen(count));
        if (text_columns(footer.data) > app->cols) {
            buffer_clear(&footer);
            if (first_menu && (app->cols > 1 || !below))
                buffer_append(&footer, "↑", strlen("↑"));
            if (first_menu && below && app->cols > 2)
                buffer_append(&footer, " ", 1);
            if (below) buffer_append(&footer, "↓", strlen("↓"));
        }
        lines_add(&lines, STYLE_MUTED, footer.data, footer.length);
        display_line *row = &lines.items[lines.count - 1];
        char *arrow = strstr(footer.data, "↑");
        if (arrow) {
            row->accent_start = (size_t)(arrow - footer.data);
            row->accent_end = row->accent_start + strlen("↑");
            row->accent_style = STYLE_INLINE;
        }
        arrow = strstr(footer.data, "↓");
        if (arrow) {
            row->accent2_start = (size_t)(arrow - footer.data);
            row->accent2_end = row->accent2_start + strlen("↓");
            row->accent2_style = STYLE_INLINE;
        }
        buffer_free(&footer);
    }
    if (status_rows) {
        size_t right_start;
        char *status_line = status_text(app, &right_start);
        lines_add_full(&lines, STYLE_MUTED, status_line, strlen(status_line), 0);
        display_line *row = &lines.items[lines.count - 1];
        char *view = app->side_chat ? strstr(status_line, "Temporary chat (") :
                     app->side_view ? strstr(status_line, "Main chat (") : NULL;
        char *key = view ? strstr(view, "Ctrl+/") : NULL;
        if (key) {
            row->accent_start = (size_t)(key - status_line);
            row->accent_end = row->accent_start + strlen("Ctrl+/");
            row->accent_style = STYLE_INLINE;
        }
        key = app->side_chat && view ? strstr(view, "Ctrl+C") : NULL;
        if (key) {
            row->accent2_start = (size_t)(key - status_line);
            row->accent2_end = row->accent2_start + strlen("Ctrl+C");
            row->accent2_style = STYLE_INLINE;
        }
        char *at = oca_core_hints_enabled(app->core) && right_start != SIZE_MAX ?
             strstr(status_line + right_start, "hints") : NULL;
        if (at) {
            row->accent3_start = (size_t)(at - status_line);
            row->accent3_end = row->accent3_start + 5;
            row->accent3_style = STYLE_HINT_TEXT;
        }
        const char *goal = goal_name(status.goal);
        at = goal && right_start != SIZE_MAX ? strstr(status_line + right_start, goal) : NULL;
        if (at) {
            row->accent4_start = (size_t)(at - status_line);
            row->accent4_end = strlen(status_line);
            row->accent4_style = STYLE_PURPLE;
        }
        free(status_line);
    }
    terminal_update_begin();
    int history_rows = 0;
    if (main_view(app)->overlay) {
        history_rows = app->rows - (int)lines.count;
        display_lines page = {0};
        overlay_history(app, &page, (size_t)history_rows);
        size_t shown = page.count < (size_t)history_rows ? page.count : (size_t)history_rows;
        write_text("\x1b[H\x1b[2J");
        for (int i = 0; i < history_rows; i++) {
            if ((size_t)i >= (size_t)history_rows - shown)
                write_display_line(&page.items[page.count - shown + (size_t)i - ((size_t)history_rows - shown)], app->cols);
            write_text("\r\n");
        }
        lines_free(&page);
        desired += history_rows;
    } else clear_block(app);
    for (size_t i = 0; i < lines.count; i++) {
        write_display_line(&lines.items[i], app->cols);
        if (i + 1 < lines.count) write_text("\r\n");
    }
    int printed = history_rows + (int)lines.count;
    int up = printed - 1 - desired;
    if (up > 0) {
        char move[32];
        snprintf(move, sizeof(move), "\x1b[%dA", up);
        write_text(move);
    }
    write_text("\r");
    if (cursor_col > 0) {
        char move[32];
        snprintf(move, sizeof(move), "\x1b[%dC", cursor_col);
        write_text(move);
    }
    terminal_update_end();
    if (choices) write_text("\x1b[?25l");
    app->block_lines = printed;
    app->block_cursor_row = desired;
    app->dirty = 0;
    app->redraw_urgent = 0;
    app->last_render_ms = monotonic_ms();
    free(notice);
    lines_free(&stream);
    lines_free(&control);
    lines_free(&composer);
    lines_free(&lines);
}

static void transcript_literal(tui_app *app, const char *style, const char *text, int gap) {
    stream_commit(app, 1);
    refresh_geometry(app);
    display_lines lines = {0};
    lines_wrap(&lines, style, "", text, app->cols, SIZE_MAX, NULL, NULL);
    history_emit(app, &lines, lines.count, gap);
    lines_free(&lines);
    app->dirty = app->redraw_urgent = 1;
}

/* Copy only the bounded count annotation, never the result body. */
static void append_change_counts(text_buffer *buffer, const char *result) {
    const char *edit_prefix = "Edit applied. (", *write_prefix = "File written. (";
    const char *plus = NULL;
    if (result && !strncmp(result, edit_prefix, strlen(edit_prefix))) plus = result + strlen(edit_prefix);
    else if (result && !strncmp(result, write_prefix, strlen(write_prefix))) plus = result + strlen(write_prefix);
    if (!plus) return;
    if (*plus != '+') return;
    const char *minus = plus + 1 + strspn(plus + 1, "0123456789");
    if (minus == plus + 1 || *minus != ' ' || minus[1] != '-') return;
    minus++;
    const char *end = minus + 1 + strspn(minus + 1, "0123456789");
    if (end == minus + 1 || *end != ')' || (end[1] && end[1] != '\n') ||
        end - (plus - 1) >= 80) return;
    buffer_append(buffer, " (", 2);
    buffer_append(buffer, STYLE_SELECTED, strlen(STYLE_SELECTED));
    buffer_append(buffer, plus, (size_t)(minus - plus - 1));
    buffer_append(buffer, STYLE_RESET, strlen(STYLE_RESET));
    buffer_append(buffer, " ", 1);
    buffer_append(buffer, STYLE_DIFF_REMOVE, strlen(STYLE_DIFF_REMOVE));
    buffer_append(buffer, minus, (size_t)(end - minus));
    buffer_append(buffer, STYLE_RESET, strlen(STYLE_RESET));
    buffer_append(buffer, end, 1);
}

static int tool_body_visible(const tui_app *app, const char *name) {
    if (!strcmp(name, "read")) return app->display.show_read;
    if (!strcmp(name, "write")) return app->display.show_write;
    if (!strcmp(name, "edit")) return app->display.show_edit;
    if (!strcmp(name, "bash")) return app->display.show_bash;
    return 0;
}

static void tool_diff_lines(tui_app *app, display_lines *lines, const char *result) {
    const char *body = result ? strchr(result, '\n') : NULL;
    if (!body || !*++body) return;
    char *safe = conversation_safe(body);
    for (const char *at = safe; *at;) {
        size_t n = strcspn(at, "\n"), digits = strspn(at, "0123456789");
        text_buffer line = {0};
        const char *style = STYLE_RESET;
        int fill = 0;
        if (digits && digits + 2 < n && at[digits] == ' ' &&
            (at[digits + 1] == '+' || at[digits + 1] == '-' || at[digits + 1] == ' ')) {
            style = at[digits + 1] == '+' ? STYLE_DIFF_ADD :
                    at[digits + 1] == '-' ? STYLE_DIFF_DELETE : STYLE_RESET;
            fill = at[digits + 1] != ' ';
            buffer_append(&line, style, strlen(style));
            buffer_append(&line, STYLE_MUTED, strlen(STYLE_MUTED));
            buffer_append(&line, at, digits);
            buffer_append(&line, style, strlen(style));
            buffer_append(&line, at + digits, 1);
            const char *sign = at[digits + 1] == '+' ? STYLE_SELECTED :
                               at[digits + 1] == '-' ? STYLE_DIFF_REMOVE : "";
            buffer_append(&line, sign, strlen(sign));
            buffer_append(&line, at + digits + 1, 1);
            buffer_append(&line, style, strlen(style));
            buffer_append(&line, " ", 1);
            buffer_append(&line, at + digits + 2, n - digits - 2);
        } else if (!n) {
            style = STYLE_MUTED;
            buffer_append(&line, "⋮", strlen("⋮"));
        } else buffer_append(&line, at, n);
        size_t first = lines->count;
        conversation_wrap(lines, CONVERSATION_DIFF, 0, line.data ? line.data : "", app->cols);
        for (size_t i = first; i < lines->count; i++) {
            lines->items[i].style = style;
            lines->items[i].fill = fill;
        }
        buffer_free(&line);
        at += n;
        if (*at) at++;
    }
    free(safe);
}

static void tool_action_lines(tui_app *app, display_lines *lines, int state, const char *result) {
    const char *name = app->tool_name ? app->tool_name : "tool";
    if (!strcmp(name, "goal_status") || (!strcmp(name, "ask_user") && state <= OCA_TOOL_COMPLETE)) return;
    cJSON *args = cJSON_Parse(app->tool_arguments ? app->tool_arguments : "{}");
    const char *label = name;
    /* Tool colors identify the operation; result states use the row style. */
    const char *accent = !strcmp(name, "bash") ? STYLE_TOOL_BASH :
                         (!strcmp(name, "write") || !strcmp(name, "edit")) ? STYLE_TOOL_FILE :
                         STYLE_BOLD;
    int bracketed = 1;
    text_buffer row = {0};
    if (bracketed) buffer_append(&row, "[", 1);
    buffer_append(&row, label, strlen(label));
    if (bracketed) buffer_append(&row, "]", 1);
    const char *key = !strcmp(name, "bash") ? "command" : "path";
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, key));
    if (value) { buffer_append(&row, " ", 1); buffer_append(&row, value, strlen(value)); }
    if (!strcmp(name, "read")) {
        const cJSON *offset = cJSON_GetObjectItemCaseSensitive(args, "offset"),
                    *limit = cJSON_GetObjectItemCaseSensitive(args, "limit");
        if (cJSON_IsNumber(offset) || cJSON_IsNumber(limit)) {
            double number = cJSON_IsNumber(offset) ? offset->valuedouble : 1;
            long first = number >= (double)LONG_MAX ? LONG_MAX : number >= 1 ? (long)number : 1;
            long count = cJSON_IsNumber(limit) && limit->valuedouble >= 1 && limit->valuedouble < (double)LONG_MAX ?
                         (long)limit->valuedouble : 0;
            char range[96];
            if (count && first <= LONG_MAX - count + 1) snprintf(range, sizeof(range), " (lines %ld-%ld)", first, first + count - 1);
            else snprintf(range, sizeof(range), " (from line %ld)", first);
            buffer_append(&row, range, strlen(range));
        }
    }
    int diff = !strcmp(name, "write") || !strcmp(name, "edit");
    if (state == OCA_TOOL_ERROR || state == OCA_TOOL_INTERRUPTED) {
        const char *status = state == OCA_TOOL_INTERRUPTED ? " (interrupted" : " (failed";
        buffer_append(&row, status, strlen(status));
        buffer_append(&row, ")", 1);
    }
    /* Escape the tool text first. The count annotation carries terminal
     * controls and must not pass through escaping. */
    char *safe = conversation_safe(row.data ? row.data : label);
    text_buffer header = {0};
    buffer_append(&header, safe, strlen(safe));
    free(safe);
    if (diff && state == OCA_TOOL_COMPLETE) append_change_counts(&header, result);
    size_t first_row = lines->count;
    conversation_wrap(lines, CONVERSATION_ACTION, 0, header.data ? header.data : "", app->cols);
    if (lines->count > first_row) {
        display_line *head = &lines->items[first_row];
        head->accent_style = accent;
        head->accent_start = 0;
        head->accent_end = strlen(label) + (bracketed ? 2u : 0u);
    }

    buffer_free(&header);
    buffer_free(&row);
    cJSON_Delete(args);
}

static void tool_start(tui_app *app, const oca_core_event *event) {
    if (event->name && !strcmp(event->name, "goal_status")) return;
    if (event->detail && !strncmp(event->detail, "manual_", 7) &&
        !strcmp(app->notice, "Bash command queued.")) app->notice[0] = 0;
    stream_commit(app, 1);
    free(app->tool_id);
    free(app->tool_name);
    free(app->tool_arguments);
    app->tool_streamed_bytes = 0;
    app->tool_id = copy_text(event->detail ? event->detail : "");
    app->tool_name = copy_text(event->name ? event->name : "tool");
    app->tool_arguments = copy_text(event->text ? event->text : "{}");
    app->dirty = app->redraw_urgent = 1;
}

static void replay_interaction(tui_app *app, const char *result) {
    cJSON *args = cJSON_Parse(app->tool_arguments ? app->tool_arguments : "{}");
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(args, "questions");
    size_t count = (size_t)cJSON_GetArraySize(items);
    oca_interaction_question questions[OCA_INTERACTION_MAX_QUESTIONS] = {0};
    char *options[OCA_INTERACTION_MAX_QUESTIONS][4] = {{0}};
    char *answers[OCA_INTERACTION_MAX_QUESTIONS] = {0};
    const char *cursor = result ? result : "";
    int valid = count > 0 && count <= OCA_INTERACTION_MAX_QUESTIONS;
    for (size_t i = 0; valid && i < count; i++) {
        const cJSON *item = cJSON_GetArrayItem(items, (int)i);
        const cJSON *choices = cJSON_GetObjectItemCaseSensitive(item, "options");
        const cJSON *question = cJSON_GetObjectItemCaseSensitive(item, "question");
        size_t choice_count = (size_t)cJSON_GetArraySize(choices);
        if (!cJSON_IsString(question) || choice_count < 2 || choice_count > 3) { valid = 0; break; }
        questions[i].question = question->valuestring;
        questions[i].options = options[i];
        questions[i].option_count = choice_count;
        int add_none = 1;
        for (size_t k = 0; k < choice_count; k++) {
            const cJSON *choice = cJSON_GetArrayItem(choices, (int)k);
            if (!cJSON_IsString(choice)) { valid = 0; break; }
            options[i][k] = choice->valuestring;
            if (!strcasecmp(choice->valuestring, "None of the above")) add_none = 0;
        }
        if (!valid) break;
        if (add_none) options[i][questions[i].option_count++] = "None of the above";
        text_buffer marker = {0};
        buffer_append(&marker, "Q: ", 3);
        buffer_append(&marker, question->valuestring, strlen(question->valuestring));
        buffer_append(&marker, "\nA: ", 4);
        valid = !strncmp(cursor, marker.data, marker.length);
        if (valid) cursor += marker.length;
        buffer_clear(&marker);
        const char *end = cursor + strlen(cursor);
        if (valid && i + 1 < count) {
            const cJSON *next = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(items, (int)i + 1), "question");
            if (!cJSON_IsString(next)) valid = 0;
            else {
                buffer_append(&marker, "\nQ: ", 4);
                buffer_append(&marker, next->valuestring, strlen(next->valuestring));
                buffer_append(&marker, "\nA: ", 4);
                end = strstr(cursor, marker.data);
                if (!end || strstr(end + 1, marker.data)) valid = 0;
            }
        } else if (valid && end > cursor && end[-1] == '\n') end--;
        if (valid) {
            size_t length = (size_t)(end - cursor);
            answers[i] = checked_realloc(NULL, length + 1);
            memcpy(answers[i], cursor, length);
            answers[i][length] = 0;
            cursor = end + (i + 1 < count);
        }
        buffer_free(&marker);
    }
    if (valid) {
        for (size_t i = 0; i < count; i++) {
            display_lines lines = {0};
            question_answer_lines(&lines, &questions[i], answers[i], app->cols);
            history_emit(app, &lines, lines.count, 2);
            lines_free(&lines);
        }
    } else {
        display_lines lines = {0};
        question_heading(&lines, "Recorded answers", app->cols);
        question_field(&lines, "", result ? result : "", app->cols);
        history_emit(app, &lines, lines.count, 2);
        lines_free(&lines);
    }
    for (size_t i = 0; i < OCA_INTERACTION_MAX_QUESTIONS; i++) free(answers[i]);
    cJSON_Delete(args);
}

static void stream_append(tui_app *app, conversation_kind kind, const char *text);

static void tool_output(tui_app *app, const char *text) {
    if (!text || !*text || !app->tool_name || strcmp(app->tool_name, "bash") ||
        !tool_body_visible(app, "bash")) return;
    if (!app->tool_streamed_bytes) {
        refresh_geometry(app);
        display_lines lines = {0};
        tool_action_lines(app, &lines, TOOL_RUNNING, NULL);
        history_emit(app, &lines, lines.count, 0);
        lines_free(&lines);
    }
    app->tool_streamed_bytes += strlen(text);
    stream_append(app, CONVERSATION_BASH, text);
}

static void tool_result(tui_app *app, const oca_core_event *event) {
    const char *name = event->name ? event->name : "tool";
    int matched = app->tool_id && event->detail && !strcmp(app->tool_id, event->detail);
    if (!matched) {
        free(app->tool_name);
        free(app->tool_arguments);
        app->tool_streamed_bytes = 0;
        app->tool_name = copy_text(name);
        app->tool_arguments = copy_text("{}");
    }
    if (!strcmp(name, "ask_user") && event->tool_state == OCA_TOOL_COMPLETE) {
        if (app->replaying) replay_interaction(app, event->text);
    } else {
        int state = event->tool_state;
        int bash = !strcmp(name, "bash"), exit_code = 0, has_exit_code = 0;
        const char *text = event->text ? event->text : "";
        size_t body_length = strlen(text);
        if (bash) {
            const char *last = strrchr(text, '\n');
            last = last ? last + 1 : text;
            int code, used = 0;
            if (sscanf(last, "[exit %d]%n", &code, &used) == 1 && used && !last[used]) {
                exit_code = code;
                has_exit_code = 1;
                if (state == OCA_TOOL_COMPLETE && code != 0) state = OCA_TOOL_ERROR;
                body_length = (size_t)(last - text);
                if (body_length) body_length--; /* Separator before execution metadata. */
            }
        }
        refresh_geometry(app);
        display_lines lines = {0};
        if (!app->tool_streamed_bytes)
            tool_action_lines(app, &lines, bash ? TOOL_RUNNING : state, event->text);
        if (tool_body_visible(app, name) && event->text && *event->text) {
            if ((!strcmp(name, "write") || !strcmp(name, "edit")) && state == OCA_TOOL_COMPLETE)
                tool_diff_lines(app, &lines, event->text);
            else if (body_length > app->tool_streamed_bytes) {
                size_t remaining = body_length - app->tool_streamed_bytes;
                char *body = checked_realloc(NULL, remaining + 1);
                memcpy(body, text + app->tool_streamed_bytes, remaining);
                body[remaining] = '\0';
                if (app->tool_streamed_bytes) stream_append(app, CONVERSATION_BASH, body);
                else {
                    char *safe = conversation_safe(body);
                    conversation_wrap(&lines, bash ? CONVERSATION_BASH : CONVERSATION_BODY, 0, safe, app->cols);
                    free(safe);
                }
                free(body);
            }
        }
        stream_commit(app, 1);
        if (bash && state != OCA_TOOL_COMPLETE) {
            char completion[64];
            if (state == OCA_TOOL_INTERRUPTED) snprintf(completion, sizeof(completion), "(interrupted)");
            else if (has_exit_code && exit_code != 0) snprintf(completion, sizeof(completion), "(exit %d)", exit_code);
            else snprintf(completion, sizeof(completion), "(failed)");
            size_t first = lines.count;
            conversation_wrap(&lines, CONVERSATION_BASH, 0, completion, app->cols);
            for (size_t i = first; i < lines.count; i++)
                lines.items[i].style = state == OCA_TOOL_INTERRUPTED ? STYLE_WARNING : STYLE_DANGER;
        }
        if (!bash && state == OCA_TOOL_ERROR)
            for (size_t i = 0; i < lines.count; i++)
                if (lines.items[i].conversation_kind == CONVERSATION_ACTION) lines.items[i].style = STYLE_DANGER;
        history_emit(app, &lines, lines.count, 1);
        lines_free(&lines);
    }
    if (!strcmp(name, "ask_user") && app->question_count) interaction_restore(app);
    free(app->tool_id);
    free(app->tool_name);
    free(app->tool_arguments);
    app->tool_id = app->tool_name = app->tool_arguments = NULL;
    app->tool_streamed_bytes = 0;
}

/* Append completed rows to terminal-owned scrollback. */
static void transcript(tui_app *app, const char *style, const char *format, ...) {
    if (!format) return;
    stream_commit(app, 1);
    va_list ap;
    va_start(ap, format);
    va_list copy;
    va_copy(copy, ap);
    int needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(ap);
        return;
    }
    char *text = checked_realloc(NULL, (size_t)needed + 1);
    vsnprintf(text, (size_t)needed + 1, format, ap);
    va_end(ap);

    transcript_literal(app, style, text, 1);
    free(text);
}

static void write_spaces(int count) {
    while (count-- > 0) write_text(" ");
}

static void terminal_title(tui_app *app) {
    const char *term = getenv("TERM");
    if (!term || !strcmp(term, "dumb")) return;
    if (!app->terminal.title_saved) {
        write_text("\x1b[22;2t");
        app->terminal.title_saved = 1;
    }
    const char *folder = strrchr(app->workspace, '/');
    folder = folder && folder[1] ? folder + 1 : app->workspace;
    char *safe = conversation_safe(folder);
    for (char *at = safe; *at; at++) if (*at == '\n') *at = ' ';
    write_text("\x1b]2;oca | ");
    write_text(safe);
    write_text("\x1b\\");
    free(safe);
}

static void brand_border(const char *left, const char *right, int width) {
    write_text(STYLE_ACCENT);
    write_text(left);
    for (int i = 0; i < width; i++) write_text("─");
    write_text(right);
    write_text(STYLE_RESET "\r\n");
}

static void print_brand(tui_app *app) {
    terminal_size(app);
    int title_width = text_columns("OCA 🪿");
    if (app->cols < title_width + 4 || app->rows < 12) return;
    char *description = safe_clip(app->workspace ? app->workspace : "", app->cols - 4);
    int path_width = text_columns(description);
    int inside = (path_width > title_width ? path_width : title_width) + 2;
    terminal_update_begin();
    brand_border("╭", "╮", inside);
    write_text(STYLE_ACCENT "│ " STYLE_RESET "\x1b[1mOCA" STYLE_RESET " 🪿");
    write_spaces(inside - 1 - title_width);
    write_text(STYLE_ACCENT "│" STYLE_RESET "\r\n" STYLE_ACCENT "│");
    write_spaces(inside);
    write_text(STYLE_ACCENT "│" STYLE_RESET "\r\n" STYLE_ACCENT "│ " STYLE_MUTED);
    write_text(description);
    write_spaces(inside - 1 - path_width);
    write_text(STYLE_ACCENT "│" STYLE_RESET "\r\n");
    brand_border("╰", "╯", inside);
    terminal_update_end();
    free(description);
}

static void print_startup_context(tui_app *app) {
    oca_core_status status;
    oca_core_get_status(app->core, &status);
    /* One column in from the banner's left border. */
    if (status.has_project_context) transcript(app, STYLE_BADGE, " [AGENTS.md]");
}

static void welcome_reset(tui_app *app) {
    erase_block(app);
    input_clear(app);
    app->notice[0] = 0;
    app->restore_pending = app->welcome_pending = 0;
    write_text("\x1b[H\x1b[2J");
    print_brand(app);
    print_startup_context(app);
    app->dirty = app->redraw_urgent = 1;
}

/* ============================================================================
 * History and menus
 * ========================================================================== */

static int history_path(char *path, size_t size) {
    const char *home = getenv("HOME");
    if (!home || !*home) return -1;
    int n = snprintf(path, size, "%s/.oca/history", home);
    return n >= 0 && (size_t)n < size ? 0 : -1;
}

static int history_nul_escape(const char *line, size_t length) {
    for (size_t i = 1; i + 1 < length; i++) {
        if (line[i] != '\\') continue;
        if (i + 5 < length && !memcmp(line + i + 1, "u0000", 5)) return 1;
        i++;
    }
    return 0;
}

static int history_load_jsonl(tui_app *app, FILE *file) {
    size_t maximum = OCA_PROMPT_MAX_BYTES * 6 + 3;
    char *line = checked_realloc(NULL, maximum + 1);
    int valid = 1;
    for (;;) {
        size_t length = 0;
        int ch;
        while ((ch = fgetc(file)) != EOF && ch != '\n') {
            if (length == maximum) { valid = 0; break; }
            line[length++] = (char)ch;
        }
        if (!valid || (ch == EOF && length == 0)) break;
        if (app->history_count == HISTORY_MAX || length < 2 ||
            memchr(line, 0, length)) {
            valid = 0;
            break;
        }
        line[length] = 0;
        const char *end = NULL;
        cJSON *value = cJSON_ParseWithLengthOpts(line, length, &end, 0);
        const char *prompt = cJSON_GetStringValue(value);
        size_t size = prompt ? strlen(prompt) : 0;
        while (end && end < line + length && isspace((unsigned char)*end)) end++;
        if (!prompt || end != line + length || !size ||
            size > OCA_PROMPT_MAX_BYTES || !utf8_bytes_valid(prompt, size) ||
            history_nul_escape(line, length)) {
            cJSON_Delete(value);
            valid = 0;
            break;
        }
        app->history = checked_realloc(app->history,
            (app->history_count + 1) * sizeof(*app->history));
        app->history[app->history_count++] = copy_text(prompt);
        cJSON_Delete(value);
    }
    if (ferror(file)) valid = 0;
    free(line);
    return valid ? 0 : -1;
}

static void history_load(tui_app *app) {
    char path[4096];
    if (history_path(path, sizeof(path)) != 0) return;
    FILE *file = fopen(path, "rb");
    if (!file) return;
    int valid = history_load_jsonl(app, file);
    fclose(file);
    if (valid) {
        for (size_t i = 0; i < app->history_count; i++) free(app->history[i]);
        free(app->history);
        app->history = NULL;
        app->history_count = 0;
    }
    app->history_at = app->history_count;
}

static int ensure_state_directory(const char *path) {
    char copy[4096];
    size_t len = strlen(path);
    if (len >= sizeof(copy)) return -1;
    memcpy(copy, path, len + 1);
    char *slash = strrchr(copy, '/');
    if (!slash) return 0;
    *slash = '\0';
    if (mkdir(copy, 0700) != 0 && errno != EEXIST) return -1;
    return 0;
}

static void history_save(tui_app *app) {
    char path[4096], temp[4160];
    if (history_path(path, sizeof(path)) != 0 || ensure_state_directory(path) != 0) return;
    snprintf(temp, sizeof(temp), "%s.tmp.%ld", path, (long)getpid());
    unlink(temp);
    int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return;
    FILE *file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        unlink(temp);
        return;
    }
    int failed = 0;
    for (size_t i = 0; i < app->history_count; i++) {
        cJSON *value = cJSON_CreateString(app->history[i]);
        char *line = value ? cJSON_PrintUnformatted(value) : NULL;
        if (!line || fputs(line, file) == EOF || fputc('\n', file) == EOF) {
            cJSON_free(line);
            cJSON_Delete(value);
            failed = 1;
            break;
        }
        cJSON_free(line);
        cJSON_Delete(value);
    }
    if (ferror(file) || fflush(file) != 0) failed = 1;
    if (!failed && fsync(fd) != 0) failed = 1;
    if (fclose(file) != 0) failed = 1;
    if (!failed && rename(temp, path) != 0) failed = 1;
    if (failed) unlink(temp);
}

static void history_add(tui_app *app, const char *prompt) {
    if (app->history_count && !strcmp(app->history[app->history_count - 1], prompt)) {
        app->history_at = app->history_count;
        return;
    }
    if (app->history_count == HISTORY_MAX) {
        free(app->history[0]);
        memmove(app->history, app->history + 1,
                (HISTORY_MAX - 1) * sizeof(*app->history));
        app->history_count--;
    }
    app->history = checked_realloc(app->history,
        (app->history_count + 1) * sizeof(*app->history));
    app->history[app->history_count++] = copy_text(prompt);
    app->history_at = app->history_count;
    if (!app->side_chat) history_save(app);
}

static void input_set(tui_app *app, const char *text) {
    app->invalid_input = 0;
    size_t len = strlen(text);
    if (len > OCA_PROMPT_MAX_BYTES || !utf8_bytes_valid(text, len)) return;
    if (len) {
        app->exit_hint = 0;
        app->exit_hint_restore_ms = 0;
    }
    memcpy(app->input, text, len + 1);
    app->input_len = len;
    app->cursor = len;
    app->dirty = 1;
    if (app->input[0] == '/' && !memchr(app->input, ' ', app->input_len) &&
        !memchr(app->input, '\n', app->input_len)) command_menu(app);
    else if (app->menu == MENU_COMMANDS) menu_clear(app);
}

static void history_move(tui_app *app, int direction) {
    if (!app->history_count) return;
    if (direction < 0) {
        if (app->history_at == app->history_count) {
            free(app->history_draft);
            app->history_draft = copy_text(app->input);
        }
        if (app->history_at > 0) app->history_at--;
        input_set(app, app->history[app->history_at]);
    } else if (app->history_at < app->history_count) {
        app->history_at++;
        input_set(app, app->history_at == app->history_count ?
                  (app->history_draft ? app->history_draft : "") :
                  app->history[app->history_at]);
    }
}

static void input_vertical(tui_app *app, int direction) {
    if (!app->input_len) {
        if (app->menu != MENU_INTERACTION) history_move(app, direction);
        return;
    }
    refresh_geometry(app);
    char *notice = shared_row(app);
    int available = app->rows - (app->rows >= 2) - (notice && app->rows >= 3);
    free(notice);
    int cols = app->menu == MENU_INTERACTION ? question_width(app->cols) :
        app->cols - (app->cols >= 8 && available >= 3 ? 4 : 0);
    display_lines lines = {0};
    int row = 0, column = 0;
    composer_wrap(app, &lines, STYLE_RESET, cols, &row, &column);
    int target = row + direction;
    size_t rows = lines.count;
    lines_free(&lines);
    if (target < 0 || (size_t)target >= rows) {
        app->vertical_move = 0;
        if (app->menu != MENU_INTERACTION) history_move(app, direction);
        return;
    }
    if (!app->vertical_move) app->vertical_column = column;
    app->vertical_move = 1;
    size_t *positions = checked_realloc(NULL, (app->input_len + 1) * sizeof(*positions));
    size_t count = 1;
    positions[0] = 0;
    for (size_t at = 0; at < app->input_len;) {
        at = next_grapheme(app->input, app->input_len, at);
        positions[count++] = at;
    }
    size_t low = 0, high = count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        app->cursor = positions[middle];
        composer_wrap(app, &lines, STYLE_RESET, cols, &row, &column);
        lines_free(&lines);
        if (row < target || (row == target && column <= app->vertical_column)) low = middle + 1;
        else high = middle;
    }
    app->cursor = positions[low ? low - 1 : 0];
    free(positions);
    app->dirty = 1;
}

static void menu_clear(tui_app *app) {
    for (size_t i = 0; i < app->menu_count; i++) {
        free(app->menu_items[i].label);
        free(app->menu_items[i].value);
    }
    free(app->menu_items);
    app->menu_items = NULL;
    app->menu_count = 0;
    app->menu_selection = 0;
    app->menu_first = 0;
    app->menu = MENU_NONE;
    app->save_input_len = 0;
    app->save_input[0] = 0;
    app->dirty = 1;
}

static void menu_add(tui_app *app, const char *label, const char *value, int number) {
    app->menu_items = checked_realloc(app->menu_items,
        (app->menu_count + 1) * sizeof(*app->menu_items));
    menu_item *item = &app->menu_items[app->menu_count++];
    item->label = copy_text(label);
    item->value = value ? copy_text(value) : NULL;
    item->number = number;
}

static void command_menu(tui_app *app) {
    menu_clear(app);
    app->menu = MENU_COMMANDS;
    const char *filter = app->input;
    size_t len = strcspn(filter, " \t\n");
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
        if (!command_visible(app, &COMMANDS[i]) ||
            strncmp(COMMANDS[i].name, filter, len) != 0) continue;
        char label[256];
        snprintf(label, sizeof(label), "%-10s %s", COMMANDS[i].name,
                 !strcmp(COMMANDS[i].name, "/hints") ?
                 (oca_core_hints_enabled(app->core) ?
                  "Toggle off programming hints" : "Toggle on programming hints") :
                 COMMANDS[i].description);
        menu_add(app, label, COMMANDS[i].name, 0);
    }
}

static void help_menu(tui_app *app) {
    menu_clear(app);
    app->menu = MENU_HELP;
    const char *controls[] = {
        "Enter  Complete or submit", "Ctrl+J  Newline",
        "Up/Down  Input rows or history", "Esc  Close a menu or interrupt work",
        "Ctrl+X  Edit oldest queued message",
        "Alt+K/L  Model", "Alt+,/.  Reasoning",
        "Ctrl+C  Clear input/queued; show exit hint",
        "Ctrl+D  Exit", "?  Help", "/  Commands", "!  One Bash command", "Ctrl+/  Switch conversations",
        "PgUp/PgDown  Scroll history",
        "Tab  Add details to a question answer"
    };
    size_t key_width = 0;
    for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); i++) {
        size_t length = strcspn(controls[i], " ");
        if (length > key_width) key_width = length;
    }
    for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); i++) {
        if (app->side_chat && (!strncmp(controls[i], "Shift+Tab", 9) || controls[i][0] == '!')) continue;
        const char *label = controls[i];
        if (app->side_chat) {
            if (!strncmp(label, "Ctrl+C", 6)) label = "Ctrl+C  Close temporary chat";
            else if (!strncmp(label, "Ctrl+D", 6)) label = "Ctrl+D  Delete next grapheme";
        }
        size_t length = strcspn(label, " ");
        char aligned[160];
        snprintf(aligned, sizeof(aligned), " %-*.*s  %s", (int)key_width, (int)length, label,
                 label + length + strspn(label + length, " "));
        menu_add(app, aligned, NULL, 0);
    }
}

static void model_menu(tui_app *app) {
    menu_clear(app);
    app->menu = MENU_MODEL;
    for (size_t i = 0; i < oca_core_model_count(app->core); i++) {
        const oca_model *model = oca_core_model_at(app->core, i);
        if (!oca_core_model_available(app->core, model)) continue;
        char label[OCA_LLM_MODEL_ID_MAX + 64];
        snprintf(label, sizeof(label), "%s · %s", model->id, model->provider);
        menu_add(app, label, model->id, (int)i);
    }
}

static void reasoning_menu(tui_app *app, const oca_model *model) {
    menu_clear(app);
    app->menu = MENU_REASONING;
    for (size_t i = 0; i < model->reasoning_count; i++) {
        const char *name = oca_model_reasoning_name(model, (oca_reasoning)i);
        char label[128];
        snprintf(label, sizeof(label), "%s reasoning", name);
        menu_add(app, label, name, (int)i);
    }
}

static void interaction_free(tui_app *app) {
    for (size_t i = 0; i < app->question_count; i++) {
        free(app->questions[i].question);
        for (size_t k = 0; k < app->questions[i].option_count; k++)
            free(app->questions[i].options[k]);
        free(app->questions[i].options);
        free(app->answers ? app->answers[i] : NULL);
    }
    free(app->questions);
    free(app->answers);
    app->questions = NULL;
    app->answers = NULL;
    app->question_count = 0;
    app->interaction_question = 0;
    free(app->interaction_draft);
    app->interaction_draft = NULL;
    app->interaction_details = 0;
}

static void interaction_restore(tui_app *app) {
    char *draft = app->interaction_draft;
    size_t cursor = app->interaction_cursor;
    app->interaction_draft = NULL;
    interaction_free(app);
    menu_clear(app);
    if (app->input) {
        input_set(app, draft ? draft : "");
        app->cursor = cursor < app->input_len ? cursor : app->input_len;
    }
    free(draft);
}

static void question_wrap(display_lines *lines, const char *style,
                          const char *text, int cols) {
    const char *at = text;
    for (;;) {
        size_t length = strcspn(at, "\n");
        char *part = checked_realloc(NULL, length + 1);
        memcpy(part, at, length);
        part[length] = 0;
        size_t first = lines->count;
        lines_wrap(lines, style, "", part, cols, SIZE_MAX, NULL, NULL);
        for (size_t i = first; i + 1 < lines->count; i++) lines->items[i].soft_wrap = 1;
        free(part);
        at += length;
        if (!*at) break;
        at++;
    }
}

static void question_heading(display_lines *lines, const char *question, int cols) {
    char *safe = conversation_safe(question);
    markdown_state md = {.columns = cols};
    text_buffer formatted = {0};
    markdown_feed(&md, &formatted, safe);
    markdown_finish(&md, &formatted);
    size_t first = lines->count;
    conversation_wrap(lines, CONVERSATION_ASSISTANT, 0,
                      formatted.data ? formatted.data : "", cols);
    for (size_t i = first; i < lines->count; i++) lines->items[i].style = STYLE_BOLD;
    buffer_free(&formatted);
    free(safe);
}

static void question_field(display_lines *lines, const char *label, const char *value, int cols) {
    char *safe = conversation_safe(value);
    text_buffer text = {0};
    buffer_append(&text, label, strlen(label));
    buffer_append(&text, safe, strlen(safe));
    question_wrap(lines, STYLE_RESET, text.data ? text.data : "", cols);
    buffer_free(&text);
    free(safe);
}

static int question_width(int cols) {
    return cols >= 8 ? cols - 4 : cols;
}

static void question_card(display_lines *lines, const display_lines *body, int cols, int active) {
    if (cols < 8) {
        lines_copy_range(lines, body, 0, body->count);
        return;
    }
    frame_border(lines, "╭", "╮", cols);
    lines->items[lines->count - 1].card_role = 1;
    lines->items[lines->count - 1].style = active ? STYLE_ACCENT : STYLE_MUTED;
    for (size_t i = 0; i < body->count; i++) {
        const display_line *source = &body->items[i];
        text_buffer row = {0};
        buffer_append(&row, "│ ", strlen("│ "));
        buffer_append(&row, source->text, strlen(source->text));
        int padding = cols - 4 - text_columns(source->text);
        while (padding-- > 0) buffer_append(&row, " ", 1);
        buffer_append(&row, " │", strlen(" │"));
        lines_add(lines, active ? STYLE_ACCENT : STYLE_MUTED, row.data, row.length);
        display_line *item = &lines->items[lines->count - 1];
        item->accent_start = strlen("│ ");
        item->accent_end = row.length - strlen(" │");
        item->accent_style = !strcmp(source->style, STYLE_BOLD) ?
            STYLE_RESET STYLE_BOLD : source->style;
        item->source = copy_text(source->source ? source->source : source->text);
        item->soft_wrap = source->soft_wrap;
        item->conversation_kind = source->conversation_kind;
        item->card_role = 2;
        buffer_free(&row);
    }
    frame_border(lines, "╰", "╯", cols);
    lines->items[lines->count - 1].card_role = 3;
    lines->items[lines->count - 1].style = active ? STYLE_ACCENT : STYLE_MUTED;
}

static void question_answer_lines(display_lines *lines, const oca_interaction_question *question,
                                  const char *answer, int cols) {
    const char *selected = answer, *details = NULL;
    const char *separator = "\n\nDetails: ";
    size_t matches = 0;
    for (size_t i = 0; i < question->option_count; i++) {
        const char *option = question->options[i];
        size_t length = strlen(option);
        if (!strncmp(answer, option, length) &&
            (!answer[length] || !strncmp(answer + length, separator, strlen(separator)))) {
            selected = option;
            details = answer[length] ? answer + length + strlen(separator) : NULL;
            matches++;
        }
    }
    /* An ambiguous legacy answer stays literal within its question. */
    if (matches != 1) { selected = answer; details = NULL; }
    display_lines body = {0};
    int width = question_width(cols);
    question_heading(&body, question->question, width);
    question_field(&body, "Answer: ", selected, width);
    if (details && *details) question_field(&body, "Details: ", details, width);
    question_card(lines, &body, cols, 0);
    lines_free(&body);
}

static void question_lines(tui_app *app, display_lines *lines, int *cursor_row, int *cursor_col) {
    if (app->interaction_question >= app->question_count) return;
    const oca_interaction_question *question = &app->questions[app->interaction_question];
    display_lines body = {0};
    int width = question_width(app->cols);
    question_heading(&body, question->question, width);
    for (size_t i = 0; i < question->option_count; i++) {
        char *safe = conversation_safe(question->options[i]);
        text_buffer option = {0};
        buffer_append(&option, "  ", 2);
        buffer_append(&option, safe, strlen(safe));
        question_wrap(&body, i == app->menu_selection ? STYLE_SELECTED : STYLE_RESET,
                      option.data, width);
        buffer_free(&option);
        free(safe);
    }
    lines_add(&body, STYLE_RESET, "", 0);
    display_lines editor = {0};
    int row = 0, column = 0;
    size_t first = body.count;
    composer_wrap(app, &editor, app->input_len ? STYLE_RESET : STYLE_SUBTLE,
                  width, &row, &column);
    lines_copy_range(&body, &editor, 0, editor.count);
    if (cursor_row) *cursor_row = (int)lines->count + (int)first + row + (app->cols >= 8);
    if (cursor_col) *cursor_col = column + (app->cols >= 8 ? 2 : 0);
    lines_free(&editor);
    lines_add(&body, STYLE_RESET, "", 0);
    conversation_wrap(&body, CONVERSATION_NONE, 0, app->input_len ?
        STYLE_INLINE "Ctrl+C" STYLE_MUTED ": clear details · "
        STYLE_INLINE "Enter" STYLE_MUTED ": confirm · "
        STYLE_INLINE "Esc" STYLE_MUTED ": cancel" : app->interaction_details ?
        STYLE_INLINE "Tab" STYLE_MUTED ": go back · "
        STYLE_INLINE "Enter" STYLE_MUTED ": confirm · "
        STYLE_INLINE "Esc" STYLE_MUTED ": cancel" :
        STYLE_INLINE "Tab" STYLE_MUTED ": add details · "
        STYLE_INLINE "Enter" STYLE_MUTED ": confirm · "
        STYLE_INLINE "Esc" STYLE_MUTED ": cancel", width);
    question_card(lines, &body, app->cols, 1);
    lines_free(&body);
}

static void interaction_menu(tui_app *app) {
    menu_clear(app);
    app->menu = MENU_INTERACTION;
    if (app->interaction_question >= app->question_count) return;
    oca_interaction_question *question = &app->questions[app->interaction_question];
    for (size_t i = 0; i < question->option_count; i++)
        menu_add(app, question->options[i], question->options[i], (int)i);
    app->interaction_details = 0;
}

/* ============================================================================
 * Core events
 * ========================================================================== */

static void stream_clear(tui_app *app) {
    buffer_free(&app->markdown.source_line);
    buffer_free(&app->markdown.table);
    memset(&app->markdown, 0, sizeof(app->markdown));
    free(app->stream_text);
    app->stream_text = NULL;
    app->stream_text_len = 0;
    app->stream_kind = CONVERSATION_NONE;
    app->stream_continued = 0;
}

static void stream_append(tui_app *app, conversation_kind kind, const char *text) {
    if (!text || !*text) return;
    if (app->stream_kind && app->stream_kind != kind) stream_commit(app, 1);
    app->stream_kind = kind;
    char *safe = conversation_safe(text);
    text_buffer formatted = {0};
    if (kind == CONVERSATION_ASSISTANT) {
        app->markdown.columns = app->cols;
        markdown_feed(&app->markdown, &formatted, safe);
    }
    else buffer_append(&formatted, safe, strlen(safe));
    if (formatted.length) append_text(&app->stream_text, &app->stream_text_len, formatted.data);
    buffer_free(&formatted);
    free(safe);
    app->dirty = 1;
}

static void deliver_input(tui_app *app, const char *text) {
    if (!text || !*text) return;
    if (!app->input_len) {
        input_set(app, text);
        return;
    }
    app->deferred_inputs = checked_realloc(app->deferred_inputs,
        (app->deferred_input_count + 1) * sizeof(*app->deferred_inputs));
    app->deferred_inputs[app->deferred_input_count++] = copy_text(text);
}

static void restore_deferred_input(tui_app *app) {
    if (app->input_len || !app->deferred_input_count) return;
    char *text = app->deferred_inputs[0];
    memmove(app->deferred_inputs, app->deferred_inputs + 1,
            (app->deferred_input_count - 1) * sizeof(*app->deferred_inputs));
    app->deferred_input_count--;
    input_set(app, text);
    free(text);
}

static void interaction_copy(tui_app *app, const oca_core_event *event) {
    interaction_free(app);
    app->interaction_draft = copy_text(app->input ? app->input : "");
    app->interaction_cursor = app->cursor;
    if (app->input) input_clear(app);
    app->question_count = event->question_count;
    app->questions = checked_realloc(NULL,
        app->question_count * sizeof(*app->questions));
    memset(app->questions, 0, app->question_count * sizeof(*app->questions));
    app->answers = checked_realloc(NULL, app->question_count * sizeof(*app->answers));
    memset(app->answers, 0, app->question_count * sizeof(*app->answers));
    for (size_t i = 0; i < app->question_count; i++) {
        app->questions[i].question = copy_text(event->questions[i].question);
        int add_none = 1;
        for (size_t k = 0; k < event->questions[i].option_count; k++)
            if (!strcasecmp(event->questions[i].options[k], "None of the above")) add_none = 0;
        app->questions[i].option_count = event->questions[i].option_count + (size_t)add_none;
        app->questions[i].options = checked_realloc(NULL,
            app->questions[i].option_count * sizeof(char *));
        for (size_t k = 0; k < event->questions[i].option_count; k++)
            app->questions[i].options[k] = copy_text(event->questions[i].options[k]);
        if (add_none) app->questions[i].options[event->questions[i].option_count] = copy_text("None of the above");
    }
    interaction_menu(app);
}

static void buffer_json_string(text_buffer *buffer, const char *text) {
    buffer_append(buffer, "\"", 1);
    for (const unsigned char *at = (const unsigned char *)text; *at; at++) {
        char escaped[7];
        switch (*at) {
        case '"':
            buffer_append(buffer, "\\\"", 2);
            break;
        case '\\':
            buffer_append(buffer, "\\\\", 2);
            break;
        case '\b':
            buffer_append(buffer, "\\b", 2);
            break;
        case '\f':
            buffer_append(buffer, "\\f", 2);
            break;
        case '\n':
            buffer_append(buffer, "\\n", 2);
            break;
        case '\r':
            buffer_append(buffer, "\\r", 2);
            break;
        case '\t':
            buffer_append(buffer, "\\t", 2);
            break;
        default:
            if (*at < 0x20) {
                snprintf(escaped, sizeof(escaped), "\\u%04x", *at);
                buffer_append(buffer, escaped, 6);
            } else {
                buffer_append(buffer, (const char *)at, 1);
            }
            break;
        }
    }
    buffer_append(buffer, "\"", 1);
}

static int send_interaction_answers(tui_app *app) {
    text_buffer json = {0};
    buffer_append(&json, "[", 1);
    for (size_t i = 0; i < app->question_count; i++) {
        const char *q = app->questions[i].question;
        const char *a = app->answers[i] ? app->answers[i] : "";
        if (i) buffer_append(&json, ",", 1);
        buffer_append(&json, "{\"question\":", sizeof("{\"question\":") - 1);
        buffer_json_string(&json, q);
        buffer_append(&json, ",\"answer\":", sizeof(",\"answer\":") - 1);
        buffer_json_string(&json, a);
        buffer_append(&json, "}", 1);
    }
    buffer_append(&json, "]", 1);
    char err[256];
    int result = oca_core_submit_answers(app->core, json.data, err, sizeof(err));
    if (result != 0) notice_set(app, err);
    buffer_free(&json);
    return result;
}

static void sessions_event(tui_app *app, const oca_core_event *event) {
    menu_clear(app);
    app->menu = MENU_SESSIONS;
    for (size_t i = 0; i < event->session_count; i++) {
        char label[512];
        long long age_seconds = (long long)time(NULL) - event->sessions[i].last_used;
        if (age_seconds < 0) age_seconds = 0;
        char age[32];
        if (age_seconds < 60) snprintf(age, sizeof(age), "%llds", age_seconds);
        else if (age_seconds < 3600) snprintf(age, sizeof(age), "%lldm", age_seconds / 60);
        else if (age_seconds < 86400) snprintf(age, sizeof(age), "%lldh", age_seconds / 3600);
        else snprintf(age, sizeof(age), "%lldd", age_seconds / 86400);
        snprintf(label, sizeof(label), "%.80s · %s · %.40s · %.80s",
                 event->sessions[i].title, age, event->sessions[i].model,
                 event->sessions[i].workspace);
        menu_add(app, label, event->sessions[i].id, 0);
    }
    if (!event->session_count) transcript(app, STYLE_MUTED, "No sessions found.");
}

static void skills_event(tui_app *app, const oca_core_event *event) {
    menu_clear(app);
    app->menu = MENU_SKILLS;
    for (size_t i = 0; i < event->skill_count; i++) {
        char label[1024];
        snprintf(label, sizeof(label), "%s · %s", event->skills[i].name,
                 event->skills[i].description);
        menu_add(app, label, NULL, 0);
    }
    if (!event->skill_count) transcript(app, STYLE_MUTED, "No skills found.");
}

static void goal_event(tui_app *app, const oca_core_event *event) {
    int show = event->name && !strcmp(event->name, "show");
    const char *state = goal_name(event->goal);
    if (!state) {
        transcript(app, show ? STYLE_LILAC : STYLE_MUTED, "%s", show ? "No goal set." : "goal cleared");
        return;
    }
    stream_commit(app, 1);
    text_buffer text = {0};
    const char *label = show || event->goal == OCA_GOAL_ACTIVE ? "goal" : state;
    buffer_append(&text, STYLE_PURPLE, strlen(STYLE_PURPLE));
    buffer_append(&text, label, strlen(label));
    if ((show || event->goal == OCA_GOAL_ACTIVE || event->goal == OCA_GOAL_COMPLETED) && event->text) {
        char *objective = conversation_safe(event->text);
        buffer_append(&text, ": " STYLE_MUTED, strlen(": " STYLE_MUTED));
        buffer_append(&text, objective, strlen(objective));
        free(objective);
    }
    buffer_append(&text, STYLE_MUTED, strlen(STYLE_MUTED));
    buffer_append(&text, STYLE_RESET, strlen(STYLE_RESET));
    display_lines lines = {0};
    conversation_wrap(&lines, CONVERSATION_NONE, 0, text.data, app->cols);
    history_emit(app, &lines, lines.count, 1);
    lines_free(&lines);
    buffer_free(&text);
}

static void handle_event(tui_app *app, oca_core_event *event) {
    if (event->kind != OCA_CE_ASSISTANT_TEXT && event->kind != OCA_CE_ASSISTANT_REASON && event->kind != OCA_CE_TOOL_OUTPUT)
        app->redraw_urgent = 1;
    switch (event->kind) {
    case OCA_CE_WARN:
        transcript(app, STYLE_WARNING, "Warning: %s", event->text ? event->text : "");
        break;
    case OCA_CE_ASSISTANT_TEXT:
        if (event->text && *event->text) {
            stream_append(app, CONVERSATION_ASSISTANT, event->text);
        }
        break;
    case OCA_CE_ASSISTANT_REASON:
        if (app->display.show_reasoning) stream_append(app, CONVERSATION_REASONING, event->text);
        break;
    case OCA_CE_TOOL_START:
        tool_start(app, event);
        break;
    case OCA_CE_TOOL_OUTPUT:
        tool_output(app, event->text);
        break;
    case OCA_CE_TOOL_RESULT:
        tool_result(app, event);
        break;
    case OCA_CE_INTERACTION:
        interaction_copy(app, event);
        break;
    case OCA_CE_GOAL:
        goal_event(app, event);
        if (event->name && !strcmp(event->name, "edit")) deliver_input(app, event->text);
        break;
    case OCA_CE_SESSIONS:
        sessions_event(app, event);
        break;
    case OCA_CE_SKILLS:
        skills_event(app, event);
        break;
    case OCA_CE_SAVE_ASK:
        menu_clear(app);
        app->menu = MENU_SAVE;
        break;
    case OCA_CE_SAVE_ASK_CONT:
        menu_clear(app);
        app->menu = MENU_SAVE_CONTINUE;
        break;
    case OCA_CE_ERROR:
        stream_commit(app, 1);
        if (event->name && !strcmp(event->name, "diagnostic_failed")) app->failed = 1;
        transcript(app, STYLE_DANGER, "%s: %s", event->name ? event->name : "Error",
                   event->text ? event->text : "");
        break;
    case OCA_CE_ROUTE:
        if (!event->route_available) notice_set(app, "Selected model is unavailable.");
        app->dirty = 1;
        break;
    case OCA_CE_TURN_DONE:
        stream_commit(app, 1);
        break;
    case OCA_CE_SESSION: {
        if (event->session_replaced) {
            stream_clear(app);
            side_close(app);
            lines_free(&app->retained);
            app->retained_bytes = app->flushed_rows = app->view_end = 0;
            app->history_omitted = 0;
            if (event->session_new) main_view(app)->welcome_pending = 1;
        }
        if (event->name) {
            free(app->owned_workspace);
            app->owned_workspace = copy_text(event->name);
            app->workspace = app->owned_workspace;
            terminal_title(app);
        }
        if (event->session_replaced) {
            if (event->session_new) break;
            if (app->main_view) view_restore(app);
            char err[256];
            if (oca_core_replay(app->core, err, sizeof(err)) != 0) notice_set(app, err);
        }
        break;
    }
    case OCA_CE_USER_TEXT:
        conversation_print(app, CONVERSATION_USER, event->text ? event->text : "");
        break;
    case OCA_CE_TRANSCRIPT:
        app->replaying = 1;
        for (size_t i = 0; i < event->entry_count; i++) {
            const oca_transcript_entry *entry = &event->entries[i];
            /* The handler borrows these fields while the replay batch remains alive. */
            oca_core_event item = { .kind = entry->kind, .text = entry->text,
                .name = entry->name, .detail = entry->detail, .tool_state = entry->tool_state };
            handle_event(app, &item);
        }
        stream_commit(app, 1);
        app->replaying = 0;
        break;
    case OCA_CE_QUIT_OK:
        app->running = 0;
        break;
    case OCA_CE_STATUS:
        app->dirty = 1;
        break;
    }
}

static void drain_events(tui_app *app) {
    drain_descriptor(oca_core_wake_fd(app->core));
    oca_core_event event;
    size_t count = 0;
    while (count < 128 && oca_core_poll_event(app->core, &event)) {
        if (!app->closing) handle_event(app, &event);
        oca_core_event_release(&event);
        count++;
    }
    if (app->hidden && !app->closing) stream_commit(app, 0);
    app->events_pending = count == 128;
}

/* ============================================================================
 * Commands and editor
 * ========================================================================== */

static void report_action_error(tui_app *app, int rc, const char *err) {
    if (rc != 0) notice_set(app, err);
}

static void run_command(tui_app *app, const char *text) {
    char err[256];
    if (!strcmp(text, "/btw") || !strncmp(text, "/btw ", 5)) {
        side_open(app, text[4] ? text + 5 : NULL);
    } else if (!strcmp(text, "/help")) {
        help_menu(app);
    } else if (!strcmp(text, "/goal")) {
        report_action_error(app, oca_core_goal(app->core, OCA_GOAL_CMD_SHOW,
                                               NULL, err, sizeof(err)), err);
    } else if (!strncmp(text, "/goal ", 6)) {
        const char *arg = text + 6;
        int command = OCA_GOAL_CMD_SET;
        const char *objective = arg;
        for (size_t i = 0; i < sizeof(GOAL_ACTIONS) / sizeof(GOAL_ACTIONS[0]); i++) {
            if (!strcmp(arg, GOAL_ACTIONS[i].name)) {
                command = GOAL_ACTIONS[i].command;
                objective = NULL;
                break;
            }
        }
        report_action_error(app, oca_core_goal(app->core, command, objective,
                                               err, sizeof(err)), err);
    } else if (!strcmp(text, "/new")) {
        report_action_error(app, oca_core_new_session(app->core, err, sizeof(err)), err);
    } else if (!strcmp(text, "/resume")) {
        report_action_error(app, oca_core_list_sessions(app->core, err, sizeof(err)), err);
    } else if (!strcmp(text, "/model")) {
        model_menu(app);
    } else if (!strcmp(text, "/hints")) {
        int enabled = !oca_core_hints_enabled(app->core);
        if (oca_core_set_hints(app->core, enabled, err, sizeof(err)) != 0) notice_set(app, err);
        else app->dirty = app->redraw_urgent = 1;
    } else if (!strcmp(text, "/skills")) {
        report_action_error(app, oca_core_list_skills(app->core, err, sizeof(err)), err);
    } else if (!strcmp(text, "/quit")) {
        report_action_error(app, oca_core_quit(app->core, err, sizeof(err)), err);
    } else {
        transcript(app, STYLE_WARNING, "Unknown command: %s", text);
    }
}

static void input_clear(tui_app *app) {
    app->vertical_move = 0;
    app->invalid_input = 0;
    app->input[0] = '\0';
    app->input_len = 0;
    app->cursor = 0;
    app->history_at = app->history_count;
    app->dirty = 1;
    if (app->menu == MENU_COMMANDS) menu_clear(app);
}

static void input_insert(tui_app *app, const char *data, size_t len) {
    if (!len || len > OCA_PROMPT_MAX_BYTES - app->input_len) return;
    app->exit_hint = 0;
    app->exit_hint_restore_ms = 0;
    memmove(app->input + app->cursor + len, app->input + app->cursor,
            app->input_len - app->cursor + 1);
    memcpy(app->input + app->cursor, data, len);
    app->input_len += len;
    app->cursor += len;
    app->invalid_input = 0;
    app->dirty = 1;
    if ((app->menu == MENU_NONE || app->menu == MENU_COMMANDS) &&
        app->input[0] == '/' && !memchr(app->input, ' ', app->input_len) &&
        !memchr(app->input, '\n', app->input_len)) command_menu(app);
    else if (app->menu == MENU_COMMANDS) menu_clear(app);
}

static void input_backspace(tui_app *app) {
    app->invalid_input = 0;
    if (!app->cursor) return;
    size_t previous = previous_grapheme(app->input, app->cursor);
    memmove(app->input + previous, app->input + app->cursor,
            app->input_len - app->cursor + 1);
    app->input_len -= app->cursor - previous;
    app->cursor = previous;
    app->dirty = 1;
    if (!app->input_len || (app->input_len == 1 && app->input[0] == '/')) {
        if (app->menu != MENU_INTERACTION) {
            if (!app->input_len) menu_clear(app);
            else command_menu(app);
        }
    } else if (app->menu == MENU_COMMANDS) command_menu(app);
}

static void input_delete(tui_app *app) {
    app->invalid_input = 0;
    if (app->cursor >= app->input_len) return;
    size_t next = next_grapheme(app->input, app->input_len, app->cursor);
    memmove(app->input + app->cursor, app->input + next,
            app->input_len - next + 1);
    app->input_len -= next - app->cursor;
    app->dirty = 1;
    if (!app->input_len && app->menu != MENU_INTERACTION) menu_clear(app);
    else if ((app->menu == MENU_NONE || app->menu == MENU_COMMANDS) &&
             app->input[0] == '/' && !memchr(app->input, ' ', app->input_len) &&
             !memchr(app->input, '\n', app->input_len)) command_menu(app);
    else if (app->menu == MENU_COMMANDS) menu_clear(app);
}

static int grapheme_is_space(const char *text, size_t start, size_t end) {
    uint32_t value;
    int width, valid;
    (void)utf8_decode(text + start, end - start, &value, &width, &valid);
    if (!valid) return 0;
#if WCHAR_MAX >= 0x10FFFF
    return iswspace((wint_t)value) != 0;
#else
    return value <= (uint32_t)WCHAR_MAX && iswspace((wint_t)value) != 0;
#endif
}

static void input_word_delete(tui_app *app) {
    while (app->cursor) {
        size_t previous = previous_grapheme(app->input, app->cursor);
        if (!grapheme_is_space(app->input, previous, app->cursor)) break;
        input_backspace(app);
    }
    while (app->cursor) {
        size_t previous = previous_grapheme(app->input, app->cursor);
        if (grapheme_is_space(app->input, previous, app->cursor)) break;
        input_backspace(app);
    }
}

static void submit_prompt(tui_app *app) {
    if (!app->input_len) return;
    char *text = copy_text(app->input);
    if (!app->side_chat && text[0] == '!') {
        const char *command = text + 1;
        while (*command == ' ' || *command == '\t' || *command == '\n') command++;
        if (*command) {
            char err[256];
            oca_core_status status;
            oca_core_get_status(app->core, &status);
            if (oca_core_bash(app->core, command, err, sizeof(err)) != 0) {
                notice_set(app, err);
                free(text);
                return;
            }
            if (status.working) notice_set(app, "Bash command queued.");
        }
        input_clear(app);
        free(text);
        return;
    }
    if (text[0] == '/') {
        const command_info *command = command_for_input(app);
        size_t name_length = command ? strlen(command->name) : 0;
        int empty_objective = command && !strcmp(command->name, "/goal") &&
            text[name_length] && !text[name_length + strspn(text + name_length, " \t\n\r")];
        if (empty_objective || !command || (!command_accepts_arguments(command) && text[name_length])) {
            app->invalid_input = 1;
            app->dirty = 1;
            free(text);
            return;
        }
        input_clear(app);
        run_command(app, text);
        free(text);
        return;
    }
    char err[256];
    if (oca_core_submit_prompt(app->core, text, err, sizeof(err)) != 0) {
        transcript(app, STYLE_DANGER, "Error: %s", err);
        free(text);
        return;
    }
    input_clear(app);
    history_add(app, text);
    free(text);
    restore_deferred_input(app);
}

static void menu_activate(tui_app *app) {
    if (!app->menu_count || app->menu_selection >= app->menu_count) return;
    menu_item *item = &app->menu_items[app->menu_selection];
    char *value = item->value ? copy_text(item->value) : NULL;
    int number = item->number;
    menu_kind menu = app->menu;
    char err[256];
    if (menu == MENU_COMMANDS && value) {
        input_set(app, value);
        menu_clear(app);
        submit_prompt(app);
    } else if (menu == MENU_MODEL && value) {
        const oca_model *model = oca_core_model_at(app->core, (size_t)number);
        free(app->pending_model);
        app->pending_model = copy_text(value);
        reasoning_menu(app, model);
    } else if (menu == MENU_REASONING && value && app->pending_model) {
        menu_clear(app);
        report_action_error(app, oca_core_set_route(app->core, app->pending_model,
                                                    value, err, sizeof(err)), err);
        free(app->pending_model);
        app->pending_model = NULL;
    } else if (menu == MENU_SESSIONS && value) {
        menu_clear(app);
        report_action_error(app, oca_core_resume_session(app->core, value,
                                                         err, sizeof(err)), err);
    } else if (menu == MENU_INTERACTION) {
        text_buffer answer = {0};
        buffer_append(&answer, value ? value : "", value ? strlen(value) : 0);
        if (app->input_len) {
            buffer_append(&answer, "\n\nDetails: ", strlen("\n\nDetails: "));
            buffer_append(&answer, app->input, app->input_len);
        }
        size_t total = 0;
        for (size_t i = 0; i < app->question_count; i++) {
            const char *text = i == app->interaction_question ? answer.data : app->answers[i];
            total += strlen(app->questions[i].question) + (text ? strlen(text) : strlen("None of the above")) + 8;
        }
        if (total > OCA_TOOL_RESULT_MAX_BYTES) {
            notice_set(app, "Answers exceed the result limit. Shorten the details.");
            buffer_free(&answer);
            free(value);
            return;
        }
        free(app->answers[app->interaction_question]);
        app->answers[app->interaction_question] = answer.data;
        int last = app->interaction_question + 1 == app->question_count;
        if (last && send_interaction_answers(app) != 0) { free(value); return; }
        display_lines lines = {0};
        question_answer_lines(&lines, &app->questions[app->interaction_question], answer.data, app->cols);
        history_emit(app, &lines, lines.count, 2);
        lines_free(&lines);
        input_clear(app);
        app->interaction_question++;
        if (last) interaction_restore(app);
        else interaction_menu(app);
    } else if (menu == MENU_SAVE || menu == MENU_SAVE_CONTINUE) {
        menu_clear(app);
        report_action_error(app, oca_core_save_answer(app->core, number,
                                                      err, sizeof(err)), err);
    }
    free(value);
}

static void menu_move(tui_app *app, int direction) {
    if (!app->menu_count) return;
    if (app->menu == MENU_HELP) {
        size_t columns = menu_columns(app);
        size_t count = (app->menu_count + columns - 1) / columns;
        if (direction < 0 && app->menu_selection) app->menu_selection--;
        if (direction > 0 && app->menu_selection + 1 < count) app->menu_selection++;
        app->dirty = 1;
        return;
    }
    if (direction < 0)
        app->menu_selection = app->menu_selection ? app->menu_selection - 1 :
                              app->menu_count - 1;
    else
        app->menu_selection = (app->menu_selection + 1) % app->menu_count;
    app->dirty = 1;
}

static void cycle_model(tui_app *app, int direction) {
    oca_core_status status;
    oca_core_get_status(app->core, &status);
    size_t count = oca_core_model_count(app->core);
    if (!count) return;
    size_t current = 0;
    for (size_t i = 0; i < count; i++) {
        if (!strcmp((oca_core_model_at(app->core, i))->id, status.model_id)) current = i;
    }
    for (size_t step = 0; step < count; step++) {
        current = direction > 0 ? (current + 1) % count : (current + count - 1) % count;
        const oca_model *model = oca_core_model_at(app->core, current);
        if (!oca_core_model_available(app->core, model)) continue;
        /* Retain the current level when the new model offers it. */
        const char *level = status.reasoning_name[0] ? status.reasoning_name : NULL;
        char err[256];
        if (oca_core_set_route(app->core, model->id, level, err, sizeof(err)) == 0) return;
        free(app->pending_model);
        app->pending_model = copy_text(model->id);
        reasoning_menu(app, model);
        return;
    }
}

static void cycle_reasoning(tui_app *app, int direction) {
    oca_core_status status;
    oca_core_get_status(app->core, &status);
    const oca_model *model = NULL;
    for (size_t i = 0; i < oca_core_model_count(app->core); i++) {
        const oca_model *candidate = oca_core_model_at(app->core, i);
        if (!strcmp(candidate->id, status.model_id)) model = candidate;
    }
    if (!model || model->reasoning_count == 0) return;
    oca_reasoning current = oca_model_reasoning_index(model, status.reasoning_name);
    if (current == OCA_REASONING_NONE) current = 0;
    size_t count = model->reasoning_count;
    current = (oca_reasoning)((current + (direction > 0 ? 1 : (oca_reasoning)count - 1)) % (oca_reasoning)count);
    const char *name = oca_model_reasoning_name(model, current);
    char err[256];
    report_action_error(app, oca_core_set_route(app->core, status.model_id,
                                                name, err, sizeof(err)), err);
}

/* ============================================================================
 * Input decoding and lifecycle
 * ========================================================================== */

static int escape_match(const char *text, size_t len, int complete) {
    static const char *const sequences[] = {
        "\x1b[A", "\x1b[B", "\x1b[C", "\x1b[D", "\x1b[H", "\x1b[F",
        "\x1b[1~", "\x1b[4~", "\x1b[7~", "\x1b[8~", "\x1bOH", "\x1bOF",
        "\x1b[3~", "\x1b[200~", "\x1b[201~", "\x1b[Z", "\x1b[47;5u",
        "\x1b[5~", "\x1b[6~",
        "\x1bk", "\x1bl", "\x1bK", "\x1bL", "\x1b,", "\x1b."
    };
    for (size_t i = 0; i < sizeof(sequences) / sizeof(sequences[0]); i++) {
        size_t full = strlen(sequences[i]);
        if (len <= full && (!complete || len == full) && !memcmp(text, sequences[i], len)) return 1;
    }
    return 0;
}

static int escape_prefix(const char *text, size_t len) {
    return escape_match(text, len, 0);
}

static int escape_action(tui_app *app) {
    const char *sequence = app->escape;
    if (!escape_match(sequence, strlen(sequence), 1)) return 0;
    if ((app->menu == MENU_SAVE || app->menu == MENU_SAVE_CONTINUE) &&
        strcmp(sequence, "\x1b[200~") && strcmp(sequence, "\x1b[201~")) return 1;
    if (strcmp(sequence, "\x1b[A") && strcmp(sequence, "\x1b[B")) app->vertical_move = 0;
    if (!strcmp(sequence, "\x1b[47;5u")) {
        view_switch(app);
    } else if (!strcmp(sequence, "\x1b[5~") || !strcmp(sequence, "\x1b[6~")) {
        if (main_view(app)->overlay && app->menu == MENU_NONE) {
            size_t step = app->rows > 8 ? (size_t)app->rows - 8 : 1;
            size_t first = step < app->retained.count ? step : app->retained.count;
            if (!strcmp(sequence, "\x1b[5~")) {
                size_t end = app->view_end > step ? app->view_end - step : 0;
                app->view_end = end > first ? end : first;
            } else {
                size_t remaining = app->retained.count - app->view_end;
                app->view_end += step < remaining ? step : remaining;
            }
        }
    } else if (!strcmp(sequence, "\x1b[A")) {
        if (app->menu == MENU_INTERACTION && app->interaction_details) input_vertical(app, -1);
        else if (app->menu != MENU_NONE) menu_move(app, -1);
        else input_vertical(app, -1);
    } else if (!strcmp(sequence, "\x1b[B")) {
        if (app->menu == MENU_INTERACTION && app->interaction_details) input_vertical(app, 1);
        else if (app->menu != MENU_NONE) menu_move(app, 1);
        else input_vertical(app, 1);
    } else if (!strcmp(sequence, "\x1b[C")) {
        app->cursor = next_grapheme(app->input, app->input_len, app->cursor);
    } else if (!strcmp(sequence, "\x1b[D")) {
        app->cursor = previous_grapheme(app->input, app->cursor);
    } else if (!strcmp(sequence, "\x1b[H") || !strcmp(sequence, "\x1b[1~") ||
               !strcmp(sequence, "\x1b[7~") || !strcmp(sequence, "\x1bOH")) {
        app->cursor = 0;
    } else if (!strcmp(sequence, "\x1b[F") || !strcmp(sequence, "\x1b[4~") ||
               !strcmp(sequence, "\x1b[8~") || !strcmp(sequence, "\x1bOF")) {
        app->cursor = app->input_len;
    } else if (!strcmp(sequence, "\x1b[3~")) {
        input_delete(app);
    } else if (!strcmp(sequence, "\x1b[200~")) {
        free(app->paste_text);
        app->paste_text = NULL;
        app->paste_text_len = 0;
        app->paste_text_capacity = 0;
        app->paste_end_len = 0;
        app->paste_saw_cr = 0;
        app->paste_rejected = 0;
        app->utf8_pending_len = 0;
        app->utf8_pending_need = 0;
        app->paste = 1;
    } else if (!strcmp(sequence, "\x1b[201~")) {
        app->paste = 0;
    } else if (!strcmp(sequence, "\x1b[Z")) {
        if (app->menu == MENU_INTERACTION && !app->input_len) {
            app->interaction_details = !app->interaction_details;
            app->dirty = 1;
        }
    } else if (!strcmp(sequence, "\x1bk") || !strcmp(sequence, "\x1bK")) {
        cycle_model(app, -1);
    } else if (!strcmp(sequence, "\x1bl") || !strcmp(sequence, "\x1bL")) {
        cycle_model(app, 1);
    } else if (!strcmp(sequence, "\x1b,")) {
        cycle_reasoning(app, -1);
    } else if (!strcmp(sequence, "\x1b.")) {
        cycle_reasoning(app, 1);
    } else {
        return 0;
    }
    app->dirty = 1;
    return 1;
}

static void plain_escape(tui_app *app) {
    if (app->menu == MENU_SAVE || app->menu == MENU_SAVE_CONTINUE) {
        oca_core_cancel(app->core);
        menu_clear(app);
        app->notice[0] = 0;
    } else if (app->menu == MENU_INTERACTION) {
        oca_core_cancel(app->core);
        interaction_restore(app);
    } else if (app->menu == MENU_COMMANDS) {
        if (app->input_len == 1 && app->input[0] == '/') {
            app->input[0] = '\0';
            app->input_len = 0;
            app->cursor = 0;
        }
        menu_clear(app);
    } else if (app->menu != MENU_NONE) {
        menu_clear(app);
    } else {
        oca_core_status status;
        oca_core_get_status(app->core, &status);
        if (status.working || status.state != OCA_S_IDLE) oca_core_cancel(app->core);
        else oca_core_send_queued(app->core);
    }
    app->dirty = 1;
}

static void handle_byte(tui_app *app, unsigned char byte);

static void paste_append_raw(tui_app *app, const unsigned char *bytes, size_t length) {
    if (app->paste_rejected) return;
    if (length > OCA_PROMPT_MAX_BYTES - app->input_len ||
        app->paste_text_len > OCA_PROMPT_MAX_BYTES - app->input_len - length) {
        app->paste_rejected = 1;
        return;
    }
    size_t required = app->paste_text_len + length + 1;
    if (required > app->paste_text_capacity) {
        size_t capacity = app->paste_text_capacity ? app->paste_text_capacity : 256;
        while (capacity < required) capacity *= 2;
        app->paste_text = checked_realloc(app->paste_text, capacity);
        app->paste_text_capacity = capacity;
    }
    memcpy(app->paste_text + app->paste_text_len, bytes, length);
    app->paste_text_len += length;
    app->paste_text[app->paste_text_len] = '\0';
}

static void paste_append_byte(tui_app *app, unsigned char byte) {
    if (app->paste_saw_cr) {
        app->paste_saw_cr = 0;
        if (byte == '\n') return;
    }
    if (byte == '\0') {
        app->paste_rejected = 2;
    } else if (byte == '\r') {
        const unsigned char newline = '\n';
        paste_append_raw(app, &newline, 1);
        app->paste_saw_cr = 1;
    } else if (byte == '\t') {
        static const unsigned char spaces[] = "    ";
        paste_append_raw(app, spaces, sizeof(spaces) - 1);
    } else {
        paste_append_raw(app, &byte, 1);
    }
}

static void paste_finish(tui_app *app) {
    app->paste = 0;
    app->paste_end_len = 0;
    app->paste_saw_cr = 0;
    if (!app->paste_rejected &&
        !utf8_bytes_valid(app->paste_text ? app->paste_text : "", app->paste_text_len))
        app->paste_rejected = 2;
    if (!app->paste_rejected && app->paste_text_len) {
        if (app->menu == MENU_SAVE || app->menu == MENU_SAVE_CONTINUE) {
            for (size_t i = 0; i < app->paste_text_len && app->save_input_len + 1 < sizeof(app->save_input); i++) {
                unsigned char byte = (unsigned char)app->paste_text[i];
                app->save_input[app->save_input_len++] = byte >= 0x20 && byte < 0x7f ? (char)byte : '?';
            }
            app->save_input[app->save_input_len] = 0;
            app->dirty = 1;
        } else {
            if (app->menu == MENU_INTERACTION) app->interaction_details = 1;
            input_insert(app, app->paste_text, app->paste_text_len);
        }
    } else if (app->paste_rejected == 1)
        transcript(app, STYLE_WARNING, "Paste rejected: input limit exceeded.");
    else if (app->paste_rejected == 2)
        transcript(app, STYLE_WARNING, "Paste rejected: invalid UTF-8 or NUL input.");
    free(app->paste_text);
    app->paste_text = NULL;
    app->paste_text_len = 0;
    app->paste_text_capacity = 0;
    app->paste_rejected = 0;
}

static void paste_feed(tui_app *app, unsigned char byte) {
    static const unsigned char end_marker[] = "\x1b[201~";
    for (;;) {
        if (byte == end_marker[app->paste_end_len]) {
            app->paste_end_len++;
            if (app->paste_end_len == sizeof(end_marker) - 1) paste_finish(app);
            return;
        }
        if (app->paste_end_len) {
            size_t count = app->paste_end_len;
            app->paste_end_len = 0;
            for (size_t i = 0; i < count; i++) paste_append_byte(app, end_marker[i]);
            continue;
        }
        paste_append_byte(app, byte);
        return;
    }
}

static size_t utf8_expected(unsigned char byte) {
    if (byte >= 0xC2 && byte <= 0xDF) return 2;
    if (byte >= 0xE0 && byte <= 0xEF) return 3;
    if (byte >= 0xF0 && byte <= 0xF4) return 4;
    return 0;
}

static void input_utf8_byte(tui_app *app, unsigned char byte) {
    if (!app->utf8_pending_len) {
        size_t expected = utf8_expected(byte);
        if (!expected) return;
        app->utf8_pending[0] = byte;
        app->utf8_pending_len = 1;
        app->utf8_pending_need = expected;
        return;
    }
    if ((byte & 0xC0) != 0x80) {
        app->utf8_pending_len = 0;
        app->utf8_pending_need = 0;
        input_utf8_byte(app, byte);
        return;
    }
    app->utf8_pending[app->utf8_pending_len++] = byte;
    if (app->utf8_pending_len == app->utf8_pending_need) {
        if (utf8_bytes_valid((const char *)app->utf8_pending, app->utf8_pending_len))
            input_insert(app, (const char *)app->utf8_pending, app->utf8_pending_len);
        app->utf8_pending_len = 0;
        app->utf8_pending_need = 0;
    }
}

static void escape_fallback(tui_app *app) {
    unsigned char replay[ESCAPE_MAX];
    size_t count = app->escape_len > 1 ? app->escape_len - 1 : 0;
    if (count) memcpy(replay, app->escape + 1, count);
    app->escape_len = 0;
    app->escape_deadline_ms = 0;
    plain_escape(app);
    for (size_t i = 0; i < count; i++) handle_byte(app, replay[i]);
}

static void feed_escape(tui_app *app, unsigned char byte) {
    if (app->escape_len + 1 >= sizeof(app->escape)) {
        app->escape_len = 0;
        app->escape_deadline_ms = 0;
        plain_escape(app);
        return;
    }
    app->escape[app->escape_len++] = (char)byte;
    app->escape[app->escape_len] = '\0';
    if (escape_action(app)) {
        app->escape_len = 0;
        app->escape_deadline_ms = 0;
        return;
    }
    if (!escape_prefix(app->escape, app->escape_len)) escape_fallback(app);
}


static void exit_control(tui_app *app, int key) {
    if (key == 4 && (app->menu == MENU_SAVE || app->menu == MENU_SAVE_CONTINUE ||
                     app->menu == MENU_INTERACTION)) return;
    if (key == 3 && app->menu != MENU_SAVE && app->menu != MENU_SAVE_CONTINUE &&
        app->menu != MENU_INTERACTION) {
        oca_core_status status;
        oca_core_get_status(app->core, &status);
        int busy = status.working || status.state != OCA_S_IDLE || status.goal == OCA_GOAL_ACTIVE;
        if (app->input_len) {
            if (!busy && (app->menu == MENU_HELP || app->menu == MENU_SKILLS || app->menu == MENU_COMMANDS))
                menu_clear(app);
            input_clear(app);
            return;
        }
        if (status.followup_count) {
            char *queued = NULL, err[256];
            int rc = oca_core_pop_followup(app->core, &queued, err, sizeof(err));
            free(queued);
            report_action_error(app, rc, err);
            app->exit_hint = 0;
            app->exit_hint_restore_ms = 0;
            app->dirty = app->redraw_urgent = 1;
            return;
        }
        if (busy) return;
    }
    if (app->menu == MENU_HELP || app->menu == MENU_SKILLS || app->menu == MENU_COMMANDS) menu_clear(app);
    if (app->menu == MENU_SAVE || app->menu == MENU_SAVE_CONTINUE) {
        oca_core_cancel(app->core);
        menu_clear(app);
        return;
    }
    if (app->menu == MENU_INTERACTION) {
        if (key == 3 && app->interaction_details && app->input_len) {
            input_clear(app);
        }
        return;
    }
    oca_core_status status;
    oca_core_get_status(app->core, &status);
    if (status.working || status.state != OCA_S_IDLE || status.goal == OCA_GOAL_ACTIVE) {
        return;
    } else if (app->input_len) {
        if (key == 4) input_delete(app);
        else input_clear(app);
    } else if (key == 4) {
        app->exit_hint = 0;
        app->exit_hint_restore_ms = 0;
        char err[256];
        int rc = oca_core_quit(app->core, err, sizeof(err));
        report_action_error(app, rc, err);
    } else {
        app->exit_hint_restore_ms = app->exit_hint ? monotonic_ms() + 50 : 0;
        app->exit_hint = 1;
        app->dirty = app->redraw_urgent = 1;
    }
}

static void handle_byte(tui_app *app, unsigned char byte) {
    app->redraw_urgent = 1;
    if (app->paste) {
        paste_feed(app, byte);
        return;
    }
    if (app->utf8_pending_len && (byte & 0xC0) != 0x80) {
        app->utf8_pending_len = 0;
        app->utf8_pending_need = 0;
    }
    if (app->escape_len) {
        feed_escape(app, byte);
        return;
    }
    if (byte == 0x1b) {
        app->utf8_pending_len = 0;
        app->utf8_pending_need = 0;
        app->escape[0] = '\x1b';
        app->escape[1] = '\0';
        app->escape_len = 1;
        app->escape_deadline_ms = monotonic_ms() + 35;
        return;
    }
    if (byte == 0x18 && app->menu == MENU_NONE) {
        char *queued = NULL, err[256];
        if (oca_core_pop_followup(app->core, &queued, err, sizeof(err))) notice_set(app, err);
        else if (queued) input_set(app, queued);
        free(queued);
        return;
    }
    if (app->menu == MENU_INTERACTION && byte == '\t') {
        if (!app->input_len) app->interaction_details = !app->interaction_details;
        app->dirty = 1;
        return;
    }
    if (app->menu == MENU_INTERACTION && !app->interaction_details &&
        byte != '\r' && byte != 0x03 && byte != 0x04 && byte != 0x1f) return;
    if (app->menu == MENU_SAVE || app->menu == MENU_SAVE_CONTINUE) {
        if (byte == 0x03) {
            plain_escape(app);
        } else if (byte == '\r') {
            if (app->save_input_len == 1 && strchr("yYnN", app->save_input[0])) {
                int answer = app->save_input[0] == 'y' || app->save_input[0] == 'Y';
                char question[96], err[256];
                snprintf(question, sizeof(question), "%s (y/n) %s",
                         app->menu == MENU_SAVE ? "Save current session?" : "Continue anyway?", app->save_input);
                app->notice[0] = 0;
                transcript_literal(app, STYLE_RESET, question, 1);
                menu_clear(app);
                report_action_error(app, oca_core_save_answer(app->core, answer, err, sizeof(err)), err);
            } else {
                app->save_input[0] = 0;
                app->save_input_len = 0;
                notice_set(app, "Enter y or n, then press Enter.");
            }
        } else if (byte == 0x7f || byte == 0x08) {
            if (app->save_input_len) app->save_input[--app->save_input_len] = 0;
        } else if (byte >= 0x20 && byte < 0x7f && app->save_input_len + 1 < sizeof(app->save_input)) {
            app->save_input[app->save_input_len++] = (char)byte;
            app->save_input[app->save_input_len] = 0;
        } else if (byte >= 0x80 && app->save_input_len + 1 < sizeof(app->save_input)) {
            app->save_input[app->save_input_len++] = '?';
            app->save_input[app->save_input_len] = 0;
        }
        app->dirty = 1;
        return;
    }
    app->vertical_move = 0;
    if (byte == '\r') {
        if (app->menu != MENU_NONE &&
            app->menu != MENU_HELP && app->menu != MENU_SKILLS)
            menu_activate(app);
        else
            submit_prompt(app);
    } else if (byte == '\n') {
        input_insert(app, "\n", 1);
    } else if (byte == 0x1f) {
        view_switch(app);
    } else if (byte == 0x03) {
        if (app->side_chat) side_close(app);
        else exit_control(app, 3);
    } else if (byte == 0x04) {
        if (app->side_chat) input_delete(app);
        else exit_control(app, 4);
    } else if (byte == 0x7f || byte == 0x08) {
        input_backspace(app);
    } else if (byte == 0x17) {
        input_word_delete(app);
    } else if (byte == '\t') {
        if (app->menu == MENU_NONE) input_insert(app, "    ", 4);
    } else if (byte == '?' && !app->input_len && app->menu == MENU_NONE) {
        help_menu(app);
    } else if (byte >= 0x80) {
        input_utf8_byte(app, byte);
    } else if (byte >= 0x20) {
        input_insert(app, (char *)&byte, 1);
    }
}

static void app_free(tui_app *app) {
    menu_clear(app);
    interaction_free(app);
    stream_clear(app);
    lines_free(&app->retained);
    free(app->owned_workspace);
    free(app->tool_id);
    free(app->tool_name);
    free(app->tool_arguments);
    free(app->input);
    for (size_t i = 0; i < app->history_count; i++) free(app->history[i]);
    free(app->history);
    free(app->history_draft);
    free(app->pending_model);
    free(app->paste_text);
    for (size_t i = 0; i < app->deferred_input_count; i++)
        free(app->deferred_inputs[i]);
    free(app->deferred_inputs);
}

/* ============================================================================
 * Conversation views
 * ========================================================================== */

static void view_restore(tui_app *app) {
    refresh_geometry(app);
    if (!main_view(app)->overlay) erase_block(app);
    app->block_lines = app->block_cursor_row = 0;
    main_view(app)->restore_pending = 0;
    app->dirty = app->redraw_urgent = 1;
}

static void view_select(tui_app *main, tui_app *target) {
    tui_app *previous = main->active_view;
    if (previous == target) return;
    stream_commit(previous, 0);
    if (!main->overlay) erase_block(previous);
    previous->block_lines = previous->block_cursor_row = 0;
    previous->hidden = 1;
    target->hidden = 0;
    main->active_view = target;
    view_restore(target);
}

static void view_switch(tui_app *app) {
    tui_app *main = main_view(app);
    if (!main->side_view || main->side_view->closing) {
        notice_set(app, "Use /btw to open a temporary chat.");
        return;
    }
    view_select(main, app->side_chat ? main : main->side_view);
}

static void side_open(tui_app *app, const char *message) {
    tui_app *main = main_view(app);
    if (main->side_view && main->side_view->closing) {
        notice_set(app, "The temporary chat is closing.");
        return;
    }
    if (!main->side_view) {
        char err[256];
        oca_core *core = oca_core_create_ephemeral(main->core, err, sizeof(err));
        if (!core) {
            notice_set(app, err);
            return;
        }
        tui_app *side = checked_realloc(NULL, sizeof(*side));
        memset(side, 0, sizeof(*side));
        side->core = core;
        side->display = main->display;
        side->main_view = main;
        side->side_chat = 1;
        side->hidden = 1;
        side->running = 1;
        side->input = checked_realloc(NULL, OCA_PROMPT_MAX_BYTES + 1);
        side->input[0] = 0;
        side->owned_workspace = copy_text(main->workspace);
        side->workspace = side->owned_workspace;
        side->signal_pipe[0] = side->signal_pipe[1] = -1;
        main->side_view = side;
        if (!main->overlay) {
            stream_commit(main, 0);
            erase_block(main);
            write_text("\x1b[?1049h\x1b[H\x1b[2J");
            main->terminal.alternate_screen = 1;
            main->overlay = 1;
        }
    }
    tui_app *side = main->side_view;
    view_select(main, side);
    if (message && *message) {
        char err[256];
        if (oca_core_submit_prompt(side->core, message, err, sizeof(err))) {
            /* Keep a rejected temporary-chat message in its composer. */
            notice_set(side, err);
            input_set(side, message);
        } else history_add(side, message);
    }
}

static void side_close(tui_app *app) {
    tui_app *main = main_view(app);
    tui_app *side = main->side_view;
    if (!side || side->closing) return;
    side->closing = 1;
    oca_core_stop(side->core);
    if (main->active_view == side) view_select(main, main);
}

static void side_reap(tui_app *main) {
    tui_app *side = main->side_view;
    if (!side || !side->closing) return;
    oca_core_status status;
    oca_core_get_status(side->core, &status);
    if (status.state != OCA_S_STOPPED) return;
    oca_core_destroy(side->core);
    app_free(side);
    free(side);
    main->side_view = NULL;
    if (main->view_end == main->retained.count) overlay_leave(main);
    main->dirty = main->redraw_urgent = 1;
}

static void overlay_leave(tui_app *main) {
    if (!main->overlay) return;
    write_text("\x1b[?1049l");
    main->terminal.alternate_screen = 0;
    main->overlay = 0;
    main->hidden = 0;
    main->active_view = main;
    main->block_lines = main->block_cursor_row = 0;
    history_flush(main);
    main->dirty = main->redraw_urgent = 1;
}

static int view_tick(tui_app *app, long long now) {
    if (app->exit_hint_restore_ms && now >= app->exit_hint_restore_ms) {
        app->exit_hint_restore_ms = 0;
        app->dirty = app->redraw_urgent = 1;
    }
    if (app->notice[0] && now >= app->notice_deadline_ms) {
        app->notice[0] = 0;
        app->dirty = app->redraw_urgent = 1;
    }
    oca_core_status status;
    oca_core_get_status(app->core, &status);
    if (app->exit_hint && (status.working || status.state != OCA_S_IDLE)) {
        app->exit_hint = 0;
        app->exit_hint_restore_ms = 0;
        app->dirty = app->redraw_urgent = 1;
    }
    int timed = status.working || status.goal == OCA_GOAL_ACTIVE;
    if (timed && now >= app->timer_tick_ms) {
        app->timer_tick_ms = now + 1000;
        app->dirty = app->redraw_urgent = 1;
    }
    int timeout = -1;
    long long deadlines[] = {
        app->notice[0] ? app->notice_deadline_ms : 0,
        timed ? app->timer_tick_ms : 0,
        app->escape_len ? app->escape_deadline_ms : 0,
        app->exit_hint_restore_ms,
        app->dirty && !app->redraw_urgent ? app->last_render_ms + REDRAW_INTERVAL_MS : 0
    };
    for (size_t i = 0; i < sizeof(deadlines) / sizeof(deadlines[0]); i++) {
        if (!deadlines[i]) continue;
        long long remaining = deadlines[i] - now;
        int wait = remaining > 0 && remaining < INT_MAX ? (int)remaining : 0;
        if (timeout < 0 || wait < timeout) timeout = wait;
    }
    return timeout;
}

int oca_tui_run(oca_core *core, const char *workspace, const char *initial_prompt,
                const oca_tui_options *options) {
    setlocale(LC_CTYPE, "");
    signal_exit = 0;
    signal_resize = 0;
    int result = 1;
    tui_app app = {0};
    app.core = core;
    app.display = options ? *options : (oca_tui_options)OCA_TUI_DEFAULT_OPTIONS;
    app.main_view = &app;
    app.active_view = &app;
    app.workspace = workspace;
    app.signal_pipe[0] = -1;
    app.signal_pipe[1] = -1;
    app.input = checked_realloc(NULL, OCA_PROMPT_MAX_BYTES + 1);
    app.input[0] = '\0';
    app.running = 1;
    app.dirty = 1;
    app.redraw_urgent = 1;
    history_load(&app);
    if (signal_pipe_open(app.signal_pipe) != 0) {
        fputs("oca: cannot create the signal wake pipe\n", stderr);
        goto cleanup;
    }
    signal_wake_write = app.signal_pipe[1];
    if (install_signals() != 0) {
        fputs("oca: cannot install terminal signal handlers\n", stderr);
        goto cleanup;
    }
    if (terminal_enter(&app.terminal) != 0) {
        goto cleanup;
    }
    terminal_title(&app);
    print_brand(&app);
    drain_events(&app);
    print_startup_context(&app);
    char replay_error[256];
    if (oca_core_replay(core, replay_error, sizeof(replay_error)) != 0)
        notice_set(&app, replay_error);
    if (initial_prompt && *initial_prompt) {
        char err[256];
        if (oca_core_submit_prompt(core, initial_prompt, err, sizeof(err)) != 0) {
            /* A rejected initial prompt stays in the composer for the user. */
            report_action_error(&app, 1, err);
            input_set(&app, initial_prompt);
        } else history_add(&app, initial_prompt);
    }

    int input_closed = 0;
    while (app.running && !signal_exit && !input_closed) {
        if (signal_resize) {
            signal_resize = 0;
            app.active_view->dirty = app.active_view->redraw_urgent = 1;
        }
        drain_events(&app);
        if (app.side_view) drain_events(app.side_view);
        side_reap(&app);
        if (app.overlay && !app.side_view && app.view_end == app.retained.count) overlay_leave(&app);
        if (app.welcome_pending && !app.side_view) welcome_reset(&app);
        if (!app.running || signal_exit) break;
        tui_app *active = app.active_view;
        if (app.restore_pending) view_restore(active);
        long long now = monotonic_ms();
        int timeout = view_tick(active, now);
        if (active->dirty && (active->redraw_urgent ||
                             now - active->last_render_ms >= REDRAW_INTERVAL_MS))
            render_block(active);
        struct pollfd fds[4] = {
            { .fd = STDIN_FILENO, .events = POLLIN },
            { .fd = oca_core_wake_fd(core), .events = POLLIN },
            { .fd = app.signal_pipe[0], .events = POLLIN },
            { .fd = app.side_view ? oca_core_wake_fd(app.side_view->core) : -1, .events = POLLIN }
        };
        if (app.events_pending || (app.side_view && app.side_view->events_pending)) timeout = 0;
        int result = poll(fds, 4, timeout);
        if (result < 0) {
            if (errno == EINTR) continue;
            transcript(active, STYLE_DANGER, "Terminal poll failed: %s", strerror(errno));
            break;
        }
        now = monotonic_ms();
        if (active->escape_len && now >= active->escape_deadline_ms) escape_fallback(active);
        if (fds[2].revents & (POLLIN | POLLHUP)) drain_descriptor(app.signal_pipe[0]);
        if (fds[1].revents & (POLLIN | POLLHUP)) drain_events(&app);
        if (app.side_view && (fds[3].revents & (POLLIN | POLLHUP))) drain_events(app.side_view);
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            int hung_up = (fds[0].revents & POLLHUP) != 0;
            unsigned char bytes[512];
            ssize_t count = -1;
            while ((count = read(STDIN_FILENO, bytes, sizeof(bytes))) > 0) {
                for (ssize_t i = 0; i < count; i++) handle_byte(app.active_view, bytes[i]);
            }
            if (count == 0 && hung_up) input_closed = 1;
            if (count < 0 && errno != EAGAIN && errno != EINTR) break;
        }
    }
    if (signal_exit || input_closed) oca_core_cancel(core);
    if (app.side_view) {
        oca_core_stop(app.side_view->core);
        if (app.active_view == app.side_view) erase_block(app.side_view);
        oca_core_destroy(app.side_view->core);
        app_free(app.side_view);
        free(app.side_view);
        app.side_view = NULL;
    }
    app.hidden = 0;
    app.active_view = &app;
    overlay_leave(&app);
    stream_commit(&app, 1);
    history_flush(&app);
    erase_block(&app);
    result = app.failed ? 1 : 0;
cleanup:
    terminal_restore();
    signal_wake_write = -1;
    if (app.signal_pipe[0] >= 0) close(app.signal_pipe[0]);
    if (app.signal_pipe[1] >= 0) close(app.signal_pipe[1]);
    app_free(&app);
    return result;
}
