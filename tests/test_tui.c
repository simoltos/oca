#define _XOPEN_SOURCE 700
#include "oca_core.h"

#include "fixture_server.h"
#include "fixture_models.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#define CSI_MAX 128

typedef struct {
    int rows;
    int cols;
    int row;
    int col;
    int saved_row;
    int saved_col;
    int top;
    int bottom;
    int autowrap;
    int pending_wrap;
    int cursor_visible;
    int mouse_tracking, mouse_sgr;
    int bracketed_paste;
    int alternate_screen;
    int nondefault_margin;
    int parser;
    int foreground;
    char selected_ascii;
    int check_colors;
    size_t frame_glyphs, queue_separators, user_markers, assistant_markers;
    char csi[CSI_MAX];
    size_t csi_len;
    unsigned char utf8[4];
    size_t utf8_len;
    size_t utf8_need;
    char *cells;
    char *primary_cells;
    int primary_rows, primary_cols, primary_row, primary_col;
    char **history;
    size_t history_count;
} virtual_terminal;

typedef struct {
    char root[64];
    char home[96];
    char workspace[96];
} test_environment;

typedef struct {
    pid_t child;
    int master;
    int slave;
    struct termios original;
    virtual_terminal terminal;
    char *raw;
    size_t raw_len;
    size_t raw_capacity;
} app_run;

static const char *test_term = "xterm-256color";

static long long now_ms(void) {
    struct timespec time;
    assert(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
    return (long long)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

static void *xrealloc(void *old, size_t size) {
    void *next = realloc(old, size ? size : 1);
    assert(next);
    return next;
}

static void raw_append(app_run *run, const char *bytes, size_t length) {
    size_t required = run->raw_len + length + 1;
    if (required > run->raw_capacity) {
        size_t capacity = run->raw_capacity ? run->raw_capacity : 65536;
        while (capacity < required) capacity *= 2;
        run->raw = xrealloc(run->raw, capacity);
        run->raw_capacity = capacity;
    }
    memcpy(run->raw + run->raw_len, bytes, length);
    run->raw_len += length;
    run->raw[run->raw_len] = '\0';
}

static int bytes_contain(const char *haystack, size_t haystack_len,
                         const char *needle, size_t needle_len) {
    if (!needle_len) return 1;
    if (needle_len > haystack_len) return 0;
    for (size_t i = 0; i <= haystack_len - needle_len; i++) {
        if (!memcmp(haystack + i, needle, needle_len)) return 1;
    }
    return 0;
}

static void vt_clear_row(virtual_terminal *terminal, int row) {
    memset(terminal->cells + (size_t)row * (size_t)terminal->cols,
           ' ', (size_t)terminal->cols);
}

static char *vt_row_copy(const virtual_terminal *terminal, int row) {
    char *copy = xrealloc(NULL, (size_t)terminal->cols + 1);
    memcpy(copy, terminal->cells + (size_t)row * (size_t)terminal->cols,
           (size_t)terminal->cols);
    int end = terminal->cols;
    while (end > 0 && copy[end - 1] == ' ') end--;
    copy[end] = '\0';
    return copy;
}

static void vt_save_history(virtual_terminal *terminal, int row) {
    if (terminal->alternate_screen) return;
    terminal->history = xrealloc(terminal->history,
        (terminal->history_count + 1) * sizeof(*terminal->history));
    terminal->history[terminal->history_count++] = vt_row_copy(terminal, row);
}

static void vt_scroll(virtual_terminal *terminal) {
    if (terminal->top == 0 && terminal->bottom == terminal->rows - 1)
        vt_save_history(terminal, terminal->top);
    size_t row_bytes = (size_t)terminal->cols;
    size_t rows = (size_t)(terminal->bottom - terminal->top);
    memmove(terminal->cells + (size_t)terminal->top * row_bytes,
            terminal->cells + (size_t)(terminal->top + 1) * row_bytes,
            rows * row_bytes);
    vt_clear_row(terminal, terminal->bottom);
}

static void vt_linefeed(virtual_terminal *terminal) {
    terminal->pending_wrap = 0;
    if (terminal->row == terminal->bottom) vt_scroll(terminal);
    else if (terminal->row < terminal->rows - 1) terminal->row++;
}

static void vt_put(virtual_terminal *terminal, char value, int width) {
    if (width <= 0) return;
    if (!terminal->selected_ascii && terminal->foreground == 2 &&
        value >= '!' && value <= '~' && value != '#') terminal->selected_ascii = value;
    if (terminal->pending_wrap && terminal->autowrap) {
        terminal->col = 0;
        vt_linefeed(terminal);
    }
    terminal->pending_wrap = 0;
    if (terminal->col >= terminal->cols) terminal->col = terminal->cols - 1;
    terminal->cells[(size_t)terminal->row * (size_t)terminal->cols +
                    (size_t)terminal->col] = value;
    for (int i = 1; i < width && terminal->col + i < terminal->cols; i++) {
        terminal->cells[(size_t)terminal->row * (size_t)terminal->cols +
                        (size_t)(terminal->col + i)] = ' ';
    }
    if (terminal->col + width >= terminal->cols) {
        terminal->col = terminal->cols - 1;
        terminal->pending_wrap = terminal->autowrap;
    } else {
        terminal->col += width;
    }
}

static int csi_parameter(const virtual_terminal *terminal, size_t wanted,
                         int fallback) {
    const char *at = terminal->csi;
    if (*at == '?') at++;
    size_t index = 0;
    while (*at) {
        char *end = NULL;
        long value = strtol(at, &end, 10);
        if (index == wanted) return end == at ? fallback : (int)value;
        if (*end != ';') break;
        at = end + 1;
        index++;
    }
    return fallback;
}

static void vt_csi(virtual_terminal *terminal, unsigned char final) {
    terminal->csi[terminal->csi_len] = '\0';
    int first = csi_parameter(terminal, 0, 0);
    int amount = first > 0 ? first : 1;
    int private_mode = terminal->csi[0] == '?';
    if (final == 'm') {
        for (size_t i = 0;; i++) {
            int code = csi_parameter(terminal, i, -1);
            if (code < 0) break;
            if (code == 0 || code == 39) terminal->foreground = -1;
            else if (code >= 30 && code <= 37) terminal->foreground = code - 30;
            else if (code == 38 || code == 48) {
                int mode = csi_parameter(terminal, ++i, -1), color = -1;
                if (mode == 5) color = csi_parameter(terminal, ++i, -1);
                else if (mode == 2) {
                    int red = csi_parameter(terminal, ++i, 0);
                    int green = csi_parameter(terminal, ++i, 0);
                    int blue = csi_parameter(terminal, ++i, 0);
                    color = 0x1000000 | (red << 16) | (green << 8) | blue;
                }
                if (code == 38) terminal->foreground = color;
            }
        }
    } else if (final == 'A') {
        terminal->row -= amount;
        if (terminal->row < 0) terminal->row = 0;
    } else if (final == 'B') {
        terminal->row += amount;
        if (terminal->row >= terminal->rows) terminal->row = terminal->rows - 1;
    } else if (final == 'C') {
        terminal->col += amount;
        if (terminal->col >= terminal->cols) terminal->col = terminal->cols - 1;
    } else if (final == 'D') {
        terminal->col -= amount;
        if (terminal->col < 0) terminal->col = 0;
    } else if (final == 'H' || final == 'f') {
        int row = csi_parameter(terminal, 0, 1);
        int col = csi_parameter(terminal, 1, 1);
        terminal->row = row > 0 ? row - 1 : 0;
        terminal->col = col > 0 ? col - 1 : 0;
        if (terminal->row >= terminal->rows) terminal->row = terminal->rows - 1;
        if (terminal->col >= terminal->cols) terminal->col = terminal->cols - 1;
    } else if (final == 'K') {
        if (first == 2) {
            vt_clear_row(terminal, terminal->row);
        } else {
            memset(terminal->cells + (size_t)terminal->row * (size_t)terminal->cols +
                   (size_t)terminal->col, ' ',
                   (size_t)(terminal->cols - terminal->col));
        }
    } else if (final == 'J') {
        size_t start = first == 2 ? 0 :
            (size_t)terminal->row * (size_t)terminal->cols + (size_t)terminal->col;
        memset(terminal->cells + start, ' ',
               (size_t)terminal->rows * (size_t)terminal->cols - start);
    } else if (final == 'r') {
        terminal->row = 0;
        terminal->col = 0;
        if (!terminal->csi_len) {
            terminal->top = 0;
            terminal->bottom = terminal->rows - 1;
        } else {
            terminal->top = csi_parameter(terminal, 0, 1) - 1;
            terminal->bottom = csi_parameter(terminal, 1, terminal->rows) - 1;
            terminal->nondefault_margin = 1;
        }
    } else if (private_mode && (final == 'h' || final == 'l')) {
        int enabled = final == 'h';
        if (first == 6) terminal->row = terminal->col = 0;
        else if (first == 7) terminal->autowrap = enabled;
        else if (first == 25) terminal->cursor_visible = enabled;
        else if (first == 2004) terminal->bracketed_paste = enabled;
        else if (first == 1000) terminal->mouse_tracking = enabled;
        else if (first == 1006) terminal->mouse_sgr = enabled;
        else if (first == 1049 && enabled != terminal->alternate_screen) {
            if (enabled) {
                terminal->primary_cells = terminal->cells;
                terminal->primary_rows = terminal->rows;
                terminal->primary_cols = terminal->cols;
                terminal->primary_row = terminal->row;
                terminal->primary_col = terminal->col;
                terminal->cells = xrealloc(NULL, (size_t)terminal->rows * (size_t)terminal->cols);
                memset(terminal->cells, ' ', (size_t)terminal->rows * (size_t)terminal->cols);
                terminal->row = terminal->col = 0;
            } else {
                memset(terminal->cells, ' ', (size_t)terminal->rows * (size_t)terminal->cols);
                int shift = terminal->primary_row >= terminal->rows ? terminal->primary_row - terminal->rows + 1 : 0;
                int rows = terminal->primary_rows - shift;
                if (rows > terminal->rows) rows = terminal->rows;
                int cols = terminal->primary_cols < terminal->cols ? terminal->primary_cols : terminal->cols;
                for (int i = 0; i < rows; i++)
                    memcpy(terminal->cells + (size_t)i * (size_t)terminal->cols,
                           terminal->primary_cells + (size_t)(i + shift) * (size_t)terminal->primary_cols, (size_t)cols);
                free(terminal->primary_cells);
                terminal->primary_cells = NULL;
                terminal->row = terminal->primary_row - shift;
                terminal->col = terminal->primary_col < terminal->cols ? terminal->primary_col : terminal->cols - 1;
            }
            terminal->alternate_screen = enabled;
        }
    }
    terminal->pending_wrap = 0;
}

static size_t utf8_need(unsigned char byte) {
    if (byte >= 0xC2 && byte <= 0xDF) return 2;
    if (byte >= 0xE0 && byte <= 0xEF) return 3;
    if (byte >= 0xF0 && byte <= 0xF4) return 4;
    return 0;
}

static void vt_utf8_finish(virtual_terminal *terminal) {
    mbstate_t state;
    memset(&state, 0, sizeof(state));
    wchar_t value;
    size_t length = mbrtowc(&value, (const char *)terminal->utf8,
                            terminal->utf8_len, &state);
    int width = length == (size_t)-1 || length == (size_t)-2 ? 1 : wcwidth(value);
    if (width < 0) width = 1;
    if (terminal->check_colors && length != (size_t)-1 && length != (size_t)-2) {
        if (value == L'│' || value == L'╭' || value == L'╮' || value == L'╰' || value == L'╯') {
            assert(terminal->foreground == 0x17da1de);
            terminal->frame_glyphs++;
        } else if (value == L'─') {
            if (terminal->foreground == 0x17da1de) terminal->frame_glyphs++;
            else {
                assert(terminal->foreground == 0x15e6673);
                terminal->queue_separators++;
            }
        } else if (value == L'›') {
            assert(terminal->foreground == 2 || terminal->foreground == 0x1499b5b);
            terminal->user_markers++;
        } else if (value == L'✦') {
            assert(terminal->foreground == 208);
            terminal->assistant_markers++;
        }
    }
    vt_put(terminal, '#', width);
    terminal->utf8_len = 0;
    terminal->utf8_need = 0;
}

static void vt_feed(virtual_terminal *terminal, const char *bytes, size_t length) {
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char)bytes[i];
        if (terminal->parser == 3) {
            if (byte == 0x07) terminal->parser = 0;
            else if (byte == 0x1B) terminal->parser = 4;
            continue;
        }
        if (terminal->parser == 4) {
            terminal->parser = byte == '\\' ? 0 : 3;
            continue;
        }
        if (terminal->parser == 1) {
            if (byte == '[') {
                terminal->parser = 2;
                terminal->csi_len = 0;
            } else if (byte == ']') {
                terminal->parser = 3;
            } else {
                if (byte == '7') {
                    terminal->saved_row = terminal->row;
                    terminal->saved_col = terminal->col;
                } else if (byte == '8') {
                    terminal->row = terminal->saved_row < terminal->rows ? terminal->saved_row : terminal->rows - 1;
                    terminal->col = terminal->saved_col < terminal->cols ? terminal->saved_col : terminal->cols - 1;
                }
                terminal->parser = 0;
            }
            continue;
        }
        if (terminal->parser == 2) {
            if (byte >= 0x40 && byte <= 0x7E) {
                vt_csi(terminal, byte);
                terminal->parser = 0;
                terminal->csi_len = 0;
            } else if (terminal->csi_len + 1 < sizeof(terminal->csi)) {
                terminal->csi[terminal->csi_len++] = (char)byte;
            }
            continue;
        }
        if (terminal->utf8_len) {
            if ((byte & 0xC0) == 0x80) {
                terminal->utf8[terminal->utf8_len++] = byte;
                if (terminal->utf8_len == terminal->utf8_need) vt_utf8_finish(terminal);
                continue;
            }
            terminal->utf8_len = terminal->utf8_need = 0;
        }
        if (byte == 0x1B) {
            terminal->parser = 1;
        } else if (byte == '\r') {
            terminal->col = 0;
            terminal->pending_wrap = 0;
        } else if (byte == '\n') {
            vt_linefeed(terminal);
        } else if (byte == '\b') {
            if (terminal->col > 0) terminal->col--;
        } else if (byte >= 0x20 && byte < 0x7F) {
            vt_put(terminal, (char)byte, 1);
        } else if (byte >= 0x80) {
            size_t need = utf8_need(byte);
            if (need) {
                terminal->utf8[0] = byte;
                terminal->utf8_len = 1;
                terminal->utf8_need = need;
            } else {
                vt_put(terminal, '?', 1);
            }
        }
    }
}

static void vt_init(virtual_terminal *terminal, int rows, int cols) {
    memset(terminal, 0, sizeof(*terminal));
    terminal->rows = rows;
    terminal->cols = cols;
    terminal->bottom = rows - 1;
    terminal->autowrap = 1;
    terminal->cursor_visible = 1;
    terminal->cells = xrealloc(NULL, (size_t)rows * (size_t)cols);
    memset(terminal->cells, ' ', (size_t)rows * (size_t)cols);
}

static void vt_resize(virtual_terminal *terminal, int rows, int cols) {
    char *next = xrealloc(NULL, (size_t)rows * (size_t)cols);
    memset(next, ' ', (size_t)rows * (size_t)cols);
    int shift = terminal->row >= rows ? terminal->row - rows + 1 : 0;
    for (int i = 0; i < shift && i < terminal->rows; i++) vt_save_history(terminal, i);
    int copy_rows = terminal->rows - shift;
    if (copy_rows > rows) copy_rows = rows;
    int copy_cols = terminal->cols < cols ? terminal->cols : cols;
    for (int row = 0; row < copy_rows; row++) {
        memcpy(next + (size_t)row * (size_t)cols,
               terminal->cells + (size_t)(row + shift) * (size_t)terminal->cols,
               (size_t)copy_cols);
    }
    free(terminal->cells);
    terminal->cells = next;
    terminal->rows = rows;
    terminal->cols = cols;
    terminal->row -= shift;
    terminal->saved_row -= shift;
    if (terminal->saved_row < 0) terminal->saved_row = 0;
    if (terminal->saved_row >= rows) terminal->saved_row = rows - 1;
    if (terminal->saved_col >= cols) terminal->saved_col = cols - 1;
    if (terminal->row < 0) terminal->row = 0;
    if (terminal->row >= rows) terminal->row = rows - 1;
    if (terminal->col >= cols) terminal->col = cols - 1;
    terminal->top = 0;
    terminal->bottom = rows - 1;
    terminal->pending_wrap = 0;
}

static int vt_rows_contain(const virtual_terminal *terminal, const char *needle) {
    for (int row = 0; row < terminal->rows; row++) {
        char *text = vt_row_copy(terminal, row);
        int found = strstr(text, needle) != NULL;
        free(text);
        if (found) return 1;
    }
    return 0;
}

static int vt_history_contains(const virtual_terminal *terminal, const char *needle) {
    for (size_t i = 0; i < terminal->history_count; i++) {
        if (strstr(terminal->history[i], needle)) return 1;
    }
    return 0;
}

static int vt_contains(const virtual_terminal *terminal, const char *needle) {
    return vt_rows_contain(terminal, needle) || vt_history_contains(terminal, needle);
}

static int vt_find_row(const virtual_terminal *terminal, const char *needle) {
    for (int row = 0; row < terminal->rows; row++) {
        char *text = vt_row_copy(terminal, row);
        int found = strstr(text, needle) != NULL;
        free(text);
        if (found) return row;
    }
    return -1;
}

static int vt_find_last_row(const virtual_terminal *terminal, const char *needle) {
    for (int row = terminal->rows - 1; row >= 0; row--) {
        char *text = vt_row_copy(terminal, row);
        int found = strstr(text, needle) != NULL;
        free(text);
        if (found) return row;
    }
    return -1;
}

static void vt_free(virtual_terminal *terminal) {
    free(terminal->cells);
    free(terminal->primary_cells);
    for (size_t i = 0; i < terminal->history_count; i++) free(terminal->history[i]);
    free(terminal->history);
    memset(terminal, 0, sizeof(*terminal));
}

static void environment_create(test_environment *environment) {
    snprintf(environment->root, sizeof(environment->root),
             "/tmp/oca-tui-test-XXXXXX");
    assert(mkdtemp(environment->root));
    snprintf(environment->home, sizeof(environment->home), "%s/home", environment->root);
    snprintf(environment->workspace, sizeof(environment->workspace),
             "%s/work", environment->root);
    assert(mkdir(environment->home, 0700) == 0);
    assert(mkdir(environment->workspace, 0700) == 0);
    fixture_write_config_saved(environment->home);
    char config[512];
    snprintf(config, sizeof(config), "%s/.oca/config.json", environment->home);
    FILE *file = fopen(config, "w");
    assert(file && fputs("{\"last_used\":{\"model\":\"kimi-k3\",\"reasoning\":\"low\"},"
                        "\"show\":{\"reasoning\":false,\"read\":false,\"write\":false,"
                        "\"edit\":false,\"bash\":false}}", file) >= 0 && !fclose(file));
}

static void environment_visibility(const test_environment *environment, int enabled) {
    fixture_write_config_saved(environment->home);
    char path[256];
    snprintf(path, sizeof(path), "%s/.oca/config.json", environment->home);
    FILE *file = fopen(path, "r+");
    assert(file && !fseek(file, -1, SEEK_END));
    const char *value = enabled ? "true" : "false";
    assert(fprintf(file, ",\"show\":{\"reasoning\":%s,\"read\":%s,"
        "\"write\":%s,\"edit\":%s,\"bash\":%s}}",
        value, value, value, value, value) > 0);
    assert(!fclose(file));
}

static void environment_add_skill(const test_environment *environment) {
    char path[256];
    snprintf(path, sizeof(path), "%s/.oca", environment->home);
    if (mkdir(path, 0700) && errno != EEXIST) assert(0);
    snprintf(path, sizeof(path), "%s/.oca/skills", environment->home);
    assert(mkdir(path, 0700) == 0);
    snprintf(path, sizeof(path), "%s/.oca/skills/sample-skill", environment->home);
    assert(mkdir(path, 0700) == 0);
    snprintf(path, sizeof(path), "%s/.oca/skills/sample-skill/SKILL.md",
             environment->home);
    FILE *file = fopen(path, "wb");
    assert(file);
    const char *text = "---\nname: sample-skill\n"
                       "description: Inspect sample data.\n---\n\nUse sample data.\n";
    assert(fwrite(text, 1, strlen(text), file) == strlen(text));
    assert(fclose(file) == 0);
}

static void environment_destroy(test_environment *environment) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/.oca/config.json", environment->home);
    unlink(path);
    snprintf(path, sizeof(path), "%s/.oca/models.json", environment->home);
    unlink(path);
    snprintf(path, sizeof(path), "%s/.oca/history", environment->home);
    unlink(path);
    snprintf(path, sizeof(path), "%s/.oca/auth.json", environment->home);
    unlink(path);
    snprintf(path, sizeof(path), "%s/.oca/skills/sample-skill/SKILL.md",
             environment->home);
    unlink(path);
    snprintf(path, sizeof(path), "%s/.oca/skills/sample-skill", environment->home);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/.oca/skills", environment->home);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/.oca/sessions", environment->home);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/.oca", environment->home);
    rmdir(path);
    assert(rmdir(environment->workspace) == 0);
    assert(rmdir(environment->home) == 0);
    assert(rmdir(environment->root) == 0);
}

static void app_start_args(app_run *run, const char *executable,
                          const test_environment *environment, int rows, int cols,
                          char *const args[]) {
    memset(run, 0, sizeof(*run));
    run->master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    assert(run->master >= 0);
    assert(grantpt(run->master) == 0 && unlockpt(run->master) == 0);
    char *slave_name = ptsname(run->master);
    assert(slave_name);
    run->slave = open(slave_name, O_RDWR | O_NOCTTY | O_CLOEXEC);
    assert(run->slave >= 0);
    struct winsize size = { .ws_row = (unsigned short)rows,
                            .ws_col = (unsigned short)cols };
    assert(ioctl(run->slave, TIOCSWINSZ, &size) == 0);
    assert(tcgetattr(run->slave, &run->original) == 0);
    vt_init(&run->terminal, rows, cols);

    run->child = fork();
    assert(run->child >= 0);
    if (run->child == 0) {
        assert(setsid() >= 0);
#ifdef TIOCSCTTY
        assert(ioctl(run->slave, TIOCSCTTY, 0) == 0);
#endif
        assert(dup2(run->slave, STDIN_FILENO) >= 0);
        assert(dup2(run->slave, STDOUT_FILENO) >= 0);
        assert(dup2(run->slave, STDERR_FILENO) >= 0);
        close(run->master);
        close(run->slave);
        assert(chdir(environment->workspace) == 0);
        assert(setenv("HOME", environment->home, 1) == 0);
        assert(setenv("TERM", test_term, 1) == 0);
        assert(setenv("LC_ALL", "C.UTF-8", 1) == 0);
        assert(setenv("DEBUGINFOD_URLS", "", 1) == 0);
        execv(executable, args);
        _exit(127);
    }
    int flags = fcntl(run->master, F_GETFL);
    assert(flags >= 0 && fcntl(run->master, F_SETFL, flags | O_NONBLOCK) == 0);
}

static void app_start(app_run *run, const char *executable,
                      const test_environment *environment, int rows, int cols) {
    char *args[] = {(char *)executable, NULL};
    app_start_args(run, executable, environment, rows, cols, args);
}

static void app_pump(app_run *run, int milliseconds) {
    struct pollfd descriptor = { .fd = run->master, .events = POLLIN };
    int ready;
    do {
        ready = poll(&descriptor, 1, milliseconds);
    } while (ready < 0 && errno == EINTR);
    assert(ready >= 0);
    if (ready > 0 && (descriptor.revents & (POLLIN | POLLHUP))) {
        char bytes[8192];
        ssize_t count;
        while ((count = read(run->master, bytes, sizeof(bytes))) > 0) {
            raw_append(run, bytes, (size_t)count);
            vt_feed(&run->terminal, bytes, (size_t)count);
        }
        assert(count == 0 || errno == EAGAIN || errno == EIO);
    }
}

static void app_settle(app_run *run) {
    size_t previous = run->raw_len;
    int quiet = 0;
    while (quiet < 60) {
        app_pump(run, 20);
        if (run->raw_len == previous) quiet += 20;
        else quiet = 0;
        previous = run->raw_len;
    }
}

static int app_wait_current(app_run *run, const char *needle, int milliseconds) {
    long long deadline = now_ms() + milliseconds;
    while (now_ms() < deadline) {
        if (vt_rows_contain(&run->terminal, needle)) return 1;
        app_pump(run, 25);
    }
    return vt_rows_contain(&run->terminal, needle);
}

static int app_wait_queue_legend(app_run *run, int milliseconds) {
    long long deadline = now_ms() + milliseconds;
    while (now_ms() < deadline) {
        int row = vt_find_row(&run->terminal, "Esc: interrupt/send");
        if (row >= 0 && vt_rows_contain(&run->terminal, "Ctrl+X: edit") &&
            vt_rows_contain(&run->terminal, "Ctrl+C: remove")) return 1;
        app_pump(run, 25);
    }
    return 0;
}

static int app_history_contains(app_run *run, const char *needle);

static int app_wait_anywhere(app_run *run, const char *needle, int milliseconds) {
    long long deadline = now_ms() + milliseconds;
    while (now_ms() < deadline) {
        if (vt_contains(&run->terminal, needle)) return 1;
        app_pump(run, 25);
    }
    return app_history_contains(run, needle);
}

static int app_wait_raw(app_run *run, const char *needle, int milliseconds) {
    long long deadline = now_ms() + milliseconds;
    while (now_ms() < deadline) {
        if (run->raw && strstr(run->raw, needle)) return 1;
        app_pump(run, 25);
    }
    return run->raw && strstr(run->raw, needle);
}

static int app_wait_raw_after(app_run *run, size_t offset, const char *needle,
                              int milliseconds) {
    long long deadline = now_ms() + milliseconds;
    size_t length = strlen(needle);
    while (now_ms() < deadline) {
        if (run->raw_len >= offset &&
            bytes_contain(run->raw + offset, run->raw_len - offset, needle, length))
            return 1;
        app_pump(run, 25);
    }
    return run->raw_len >= offset &&
           bytes_contain(run->raw + offset, run->raw_len - offset, needle, length);
}

static int app_wait_current_absent(app_run *run, const char *needle,
                                   int milliseconds) {
    long long deadline = now_ms() + milliseconds;
    while (now_ms() < deadline) {
        app_pump(run, 25);
        if (!vt_rows_contain(&run->terminal, needle)) return 1;
    }
    return !vt_rows_contain(&run->terminal, needle);
}

static int app_wait_status_absent(app_run *run, const char *needle,
                                  int milliseconds) {
    long long deadline = now_ms() + milliseconds;
    while (now_ms() < deadline) {
        app_pump(run, 25);
        int row = vt_find_row(&run->terminal, "cache hit");
        if (row < 0) continue;
        char *text = vt_row_copy(&run->terminal, row);
        int absent = !strstr(text, needle);
        free(text);
        if (absent) return 1;
    }
    return 0;
}

static int app_wait_cursor(app_run *run, const char *line, int column,
                           int milliseconds) {
    long long deadline = now_ms() + milliseconds;
    while (now_ms() < deadline) {
        int row = vt_find_row(&run->terminal, line);
        if (row >= 0 && run->terminal.row == row && run->terminal.col == column)
            return 1;
        app_pump(run, 20);
    }
    int row = vt_find_row(&run->terminal, line);
    return row >= 0 && run->terminal.row == row && run->terminal.col == column;
}

static void app_send(app_run *run, const void *data, size_t length) {
    const unsigned char *bytes = data;
    while (length) {
        ssize_t count = write(run->master, bytes, length);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && errno == EAGAIN) {
            app_pump(run, 1);
            continue;
        }
        assert(count > 0);
        bytes += count;
        length -= (size_t)count;
    }
}

static void app_send_text(app_run *run, const char *text) {
    app_send(run, text, strlen(text));
}

/* Search OCA's retained history through its actual navigation keys, then
 * return to the latest output. Native terminal history stays untouched. */
static int app_history_contains(app_run *run, const char *needle) {
    if (vt_rows_contain(&run->terminal, needle)) return 1;
    int found = 0, pages = 0;
    /* Consecutive blank history rows can produce identical visible pages. */
    for (; pages < 200 && !found; pages++) {
        app_send_text(run, "\x1b[5~");
        app_settle(run);
        found = vt_rows_contain(&run->terminal, needle);
    }
    for (int i = 0; i <= pages; i++) app_send_text(run, "\x1b[6~");
    app_settle(run);
    return found;
}

static void app_resize(app_run *run, int rows, int cols) {
    struct winsize size = { .ws_row = (unsigned short)rows,
                            .ws_col = (unsigned short)cols };
    vt_resize(&run->terminal, rows, cols);
    assert(ioctl(run->slave, TIOCSWINSZ, &size) == 0);
    assert(kill(run->child, SIGWINCH) == 0);
    size_t before = run->raw_len;
    long long deadline = now_ms() + 2000;
    while (now_ms() < deadline && run->raw_len == before) app_pump(run, 20);
    assert(run->raw_len > before);
}

static int app_wait_exit(app_run *run, int milliseconds) {
    long long deadline = now_ms() + milliseconds;
    int status = 0;
    do {
        app_pump(run, milliseconds ? 20 : 0);
        pid_t result = waitpid(run->child, &status, WNOHANG);
        if (result == run->child) {
            run->child = -1;
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
        }
        assert(result == 0 || (result < 0 && errno == EINTR));
    } while (now_ms() < deadline);
    return -1;
}

static void app_assert_restored(app_run *run) {
    struct termios restored;
    assert(tcgetattr(run->slave, &restored) == 0);
    assert(restored.c_iflag == run->original.c_iflag);
    assert(restored.c_oflag == run->original.c_oflag);
    assert(restored.c_cflag == run->original.c_cflag);
    assert(restored.c_lflag == run->original.c_lflag);
    assert(restored.c_cc[VMIN] == run->original.c_cc[VMIN]);
    assert(restored.c_cc[VTIME] == run->original.c_cc[VTIME]);
    assert(run->raw && strstr(run->raw, "\x1b[?2004l"));
    assert(run->raw && strstr(run->raw, "\x1b[?7h"));
    assert(!run->terminal.bracketed_paste);
    assert(!run->terminal.mouse_tracking && !run->terminal.mouse_sgr);
    assert(run->terminal.cursor_visible);
    assert(run->terminal.autowrap);
    assert(!run->terminal.alternate_screen);
    assert(run->terminal.top == 0 &&
           run->terminal.bottom == run->terminal.rows - 1);
}

static void app_close(app_run *run) {
    if (run->child > 0) {
        kill(run->child, SIGKILL);
        waitpid(run->child, NULL, 0);
    }
    close(run->master);
    close(run->slave);
    vt_free(&run->terminal);
    free(run->raw);
    memset(run, 0, sizeof(*run));
    run->child = -1;
}

static void app_quit_without_save(app_run *run) {
    app_send_text(run, "\x1b");
    app_settle(run);
    if (vt_rows_contain(&run->terminal, "Working") || vt_rows_contain(&run->terminal, "Canceling")) {
        app_send_text(run, "\x1b");
        assert(app_wait_current_absent(run, "Working", 7000));
    }
    app_send_text(run, "\x03/quit\r");
    int answered = 0;
    long long deadline = now_ms() + 5000;
    int status = -1;
    while (now_ms() < deadline) {
        app_pump(run, 20);
        char *current = vt_row_copy(&run->terminal, run->terminal.row);
        int save_question = 0;
        if (!strstr(current, "Ask OCA anything") && !strstr(current, " /quit") && !strstr(current, "! ")) {
            for (int i = 0; i < 3 && i <= run->terminal.row; i++) {
                char *row = vt_row_copy(&run->terminal, run->terminal.row - i);
                if (strstr(row, "Save current")) save_question = 1;
                free(row);
            }
        }
        free(current);
        if (!answered && save_question) {
            app_settle(run);
            app_send_text(run, "n\r");
            answered = 1;
        }
        status = app_wait_exit(run, 0);
        if (status >= 0) break;
    }
    if (status < 0) status = app_wait_exit(run, 1000);
    if (status != 0) {
        fprintf(stderr, "quit status=%d answered=%d\n", status, answered);
        for (int row = 0; row < run->terminal.rows; row++) {
            char *text = vt_row_copy(&run->terminal, row);
            fprintf(stderr, "%02d: [%s]\n", row, text);
            free(text);
        }
    }
    assert(status == 0);
    app_assert_restored(run);
}

static void app_paste_text(app_run *run, const char *text, int submit) {
    app_send_text(run, "\x1b[200~");
    app_send_text(run, text);
    app_send_text(run, "\x1b[201~");
    if (submit) app_send_text(run, "\r");
}

static void test_question_details(const char *executable) {
    test_environment environment;
    environment_create(&environment);
    app_run run;
    app_start(&run, executable, &environment, 24, 80);
    assert(app_wait_current(&run, "Ask OCA anything", 5000));
    app_send_text(&run, "fixture:repair-ask\rkeep-draft");
    assert(app_wait_current(&run, "None of the above", 3000));
    assert(app_wait_current(&run, "Which color?", 3000));
    assert(vt_contains(&run.terminal, "fixture:repair-ask"));
    assert(!vt_rows_contain(&run.terminal, "Ask OCA anything"));
    assert(app_wait_current(&run, "Add details (optional)", 3000));
    assert(app_wait_raw(&run, "\x1b[36mTab", 3000));
    assert(!strstr(run.raw, "↑/↓"));
    app_send_text(&run, "\x03");
    assert(app_wait_current(&run, "Which color?", 3000));
    assert(!vt_rows_contain(&run.terminal, "Ctrl+J: newline"));
    assert(app_wait_current_absent(&run, "Working", 3000));
    assert(!vt_rows_contain(&run.terminal, "keep-draft"));
    assert(!run.terminal.cursor_visible);
    app_send_text(&run, "\twarm colors");
    assert(app_wait_current(&run, "warm colors", 3000));
    app_settle(&run);
    assert(run.terminal.cursor_visible);
    int detail_row = vt_find_row(&run.terminal, "warm colors");
    assert(detail_row > vt_find_row(&run.terminal, "blue"));
    assert(app_wait_cursor(&run, "warm colors", 15, 3000));
    char *detail = vt_row_copy(&run.terminal, detail_row);
    assert(strstr(detail, "# # warm colors") && !strstr(detail, "# # # warm colors"));
    free(detail);
    app_send_text(&run, "\x0a" "! note");
    assert(app_wait_cursor(&run, "! note", 10, 3000));
    app_send_text(&run, "\x1b[A");
    assert(app_wait_cursor(&run, "warm colors", 10, 3000));
    app_send_text(&run, "\x1b[B");
    assert(app_wait_cursor(&run, "! note", 10, 3000));
    app_send_text(&run, "\t");
    app_settle(&run);
    assert(run.terminal.cursor_visible && vt_rows_contain(&run.terminal, "warm colors"));
    assert(!vt_rows_contain(&run.terminal, "Tab: go back"));
    app_send_text(&run, "\x1b[Z");
    assert(app_wait_cursor(&run, "! note", 10, 3000));
    app_send_text(&run, "\t");
    assert(app_wait_cursor(&run, "! note", 10, 3000));
    app_resize(&run, 2, 20);
    assert(app_wait_cursor(&run, "! note", 10, 3000));
    app_resize(&run, 9, 28);
    app_resize(&run, 24, 80);
    app_send_text(&run, "\r");
    assert(app_wait_current(&run, "Which size?", 3000));
    assert(app_wait_current(&run, "Answer: red", 3000));
    assert(app_wait_current(&run, "Details: warm colors", 3000));
    assert(!vt_rows_contain(&run.terminal, "# red"));
    app_send_text(&run, "\x1b[B\x1b[B\t");
    app_paste_text(&run,
        "detail-00\ndetail-01\ndetail-02\ndetail-03\ndetail-04\n"
        "detail-05\ndetail-06\ndetail-07\ndetail-08\ndetail-09\n"
        "detail-10\ndetail-11\ndetail-12\ndetail-13\ndetail-14\n"
        "detail-15\ndetail-16\ndetail-17", 0);
    assert(app_wait_current(&run, "detail-17", 3000));
    app_send_text(&run, "\t");
    assert(app_wait_current(&run, "detail-17", 3000));
    app_settle(&run);
    assert(run.terminal.cursor_visible);
    app_send_text(&run, "\x03");
    assert(app_wait_current(&run, "Tab: go back", 3000));
    app_send_text(&run, "\x03");
    assert(app_wait_current(&run, "Which size?", 3000));
    app_send_text(&run, "\t");
    assert(app_wait_current(&run, "Tab: add details", 3000));
    app_settle(&run);
    assert(!run.terminal.cursor_visible);
    app_send_text(&run, "\t! custom size");
    assert(app_wait_current(&run, "! custom size", 3000));
    app_send_text(&run, "\x03");
    assert(app_wait_current(&run, "Which size?", 3000));
    assert(app_wait_current_absent(&run, "! custom size", 3000));
    app_send_text(&run, "! custom size");
    assert(app_wait_current(&run, "! custom size", 3000));
    app_send_text(&run, "\r");
    assert(app_wait_raw(&run, "All 1 tool calls finished", 5000));
    assert(app_wait_current(&run, "# keep-draft", 3000));
    assert(app_wait_current(&run, "Answer: None of the above", 3000));
    app_send_text(&run, "\x0f");
    app_settle(&run);
    assert(run.terminal.alternate_screen && !strstr(run.raw, "Tool output"));
    assert(app_history_contains(&run, "Answer: red"));
    assert(app_history_contains(&run, "Details: warm colors"));
    assert(app_history_contains(&run, "! note"));
    assert(app_history_contains(&run, "Answer: None of the above"));
    assert(app_history_contains(&run, "Details: ! custom size"));
    assert(app_wait_current(&run, "# keep-draft", 3000));
    app_send_text(&run, "\x03/btw\r\r");
    assert(app_wait_current(&run, "Temporary chat", 3000));
    app_send_text(&run, "\x1f");
    assert(app_wait_current(&run, "Main chat", 3000));
    app_resize(&run, 16, 32);
    for (int i = 0; i < 4 && !vt_rows_contain(&run.terminal, "Which size?"); i++) {
        app_send_text(&run, "\x1b[5~");
        app_settle(&run);
    }
    assert(vt_rows_contain(&run.terminal, "Which size?"));
    assert(!vt_rows_contain(&run.terminal, "\\x1B"));
    app_resize(&run, 24, 80);
    app_send_text(&run, "\x1f\x03");
    assert(app_wait_current_absent(&run, "Temporary chat", 3000));
    app_send_text(&run, "fixture:ask\rcancel-draft\x1b[D\x1b[D");
    assert(app_wait_current(&run, "Tab: add details", 3000));
    app_send_text(&run, "\tdiscard-me\x1b");
    assert(app_wait_current(&run, "# cancel-draft", 3000));
    assert(app_wait_cursor(&run, "# cancel-draft", 14, 3000));
    assert(app_wait_current_absent(&run, "Working", 3000));
    assert(!vt_rows_contain(&run.terminal, "discard-me"));
    app_quit_without_save(&run);
    app_close(&run);
    environment_destroy(&environment);
}

static void test_goal_show_during_work(const char *executable) {
    test_environment environment;
    environment_create(&environment);
    app_run run;
    app_start(&run, executable, &environment, 24, 100);
    assert(app_wait_current(&run, "Ask OCA anything", 5000));
    app_send_text(&run, "/goal fixture:hang\r");
    assert(app_wait_current(&run, "Working", 3000));
    size_t at = run.raw_len;
    app_send_text(&run, "/goal\r");
    assert(app_wait_raw_after(&run, at, "goal:", 2000));
    assert(app_wait_current(&run, "Working", 2000));
    assert(!strstr(run.raw + at, "Status:") && !strstr(run.raw + at, "Active time:"));
    app_send_text(&run, "\x1b");
    assert(app_wait_current_absent(&run, "Working", 3000));
    app_quit_without_save(&run);
    app_close(&run);
    environment_destroy(&environment);
}

static void test_programming_hints(const char *executable) {
    test_environment environment;
    environment_create(&environment);
    app_run run;
    app_start(&run, executable, &environment, 24, 100);
    assert(app_wait_current(&run, "Ask OCA anything", 5000));
    assert(app_wait_status_absent(&run, "hints", 3000));
    app_send_text(&run, "/");
    assert(app_wait_current(&run, "/hints     Toggle on programming hints", 3000));
    app_send_text(&run, "\x03");
    app_send_text(&run, "fixture:hints\r");
    assert(app_wait_raw(&run, "Hints: off. Full instructions: 0.", 3000));
    size_t at = run.raw_len;
    app_send_text(&run, "/hints\r");
    assert(app_wait_raw_after(&run, at, "\x1b[38;5;30mhints", 3000));
    assert(!strstr(run.raw + at, "Hints on ("));
    app_send_text(&run, "/");
    assert(app_wait_current(&run, "/hints     Toggle off programming hints", 3000));
    app_send_text(&run, "\x03" "fixture:hints\r");
    assert(app_wait_raw_after(&run, at, "Hints: on. Full instructions: 1.", 3000));
    at = run.raw_len;
    app_send_text(&run, "fixture:hints\r");
    assert(app_wait_raw_after(&run, at, "Hints: on. Full instructions: 1.", 3000));
    app_send_text(&run, "/btw\r\r");
    assert(app_wait_current(&run, "Temporary chat", 3000));
    at = run.raw_len;
    app_send_text(&run, "fixture:hints\r");
    assert(app_wait_raw_after(&run, at, "Hints: on.", 3000));
    at = run.raw_len;
    app_send_text(&run, "/hints\rfixture:hints\r");
    assert(app_wait_raw_after(&run, at, "Hints: off.", 3000));
    app_send_text(&run, "\x1f");
    assert(app_wait_current(&run, "Main chat", 3000));
    at = run.raw_len;
    app_send_text(&run, "fixture:hints\r");
    assert(app_wait_raw_after(&run, at, "Hints: on. Full instructions: 1.", 3000));
    at = run.raw_len;
    app_send_text(&run, "/hints\rfixture:hints\r");
    assert(app_wait_raw_after(&run, at, "Hints: off. Full instructions: 1.", 3000));
    at = run.raw_len;
    app_send_text(&run, "/hints\rfixture:hints\r");
    assert(app_wait_raw_after(&run, at, "Hints: on. Full instructions: 1.", 3000));
    app_send_text(&run, "\x1f\x03");
    assert(app_wait_current_absent(&run, "Temporary chat", 3000));
    app_send_text(&run, "/new\r");
    assert(app_wait_current(&run, "Save current", 3000));
    app_send_text(&run, "n\r");
    assert(app_wait_current(&run, "Ask OCA anything", 3000));
    at = run.raw_len;
    app_send_text(&run, "fixture:hints\r");
    assert(app_wait_raw_after(&run, at, "Hints: off. Full instructions: 0.", 3000));
    app_quit_without_save(&run);
    app_close(&run);
    environment_destroy(&environment);
}

static void test_resize_drag(const char *executable) {
    test_environment environment;
    environment_create(&environment);
    app_run run;
    app_start(&run, executable, &environment, 10, 80);
    assert(app_wait_current(&run, "Ask OCA anything", 5000));
    app_resize(&run, 28, 80);
    app_paste_text(&run, "draft-first\ndraft-02\ndraft-03\ndraft-04\ndraft-05\ndraft-06\n"
                         "draft-07\ndraft-08\ndraft-09\ndraft-10\ndraft-last", 0);
    assert(app_wait_current(&run, "draft-last", 3000));
    app_send_text(&run, "\x1b[H");
    assert(app_wait_cursor(&run, "draft-first", 4, 3000));
    const int sizes[][2] = {{20,72}, {12,49}, {8,32}, {6,20}, {10,27}, {18,60}, {28,80}};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        app_resize(&run, sizes[i][0], sizes[i][1]);
        app_settle(&run);
        assert(app_wait_cursor(&run, "draft-first", 4, 1500));
        assert(vt_find_last_row(&run.terminal, "kimi-k3") > run.terminal.row);
        int borders = 0;
        for (int row = 0; row < run.terminal.rows; row++) {
            char *text = vt_row_copy(&run.terminal, row);
            size_t length = strlen(text);
            if (length >= 4 && strspn(text, "#") == length) borders++;
            free(text);
        }
        if (borders != 2) {
            fprintf(stderr, "drag step %zu: borders=%d\n", i, borders);
            for (int row = 0; row < run.terminal.rows; row++) {
                char *text = vt_row_copy(&run.terminal, row);
                fprintf(stderr, "%02d: [%s]\n", row, text);
                free(text);
            }
        }
        assert(borders == 2);
    }
    size_t leaked = 0;
    for (size_t i = 0; i < run.terminal.history_count; i++) {
        const char *text = run.terminal.history[i];
        size_t length = strlen(text);
        if (strstr(text, "draft-") || (length >= 4 && strspn(text, "#") == length)) leaked++;
    }
    assert(!leaked);
    app_send_text(&run, "X");
    assert(app_wait_current(&run, "Xdraft-first", 3000));
    app_send_text(&run, "\x03/");
    assert(app_wait_current(&run, "/help", 3000));
    for (int i = 0; i < 3; i++) {
        app_resize(&run, 7, 30);
        app_resize(&run, 16, 67);
    }
    app_send_text(&run, "\x1b");
    app_settle(&run);
    assert(!vt_rows_contain(&run.terminal, "/help"));
    assert(!vt_rows_contain(&run.terminal, "/resume"));
    app_send_text(&run, "fixture:slow\rresize-draft");
    assert(app_wait_current(&run, "resize-draft", 3000));
    assert(app_wait_current(&run, "Working", 3000));
    for (int i = 0; i < 40; i++) {
        int rows = 8 + (i * 7) % 18, cols = 18 + (i * 11) % 80;
        struct winsize size = {.ws_row = (unsigned short)rows, .ws_col = (unsigned short)cols};
        vt_resize(&run.terminal, rows, cols);
        assert(!ioctl(run.slave, TIOCSWINSZ, &size));
        assert(!kill(run.child, SIGWINCH));
        app_pump(&run, 2 + i % 9);
    }
    app_resize(&run, 24, 80);
    char visible[24 * 80 + 1];
    long long deadline = now_ms() + 5000;
    do {
        app_pump(&run, 20);
        size_t used = 0;
        for (int row = 0; row < run.terminal.rows; row++) {
            char *text = vt_row_copy(&run.terminal, row);
            for (size_t i = 0; text[i]; i++) if (text[i] != ' ' && text[i] != '#') visible[used++] = text[i];
            free(text);
        }
        visible[used] = 0;
    } while (!strstr(visible, "tick59") && now_ms() < deadline);
    assert(strstr(visible, "tick59"));
    assert(app_wait_current_absent(&run, "Working", 3000));
    assert(app_wait_cursor(&run, "resize-draft", 16, 3000));
    app_quit_without_save(&run);
    app_close(&run);
    environment_destroy(&environment);
}

/* Wait for the complete frame; the first PTY read may end inside an escape. */
static void app_resize_frame(app_run *run, int rows, int cols) {
    size_t at = run->raw_len;
    const char *end = run->terminal.cursor_visible ? "\x1b[?7h\x1b[?25h" : "\x1b[?7h\x1b[?25l";
    struct winsize before, size = {.ws_row = (unsigned short)rows,
                                   .ws_col = (unsigned short)cols};
    assert(!ioctl(run->slave, TIOCGWINSZ, &before));
    run->terminal.selected_ascii = 0;
    vt_resize(&run->terminal, rows, cols);
    assert(!ioctl(run->slave, TIOCSWINSZ, &size));
    /* TIOCSWINSZ signals this PTY's foreground child. A second signal can
     * queue a duplicate frame that arrives after the next simulated resize. */
    if (before.ws_row == size.ws_row && before.ws_col == size.ws_col)
        assert(!kill(run->child, SIGWINCH));
    assert(app_wait_raw_after(run, at, end, 3000));
}

static int cursor_character_matches(const virtual_terminal *terminal, char expected, int frame) {
    if (terminal->cells[(size_t)terminal->row * (size_t)terminal->cols +
                        (size_t)terminal->col] != expected || !terminal->cursor_visible ||
        !terminal->alternate_screen || (terminal->rows >= 2 && terminal->row >= terminal->rows - 1))
        return 0;
    if (frame && terminal->cols >= 8 && terminal->rows >= 5) {
        int borders = 0;
        for (int row = 0; row < terminal->rows; row++) {
            char *text = vt_row_copy(terminal, row);
            if ((int)strlen(text) == terminal->cols && strspn(text, "#") == strlen(text)) borders++;
            free(text);
        }
        if (borders != 2) return 0;
    }
    return 1;
}

static void assert_cursor_character(app_run *run, char expected, int frame) {
    /* A timer or input redraw may already be in flight at TIOCSWINSZ. Its
     * terminator does not acknowledge the new geometry; await that layout. */
    long long deadline = now_ms() + 3000;
    while (!cursor_character_matches(&run->terminal, expected, frame) && now_ms() < deadline)
        app_pump(run, 10);
    if (!cursor_character_matches(&run->terminal, expected, frame)) {
        fprintf(stderr, "resize %dx%d: cursor (%d,%d), expected '%c'\n",
                run->terminal.rows, run->terminal.cols, run->terminal.row, run->terminal.col, expected);
        for (int row = 0; row < run->terminal.rows; row++) {
            char *text = vt_row_copy(&run->terminal, row);
            fprintf(stderr, "%02d: [%s]\n", row, text);
            free(text);
        }
        assert(0);
    }
}

static void test_resize_restore(const char *executable) {
    test_environment environment;
    environment_create(&environment);
    app_run run;
    app_start(&run, executable, &environment, 24, 80);
    run.terminal.row = 12;
    run.terminal.col = 7;
    memcpy(run.terminal.cells, "shell-before-oca", sizeof("shell-before-oca") - 1);
    assert(app_wait_current(&run, "Ask OCA anything", 5000));
    app_paste_text(&run, "draft-first\ndraft-middle\ndraft-last", 0);
    assert(app_wait_cursor(&run, "draft-last", 14, 3000));
    app_settle(&run);
    for (int rows = 40; rows >= 1; rows--) {
        app_resize_frame(&run, rows, 1 + (rows * 29) % 160);
        assert(run.terminal.alternate_screen && !run.terminal.history_count);
        app_resize_frame(&run, 24, 80);
        assert(app_wait_cursor(&run, "draft-last", 14, 3000));
        assert_cursor_character(&run, ' ', 1);
    }
    app_quit_without_save(&run);
    assert(!run.terminal.alternate_screen);
    assert(vt_rows_contain(&run.terminal, "shell-before-oca"));
    assert(run.terminal.row == 12 && run.terminal.col == 7);
    assert(!run.terminal.history_count);
    assert(!vt_rows_contain(&run.terminal, "draft-"));
    app_close(&run);
    environment_destroy(&environment);
}

static void test_resize_matrix(const char *executable) {
    test_environment environment;
    environment_create(&environment);
    app_run run;
    app_start(&run, executable, &environment, 10, 80);
    assert(app_wait_current(&run, "Ask OCA anything", 5000));
    app_resize_frame(&run, 24, 80);
    app_paste_text(&run, "before\ncursor 界 é 🙂 Xtail\nlast", 0);
    assert(app_wait_current(&run, "last", 3000));
    for (int i = 0; i < 10; i++) app_send_text(&run, "\x1b[D");
    assert(app_wait_cursor(&run, "Xtail", 19, 3000));
    app_settle(&run);
    /* Exhaust every small geometry, in both horizontal directions. The cursor
     * must keep pointing to X even when its logical row is mostly offscreen. */
    for (int rows = 1; rows <= 40; rows++) {
        for (int step = 1; step <= 160; step++) {
            int cols = rows % 2 ? step : 161 - step;
            app_resize_frame(&run, rows, cols);
            assert_cursor_character(&run, 'X', 1);
        }
    }
    app_resize_frame(&run, 24, 80);
    assert(app_wait_cursor(&run, "Xtail", 19, 3000));
    app_send_text(&run, "Y");
    assert(app_wait_current(&run, "YXtail", 3000));
    app_send_text(&run, "\x1b[F");
    assert(app_wait_cursor(&run, "last", 8, 3000));
    for (int rows = 40; rows >= 1; rows--) {
        app_resize_frame(&run, rows, 80);
        assert_cursor_character(&run, ' ', 1);
    }
    app_resize_frame(&run, 24, 80);
    assert(app_wait_cursor(&run, "last", 8, 3000));
    app_send_text(&run, "\x03/");
    assert(app_wait_current(&run, "/help", 3000));
    app_settle(&run);
    for (int cols = 160; cols >= 1; cols--) {
        int rows = 1 + cols % 40;
        app_resize_frame(&run, rows, cols);
        if (cols >= 8 && rows >= 8) assert(run.terminal.selected_ascii == '/');
        int resume_rows = 0;
        for (int row = 0; row < rows; row++) {
            char *text = vt_row_copy(&run.terminal, row);
            if (strstr(text, "/resume")) resume_rows++;
            free(text);
        }
        assert(resume_rows <= 1);
    }
    app_resize_frame(&run, 24, 80);
    assert(app_wait_current(&run, "/help", 3000));
    app_send_text(&run, "\x1b");
    assert(app_wait_current_absent(&run, "/resume", 3000));
    app_send_text(&run, "\x03" "fixture:ask\rkeep-draft");
    assert(app_wait_current(&run, "Which color?", 3000));
    app_settle(&run);
    for (int selection = 0; selection < 2; selection++) {
        char selected = selection ? 'b' : 'r';
        for (int rows = 1; rows <= 12; rows++) {
            for (int cols = 1; cols <= 80; cols++) {
                app_resize_frame(&run, rows, cols);
                assert(run.terminal.selected_ascii == selected);
                assert(!run.terminal.cursor_visible);
            }
        }
        app_resize_frame(&run, 24, 80);
        app_send_text(&run, "\x1b[B");
        app_settle(&run);
    }
    app_send_text(&run, "\tdetail Xtail");
    assert(app_wait_current(&run, "detail Xtail", 3000));
    for (int i = 0; i < 5; i++) app_send_text(&run, "\x1b[D");
    assert(app_wait_cursor(&run, "detail Xtail", 11, 3000));
    for (int cols = 1; cols <= 160; cols++) {
        app_resize_frame(&run, 1 + cols % 40, cols);
        /* Questions use their own frame, so check the edit position directly. */
        assert_cursor_character(&run, 'X', 0);
    }
    app_resize_frame(&run, 24, 80);
    app_send_text(&run, "\x1b");
    assert(app_wait_current(&run, "keep-draft", 3000));
    app_quit_without_save(&run);
    app_close(&run);
    environment_destroy(&environment);
}

static void test_vertical_input(const char *executable) {
    test_environment environment;
    environment_create(&environment);
    app_run run;
    app_start(&run, executable, &environment, 24, 40);
    assert(app_wait_current(&run, "Ask OCA anything", 5000));
    app_send_text(&run, "history-fixture\r");
    assert(app_wait_raw(&run, "Received: history-fixture", 3000));
    app_settle(&run);
    app_paste_text(&run, "abcdefghij\nxy\nklmnopqrst", 0);
    assert(app_wait_cursor(&run, "klmnopqrst", 14, 3000));
    app_send_text(&run, "\x1b[A");
    assert(app_wait_cursor(&run, "xy", 6, 3000));
    app_send_text(&run, "\x1b[A");
    assert(app_wait_cursor(&run, "abcdefghij", 14, 3000));
    app_send_text(&run, "\x1b[B\x1b[B");
    assert(app_wait_cursor(&run, "klmnopqrst", 14, 3000));
    app_send_text(&run, "\x1b[H\x1b[A");
    assert(app_wait_current(&run, "# history-fixture", 3000));
    app_send_text(&run, "\x1b[B");
    assert(app_wait_cursor(&run, "klmnopqrst", 14, 3000));
    app_send_text(&run, "\x03");
    app_paste_text(&run, "AB界CD\né\nLM界NO", 0);
    assert(app_wait_cursor(&run, "LM", 10, 3000));
    app_send_text(&run, "\x1b[A");
    assert(app_wait_cursor(&run, "  e", 5, 3000));
    app_send_text(&run, "\x1b[A");
    assert(app_wait_cursor(&run, "AB", 10, 3000));
    app_send_text(&run, "\x03");
    app_resize(&run, 24, 20);
    app_paste_text(&run, "abcdefghijklmnopqrst", 0);
    assert(app_wait_cursor(&run, "opqrst", 10, 3000));
    app_send_text(&run, "\x1b[A");
    assert(app_wait_cursor(&run, "abcdefghijklmn", 10, 3000));
    app_send_text(&run, "X");
    assert(app_wait_current(&run, "abcdefXghijklm", 3000));
    app_quit_without_save(&run);
    app_close(&run);
    environment_destroy(&environment);
}

static void test_save_question_and_reset(const char *executable) {
    test_environment environment;
    environment_create(&environment);
    app_run run;
    app_start(&run, executable, &environment, 24, 100);
    assert(app_wait_current(&run, "Ask OCA anything", 5000));
    app_settle(&run);
    int spacing = vt_find_row(&run.terminal, "Ask OCA anything") - vt_find_row(&run.terminal, "OCA");
    app_send_text(&run, "save-question-fixture\r");
    assert(app_wait_raw(&run, "Received: save-question-fixture", 3000));
    app_settle(&run);
    app_send_text(&run, "/new\r");
    assert(app_wait_current(&run, "Save current session? (y/n)", 3000));
    int row = vt_find_row(&run.terminal, "Save current session? (y/n)");
    char *question = vt_row_copy(&run.terminal, row);
    assert(!strncmp(question, "Save current session? (y/n)", 26));
    free(question);
    app_send_text(&run, "yes\r");
    assert(app_wait_current(&run, "Enter y or n", 3000));
    app_send_text(&run, "\x1b");
    assert(app_wait_current_absent(&run, "Save current session? (y/n)", 3000));
    app_send_text(&run, "/new\r");
    assert(app_wait_current(&run, "Save current session? (y/n)", 3000));
    app_send_text(&run, "\x03");
    assert(app_wait_current_absent(&run, "Save current session? (y/n)", 3000));
    char path[256];
    snprintf(path, sizeof(path), "%s/.oca/sessions", environment.home);
    FILE *blocked = fopen(path, "w");
    assert(blocked && fputs("blocked", blocked) >= 0 && !fclose(blocked));
    app_send_text(&run, "/new\r");
    assert(app_wait_current(&run, "Save current session? (y/n)", 3000));
    app_paste_text(&run, "Y", 0);
    app_settle(&run);
    assert(vt_rows_contain(&run.terminal, "Save current session? (y/n) Y"));
    app_send_text(&run, "\r");
    assert(app_wait_current(&run, "Continue anyway? (y/n)", 3000));
    app_send_text(&run, "n\r");
    assert(app_wait_current(&run, "Ask OCA anything", 3000));
    assert(!unlink(path));
    app_send_text(&run, "/new\r");
    assert(app_wait_current(&run, "Save current session? (y/n)", 3000));
    app_settle(&run);
    size_t reset_at = run.raw_len;
    app_send_text(&run, "n\r");
    assert(app_wait_raw_after(&run, reset_at, "\x1b[1mOCA", 5000));
    assert(app_wait_current(&run, environment.workspace, 3000));
    app_settle(&run);
    assert(vt_find_row(&run.terminal, "Ask OCA anything") - vt_find_row(&run.terminal, "OCA") == spacing);
    assert(!vt_rows_contain(&run.terminal, "save-question-fixture"));
    app_quit_without_save(&run);
    app_close(&run);
    environment_destroy(&environment);
}

static void test_initial_prompt(const char *executable) {
    test_environment environment;
    environment_create(&environment);
    char agents_path[256];
    snprintf(agents_path, sizeof(agents_path), "%s/AGENTS.md", environment.workspace);
    FILE *agents = fopen(agents_path, "w");
    assert(agents && fputs("Project marker.\n", agents) >= 0 && !fclose(agents));
    char *before[] = {(char *)executable, "-m", "kimi-k3", "initial-prompt-fixture", NULL};
    char *after[] = {(char *)executable, "initial-prompt-fixture", "-m", "kimi-k3", NULL};
    char *literal[] = {(char *)executable, "-m", "kimi-k3", "--", "--literal-prompt-fixture", NULL};
    char *reserved[] = {(char *)executable, "-m", "kimi-k3", "--", "resume", NULL};
    char *context[] = {(char *)executable, "-m", "kimi-k3", "fixture:prompt-contract require-project", NULL};
    char **cases[] = {before, after, literal, reserved, context};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        app_run run;
        app_start_args(&run, executable, &environment, 24, 100, cases[i]);
        assert(app_wait_raw(&run, i == 2 ? "Received: --literal-prompt-fixture" :
                                         i == 3 ? "Received: resume" :
                                         i == 4 ? "Prompt contract: normal." :
                                                  "Received: initial-prompt-fixture", 5000));
        assert(app_wait_current(&run, "Ask OCA anything", 3000));
        app_send_text(&run, "followup-prompt-fixture\r");
        assert(app_wait_raw(&run, "Received: followup-prompt-fixture", 3000));
        app_quit_without_save(&run);
        app_close(&run);
    }
    assert(!unlink(agents_path));
    environment_destroy(&environment);
}

static void test_brand(const char *executable) {
    test_environment environment;
    environment_create(&environment);
    app_run run;
    app_start(&run, executable, &environment, 24, 80);
    run.terminal.check_colors = 1;
    run.terminal.row = 20;
    memcpy(run.terminal.cells, "shell-marker", sizeof("shell-marker") - 1);
    assert(app_wait_current(&run, "Ask OCA anything", 5000));
    assert(app_wait_raw(&run, "—/245760", 3000));
    app_settle(&run);
    assert(vt_find_row(&run.terminal, "OCA") == 1);
    assert(vt_find_row(&run.terminal, environment.workspace) == 3);
    const int sizes[][2] = {{48, 120}, {12, 50}, {2, 8}, {24, 80}};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        app_resize(&run, sizes[i][0], sizes[i][1]);
        app_settle(&run);
        if (sizes[i][0] >= 12) assert(vt_find_row(&run.terminal, "OCA") == 1);
    }
    assert(vt_contains(&run.terminal, environment.workspace));
    assert(!vt_contains(&run.terminal, "Project context:") && !strstr(run.raw, "[AGENTS.md]"));
    assert(run.terminal.primary_cells &&
           !memcmp(run.terminal.primary_cells, "shell-marker", strlen("shell-marker")));
    assert(!run.terminal.history_count);
    assert(vt_find_last_row(&run.terminal, "kimi-k3") ==
           vt_find_row(&run.terminal, "Ask OCA anything") + 2);
    assert(run.terminal.row == vt_find_row(&run.terminal, "Ask OCA anything"));
    assert(run.terminal.col == 4);
    assert(bytes_contain(run.raw, run.raw_len, "\x1b[?1049h", 8));
    assert(run.terminal.alternate_screen);
    assert(!run.terminal.nondefault_margin);
    assert(run.terminal.bracketed_paste);
    char *status = vt_row_copy(&run.terminal, vt_find_last_row(&run.terminal, "kimi-k3"));
    assert(!strstr(status, environment.workspace));
    free(status);
    char *input = vt_row_copy(&run.terminal, run.terminal.row);
    assert(!strchr(input, '>'));
    free(input);
    assert(run.terminal.frame_glyphs > 0);
    app_send_text(&run, "!");
    assert(app_wait_cursor(&run, "! ", 4, 3000));
    app_send_text(&run, "\x7f");
    assert(app_wait_cursor(&run, "# Ask OCA anything", 4, 3000));

    app_send_text(&run, "first line");
    assert(app_wait_current(&run, "first line", 3000));
    app_paste_text(&run, "\nsecond line", 0);
    assert(app_wait_current(&run, "second line", 3000));
    app_settle(&run);
    app_send_text(&run, "\x03/re");
    assert(app_wait_current(&run, "# /re", 3000));
    app_settle(&run);
    app_send_text(&run, "\x03" "marker-check\r");
    assert(app_wait_raw(&run, "Received: marker-check", 3000));
    app_settle(&run);
    assert(app_wait_current(&run, "100/245760", 3000));
    assert(run.terminal.user_markers && !run.terminal.assistant_markers);
    app_quit_without_save(&run);
    assert(vt_rows_contain(&run.terminal, "shell-marker"));
    assert(run.terminal.row == 20 && run.terminal.col == 0);
    assert(!run.terminal.history_count);
    app_close(&run);
    environment_destroy(&environment);
}

static void test_startup_badges(const char *executable) {
    test_environment environment;
    environment_create(&environment);
    environment_add_skill(&environment);
    char agents_path[256];
    snprintf(agents_path, sizeof(agents_path), "%s/AGENTS.md", environment.workspace);
    FILE *agents = fopen(agents_path, "w");
    assert(agents && fputs("Project marker.\n", agents) >= 0 && !fclose(agents));
    app_run run;
    app_start(&run, executable, &environment, 12, 50);
    assert(app_wait_current(&run, "Ask OCA anything", 5000));
    app_settle(&run);
    /* Badges use the bold warning yellow one column inside the banner's left border. */
    assert(app_wait_raw(&run, "\x1b[1m\x1b[38;2;216;178;112m [AGENTS.md]", 3000));
    assert(!strstr(run.raw, "[1 skills]"));
    int path_row = vt_find_row(&run.terminal, environment.workspace);
    int badge_row = vt_find_row(&run.terminal, "[AGENTS.md]");
    assert(path_row >= 0 && badge_row >= 0);
    char *path = vt_row_copy(&run.terminal, path_row);
    char *badge = vt_row_copy(&run.terminal, badge_row);
    assert(path[0] == '#' && !strcmp(badge, " [AGENTS.md]"));
    free(path);
    free(badge);
    app_quit_without_save(&run);
    app_close(&run);
    assert(!unlink(agents_path));
    environment_destroy(&environment);
}

static void test_editor_and_layout(const char *executable) {
    test_environment environment;
    environment_create(&environment);
    environment_add_skill(&environment);
    app_run run;
    app_start(&run, executable, &environment, 12, 50);
    assert(app_wait_current(&run, "Ask OCA anything", 5000));
    assert(vt_contains(&run.terminal, environment.workspace));

    size_t menu_output = run.raw_len;
    app_send_text(&run, "/");
    assert(app_wait_current(&run, "/help", 2000));
    app_settle(&run);
    assert(strstr(run.raw + menu_output, "\x1b[32m› /help"));
    assert(!strstr(run.raw + menu_output, "48;"));
    int composer = vt_find_row(&run.terminal, "# /");
    int status = vt_find_last_row(&run.terminal, "kimi-k3");
    assert(composer >= 0 && status > composer + 3);
    assert(app_wait_current(&run, "of 9", 2000));
    assert(app_wait_raw(&run, "\x1b[36m↓", 2000));
    app_send_text(&run, "\x1b[A"); /* Wrap from first to last entry. */
    assert(app_wait_current(&run, "/quit", 2000));
    app_settle(&run);
    int footer_row = vt_find_row(&run.terminal, "of 9");
    char *bottom_page = vt_row_copy(&run.terminal, footer_row);
    app_send_text(&run, "\x1b[A");
    app_settle(&run);
    char *same_page = vt_row_copy(&run.terminal, footer_row);
    assert(!strcmp(bottom_page, same_page));
    free(same_page);
    for (int i = 0; i < 7; i++) app_send_text(&run, "\x1b[A");
    assert(app_wait_current(&run, "/help", 2000));
    app_settle(&run);
    char *top_page = vt_row_copy(&run.terminal, footer_row);
    assert(strcmp(bottom_page, top_page));
    free(bottom_page);
    free(top_page);
    app_send_text(&run, "re");
    assert(app_wait_current(&run, "# /re", 2000));
    app_settle(&run);
    assert(vt_rows_contain(&run.terminal, "/resume"));
    composer = vt_find_row(&run.terminal, "# /re");
    status = vt_find_last_row(&run.terminal, "kimi-k3");
    assert(composer >= 0 && status == composer + 3);
    assert(!vt_rows_contain(&run.terminal, "/help"));
    app_send_text(&run, "\x1b");
    app_settle(&run);
    composer = vt_find_row(&run.terminal, "# /re");
    status = vt_find_last_row(&run.terminal, "kimi-k3");
    assert(composer >= 0 && status == composer + 2);
    app_send_text(&run, "\x03");
    assert(app_wait_current(&run, "Ask OCA anything", 2000));

    app_send_text(&run, "?");
    assert(app_wait_current(&run, "Complete or submit", 2000));
    app_send_text(&run, "\x1b");
    app_settle(&run);
    assert(vt_rows_contain(&run.terminal, "Ask OCA anything"));

    app_send_text(&run, "/skills\r");
    assert(app_wait_current(&run, "sample-skill", 3000));
    assert(vt_rows_contain(&run.terminal, "Inspect sample data"));
    app_send_text(&run, "\x1b");
    app_settle(&run);

    app_send_text(&run, "/resume\r");
    assert(app_wait_anywhere(&run, "No sessions found.", 3000));
    app_send_text(&run, "\x1b");
    app_settle(&run);

    app_send_text(&run, "/mo\r");
    assert(app_wait_current(&run, "kimi-k3", 2000));
    assert(!vt_rows_contain(&run.terminal, "# /model"));
    assert(!strstr(run.raw, "smoke") && !strstr(run.raw, "Smoke"));
    app_send_text(&run, "\r");
    assert(app_wait_current(&run, "low reasoning", 2000));
    size_t route_output = run.raw_len;
    app_send_text(&run, "\r");
    assert(app_wait_raw_after(&run, route_output, "kimi-k3", 3000));

    route_output = run.raw_len;
    app_send_text(&run, "\x1bl");
    assert(app_wait_raw_after(&run, route_output, "kimi-k3", 3000));
    route_output = run.raw_len;
    app_send_text(&run, "\x1b.");
    assert(app_wait_raw_after(&run, route_output, "high", 3000));
    size_t reasoning_at = run.raw_len;
    app_send_text(&run, "reason-check\r");
    assert(app_wait_raw(&run, "Received: reason-check", 5000));
    assert(!strstr(run.raw + reasoning_at, "(Reasoned"));
    assert(!strstr(run.raw + reasoning_at, "Reviewing the fixture"));

    reasoning_at = run.raw_len;
    app_send_text(&run, "fixture:timed-reasoning\r");
    assert(app_wait_current(&run, "Working", 3000));
    assert(app_wait_raw_after(&run, reasoning_at, "Timed reasoning complete.", 3000));
    assert(!strstr(run.raw + reasoning_at, "Reviewing the fixture request."));
    route_output = run.raw_len;
    app_send_text(&run, "\x1bk");
    assert(app_wait_raw_after(&run, route_output, "kimi-k3", 3000));

    app_send_text(&run, "one two\x1b[H\x1b[3~\x1b[F\x17X\r");
    assert(app_wait_raw(&run, "Received: ne X", 5000));
    app_send_text(&run, "a\tb\r");
    assert(app_wait_raw(&run, "Received: a    b", 5000));

    app_send_text(&run, "A👩‍💻éB\x1b[D\x7f\r");
    assert(app_wait_raw(&run, "Received: A👩‍💻B", 5000));
    assert(app_wait_current_absent(&run, "Working", 3000));
    app_settle(&run);

    size_t replies_before = run.raw_len;
    app_send_text(&run, "\x1b[200~one\r\ntwo\t\x1b]0;PWN\x07three\x1b[201~");
    app_pump(&run, 100);
    assert(!bytes_contain(run.raw + replies_before, run.raw_len - replies_before,
                          "Received: one", strlen("Received: one")));
    assert(!vt_rows_contain(&run.terminal, "Working"));
    app_send_text(&run, "\r");
    assert(app_wait_raw(&run, "Received: one", 5000));
    assert(app_wait_raw(&run, "\\x1B]0;PWN\\x07three", 5000));
    assert(!bytes_contain(run.raw, run.raw_len, "\x1b]0;PWN", 8));
    assert(!bytes_contain(run.raw + replies_before, run.raw_len - replies_before,
                          "\x07", 1));

    assert(app_wait_current_absent(&run, "Working", 3000));
    app_settle(&run);
    app_send_text(&run, "fixture:hang\r");
    assert(app_wait_current(&run, "Working", 3000));
    app_send_text(&run, "next\r");
    assert(app_wait_current(&run, "queued: next", 3000));
    app_send_text(&run, "draft");
    assert(app_wait_cursor(&run, "# draft", 9, 3000));
    app_send_text(&run, "\x18");
    assert(app_wait_current(&run, "# next", 3000));
    assert(app_wait_current_absent(&run, "queued:", 3000));
    assert(vt_rows_contain(&run.terminal, "Working"));
    app_send_text(&run, "\x1b");
    assert(app_wait_current_absent(&run, "Working", 3000));
    assert(!strstr(run.raw, "Received: next"));
    app_send_text(&run, "\r");
    assert(app_wait_raw(&run, "Received: next", 5000));
    assert(app_wait_current_absent(&run, "Working", 2000));

    app_send_text(&run, "fixture:hang\r");
    int saw_streaming = app_wait_current(&run, "Working", 3000);
    if (!saw_streaming) {
        for (int row = 0; row < run.terminal.rows; row++) {
            char *text = vt_row_copy(&run.terminal, row);
            fprintf(stderr, "%02d: [%s]\n", row, text);
            free(text);
        }
    }
    assert(saw_streaming);
    app_send_text(&run, "\x1b[Z");
    app_settle(&run);
    assert(!vt_rows_contain(&run.terminal, "Talk"));
    size_t cancel_at = run.raw_len;
    app_send_text(&run, "\x1b");
    assert(app_wait_current_absent(&run, "Working", 3000));
    assert(!strstr(run.raw + cancel_at, "(Reasoning") && !strstr(run.raw + cancel_at, "(Reasoned"));
    app_settle(&run);
    assert(app_wait_current_absent(&run, "Canceling", 3000));

    app_send_text(&run, "fixture:ask\r");
    assert(app_wait_current(&run, "red", 5000));
    assert(app_wait_anywhere(&run, "Which color?", 1000));
    app_send_text(&run, "\r");
    assert(app_wait_current(&run, "large", 3000));
    assert(app_wait_anywhere(&run, "Which size?", 1000));
    app_send_text(&run, "\x1b[B\r");
    assert(app_wait_raw(&run, "All 1 tool calls finished", 5000));

    assert(app_wait_current_absent(&run, "Working", 3000));
    app_settle(&run);
    app_send_text(&run, "?");
    assert(app_wait_current(&run, "Complete or submit", 3000));
    for (int i = 0; i < 7; i++) app_send_text(&run, "\x1b[B");
    assert(app_wait_current(&run, "Ctrl+C", 3000));
    assert(!vt_rows_contain(&run.terminal, "Normal / Talk"));
    app_send_text(&run, "\x1b");
    app_send_text(&run, "\x1b[Z");
    app_settle(&run);
    assert(!vt_rows_contain(&run.terminal, "Talk"));

    app_send_text(&run, "/goal fixture:slow\r");
    assert(app_wait_current(&run, "Working", 3000));
    size_t goal_output = run.raw_len;
    app_send_text(&run, "/goal edit\rdraft-goal");
    assert(app_wait_raw_after(&run, goal_output, "goal paused", 5000));
    assert(app_wait_raw_after(&run, goal_output, "\x1b[38;5;177mgoal paused", 3000));
    app_settle(&run);
    assert(vt_rows_contain(&run.terminal, "# draft-goal"));
    app_send_text(&run, "\r");
    assert(app_wait_current(&run, "# fixture:slow", 2000));
    assert(app_wait_raw(&run, "Received: draft-goal", 5000));
    assert(app_wait_current_absent(&run, "Working", 2000));
    app_send_text(&run, "\x03");
    assert(app_wait_current(&run, "Ask OCA anything", 2000));
    app_send_text(&run, "/goal clear\r");
    assert(app_wait_raw(&run, "goal cleared", 3000));

    app_send_text(&run, "/goal fixture:goal-complete done\r");
    assert(app_wait_raw(&run, "goal completed:", 5000));
    assert(!strstr(run.raw, "Active time:"));
    assert(app_wait_current(&run, "goal completed", 3000));
    assert(app_wait_raw(&run, "\x1b[38;5;177mgoal completed", 3000));
    assert(app_wait_current_absent(&run, "Working", 3000));
    app_settle(&run);
    char *goal_status = vt_row_copy(&run.terminal, vt_find_last_row(&run.terminal, "kimi-k3"));
    assert(strstr(goal_status, "goal completed ") && goal_status[0] == ' ');
    int goal_row = vt_find_last_row(&run.terminal, "kimi-k3");
    assert(run.terminal.cells[(size_t)goal_row * (size_t)run.terminal.cols +
                              (size_t)run.terminal.cols - 1] == ' ');
    assert(run.terminal.cells[(size_t)goal_row * (size_t)run.terminal.cols +
                              (size_t)run.terminal.cols - 2] != ' ');
    free(goal_status);
    size_t summary_at = run.raw_len;
    app_send_text(&run, "/goal\r");
    assert(app_wait_raw_after(&run, summary_at, "goal:", 3000));
    assert(!strstr(run.raw + summary_at, "Status:") && !strstr(run.raw + summary_at, "Active time:"));
    app_send_text(&run, "\x1b[Z");
    app_resize(&run, 6, 8);
    app_settle(&run);
    assert(!vt_rows_contain(&run.terminal, "Talk"));
    app_resize(&run, 24, 80);
    assert(app_wait_current(&run, "goal completed", 3000));
    app_send_text(&run, "/goal clear\r");
    assert(app_wait_raw(&run, "goal cleared", 3000));

    app_resize(&run, 6, 20);
    app_send_text(&run, "/");
    assert(app_wait_current(&run, "# /", 2000));
    app_settle(&run);
    composer = vt_find_row(&run.terminal, "# /");
    status = vt_find_last_row(&run.terminal, "kimi-k3");
    if (!(composer >= 0 && status > composer && status < run.terminal.rows)) {
        fprintf(stderr, "resize composer=%d status=%d\n", composer, status);
        for (int row = 0; row < run.terminal.rows; row++) {
            char *text = vt_row_copy(&run.terminal, row);
            fprintf(stderr, "%02d: [%s]\n", row, text);
            free(text);
        }
    }
    assert(composer >= 0 && status > composer && status < run.terminal.rows);
    assert(status - composer <= 5);

    size_t narrow_menu_at = run.raw_len;
    app_resize(&run, 6, 8);
    assert(app_wait_raw_after(&run, narrow_menu_at, "\x1b[36m↓", 3000));
    assert(!run.terminal.nondefault_margin);
    app_resize(&run, 2, 8);
    app_settle(&run);
    assert(vt_rows_contain(&run.terminal, "# /"));
    assert(vt_rows_contain(&run.terminal, "kimi-k3"));
    app_resize(&run, 1, 1);
    app_settle(&run);
    assert(waitpid(run.child, NULL, WNOHANG) == 0);
    app_resize(&run, 6, 20);
    app_settle(&run);
    assert(vt_rows_contain(&run.terminal, "# /"));
    app_send_text(&run, "\x1b");
    app_settle(&run);
    size_t narrow_exit_at = run.raw_len;
    app_send_text(&run, "\x03");
    /* The persistent chat hint may be clipped in this short view. */
    assert(app_wait_raw_after(&run, narrow_exit_at, "Press Ctrl+D", 2000));

    app_send_text(&run, "safe\x1b[200~");
    const unsigned char invalid_utf8 = 0xC0;
    app_send(&run, &invalid_utf8, 1);
    app_send_text(&run, "\x1b[201~");
    assert(app_wait_anywhere(&run, "UTF-8", 3000));
    assert(app_wait_current(&run, "# safe", 2000));
    app_send_text(&run, "\x03");

    app_send_text(&run, "keep\x1b[200~");
    char block[4096];
    memset(block, 'x', sizeof(block));
    size_t remaining = OCA_PROMPT_MAX_BYTES + 1;
    while (remaining) {
        size_t amount = remaining < sizeof(block) ? remaining : sizeof(block);
        app_send(&run, block, amount);
        remaining -= amount;
    }
    app_send_text(&run, "\x1b[201~");
    assert(app_wait_anywhere(&run, "Paste rejected", 5000));
    assert(app_wait_current(&run, "# keep", 2000));
    app_send_text(&run, "\r");
    assert(app_wait_anywhere(&run, "Received: keep", 5000));

    assert(run.terminal.alternate_screen && !run.terminal.history_count);
    assert(!vt_history_contains(&run.terminal, "[1 skills]"));
    assert(!run.terminal.nondefault_margin);
    app_quit_without_save(&run);
    app_close(&run);

    app_start(&run, executable, &environment, 10, 40);
    assert(app_wait_current(&run, "Ask OCA anything", 5000));
    app_send_text(&run, "\x1b[A");
    assert(app_wait_current(&run, "# keep", 2000));
    app_send_text(&run, "\x03\x03\x03");
    app_settle(&run);
    assert(waitpid(run.child, NULL, WNOHANG) == 0);
    app_send_text(&run, "\x04");
    assert(app_wait_exit(&run, 5000) == 0);
    app_assert_restored(&run);
    app_close(&run);
    environment_destroy(&environment);
}

static void test_new_controls(const char *executable) {
    test_environment environment;
    environment_create(&environment);
    app_run run;
    app_start(&run, executable, &environment, 24, 100);
    assert(app_wait_current(&run, "Ask OCA anything", 5000));
    app_settle(&run);
    assert(strstr(run.raw, "\x1b[22;2t"));
    assert(strstr(run.raw, "\x1b]2;oca | work\x1b\\"));
    assert(!strstr(run.raw, "\x1b[r") && !strstr(run.raw, "\x1b[?6l"));
    assert(!strstr(run.raw, "Model:") && !strstr(run.raw, "48;5;252"));
    size_t help_at = run.raw_len;
    app_send_text(&run, "?");
    assert(app_wait_current(&run, "Ctrl+C", 2000));
    assert(!vt_rows_contain(&run.terminal, "/quit"));
    app_settle(&run);
    assert(!strstr(run.raw + help_at, "\x1b[1;30;48;2;125;161;222m"));
    app_send_text(&run, "\x1b");
    app_settle(&run);
    app_send_text(&run, "/help\r");
    assert(app_wait_current(&run, "Ctrl+C", 2000));
    assert(!vt_rows_contain(&run.terminal, "/goal edit"));
    app_send_text(&run, "\x1b");
    app_settle(&run);
    app_send_text(&run, "/go");
    assert(app_wait_raw_after(&run, help_at, "[objective", 2000));
    app_send_text(&run, "\r");
    assert(app_wait_raw(&run, "\x1b[38;5;183mNo goal set.", 2000));
    assert(app_wait_current(&run, "Ask OCA anything", 2000));
    app_send_text(&run, "/goal \r");
    assert(app_wait_current(&run, "# /goal ", 2000));
    app_send_text(&run, "\x03");
    app_send_text(&run, "/new extra\r");
    assert(app_wait_current(&run, "# /new extra", 2000));
    app_send_text(&run, "\x03\x03");
    assert(app_wait_current(&run, "Press Ctrl+D", 2000));
    assert(app_wait_raw(&run, "\x1b[1m\x1b[33m Press Ctrl+D to exit", 2000));
    app_settle(&run);
    assert(vt_rows_contain(&run.terminal, "Press Ctrl+D"));
    assert(waitpid(run.child, NULL, WNOHANG) == 0);
    app_send_text(&run, "\x03");
    assert(app_wait_current_absent(&run, "Press Ctrl+D", 1000));
    assert(app_wait_current(&run, "Press Ctrl+D", 2000));
    app_settle(&run);
    int hint_row = vt_find_last_row(&run.terminal, "Press Ctrl+D");
    assert(hint_row > 0);
    char *hint = vt_row_copy(&run.terminal, hint_row);
    assert(!strcmp(hint, " Press Ctrl+D to exit"));
    free(hint);
    hint = vt_row_copy(&run.terminal, hint_row - 1);
    assert(!strstr(hint, "Press Ctrl+D"));
    free(hint);
    app_send_text(&run, "x");
    assert(app_wait_current(&run, "# x", 2000));
    assert(!vt_rows_contain(&run.terminal, "Press Ctrl+D"));
    for (size_t i = 0; i < run.terminal.history_count; i++)
        assert(!strstr(run.terminal.history[i], "Press Ctrl+D"));
    app_send_text(&run, "\x1b[H\x04");
    assert(app_wait_current(&run, "Ask OCA anything", 2000));
    app_send_text(&run, "fixture:hang\r");
    assert(app_wait_current(&run, "Working 0s", 2000));
    assert(app_wait_current(&run, "(Esc to interrupt)", 2000));
    assert(app_wait_raw(&run, "\x1b[36mEsc", 2000));
    app_send_text(&run, "draft\x03");
    app_settle(&run);
    assert(vt_rows_contain(&run.terminal, "Ask OCA anything") && vt_rows_contain(&run.terminal, "Working"));
    assert(!vt_rows_contain(&run.terminal, "# draft"));
    assert(!vt_rows_contain(&run.terminal, "Canceling"));
    app_send_text(&run, "\x17");
    assert(app_wait_current(&run, "Working 1s", 2000));
    app_settle(&run);
    int timer_row = vt_find_row(&run.terminal, "Working");
    char *working_row = vt_row_copy(&run.terminal, timer_row);
    assert(!strncmp(working_row, " Working", 8));
    free(working_row);
    int input_row = vt_find_row(&run.terminal, "Ask OCA anything");
    assert(timer_row + 2 == input_row);
    app_send_text(&run, "\x04");
    app_pump(&run, 100);
    assert(waitpid(run.child, NULL, WNOHANG) == 0);
    assert(vt_rows_contain(&run.terminal, "Working"));
    app_send_text(&run, "\x1b");
    assert(app_wait_current_absent(&run, "Working", 2000));
    app_send_text(&run, "fixture:hang\r");
    assert(app_wait_current(&run, "Working", 3000));
    run.terminal.check_colors = 1;
    size_t removed_at = run.raw_len;
    app_send_text(&run, "discarded-first\rdiscarded-second\rremove-draft");
    assert(app_wait_current(&run, "queued: discarded-first", 3000));
    assert(app_wait_current(&run, "# remove-draft", 3000));
    assert(!vt_rows_contain(&run.terminal, "Ctrl+C: remove"));
    app_send_text(&run, "\x03");
    assert(app_wait_current(&run, "Ask OCA anything", 3000));
    assert(app_wait_current(&run, "Ctrl+C: remove", 3000));
    assert(app_wait_raw_after(&run, removed_at, "\x1b[36mCtrl+C", 3000));
    assert(vt_rows_contain(&run.terminal, "queued: discarded-first"));
    app_send_text(&run, "\x03");
    assert(app_wait_current(&run, "queued: discarded-second", 3000));
    assert(!vt_rows_contain(&run.terminal, "discarded-first"));
    assert(vt_rows_contain(&run.terminal, "Working"));
    app_send_text(&run, "\x03");
    assert(app_wait_current_absent(&run, "queued:", 3000));
    assert(app_wait_current(&run, "(Esc to interrupt)", 3000));
    app_send_text(&run, "\x03");
    app_pump(&run, 100);
    assert(vt_rows_contain(&run.terminal, "Working"));
    assert(!vt_rows_contain(&run.terminal, "Press Ctrl+D"));
    assert(!vt_rows_contain(&run.terminal, "Canceling"));
    app_send_text(&run, "queued-first\rqueued-second\rkeep-draft");
    assert(app_wait_current(&run, "queued-first", 3000));
    assert(app_wait_raw(&run, "\x1b[1mqueued:", 3000));
    assert(app_wait_current(&run, "queued: queued-first", 3000));
    assert(app_wait_raw(&run, "\x1b[36mEsc", 3000));
    assert(app_wait_current(&run, "Ctrl+X: edit first", 3000));
    assert(!strstr(run.raw, "queued (2):") && !strstr(run.raw, "will be sent automatically"));
    int preview_row = vt_find_row(&run.terminal, "queued-first");
    assert(preview_row >= 0 && !run.terminal.queue_separators);
    char *queued_line = vt_row_copy(&run.terminal, preview_row);
    assert(!strncmp(queued_line, " queued:", 8));
    free(queued_line);
    assert(!vt_rows_contain(&run.terminal, "(Esc to interrupt)"));
    char *queue_legend = vt_row_copy(&run.terminal, vt_find_row(&run.terminal, "Esc: interrupt and send"));
    assert(queue_legend[0] == ' ' && queue_legend[1] == 'E');
    free(queue_legend);
    if (preview_row > 0) {
        char *previous = vt_row_copy(&run.terminal, preview_row - 1);
        assert(!*previous);
        free(previous);
    }
    app_resize(&run, 12, 20);
    assert(app_wait_current(&run, " Esc: interrupt/send", 3000));
    int narrow_preview = vt_find_row(&run.terminal, "queued:");
    assert(narrow_preview >= 0);
    queued_line = vt_row_copy(&run.terminal, narrow_preview);
    assert(!strcmp(queued_line, " queued:"));
    free(queued_line);
    queue_legend = vt_row_copy(&run.terminal, vt_find_row(&run.terminal, "Ctrl+X: edit"));
    assert(queue_legend[0] == ' ' && queue_legend[1] == 'C');
    free(queue_legend);
    app_resize(&run, 24, 100);
    app_settle(&run);
    size_t single_queue_at = run.raw_len;
    app_send_text(&run, "\x18");
    assert(app_wait_current(&run, "# queued-first", 3000));
    assert(app_wait_current(&run, "queued-second", 3000));
    assert(app_wait_current(&run, "Esc: interrupt and send", 3000));
    assert(app_wait_current(&run, "Ctrl+X: edit", 3000));
    assert(!strstr(run.raw + single_queue_at, "Ctrl+X: edit first"));
    assert(vt_rows_contain(&run.terminal, "Working"));
    size_t queued_at = run.raw_len;
    app_send_text(&run, "\x1b");
    assert(app_wait_raw_after(&run, queued_at, "Received: queued-second", 5000));
    assert(app_wait_current_absent(&run, "Working", 3000));
    assert(app_wait_current(&run, "# queued-first", 3000));
    assert(!strstr(run.raw + queued_at, "Received: queued-first"));
    assert(!strstr(run.raw + removed_at, "Received: discarded-first"));
    assert(!strstr(run.raw + removed_at, "Received: discarded-second"));
    app_send_text(&run, "\x03");

    app_send_text(&run, "fixture:hang\r");
    assert(app_wait_current(&run, "Working", 3000));
    app_resize(&run, 12, 20);
    char long_preview[1025];
    memset(long_preview, 'q', sizeof(long_preview) - 1);
    long_preview[sizeof(long_preview) - 1] = 0;
    size_t long_queue_at = run.raw_len;
    app_send_text(&run, long_preview);
    app_send_text(&run, "\r");
    assert(app_wait_current(&run, "qqqqqqqq", 3000));
    assert(app_wait_raw_after(&run, long_queue_at, "\x1b[1mqueued:", 3000));
    assert(app_wait_current(&run, "Esc: interrupt/send", 3000));
    assert(app_wait_current(&run, "Ctrl+X: edit", 3000));
    int long_preview_row = vt_find_row(&run.terminal, "qqqqqqqq");
    assert(long_preview_row >= 0 && !run.terminal.queue_separators);
    int long_preview_rows = 0;
    for (int row = 0; row < run.terminal.rows; row++) {
        char *text = vt_row_copy(&run.terminal, row);
        if (strstr(text, "qqqqqqqq")) long_preview_rows++;
        free(text);
    }
    assert(long_preview_rows == 1);
    for (int rows = 8; rows <= 9; rows++) {
        app_resize(&run, rows, 20);
        assert(app_wait_queue_legend(&run, 3000));
    }
    app_resize(&run, 24, 100);
    size_t long_sent_at = run.raw_len;
    app_send_text(&run, "\x1b");
    assert(app_wait_raw_after(&run, long_sent_at, "Received: qqqqqqqq", 5000));
    assert(app_wait_current_absent(&run, "Working", 3000));
    app_send_text(&run, "\x03");
    app_quit_without_save(&run);
    assert(strstr(run.raw, "\x1b[23;2t"));
    app_close(&run);

    test_term = "dumb";
    app_start(&run, executable, &environment, 24, 100);
    assert(app_wait_current(&run, "Ask OCA anything", 5000));
    assert(!strstr(run.raw, "\x1b]2;") && !strstr(run.raw, "\x1b[22;2t"));
    app_send_text(&run, "\x04");
    assert(app_wait_exit(&run, 5000) == 0);
    app_assert_restored(&run);
    app_close(&run);
    test_term = "xterm-256color";
    char hostile_workspace[96];
    snprintf(hostile_workspace, sizeof(hostile_workspace), "%s/work\x1b]2;attack\x07", environment.root);
    assert(!rename(environment.workspace, hostile_workspace));
    snprintf(environment.workspace, sizeof(environment.workspace), "%s", hostile_workspace);
    app_start(&run, executable, &environment, 24, 100);
    assert(app_wait_current(&run, "Ask OCA anything", 5000));
    assert(strstr(run.raw, "oca | work\\x1B]2;attack\\x07"));
    assert(!strstr(run.raw, "\x1b]2;attack\x07"));
    app_send_text(&run, "\x04");
    assert(app_wait_exit(&run, 5000) == 0);
    app_close(&run);
    environment_destroy(&environment);
}

static void test_session_replay(const char *executable) {
    test_environment environment;
    environment_create(&environment);
    char file_path[256];
    snprintf(file_path, sizeof(file_path), "%s/memo.txt", environment.workspace);
    FILE *file = fopen(file_path, "w");
    assert(file);
    assert(fputs("first line\nreplayed result\n", file) >= 0 && !fclose(file));
    app_run run;
    app_start(&run, executable, &environment, 24, 100);
    assert(app_wait_current(&run, "Ask OCA anything", 5000));
    app_send_text(&run, "\x1bl");
    assert(app_wait_current(&run, "kimi-k3", 3000));
    app_send_text(&run, "fixture:tool read {\"path\":\"memo.txt\",\"offset\":2,\"limit\":1}\r");
    assert(app_wait_raw(&run, "\x1b[1m[read]\x1b[0m memo.txt (lines 2-2)", 5000));
    assert(app_wait_raw(&run, "All 1 tool calls finished", 5000));
    assert(!strstr(run.raw, "replayed result"));
    app_send_text(&run, "\x0f");
    app_settle(&run);
    assert(run.terminal.alternate_screen && !strstr(run.raw, "replayed result"));
    assert(!strstr(run.raw, "Tool: read") && !strstr(run.raw, "read: replayed"));
    app_send_text(&run, "fixture:ask\r");
    assert(app_wait_current(&run, "Which color?", 3000));
    assert(!vt_rows_contain(&run.terminal, "Ask OCA anything"));
    assert(app_wait_current(&run, "Add details (optional)", 3000));
    app_send_text(&run, "\treplay detail\r");
    assert(app_wait_current(&run, "Which size?", 3000));
    app_send_text(&run, "\x1b[B\x1b[B\r");
    assert(app_wait_current(&run, "Answer: None of the above", 3000));
    assert(app_wait_current_absent(&run, "Add details (optional)", 3000));
    assert(!strstr(run.raw, "Q: Which color?"));
    assert(app_wait_current_absent(&run, "Working", 3000));
    app_send_text(&run, "fixture:tool edit {\"path\":\"memo.txt\",\"old_text\":\"replayed result\",\"new_text\":\"changed result\"}\r");
    assert(app_wait_raw(&run, "\x1b[1m\x1b[38;2;125;161;222m[edit]\x1b[0m memo.txt", 5000));
    assert(app_wait_raw(&run, "(\x1b[32m+1\x1b[0m \x1b[31m-1\x1b[0m)", 3000));
    assert(!strstr(run.raw, "\x1b[97;48;2;24;55;35m\x1b[38;2;141;149;166m2\x1b[97;48;2;24;55;35m \x1b[32m+\x1b[97;48;2;24;55;35m  changed result"));
    assert(app_wait_current_absent(&run, "Working", 3000));
    app_send_text(&run, "/quit\r");
    assert(app_wait_current(&run, "Save current", 3000));
    app_send_text(&run, "y\r");
    assert(app_wait_exit(&run, 5000) == 0);
    app_close(&run);

    char saved_directory[256], id[33] = "";
    snprintf(saved_directory, sizeof(saved_directory), "%s/.oca/sessions", environment.home);
    DIR *saved = opendir(saved_directory);
    assert(saved);
    struct dirent *record;
    while ((record = readdir(saved))) {
        if (strlen(record->d_name) == 36 && !strcmp(record->d_name + 32, ".oca"))
            memcpy(id, record->d_name, 32);
    }
    assert(!closedir(saved) && *id);
    for (int direct = 0; direct < 2; direct++) {
        environment_visibility(&environment, direct);
        if (direct) {
            char *args[] = {(char *)executable, "resume", id, NULL};
            app_start_args(&run, executable, &environment, 24, 100, args);
        } else {
            app_start(&run, executable, &environment, 24, 100);
            assert(app_wait_current(&run, "Ask OCA anything", 5000));
            app_send_text(&run, "/resume\r");
            assert(app_wait_current(&run, "memo.txt", 3000));
            app_send_text(&run, "\r");
        }
        /* Replay enters retained history; visit its pages before checking
         * the emitted styles and visibility of earlier tool results. */
        assert(app_wait_current(&run, "Ask OCA anything", 3000));
        for (int page = 0; page < 30; page++) {
            app_send_text(&run, "\x1b[5~");
            app_settle(&run);
        }
        for (int page = 0; page < 30; page++) {
            app_send_text(&run, "\x1b[6~");
            app_settle(&run);
        }
        assert(app_wait_raw(&run, "\x1b[1m[read]\x1b[0m memo.txt (lines 2-2)", 3000));
        assert(app_wait_raw(&run, "replayed result", 3000));
        assert(!strstr(run.raw, "(Reasoned)"));
        assert(!!strstr(run.raw, "Reviewing the fixture") == direct);
        assert(app_wait_raw(&run, "All 1 tool calls finished", 3000));
        assert(app_wait_raw(&run, "\x1b[1m\x1b[38;2;125;161;222m[edit]\x1b[0m memo.txt", 3000));
        assert(app_wait_raw(&run, "(\x1b[32m+1\x1b[0m \x1b[31m-1\x1b[0m)", 3000));
        assert(app_wait_raw(&run, "Answer: red", 3000));
        assert(app_wait_raw(&run, "Details: replay detail", 3000));
        assert(app_wait_raw(&run, "Answer: None of the above", 3000));
        assert(!strstr(run.raw, "Q: Which color?"));
        assert(!strstr(run.raw, "Decision: Apply"));
        assert(!vt_rows_contain(&run.terminal, "Enter: confirm"));
        assert(!strstr(run.raw, "\x1b[31m›\x1b[0m \x1b[0mred"));
        if (direct) assert(app_wait_raw(&run, "\x1b[97;48;2;24;55;35m\x1b[38;2;141;149;166m2\x1b[97;48;2;24;55;35m \x1b[32m+\x1b[97;48;2;24;55;35m  changed result", 3000));
        else assert(!strstr(run.raw, "\x1b[97;48;2;24;55;35m\x1b[38;2;141;149;166m2\x1b[97;48;2;24;55;35m \x1b[32m+\x1b[97;48;2;24;55;35m  changed result"));
        assert(!strstr(run.raw, "Reasoning:") && !strstr(run.raw, "Tool: read"));
        size_t quit_at = run.raw_len;
        app_quit_without_save(&run);
        assert(!strstr(run.raw + quit_at, "Save current"));
        app_close(&run);
    }

    char session_dir[256];
    snprintf(session_dir, sizeof(session_dir), "%s/.oca/sessions", environment.home);
    DIR *directory = opendir(session_dir);
    assert(directory);
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (entry->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", session_dir, entry->d_name);
        assert(!unlink(path));
    }
    closedir(directory);
    assert(!unlink(file_path));
    environment_destroy(&environment);
}

static void test_manual_bash(const char *executable) {
    test_environment environment;
    environment_create(&environment);
    app_run run;
    app_start(&run, executable, &environment, 24, 100);
    assert(app_wait_current(&run, "Ask OCA anything", 5000));
    size_t model_at = run.raw_len;
    app_send_text(&run, "fixture:tool bash {\"command\":\"printf '\\\\115\\\\117\\\\104\\\\105\\\\114\\\\137\\\\123\\\\105\\\\103\\\\122\\\\105\\\\124\\\\137\\\\067'\"}\r");
    assert(app_wait_raw_after(&run, model_at, "\x1b[1m\x1b[32m[bash]\x1b[0m printf", 5000));
    assert(app_wait_raw_after(&run, model_at, "All 1 tool calls finished", 5000));
    assert(!strstr(run.raw + model_at, "MODEL_SECRET_7"));
    app_send_text(&run, "!");
    assert(app_wait_current(&run, "! ", 2000));
    app_settle(&run);
    assert(strstr(run.raw, "\x1b[38;2;218;138;147m!"));
    app_send_text(&run, "\r");
    assert(app_wait_current(&run, "Ask OCA anything", 2000));
    assert(!strstr(run.raw, "[exit"));
    size_t command_at = run.raw_len;
    app_send_text(&run, "! printf '\\150\\151\\144\\144\\145\\156\\055\\142\\157\\144\\171'\r");
    assert(app_wait_raw_after(&run, command_at, "\x1b[1m\x1b[32m[bash]\x1b[0m printf", 3000));
    assert(!strstr(run.raw + command_at, "[exit 0]"));
    assert(app_wait_current_absent(&run, "hidden-body", 1000));
    assert(!strstr(run.raw + command_at, "hidden-body"));
    assert(!strstr(run.raw + command_at, "Received:"));
    app_send_text(&run, "! cd /\r");
    assert(app_wait_raw(&run, "\x1b[1m\x1b[32m[bash]\x1b[0m cd /", 3000));
    assert(app_wait_current_absent(&run, "Working", 3000));
    app_settle(&run);
    command_at = run.raw_len;
    app_send_text(&run, "! pwd\r");
    assert(app_wait_raw_after(&run, command_at, "\x1b[1m\x1b[32m[bash]\x1b[0m pwd", 3000));
    app_settle(&run);
    char hidden_output[256];
    snprintf(hidden_output, sizeof(hidden_output), "\x1b[2K\x1b[0m  %s", environment.workspace);
    /* The banner may redraw the workspace; Bash body rows must stay hidden. */
    assert(!strstr(run.raw + command_at, hidden_output));
    command_at = run.raw_len;
    app_send_text(&run, "! exit 7\r");
    assert(app_wait_raw_after(&run, command_at, "\x1b[1m\x1b[32m[bash]\x1b[0m exit 7", 3000));
    assert(app_wait_raw_after(&run, command_at, "(exit 7)", 3000));
    assert(!strstr(run.raw + command_at, "[exit 7]") && !strstr(run.raw + command_at, "exit 7 (failed)"));
    app_send_text(&run, "! for i in {1..80}; do printf 'output-line-%03d abcdefghijklmnopqrstuvwxyz\\n' \"$i\"; done\r");
    assert(app_wait_raw(&run, "\x1b[1m\x1b[32m[bash]\x1b[0m for i", 3000));
    assert(app_wait_current_absent(&run, "output-line-040", 1000));
    assert(!strstr(run.raw + command_at, "output-line-040"));
    int alternate = run.terminal.alternate_screen;
    app_send_text(&run, "\x0f");
    app_settle(&run);
    assert(run.terminal.alternate_screen == alternate);
    assert(!vt_rows_contain(&run.terminal, "output-line-080"));
    app_resize(&run, 9, 22);
    app_resize(&run, 18, 67);
    assert(!vt_rows_contain(&run.terminal, "output-line-080"));
    assert(!strstr(run.raw, "Tool output") && !strstr(run.raw, "Arguments"));
    size_t quit_at = run.raw_len;
    app_quit_without_save(&run);
    assert(strstr(run.raw + quit_at, "Save current"));
    app_close(&run);

    app_start(&run, executable, &environment, 24, 100);
    assert(app_wait_current(&run, "Ask OCA anything", 5000));
    app_send_text(&run, "fixture:hang\r");
    assert(app_wait_current(&run, "Working", 3000));
    command_at = run.raw_len;
    app_send_text(&run, "! printf '\\117\\125\\124\\120\\125\\124\\137\\123\\105\\103\\122\\105\\124\\137\\064\\062'\r");
    assert(app_wait_current(&run, "Bash command queued.", 3000));
    assert(!strstr(run.raw + command_at, "\x1b[1m\x1b[32m[bash]\x1b[0m printf"));
    app_send_text(&run, "\x1b");
    assert(app_wait_raw_after(&run, command_at, "\x1b[1m\x1b[32m[bash]\x1b[0m printf", 3000));
    assert(!strstr(run.raw + command_at, "OUTPUT_SECRET_42"));
    app_settle(&run);
    assert(!vt_rows_contain(&run.terminal, "Bash command queued."));
    app_send_text(&run, "! for i in {1..8}; do echo live-$i; done; sleep 30\r");
    assert(app_wait_raw(&run, "\x1b[1m\x1b[32m[bash]\x1b[0m for", 3000));
    assert(app_wait_current(&run, "Working", 3000));
    assert(app_wait_current_absent(&run, "live-4", 1000));
    app_send_text(&run, "\x0f");
    app_settle(&run);
    assert(!vt_rows_contain(&run.terminal, "live-4"));
    app_send_text(&run, "bash-draft\x03");
    assert(app_wait_current(&run, "Ask OCA anything", 2000));
    assert(vt_rows_contain(&run.terminal, "Working"));
    assert(!vt_rows_contain(&run.terminal, "# bash-draft"));
    command_at = run.raw_len;
    app_send_text(&run, "\x03");
    app_pump(&run, 200);
    assert(vt_rows_contain(&run.terminal, "Working") && !strstr(run.raw + command_at, "(interrupted)"));
    app_send_text(&run, "\x1b");
    assert(app_wait_current_absent(&run, "Working", 7000));
    assert(app_wait_raw_after(&run, command_at, "(interrupted)", 3000));
    assert(!strstr(run.raw + command_at, "live-4"));
    app_quit_without_save(&run);
    app_close(&run);
    environment_destroy(&environment);
}

static int last_view_line(const virtual_terminal *terminal, const char *prefix) {
    int number = -1;
    for (int i = 0; i < terminal->rows; i++) {
        char *row = vt_row_copy(terminal, i);
        const char *at = strstr(row, prefix);
        if (at) number = atoi(at + strlen(prefix));
        free(row);
    }
    return number;
}

static void assert_history_excludes(const virtual_terminal *terminal, const char *text) {
    for (size_t i = 0; i < terminal->history_count; i++) assert(!strstr(terminal->history[i], text));
}

static void test_ephemeral_scrolling(const char *executable) {
    test_environment environment;
    environment_create(&environment);
    app_run run;
    app_start(&run, executable, &environment, 24, 100);
    assert(app_wait_current(&run, "Ask OCA anything", 5000));
    char text[2048];
    size_t used = 0;
    for (int i = 0; i < 50; i++)
        used += (size_t)snprintf(text + used, sizeof(text) - used, "main-line-%03d\n", i);
    app_paste_text(&run, text, 1);
    assert(app_wait_current(&run, "cache hit 60%", 3000));
    assert(app_wait_current_absent(&run, "Working", 3000));
    app_settle(&run);
    assert(app_history_contains(&run, "Received: main-line-000"));
    size_t primary_history = run.terminal.history_count;
    app_send_text(&run, "/btw\r\r");
    assert(app_wait_current(&run, "Temporary chat", 3000));
    assert(run.terminal.alternate_screen);
    used = 0;
    for (int i = 0; i < 50; i++)
        used += (size_t)snprintf(text + used, sizeof(text) - used, "side-line-%03d\n", i);
    app_paste_text(&run, text, 1);
    assert(app_wait_current(&run, "cache hit 60%", 3000));
    assert(app_wait_current_absent(&run, "Working", 3000));
    for (int i = 0; i < 6 && !vt_rows_contain(&run.terminal, "Received: side-line-000"); i++) {
        app_send_text(&run, "\x1b[5~");
        app_settle(&run);
    }
    assert(vt_rows_contain(&run.terminal, "Received: side-line-000"));
    int last_side = last_view_line(&run.terminal, "side-line-");
    assert(last_side >= 0 && last_side < 49);
    assert(!vt_rows_contain(&run.terminal, "main-line-"));
    assert(run.terminal.history_count == primary_history);
    assert_history_excludes(&run.terminal, "side-line-");
    size_t size = (size_t)run.terminal.rows * (size_t)run.terminal.cols;
    char *screen = xrealloc(NULL, size);
    memcpy(screen, run.terminal.cells, size);
    app_send_text(&run, "\x1f");
    assert(app_wait_current(&run, "Main chat", 3000));
    app_settle(&run);
    assert(!vt_rows_contain(&run.terminal, "side-line-"));
    app_send_text(&run, "\x1f");
    assert(app_wait_current(&run, "Temporary chat", 3000));
    app_settle(&run);
    assert(!memcmp(screen, run.terminal.cells, size));
    free(screen);
    app_send_text(&run, "\x1f");
    assert(app_wait_current(&run, "Main chat", 3000));
    app_resize(&run, 18, 65);
    app_send_text(&run, "\x1f");
    assert(app_wait_current(&run, "Temporary chat", 3000));
    app_settle(&run);
    assert(last_view_line(&run.terminal, "side-line-") == last_side);
    app_send_text(&run, "\x1fmain-new-hidden\r\x1f");
    app_settle(&run);
    assert(vt_rows_contain(&run.terminal, "Temporary chat") && !vt_rows_contain(&run.terminal, "main-new-hidden"));
    app_send_text(&run, "\x1f");
    assert(app_wait_current(&run, "Received: main-new-hidden", 5000));
    app_send_text(&run, "\x1b[5~");
    app_settle(&run);
    int last_main = last_view_line(&run.terminal, "main-line-");
    assert(last_main >= 0);
    app_send_text(&run, "\x1f");
    assert(app_wait_current(&run, "Temporary chat", 3000));
    app_settle(&run);
    assert(last_view_line(&run.terminal, "side-line-") == last_side);
    app_send_text(&run, "\x03");
    assert(app_wait_current_absent(&run, "Temporary chat", 3000));
    app_settle(&run);
    assert(run.terminal.alternate_screen && last_view_line(&run.terminal, "main-line-") == last_main);
    size_t close_at = run.raw_len;
    for (int i = 0; i < 20; i++) app_send_text(&run, "\x1b[6~");
    assert(app_wait_current(&run, "Received: main-new-hidden", 3000));
    app_settle(&run);
    assert(run.terminal.alternate_screen && !strstr(run.raw + close_at, "\x1b[?1049l"));
    assert_history_excludes(&run.terminal, "side-line-");
    app_quit_without_save(&run);
    app_close(&run);
    environment_destroy(&environment);
}

static void test_startup_visibility(const char *executable) {
    for (int enabled = 0; enabled < 2; enabled++) {
        test_environment environment;
        environment_create(&environment);
        environment_visibility(&environment, enabled);
        char path[256];
        snprintf(path, sizeof(path), "%s/body.txt", environment.workspace);
        FILE *file = fopen(path, "w");
        assert(file && fputs("VISIBLE_BODY **plain** `literal`\n", file) >= 0 && !fclose(file));
        app_run run;
        app_start(&run, executable, &environment, 24, 100);
        assert(app_wait_current(&run, "Ask OCA anything", 5000));
        app_send_text(&run, "fixture:tool read {\"path\":\"body.txt\"}\r");
        assert(app_wait_raw(&run, "All 1 tool calls finished", 5000));
        assert(!!strstr(run.raw, "VISIBLE_BODY **plain** `literal`") == enabled);
        assert(!!strstr(run.raw, "Reviewing the fixture request.") == enabled);
        if (enabled) assert(strstr(run.raw, "\x1b[38;2;141;149;166m\x1b[3m"));
        assert(!strstr(run.raw, "(Reasoned") && !strstr(run.raw, "(Reasoning"));
        app_send_text(&run, "! cat body.txt; sleep 1; exit 7\r");
        assert(app_wait_raw(&run, "[bash]", 3000));
        assert(app_wait_raw(&run, "(exit 7)", 5000));
        assert(!strstr(run.raw, "[exit 7]"));
        assert(app_wait_current_absent(&run, "Working", 3000));
        size_t side_at = run.raw_len;
        app_send_text(&run, "/btw side-visible\r");
        assert(app_wait_raw_after(&run, side_at, "Received: side-visible", 5000));
        assert(!!strstr(run.raw + side_at, "Reviewing the fixture request.") == enabled);
        app_send_text(&run, "\x03");
        assert(app_wait_current_absent(&run, "Temporary chat", 3000));
        app_quit_without_save(&run);
        app_close(&run);
        assert(!unlink(path));
        environment_destroy(&environment);
    }
}

static void test_default_visibility(const char *executable) {
    test_environment environment;
    environment_create(&environment);
    fixture_write_config_saved(environment.home); /* No show overrides. */
    char file[512];
    snprintf(file, sizeof(file), "%s/default-read.txt", environment.workspace);
    FILE *text = fopen(file, "w");
    assert(text && fputs("READ_DEFAULT_HIDDEN", text) >= 0 && !fclose(text));
    app_run run;
    app_start(&run, executable, &environment, 24, 100);
    assert(app_wait_current(&run, "Ask OCA anything", 5000));
    app_send_text(&run, "fixture:tool read {\"path\":\"default-read.txt\"}\r");
    assert(app_wait_raw(&run, "All 1 tool calls finished", 5000));
    assert(app_wait_raw(&run, "Reviewing the fixture request.", 3000));
    assert(!strstr(run.raw, "READ_DEFAULT_HIDDEN"));
    app_send_text(&run, "! printf '\\142\\141\\163\\150\\055\\166\\151\\163\\151\\142\\154\\145'\r");
    assert(app_wait_raw(&run, "bash-visible", 5000));
    app_send_text(&run, "fixture:tool write {\"path\":\"default-write.txt\",\"content\":\"WRITE_VISIBLE\"}\r");
    assert(app_wait_raw(&run, "\x1b[97;48;2;24;55;35m\x1b[38;2;141;149;166m1\x1b[97;48;2;24;55;35m \x1b[32m+\x1b[97;48;2;24;55;35m  WRITE_VISIBLE", 5000));
    assert(app_wait_current_absent(&run, "Working", 5000));
    app_send_text(&run, "fixture:tool edit {\"path\":\"default-write.txt\",\"old_text\":\"WRITE_VISIBLE\",\"new_text\":\"EDIT_VISIBLE\"}\r");
    assert(app_wait_raw(&run, "\x1b[97;48;2;24;55;35m\x1b[38;2;141;149;166m1\x1b[97;48;2;24;55;35m \x1b[32m+\x1b[97;48;2;24;55;35m  EDIT_VISIBLE", 5000));
    app_quit_without_save(&run);
    app_close(&run);
    assert(!unlink(file));
    snprintf(file, sizeof(file), "%s/default-write.txt", environment.workspace);
    assert(!unlink(file));
    environment_destroy(&environment);
}

static void test_ephemeral_chat(const char *executable) {
    test_environment environment;
    environment_create(&environment);
    app_run run;
    app_start(&run, executable, &environment, 24, 100);
    assert(app_wait_current(&run, "Ask OCA anything", 5000));
    app_send_text(&run, "main-seed\r");
    assert(app_wait_raw(&run, "Received: main-seed", 3000));
    app_settle(&run);
    app_send_text(&run, "/bt");
    assert(app_wait_raw(&run, "[message]", 2000));
    size_t side_status_at = run.raw_len;
    app_send_text(&run, "\r\r");
    assert(app_wait_current(&run, "Temporary chat (Ctrl+/ to switch", 3000));
    assert(app_wait_current(&run, "Ctrl+C to close)", 3000));
    assert(app_wait_raw_after(&run, side_status_at, "\x1b[36mCtrl+/", 3000));
    assert(app_wait_raw_after(&run, side_status_at, "\x1b[36mCtrl+C", 3000));
    app_settle(&run);
    assert(!vt_rows_contain(&run.terminal, "Received: main-seed"));
    app_send_text(&run, "side-private-one\r");
    assert(app_wait_raw(&run, "Received: side-private-one", 3000));
    app_settle(&run);
    size_t main_status_at = run.raw_len;
    app_send_text(&run, "side-draft\x1b[D\x1f");
    assert(app_wait_current(&run, "Main chat (Ctrl+/ to switch)", 3000));
    assert(app_wait_raw_after(&run, main_status_at, "\x1b[36mCtrl+/", 3000));
    assert(app_wait_current(&run, "Received: main-seed", 3000));
    app_send_text(&run, "main-draft\x1b[D\x1b[D\x1f");
    assert(app_wait_current(&run, "# side-draft", 3000));
    app_settle(&run);
    assert(run.terminal.col == 13);
    assert(vt_rows_contain(&run.terminal, "Received: side-private-one"));
    app_send_text(&run, "\x1b[47;5u");
    assert(app_wait_current(&run, "# main-draft", 3000));
    app_settle(&run);
    assert(run.terminal.col == 12);
    app_send_text(&run, "\x03/btw side-private-two\r");
    assert(app_wait_current(&run, "Temporary chat", 3000));
    assert(app_wait_raw(&run, "Received: side-private-two", 3000));
    assert(app_wait_current(&run, "# side-draft", 3000));
    app_send_text(&run, "\x03");
    assert(app_wait_current_absent(&run, "Temporary chat", 3000));
    app_settle(&run);
    app_send_text(&run, "/btw\r\r");
    assert(app_wait_current(&run, "Temporary chat", 3000));
    app_settle(&run);
    assert(vt_rows_contain(&run.terminal, "Ask OCA anything"));
    assert(!vt_rows_contain(&run.terminal, "side-private-one"));
    app_send_text(&run, "\x03");
    assert(app_wait_current_absent(&run, "Temporary chat", 3000));
    app_settle(&run);

    app_send_text(&run, "/goal fixture:hang\r");
    assert(app_wait_current(&run, "Working", 3000));
    app_send_text(&run, "/btw while-main-runs\r");
    assert(app_wait_current(&run, "Temporary chat", 3000));
    assert(app_wait_raw(&run, "Received: while-main-runs", 3000));
    app_send_text(&run, "\x1f");
    assert(app_wait_current(&run, "Working", 3000));
    assert(app_wait_current(&run, "goal", 3000));
    app_send_text(&run, "\x1f" "fixture:hang\r");
    assert(app_wait_current(&run, "Temporary chat", 3000));
    assert(app_wait_current(&run, "Working", 3000));
    app_send_text(&run, "side-queued\r");
    assert(app_wait_current(&run, "queued: side-queued", 3000));
    app_settle(&run);
    assert(!vt_rows_contain(&run.terminal, "Ctrl+C: remove"));
    app_send_text(&run, "\x03");
    /* The Main chat label disappears when the temporary core finishes closing. */
    assert(app_wait_current_absent(&run, "Temporary chat", 3000));
    assert(app_wait_current(&run, "Working", 3000));
    app_settle(&run);
    assert(vt_rows_contain(&run.terminal, "goal"));
    assert(!vt_rows_contain(&run.terminal, "goal paused"));
    app_send_text(&run, "\x1b");
    assert(app_wait_current_absent(&run, "Working", 3000));
    app_settle(&run);
    app_send_text(&run, "\x1b/goal clear\r");
    assert(app_wait_raw(&run, "goal cleared", 3000));
    app_quit_without_save(&run);
    assert(!run.terminal.alternate_screen && !run.terminal.nondefault_margin);
    app_close(&run);
    char path[256];
    snprintf(path, sizeof(path), "%s/.oca/history", environment.home);
    FILE *history = fopen(path, "r");
    assert(history);
    char text[8192];
    size_t size = fread(text, 1, sizeof(text) - 1, history);
    text[size] = 0;
    assert(!ferror(history) && feof(history));
    fclose(history);
    assert(strstr(text, "main-seed") && !strstr(text, "side-private") && !strstr(text, "while-main-runs"));
    environment_destroy(&environment);
}

static void test_ephemeral_lifecycle(const char *executable) {
    test_environment environment;
    environment_create(&environment);
    app_run run;
    app_start(&run, executable, &environment, 24, 100);
    assert(app_wait_current(&run, "Ask OCA anything", 5000));
    app_send_text(&run, "/btw side-only-private\r");
    assert(app_wait_raw(&run, "Received: side-only-private", 3000));
    assert(app_wait_current_absent(&run, "Working", 3000));
    app_settle(&run);
    app_send_text(&run, "\x1b.");
    assert(app_wait_current(&run, "high", 3000));
    char path[256], content[2048];
    snprintf(path, sizeof(path), "%s/.oca/config.json", environment.home);
    FILE *config = fopen(path, "r");
    assert(config);
    size_t length = fread(content, 1, sizeof(content) - 1, config);
    content[length] = 0;
    fclose(config);
    const char *last_used = strstr(content, "\"last_used\"");
    assert(strstr(content, "kimi-k3") && last_used && strstr(last_used, "\"low\"") &&
           !strstr(last_used, "\"high\""));
    app_send_text(&run, "\x1f");
    assert(app_wait_current(&run, "Main chat", 3000));
    app_settle(&run);
    size_t reset_at = run.raw_len;
    app_send_text(&run, "/new\r");
    /* Input redraws can still contain Main before session replacement finishes. */
    assert(app_wait_status_absent(&run, "Main chat", 3000));
    app_settle(&run);
    assert(!strstr(run.raw + reset_at, "Save current"));
    app_send_text(&run, "\x1f");
    assert(app_wait_current(&run, "Use /btw", 3000));
    app_send_text(&run, "/btw\r\r");
    assert(app_wait_current(&run, "Temporary chat", 3000));
    app_settle(&run);
    assert(!vt_rows_contain(&run.terminal, "side-only-private"));
    app_send_text(&run, "fixture:hang\r");
    assert(app_wait_current(&run, "Working", 3000));
    assert(!kill(run.child, SIGTERM));
    assert(app_wait_exit(&run, 5000) == 0);
    app_assert_restored(&run);
    app_close(&run);
    snprintf(path, sizeof(path), "%s/.oca/history", environment.home);
    assert(access(path, F_OK) != 0);
    environment_destroy(&environment);
}

static void test_signal_restore(const char *executable) {
    test_environment environment;
    environment_create(&environment);
    const int signals[] = { SIGHUP, SIGINT, SIGTERM, SIGQUIT };
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        app_run run;
        app_start(&run, executable, &environment, 10, 40);
        assert(app_wait_current(&run, "Ask OCA anything", 5000));
        assert(kill(run.child, signals[i]) == 0);
        assert(app_wait_exit(&run, 5000) == 0);
        app_assert_restored(&run);
        app_close(&run);
    }
    environment_destroy(&environment);
}

int main(int argc, char **argv) {
    assert(unsetenv("KIMI_API_KEY") == 0);
    fixture_server server = fixture_server_start();
    assert(unsetenv("DEEPSEEK_API_KEY") == 0);
    assert(argc == 2);
    assert(setlocale(LC_CTYPE, "") != NULL);
    char executable[PATH_MAX];
    assert(realpath(argv[1], executable));
    test_programming_hints(executable);
    test_question_details(executable);
    test_goal_show_during_work(executable);
    test_resize_drag(executable);
    test_resize_matrix(executable);
    test_resize_restore(executable);
    test_brand(executable);
    test_startup_badges(executable);
    test_initial_prompt(executable);
    test_editor_and_layout(executable);
    test_signal_restore(executable);
    test_new_controls(executable);
    test_vertical_input(executable);
    test_save_question_and_reset(executable);
    test_session_replay(executable);
    test_manual_bash(executable);
    test_startup_visibility(executable);
    test_default_visibility(executable);
    test_ephemeral_chat(executable);
    test_ephemeral_scrolling(executable);
    test_ephemeral_lifecycle(executable);
    fixture_server_stop(&server);
    puts("test_tui: passed");
    return 0;
}
