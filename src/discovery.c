#include "discovery.h"
#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define SSDP_ADDR "239.255.255.250"
#define SSDP_PORT 1900

// Search targets the receivers answer to (their SSDPHandler identifiers)
static const char* const search_targets[] = {
    "urn:hyperhdr.eu:device:basic:1",
    "urn:hyperion-project.org:device:basic:1",
    NULL,
};

// Copies the value of header `name` (case-insensitive, at a line start) into
// out. Returns true if found.
static bool header_value(const char* reply, size_t len, const char* name, char* out, size_t out_size)
{
    size_t name_len = strlen(name);
    const char* end = reply + len;
    const char* line = reply;

    while (line < end) {
        const char* eol = memchr(line, '\n', (size_t)(end - line));
        if (eol == NULL) {
            eol = end;
        }
        if ((size_t)(eol - line) > name_len && strncasecmp(line, name, name_len) == 0 && line[name_len] == ':') {
            const char* v = line + name_len + 1;
            while (v < eol && (*v == ' ' || *v == '\t')) {
                v++;
            }
            const char* v_end = eol;
            while (v_end > v && (v_end[-1] == '\r' || v_end[-1] == ' ' || v_end[-1] == '\t')) {
                v_end--;
            }
            size_t n = (size_t)(v_end - v);
            if (n >= out_size) {
                n = out_size - 1;
            }
            memcpy(out, v, n);
            out[n] = '\0';
            return true;
        }
        line = eol + 1;
    }
    return false;
}

static int port_value(const char* reply, size_t len, const char* name)
{
    char value[16];
    if (!header_value(reply, len, name, value, sizeof(value))) {
        return 0;
    }
    char* endp = NULL;
    long port = strtol(value, &endp, 10);
    return (endp != value && port > 0 && port <= 65535) ? (int)port : 0;
}

bool discovery_parse_reply(const char* reply, size_t len, const char* sender_ip, receiver_info_t* out)
{
    if (reply == NULL || sender_ip == NULL || out == NULL || len < 12 || strncmp(reply, "HTTP/1.1 200", 12) != 0) {
        return false;
    }

    const char* prefix;
    const char* flavor;
    char st[96] = { 0 };
    header_value(reply, len, "ST", st, sizeof(st));
    if (strcmp(st, search_targets[0]) == 0) {
        prefix = "HYPERHDR";
        flavor = "HyperHDR";
    } else if (strcmp(st, search_targets[1]) == 0) {
        prefix = "HYPERION";
        flavor = "Hyperion";
    } else {
        return false;
    }

    struct in_addr addr;
    if (inet_pton(AF_INET, sender_ip, &addr) != 1) {
        return false;
    }

    char header[32];
    memset(out, 0, sizeof(*out));
    snprintf(out->address, sizeof(out->address), "%s", sender_ip);
    snprintf(out->flavor, sizeof(out->flavor), "%s", flavor);
    snprintf(header, sizeof(header), "%s-NAME", prefix);
    header_value(reply, len, header, out->name, sizeof(out->name));
    snprintf(header, sizeof(header), "%s-FBS-PORT", prefix);
    out->flatbuffer_port = port_value(reply, len, header);
    snprintf(header, sizeof(header), "%s-JSS-PORT", prefix);
    out->json_port = port_value(reply, len, header);
    return true;
}

static long elapsed_ms(const struct timeval* start)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    return (now.tv_sec - start->tv_sec) * 1000L + (now.tv_usec - start->tv_usec) / 1000L;
}

int discovery_search(receiver_info_t* out, int max, int timeout_ms)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        WARN("discovery: socket() failed: %s", strerror(errno));
        return -1;
    }

    unsigned char ttl = 2;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(SSDP_PORT);
    inet_pton(AF_INET, SSDP_ADDR, &dest.sin_addr);

    for (int i = 0; search_targets[i] != NULL; i++) {
        char msg[256];
        int n = snprintf(msg, sizeof(msg),
            "M-SEARCH * HTTP/1.1\r\nHOST: " SSDP_ADDR ":1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: %s\r\n\r\n",
            search_targets[i]);
        if (sendto(fd, msg, (size_t)n, 0, (struct sockaddr*)&dest, sizeof(dest)) < 0) {
            WARN("discovery: sendto() failed: %s", strerror(errno));
        }
    }

    int found = 0;
    struct timeval start;
    gettimeofday(&start, NULL);
    long left;
    while (found < max && (left = timeout_ms - elapsed_ms(&start)) > 0) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv = { left / 1000, (left % 1000) * 1000 };
        int ready = select(fd + 1, &fds, NULL, NULL, &tv);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0) {
            break;
        }

        char buf[1500];
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        ssize_t len = recvfrom(fd, buf, sizeof(buf) - 1, 0, (struct sockaddr*)&from, &from_len);
        if (len <= 0) {
            continue;
        }
        buf[len] = '\0';

        char ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip)) == NULL) {
            continue;
        }

        receiver_info_t info;
        if (!discovery_parse_reply(buf, (size_t)len, ip, &info)) {
            continue;
        }

        bool duplicate = false;
        for (int i = 0; i < found; i++) {
            if (strcmp(out[i].address, info.address) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            out[found++] = info;
        }
    }

    close(fd);
    return found;
}

int discovery_test_tcp(const char* address, int port, int timeout_ms, char* err, size_t err_size)
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
