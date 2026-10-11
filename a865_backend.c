/* AVerMedia A865 user-mode backend process and TS pipe lifecycle. */
#define _POSIX_C_SOURCE 200809L
#include "a865_backend.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
extern int f_exit;

#define A865_READY_FD 3
#define A865_READY_TIMEOUT_MS 60000
#define JAPAN_UHF_FIRST_HZ 473142857U
#define JAPAN_UHF_LAST_HZ  707142857U
#define JAPAN_UHF_STEP_HZ    6000000U

static int move_fd_above_stdio(int fd)
{
    int moved = fcntl(fd, F_DUPFD_CLOEXEC, 10);
    if(moved < 0)
        return -1;
    close(fd);
    return moved;
}

static void close_fd(int *fd)
{
    if(*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void stop_child(pid_t *child_pid)
{
    int status;
    pid_t result;

    if(*child_pid <= 0)
        return;
    if(kill(*child_pid, SIGTERM) < 0 && errno != ESRCH)
        fprintf(stderr, "A865 backend: SIGTERM failed: %s\n", strerror(errno));
    do {
        result = waitpid(*child_pid, &status, 0);
    } while(result < 0 && errno == EINTR);
    if(result < 0 && errno != ECHILD)
        fprintf(stderr, "A865 backend: waitpid failed: %s\n", strerror(errno));
    *child_pid = 0;
}

int a865_backend_start(unsigned int frequency_hz, int *stream_fd,
                       pid_t *child_pid)
{
    int stream_pipe[2] = {-1, -1};
    int ready_pipe[2] = {-1, -1};
    int stream_read = -1, stream_write = -1;
    int ready_read = -1, ready_write = -1;
    int actions_ready = 0, attributes_ready = 0;
    int elapsed_ms = 0;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    sigset_t child_mask, child_defaults;
    short spawn_flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
    const char *program;
    char frequency[16];
    char ready_fd[] = "3";
    char *arguments[6];
    pid_t child = 0;
    int result = 1;
    int spawn_error;
    struct pollfd status_poll;
    unsigned char status_byte = 0;

    if(!stream_fd || !child_pid) {
        fprintf(stderr, "A865 backend: invalid stream state output\n");
        return 1;
    }
    *stream_fd = -1;
    *child_pid = 0;
    if(frequency_hz < JAPAN_UHF_FIRST_HZ || frequency_hz > JAPAN_UHF_LAST_HZ ||
       (frequency_hz - JAPAN_UHF_FIRST_HZ) % JAPAN_UHF_STEP_HZ != 0) {
        fprintf(stderr,
                "A865 backend supports Japanese ISDB-T UHF channels 13-52 only\n");
        return 1;
    }
    if(pipe(stream_pipe) < 0 || pipe(ready_pipe) < 0) {
        fprintf(stderr, "A865 backend: pipe creation failed: %s\n", strerror(errno));
        goto done;
    }
    stream_read = move_fd_above_stdio(stream_pipe[0]);
    if(stream_read >= 0)
        stream_pipe[0] = -1;
    stream_write = move_fd_above_stdio(stream_pipe[1]);
    if(stream_write >= 0)
        stream_pipe[1] = -1;
    ready_read = move_fd_above_stdio(ready_pipe[0]);
    if(ready_read >= 0)
        ready_pipe[0] = -1;
    ready_write = move_fd_above_stdio(ready_pipe[1]);
    if(ready_write >= 0)
        ready_pipe[1] = -1;
    if(stream_read < 0 || stream_write < 0 || ready_read < 0 || ready_write < 0) {
        fprintf(stderr, "A865 backend: cannot reserve pipe descriptors: %s\n",
                strerror(errno));
        goto done;
    }

    if(snprintf(frequency, sizeof(frequency), "%u", frequency_hz) >=
       (int)sizeof(frequency)) {
        fprintf(stderr, "A865 backend: frequency is out of range\n");
        goto done;
    }
    program = getenv("RECDVB_A865_BACKEND");
    if(!program || !*program)
        program = "a865r-stream";
    arguments[0] = (char *)program;
    arguments[1] = "--frequency-hz";
    arguments[2] = frequency;
    arguments[3] = "--ready-fd";
    arguments[4] = ready_fd;
    arguments[5] = NULL;

    spawn_error = posix_spawn_file_actions_init(&actions);
    if(spawn_error != 0) {
        fprintf(stderr, "A865 backend: spawn actions: %s\n", strerror(spawn_error));
        goto done;
    }
    actions_ready = 1;
    spawn_error = posix_spawn_file_actions_addclose(&actions, stream_read);
    if(spawn_error == 0)
        spawn_error = posix_spawn_file_actions_addclose(&actions, ready_read);
    if(spawn_error == 0)
        spawn_error = posix_spawn_file_actions_adddup2(&actions, stream_write,
                                                        STDOUT_FILENO);
    if(spawn_error == 0)
        spawn_error = posix_spawn_file_actions_adddup2(&actions, ready_write,
                                                        A865_READY_FD);
    if(spawn_error == 0)
        spawn_error = posix_spawn_file_actions_addclose(&actions, stream_write);
    if(spawn_error == 0)
        spawn_error = posix_spawn_file_actions_addclose(&actions, ready_write);
    if(spawn_error != 0) {
        fprintf(stderr, "A865 backend: configure child descriptors: %s\n",
                strerror(spawn_error));
        goto done;
    }

    spawn_error = posix_spawnattr_init(&attributes);
    if(spawn_error != 0) {
        fprintf(stderr, "A865 backend: spawn attributes: %s\n", strerror(spawn_error));
        goto done;
    }
    attributes_ready = 1;
    sigemptyset(&child_mask);
    sigemptyset(&child_defaults);
    sigaddset(&child_defaults, SIGINT);
    sigaddset(&child_defaults, SIGPIPE);
    sigaddset(&child_defaults, SIGTERM);
    spawn_error = posix_spawnattr_setsigmask(&attributes, &child_mask);
    if(spawn_error == 0)
        spawn_error = posix_spawnattr_setsigdefault(&attributes, &child_defaults);
    if(spawn_error == 0)
        spawn_error = posix_spawnattr_setflags(&attributes, spawn_flags);
    if(spawn_error != 0) {
        fprintf(stderr, "A865 backend: configure child signals: %s\n",
                strerror(spawn_error));
        goto done;
    }

    spawn_error = posix_spawnp(&child, program, &actions, &attributes,
                               arguments, environ);
    if(spawn_error != 0) {
        fprintf(stderr, "A865 backend: cannot start %s: %s\n",
                program, strerror(spawn_error));
        goto done;
    }
    *child_pid = child;
    close_fd(&stream_write);
    close_fd(&ready_write);

    status_poll.fd = ready_read;
    status_poll.events = POLLIN | POLLHUP;
    status_poll.revents = 0;
    while(!f_exit && elapsed_ms < A865_READY_TIMEOUT_MS) {
        do {
            spawn_error = poll(&status_poll, 1, 500);
        } while(spawn_error < 0 && errno == EINTR && !f_exit);
        if(spawn_error > 0)
            break;
        if(spawn_error < 0) {
            fprintf(stderr, "A865 backend: readiness poll failed: %s\n", strerror(errno));
            goto done;
        }
        elapsed_ms += 500;
    }
    if(f_exit) {
        fprintf(stderr, "A865 backend: interrupted while waiting for tuner lock\n");
        goto done;
    }
    if(elapsed_ms >= A865_READY_TIMEOUT_MS) {
        fprintf(stderr, "A865 backend: initialize/tune readiness timed out\n");
        goto done;
    }
    ssize_t received;
    do {
        received = read(ready_read, &status_byte, 1);
    } while(received < 0 && errno == EINTR);
    if(received != 1 || status_byte != 'R') {
        fprintf(stderr,
                "A865 backend: child exited before MPEG lock (status=%c)\n",
                received == 1 ? status_byte : '?');
        goto done;
    }

    *stream_fd = stream_read;
    stream_read = -1;
    result = 0;
    fprintf(stderr, "A865 backend: Japanese ISDB-T tuner is locked; TS pipe ready\n");

done:
    close_fd(&stream_pipe[0]);
    close_fd(&stream_pipe[1]);
    close_fd(&ready_pipe[0]);
    close_fd(&ready_pipe[1]);
    close_fd(&stream_read);
    close_fd(&stream_write);
    close_fd(&ready_read);
    close_fd(&ready_write);
    if(actions_ready)
        posix_spawn_file_actions_destroy(&actions);
    if(attributes_ready)
        posix_spawnattr_destroy(&attributes);
    if(result != 0)
        stop_child(child_pid);
    return result;
}

int a865_backend_stop(int *stream_fd, pid_t *child_pid)
{
    int result = 0;

    if(!stream_fd || !child_pid)
        return 1;
    close_fd(stream_fd);
    if(*child_pid > 0) {
        if(kill(*child_pid, SIGTERM) < 0 && errno != ESRCH) {
            fprintf(stderr, "A865 backend: SIGTERM failed: %s\n", strerror(errno));
            result = 1;
        }
        int status;
        pid_t waited;
        do {
            waited = waitpid(*child_pid, &status, 0);
        } while(waited < 0 && errno == EINTR);
        if(waited < 0 && errno != ECHILD) {
            fprintf(stderr, "A865 backend: waitpid failed: %s\n", strerror(errno));
            result = 1;
        } else if(waited > 0 &&
                  (!WIFEXITED(status) || WEXITSTATUS(status) != 0)) {
            fprintf(stderr, "A865 backend: child exited unsuccessfully\n");
            result = 1;
        }
        *child_pid = 0;
    }
    return result;
}
