#define _POSIX_C_SOURCE 200809L
#include "a865_backend.h"

#include <errno.h>
#include <stdio.h>
#include <unistd.h>

int f_exit = 0;

int main(void)
{
    unsigned char data[1880];
    int stream_fd = -1;
    pid_t child_pid = 0;
    size_t received = 0;

    if(a865_backend_start(557142857U, &stream_fd, &child_pid) != 0)
        return 1;
    while(received < sizeof(data)) {
        ssize_t count = read(stream_fd, data + received, sizeof(data) - received);
        if(count < 0 && errno == EINTR)
            continue;
        if(count <= 0) {
            fprintf(stderr, "A865 pipe closed before TS arrived\n");
            a865_backend_stop(&stream_fd, &child_pid);
            return 2;
        }
        received += (size_t)count;
    }
    if(data[0] != 0x47 || data[188] != 0x47) {
        fprintf(stderr, "backend pipe did not carry MPEG-TS packets\n");
        a865_backend_stop(&stream_fd, &child_pid);
        return 3;
    }
    if(write(STDOUT_FILENO, data, sizeof(data)) != (ssize_t)sizeof(data)) {
        fprintf(stderr, "cannot forward test TS to stdout\n");
        a865_backend_stop(&stream_fd, &child_pid);
        return 4;
    }
    if(a865_backend_stop(&stream_fd, &child_pid) != 0)
        return 5;
    fprintf(stderr, "PASS: A865 backend readiness, TS pipe, and child release\n");
    return 0;
}
