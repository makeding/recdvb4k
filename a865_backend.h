/* AVerMedia A865 user-mode TS pipe bridge. */
#ifndef RECDVB_A865_BACKEND_H
#define RECDVB_A865_BACKEND_H

#include <sys/types.h>

int a865_backend_start(unsigned int frequency_hz, int *stream_fd,
                       pid_t *child_pid);
int a865_backend_start_selected(unsigned int frequency_hz, const char *selector,
                                int *stream_fd, pid_t *child_pid);
int a865_backend_stop(int *stream_fd, pid_t *child_pid);

#endif
