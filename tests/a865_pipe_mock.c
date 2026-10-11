#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;

static void stop_mock(int signal_number)
{
    (void)signal_number;
    running = 0;
}

int main(int argc, char **argv)
{
    unsigned char packet[188];
    const char ready = 'R';
    struct timespec pause = {0, 10000000};
    int ready_fd = 3;
    int i;

    if(argc != 5 || strcmp(argv[1], "--frequency-hz") != 0 ||
       strcmp(argv[2], "557142857") != 0 ||
       strcmp(argv[3], "--ready-fd") != 0 ||
       strcmp(argv[4], "3") != 0) {
        (void)write(ready_fd, "E", 1);
        return 2;
    }
    if(signal(SIGTERM, stop_mock) == SIG_ERR ||
       signal(SIGINT, stop_mock) == SIG_ERR ||
       signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        return 3;
    memset(packet, 0xff, sizeof(packet));
    packet[0] = 0x47;
    packet[1] = 0x1f;
    packet[2] = 0xff;
    packet[3] = 0x10;
    if(write(ready_fd, &ready, 1) != 1)
        return 4;
    close(ready_fd);

    while(running) {
        for(i = 0; i < 10 && running; i++) {
            ssize_t written = write(STDOUT_FILENO, packet, sizeof(packet));
            if(written != (ssize_t)sizeof(packet)) {
                if(errno == EPIPE) {
                    running = 0;
                    break;
                }
                return 5;
            }
        }
        nanosleep(&pause, NULL);
    }
    const char *release_marker = getenv("A865_MOCK_RELEASE_MARKER");
    if(release_marker) {
        int marker = open(release_marker, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if(marker < 0)
            return 6;
        if(write(marker, "released", 8) != 8) {
            close(marker);
            return 7;
        }
        close(marker);
    }
    return 0;
}
