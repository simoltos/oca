#ifndef OCA_FIXTURE_IO_H
#define OCA_FIXTURE_IO_H

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    IO_NORMAL, IO_INTERRUPTED_WRITE, IO_SHORT_WRITE, IO_ZERO_WRITE,
    IO_FAILED_WRITE, IO_FAILED_CHMOD
};

static int fixture_io_failure;
static int fixture_io_descriptor;

static ssize_t fixture_io_write(int fd, const void *data, size_t length) {
    int failure = fixture_io_failure;
    if (failure >= IO_INTERRUPTED_WRITE && failure <= IO_FAILED_WRITE) {
        fixture_io_failure = IO_NORMAL;
        fixture_io_descriptor = fd;
        if (failure == IO_SHORT_WRITE) return write(fd, data, length ? 1 : 0);
        if (failure == IO_ZERO_WRITE) return 0;
        errno = failure == IO_INTERRUPTED_WRITE ? EINTR : ENOSPC;
        return -1;
    }
    return write(fd, data, length);
}

static int fixture_io_chmod(int fd, mode_t mode) {
    if (fixture_io_failure == IO_FAILED_CHMOD) {
        fixture_io_failure = IO_NORMAL;
        fixture_io_descriptor = fd;
        errno = EPERM;
        return -1;
    }
    return fchmod(fd, mode);
}

typedef int (*fixture_atomic_writer)(const char *, const char *, size_t, char *, size_t);

static void fixture_check_atomic_write(fixture_atomic_writer writer, const char *directory,
                                        const char *temporary_suffix) {
    char path[1024], temporary[1100], error[256];
    snprintf(path, sizeof(path), "%s/atomic-write-check", directory);
    snprintf(temporary, sizeof(temporary), "%s.%s.%ld", path, temporary_suffix, (long)getpid());
    for (int failure = IO_INTERRUPTED_WRITE; failure <= IO_FAILED_CHMOD; failure++) {
        assert(!writer(path, "old", 3, error, sizeof(error)));
        fixture_io_failure = failure;
        fixture_io_descriptor = -1;
        int rc = writer(path, "new", 3, error, sizeof(error));
        assert(fixture_io_failure == IO_NORMAL && fixture_io_descriptor >= 0);
        int successful = failure == IO_INTERRUPTED_WRITE || failure == IO_SHORT_WRITE;
        assert(successful ? rc == 0 : rc < 0);
        assert(fcntl(fixture_io_descriptor, F_GETFD) == -1 && errno == EBADF);
        assert(access(temporary, F_OK) == -1 && errno == ENOENT);
        char contents[4] = {0};
        int fd = open(path, O_RDONLY);
        assert(fd >= 0 && read(fd, contents, sizeof(contents)) == 3);
        assert(!close(fd));
        assert(!strcmp(contents, successful ? "new" : "old"));
    }
    assert(!unlink(path));
}

#endif
