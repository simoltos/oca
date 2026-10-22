/* Startup login tests use a PTY, temporary state, and fake credentials.
 * No case submits a provider request. */
#define _XOPEN_SOURCE 700

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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

typedef struct {
    pid_t child;
    int master, slave, flags;
    struct termios original;
    char output[65536];
    size_t used;
} login_run;

static long long now_ms(void) {
    struct timespec time;
    assert(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
    return (long long)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

static void pump(login_run *run) {
    struct pollfd descriptor = {.fd = run->master, .events = POLLIN};
    int ready = poll(&descriptor, 1, 20);
    assert(ready >= 0 || errno == EINTR);
    if (ready <= 0) return;
    for (;;) {
        assert(run->used + 1 < sizeof(run->output));
        ssize_t count = read(run->master, run->output + run->used,
                             sizeof(run->output) - run->used - 1);
        if (count <= 0) break;
        run->used += (size_t)count;
        run->output[run->used] = 0;
    }
}

static int wait_text(login_run *run, const char *text) {
    long long deadline = now_ms() + 5000;
    while (now_ms() < deadline) {
        if (strstr(run->output, text)) return 1;
        pump(run);
    }
    return strstr(run->output, text) != NULL;
}

static void seed_config(const char *home) {
    char directory[512], path[1024];
    snprintf(directory, sizeof(directory), "%s/.oca", home);
    assert(mkdir(directory, 0700) == 0 || errno == EEXIST);
    snprintf(path, sizeof(path), "%s/models.json", directory);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        assert(errno == EEXIST);
        return;
    }
    FILE *file = fdopen(fd, "w");
    assert(file);
    assert(fputs("{"
                 "\"kimi-k3\":{\"alias\":\"k3\",\"provider\":\"kimi\",\"context_limit\":262144,\"reasoning_levels\":[\"low\",\"high\"]},"
                 "\"gpt-6-astra\":{\"alias\":\"astra\",\"provider\":\"openai\",\"context_limit\":872000,\"reasoning_levels\":[\"low\",\"high\"]},"
                 "\"deepseek-flash\":{\"alias\":\"dsf\",\"provider\":\"deepseek\",\"context_limit\":1000000,\"reasoning_levels\":[\"off\",\"high\"]}}",
                 file) >= 0);
    assert(fclose(file) == 0);
    snprintf(path, sizeof(path), "%s/config.json", directory);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    file = fdopen(fd, "w");
    assert(file && fputs("{}", file) >= 0 && fclose(file) == 0);
}

static void start(login_run *run, const char *executable, const char *home,
                   char *const args[], const char *environment_key, int tty_input) {
    seed_config(home);
    memset(run, 0, sizeof(*run));
    run->master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    assert(run->master >= 0);
    assert(grantpt(run->master) == 0 && unlockpt(run->master) == 0);
    run->slave = open(ptsname(run->master), O_RDWR | O_NOCTTY | O_CLOEXEC);
    assert(run->slave >= 0);
    assert(tcgetattr(run->slave, &run->original) == 0);
    run->flags = fcntl(run->slave, F_GETFL);
    assert(run->flags >= 0);
    struct winsize size = {.ws_row = 24, .ws_col = 80};
    assert(ioctl(run->slave, TIOCSWINSZ, &size) == 0);
    run->child = fork();
    assert(run->child >= 0);
    if (!run->child) {
        alarm(15);
        assert(setsid() >= 0);
#ifdef TIOCSCTTY
        assert(ioctl(run->slave, TIOCSCTTY, 0) == 0);
#endif
        int input = tty_input ? run->slave : open("/dev/null", O_RDONLY);
        assert(input >= 0 && dup2(input, STDIN_FILENO) >= 0);
        int output = tty_input == 2 ? open("/dev/null", O_WRONLY) : run->slave;
        assert(output >= 0 && dup2(output, STDOUT_FILENO) >= 0);
        if (output != run->slave) close(output);
        assert(dup2(run->slave, STDERR_FILENO) >= 0);
        if (input != run->slave) close(input);
        close(run->master);
        close(run->slave);
        char workspace[PATH_MAX];
        snprintf(workspace, sizeof(workspace), "%s/work", home);
        assert(mkdir(workspace, 0700) == 0 || errno == EEXIST);
        assert(chdir(workspace) == 0);
        assert(setenv("HOME", home, 1) == 0);
        assert(setenv("TERM", "xterm-256color", 1) == 0);
        assert(setenv("LC_ALL", "C.UTF-8", 1) == 0);
        assert(unsetenv("DEEPSEEK_API_KEY") == 0);
        assert(setenv("OPENAI_API_KEY", "fixture-key-not-supported", 1) == 0);
        if (environment_key) assert(setenv("KIMI_API_KEY", environment_key, 1) == 0);
        else assert(unsetenv("KIMI_API_KEY") == 0);
        execv(executable, args);
        _exit(127);
    }
    int flags = fcntl(run->master, F_GETFL);
    assert(flags >= 0 && fcntl(run->master, F_SETFL, flags | O_NONBLOCK) == 0);
}

static void send_input(login_run *run, const char *text) {
    size_t length = strlen(text);
    assert(write(run->master, text, length) == (ssize_t)length);
}

static void finish(login_run *run, int expected) {
    long long deadline = now_ms() + 5000;
    int status = 0;
    pid_t result = 0;
    while (now_ms() < deadline) {
        pump(run);
        result = waitpid(run->child, &status, WNOHANG);
        if (result == run->child) break;
        assert(result == 0);
    }
    assert(result == run->child && WIFEXITED(status) && WEXITSTATUS(status) == expected);
    struct termios restored;
    assert(tcgetattr(run->slave, &restored) == 0);
    assert(restored.c_lflag == run->original.c_lflag);
    assert(fcntl(run->slave, F_GETFL) == run->flags);
    assert(!strstr(run->output, "fixture-login-key"));
    assert(!strstr(run->output, "fixture-environment-key"));
    close(run->master);
    close(run->slave);
}

static void assert_hidden(const login_run *run) {
    struct termios state;
    assert(tcgetattr(run->slave, &state) == 0);
    assert(!(state.c_lflag & (ECHO | ECHONL | ICANON)));
}

static void clean_state(const char *home) {
    const char *files[] = {"auth.json", "config.json", "models.json", "history"};
    char path[512];
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(path, sizeof(path), "%s/.oca/%s", home, files[i]);
        if (unlink(path) != 0) assert(errno == ENOENT);
    }
    snprintf(path, sizeof(path), "%s/.oca", home);
    if (rmdir(path) != 0) assert(errno == ENOENT);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    char executable[PATH_MAX], auth[512], config[512], state_dir[512];
    assert(realpath(argv[1], executable));
    char home[] = "/tmp/oca-login-test-XXXXXX";
    assert(mkdtemp(home));
    snprintf(auth, sizeof(auth), "%s/.oca/auth.json", home);
    snprintf(config, sizeof(config), "%s/.oca/config.json", home);
    snprintf(state_dir, sizeof(state_dir), "%s/.oca", home);
    char *alias[] = {executable, "-m", "k3", NULL};
    char *canonical[] = {executable, "--model", "kimi-k3", NULL};
    login_run run;

    char *unknown[] = {executable, "-m", "missing-model", NULL};
    start(&run, executable, home, unknown, NULL, 1);
    finish(&run, 1);
    assert(strstr(run.output, "unknown model") && !strstr(run.output, "API key:"));

    char *reasoning[] = {executable, "-m", "k3", "--reasoning", "off", NULL};
    start(&run, executable, home, reasoning, NULL, 1);
    finish(&run, 1);
    assert(strstr(run.output, "does not support reasoning") && !strstr(run.output, "API key:"));

    char *noninteractive[] = {executable, "exec", "unused", "-m", "k3", NULL};
    start(&run, executable, home, noninteractive, NULL, 1);
    finish(&run, 1);
    assert(strstr(run.output, "oca -m kimi-k3") && !strstr(run.output, "API key:"));
    start(&run, executable, home, alias, NULL, 0);
    finish(&run, 1);
    assert(strstr(run.output, "oca -m kimi-k3") && !strstr(run.output, "API key:"));

    start(&run, executable, home, alias, NULL, 2);
    finish(&run, 1);
    assert(strstr(run.output, "oca -m kimi-k3") && !strstr(run.output, "API key:"));

    char *openai_missing[] = {executable, "exec", "unused", "-m", "astra", NULL};
    start(&run, executable, home, openai_missing, NULL, 1);
    finish(&run, 1);
    assert(strstr(run.output, "oca -m gpt-") && !strstr(run.output, "API key:"));
    char *openai_key[] = {executable, "auth", "--api-key", "openai", NULL};
    start(&run, executable, home, openai_key, NULL, 1);
    finish(&run, 2);
    assert(strstr(run.output, "unknown option: --api-key"));

    const char *cancel[] = {"\004", "\n", "fixture-login-key\003", "abc\004"};
    for (size_t i = 0; i < sizeof(cancel) / sizeof(cancel[0]); i++) {
        start(&run, executable, home, alias, NULL, 1);
        assert(wait_text(&run, "API key:"));
        assert_hidden(&run);
        send_input(&run, cancel[i]);
        finish(&run, 1);
        assert(strstr(run.output, "credential entry canceled"));
        assert(!strstr(run.output, "Ask OCA anything"));
        assert(access(auth, F_OK) != 0);
    }
    const int signals[] = {SIGTERM, SIGHUP, SIGQUIT, SIGPIPE};
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        start(&run, executable, home, alias, NULL, 1);
        assert(wait_text(&run, "API key:"));
        assert_hidden(&run);
        send_input(&run, "fixture-login-key");
        assert(kill(run.child, signals[i]) == 0);
        finish(&run, 1);
        assert(access(auth, F_OK) != 0);
    }

    start(&run, executable, home, alias, NULL, 1);
    assert(wait_text(&run, "API key:"));
    send_input(&run, "invalid key\n");
    finish(&run, 1);
    assert(strstr(run.output, "invalid characters") && access(auth, F_OK) != 0);

    start(&run, executable, home, alias, NULL, 1);
    assert(wait_text(&run, "API key:"));
    assert(mkdir(auth, 0700) == 0);
    send_input(&run, "fixture-login-key\n");
    finish(&run, 1);
    assert(strstr(run.output, "cannot replace") && !strstr(run.output, "Ask OCA anything"));
    assert(access(config, F_OK) == 0);
    assert(rmdir(auth) == 0);

    for (int canonical_name = 0; canonical_name < 2; canonical_name++) {
        start(&run, executable, home, canonical_name ? canonical : alias, NULL, 1);
        assert(wait_text(&run, "Model kimi-k3 requires credentials for kimi."));
        assert(wait_text(&run, "API key:"));
        assert_hidden(&run);
        send_input(&run, "abc");
        assert(wait_text(&run, "API key: ***"));
        assert(!strstr(run.output, "API key: abc"));
        send_input(&run, "\x7f\x15");
        assert(wait_text(&run, "***\b \b\b \b\b \b"));
        char wrapped[82];
        memset(wrapped, 'z', sizeof(wrapped) - 1);
        wrapped[sizeof(wrapped) - 1] = 0;
        send_input(&run, wrapped);
        assert(wait_text(&run, "********************************"));
        send_input(&run, "\x15");
        assert(wait_text(&run, "\x1b[A\r\x1b[79C \r\x1b[79C"));
        assert(!strstr(run.output, wrapped));
        send_input(&run, "fixture-login-key");
        assert(wait_text(&run, "*****************"));
        assert(!strstr(run.output, "Ask OCA anything"));
        send_input(&run, "\n");
        assert(wait_text(&run, "Ask OCA anything"));
        assert(strstr(run.output, "kimi-k3"));
        assert(kill(run.child, SIGTERM) == 0);
        finish(&run, 0);
        struct stat state;
        assert(stat(auth, &state) == 0 && (state.st_mode & 0777) == 0600);
        assert(stat(config, &state) == 0 && (state.st_mode & 0777) == 0600);
        assert(stat(state_dir, &state) == 0 && (state.st_mode & 0777) == 0700);
        FILE *file = fopen(auth, "rb");
        char saved[1024] = {0};
        assert(file && fread(saved, 1, sizeof(saved) - 1, file) > 0);
        assert(fclose(file) == 0 && strstr(saved, "fixture-login-key"));

        start(&run, executable, home, alias, "fixture-environment-key", 1);
        assert(wait_text(&run, "Ask OCA anything"));
        assert(!strstr(run.output, "API key:"));
        assert(kill(run.child, SIGTERM) == 0);
        finish(&run, 0);
        clean_state(home);
    }

    for (size_t length = 4096; length <= 4097; length++) {
        char key[4099];
        memset(key, 'q', length);
        key[length] = '\n';
        key[length + 1] = 0;
        start(&run, executable, home, alias, NULL, 1);
        assert(wait_text(&run, "API key:"));
        send_input(&run, key);
        if (length == 4096) {
            assert(wait_text(&run, "Ask OCA anything"));
            assert(!kill(run.child, SIGTERM));
            finish(&run, 0);
            char *stored = calloc(8192, 1);
            FILE *file = fopen(auth, "rb");
            assert(stored && file && fread(stored, 1, 8191, file) > 0 && !fclose(file));
            key[length] = 0;
            assert(strstr(stored, key));
            free(stored);
            clean_state(home);
        } else {
            finish(&run, 1);
            assert(strstr(run.output, "API key exceeds the size limit") && access(auth, F_OK));
        }
        key[64] = 0;
        assert(!strstr(run.output, key));
    }

    start(&run, executable, home, canonical, "fixture-environment-key", 1);
    assert(wait_text(&run, "Ask OCA anything"));
    assert(!strstr(run.output, "API key:"));
    assert(access(auth, F_OK) != 0);
    assert(kill(run.child, SIGTERM) == 0);
    finish(&run, 0);
    clean_state(home);
    char *go[] = {executable, "-m", "dsf", NULL};
    start(&run, executable, home, go, NULL, 1);
    assert(wait_text(&run, "Model deepseek-flash requires credentials for deepseek."));
    assert(wait_text(&run, "API key:"));
    assert_hidden(&run);
    send_input(&run, "fixture-login-key\n");
    assert(wait_text(&run, "Ask OCA anything"));
    assert(strstr(run.output, "deepseek-flash"));
    assert(kill(run.child, SIGTERM) == 0);
    finish(&run, 0);
    clean_state(home);
    char workspace[PATH_MAX];
    snprintf(workspace, sizeof(workspace), "%s/work", home);
    assert(rmdir(workspace) == 0);
    assert(rmdir(home) == 0);
    puts("test_login: passed");
    return 0;
}
