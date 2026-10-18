// receiver_test_tcp against a local listener, a closed port, an unroutable
// address and invalid input.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "check.h"
#include "receiver_test.h"

// Listens on a free loopback port and returns it (0 on failure)
static int listen_loopback(int* fd_out)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    if (fd < 0 || bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 || listen(fd, 4) != 0
        || getsockname(fd, (struct sockaddr*)&addr, &len) != 0) {
        return 0;
    }
    *fd_out = fd;
    return ntohs(addr.sin_port);
}

int main(void)
{
    char err[96];
    int srv = -1;
    int port = listen_loopback(&srv);
    CHECK(port > 0, "loopback listener");

    CHECK(receiver_test_tcp("127.0.0.1", port, 2000, err, sizeof(err)) == 0 && err[0] == '\0', "open port connects");

    // Closing the listener frees the port, so connecting to it is refused
    close(srv);
    int ret = receiver_test_tcp("127.0.0.1", port, 2000, err, sizeof(err));
    CHECK(ret != 0 && strstr(err, "refused") != NULL, "closed port says refused");

    // TEST-NET-1 (RFC 5737) is never routed: either no answer or unreachable
    ret = receiver_test_tcp("192.0.2.1", 19400, 500, err, sizeof(err));
    CHECK(ret != 0 && err[0] != '\0', "unroutable address fails with a reason");

    CHECK(receiver_test_tcp("1.2.3", 19400, 500, err, sizeof(err)) != 0 && strstr(err, "Invalid") != NULL, "invalid address rejected");
    CHECK(receiver_test_tcp("", 19400, 500, err, sizeof(err)) != 0, "empty address rejected");
    CHECK(receiver_test_tcp("127.0.0.1", 0, 500, err, sizeof(err)) != 0, "port 0 rejected");
    CHECK(receiver_test_tcp("127.0.0.1", 65536, 500, err, sizeof(err)) != 0, "port 65536 rejected");

    char tiny[4];
    receiver_test_tcp("1.2.3", 19400, 500, tiny, sizeof(tiny));
    CHECK(strlen(tiny) == 3, "error text truncated to the buffer");

    return check_summary();
}
