#include "receiver_test.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

int receiver_test_tcp(const char* address, int port, int timeout_ms, char* err, size_t err_size)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (port <= 0 || port > 65535 || inet_pton(AF_INET, address, &addr.sin_addr) != 1) {
        snprintf(err, err_size, "Invalid address or port");
        return -EINVAL;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, err_size, "%s", strerror(errno));
        return -errno;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    int ret = 0;
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        if (errno != EINPROGRESS) {
            ret = -errno;
        } else {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
            int ready = select(fd + 1, NULL, &wfds, NULL, &tv);
            if (ready == 0) {
                ret = -ETIMEDOUT;
            } else if (ready < 0) {
                ret = -errno;
            } else {
                int so_error = 0;
                socklen_t so_len = sizeof(so_error);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len);
                ret = -so_error;
            }
        }
    }
    close(fd);

    if (ret == -ETIMEDOUT) {
        snprintf(err, err_size, "No answer (firewall or wrong address?)");
    } else if (ret == -ECONNREFUSED) {
        snprintf(err, err_size, "Connection refused (receiver not running on this port?)");
    } else if (ret != 0) {
        snprintf(err, err_size, "%s", strerror(-ret));
    } else if (err_size > 0) {
        err[0] = '\0';
    }
    return ret;
}
