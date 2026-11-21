/* Host stub: the BSD socket API from the host libc, except sendto(), which
 * is redirected to a fake that records what would have been sent. */
#pragma once
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
ssize_t host_fake_sendto(int sock, const void *buf, size_t len, int flags,
                         const struct sockaddr *to, socklen_t tolen);
#define sendto host_fake_sendto
