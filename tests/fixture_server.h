#ifndef OCA_TEST_FIXTURE_SERVER_H
#define OCA_TEST_FIXTURE_SERVER_H

#include <sys/types.h>

/* The caller owns this server until fixture_server_stop. Start sets a fake
 * credential and a loopback URL for this process and its test children. */
typedef struct {
    pid_t pid;
    int stop_fd;
} fixture_server;

fixture_server fixture_server_start(void);
void fixture_server_stop(fixture_server *server);

#endif
