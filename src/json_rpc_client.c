#include "json_rpc_client.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_RESPONSE_BUF_SZ (1024 * 8)
#define HTTP_TIMEOUT_SEC 2
#define LUT_FILENAME_SDR "lut_lin_tables.3d"
#define LUT_FILENAME_HDR "lut_lin_tables_hdr.3d"
#define LUT_FILENAME_DOLBYVISION "lut_lin_tables_dv.3d"

AmbientLightingDaemon daemon_flavor = DAEMON_NOT_SET;

const char* daemon_to_string(AmbientLightingDaemon flavor)
{
    switch (flavor) {
    case DAEMON_INVALID:
        return "INVALID";
    case DAEMON_NOT_SET:
        return "NOT SET";
    case DAEMON_HYPERION_NG:
        return "Hyperion.NG";
    case DAEMON_HYPERHDR:
        return "HyperHDR";
    default:
        return "<UNKNOWN>";
    }
}

DynamicRange get_dynamic_range(const char* range)
{
    if (strcmp(range, "DolbyVision") == 0 || strcmp(range, "dolbyHdr") == 0) {
        return DOLBYVISION;
    } else if (strstr(range, "hdr") != NULL || strstr(range, "HDR") != NULL) {
        return HDR10;
    } else if (strcmp(range, "sdr") == 0 || strcmp(range, "none") == 0) {
        return SDR;
    } else {
        WARN("get_dynamic_range: Unknown dynamic range: %s", range);
        return SDR;
    }
}

static int send_all(int fd, const char* data, size_t len)
{
    while (len > 0) {
        // MSG_NOSIGNAL: a peer that hung up must not kill the service with SIGPIPE
        ssize_t n = send(fd, data, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        data += n;
        len -= (size_t)n;
    }
    return 0;
}

static long parse_content_length(const char* headers, const char* headers_end)
{
    const char* line = headers;
    while (line < headers_end) {
        const char* next = strstr(line, "\r\n");
        if (next == NULL || next > headers_end)
            next = headers_end;
        if (strncasecmp(line, "Content-Length:", 15) == 0)
            return strtol(line + 15, NULL, 10);
        line = next + 2;
    }
    return -1;
}

/*
 * Minimal HTTP/1.1 POST over a plain socket. Deliberately no shell, no curl:
 * host comes from user settings and must be a numeric IPv4 address. Every
 * socket operation is bounded by HTTP_TIMEOUT_SEC so a missing receiver cannot
 * stall the Luna main loop. On success response_body holds the NUL-terminated
 * body (truncated to out_buf_sz - 1 bytes).
 */
int do_http_post(const char* host, ushort port, const char* path, const char* post_body, char* response_body, size_t out_buf_sz)
{
    int ret = 0;
    int fd = -1;
    struct sockaddr_in addr;
    struct timeval timeout = { HTTP_TIMEOUT_SEC, 0 };
    char header[256];
    size_t body_len = strlen(post_body);
    size_t received = 0;
    char* body = NULL;
    long content_length = -1;

    if (out_buf_sz < 2) {
        return -1;
    }
    response_body[0] = '\0';

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        WARN("do_http_post: not a valid IPv4 address: %s", host);
        return -1;
    }

    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        ERR("do_http_post: socket() failed: %s", strerror(errno));
        return -2;
    }

    // On Linux SO_SNDTIMEO also bounds connect()
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0
        || setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        ERR("do_http_post: setsockopt() failed: %s", strerror(errno));
        ret = -2;
        goto exit;
    }

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        WARN("do_http_post: connect() to %s:%u failed: %s", host, (unsigned)port, strerror(errno));
        ret = -3;
        goto exit;
    }

    // HTTP/1.1 because HyperHDR's web server never answers HTTP/1.0 requests
    int header_len = snprintf(header, sizeof(header),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host, (unsigned)port, body_len);
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
        ERR("do_http_post: request header too long");
        ret = -4;
        goto exit;
    }

    if (send_all(fd, header, (size_t)header_len) != 0 || send_all(fd, post_body, body_len) != 0) {
        WARN("do_http_post: send() failed: %s", strerror(errno));
        ret = -5;
        goto exit;
    }

    // Read until the server closes, Content-Length is satisfied or the buffer is full
    while (received < out_buf_sz - 1) {
        ssize_t n = recv(fd, response_body + received, out_buf_sz - 1 - received, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            WARN("do_http_post: recv() failed: %s", strerror(errno));
            ret = -6;
            goto exit;
        }
        if (n == 0)
            break;

        received += (size_t)n;
        response_body[received] = '\0';

        if (body == NULL && (body = strstr(response_body, "\r\n\r\n")) != NULL) {
            content_length = parse_content_length(response_body, body);
            body += 4;
        }

        if (body != NULL && content_length >= 0
            && (size_t)(response_body + received - body) >= (size_t)content_length)
            break;
    }

    if (strncmp(response_body, "HTTP/1.", 7) != 0 || strncmp(response_body + 8, " 200", 4) != 0) {
        WARN("do_http_post: unexpected HTTP status from %s:%u", host, (unsigned)port);
        ret = -7;
        goto exit;
    }

    if (body == NULL) {
        WARN("do_http_post: incomplete HTTP response from %s:%u", host, (unsigned)port);
        ret = -8;
        goto exit;
    }

    memmove(response_body, body, strlen(body) + 1);

exit:
    if (ret != 0)
        response_body[0] = '\0';
    if (fd >= 0)
        close(fd);

    return ret;
}

int send_rpc_message(char* host, ushort rpc_port, jvalue_ref post_body_jval, jvalue_ref* response_body_jval)
{
    int ret = 0;
    bool command_success = false;
    jvalue_ref success_field;
    JSchemaInfo schema;

    *response_body_jval = jinvalid();

    char* response = (char*)calloc(1, MAX_RESPONSE_BUF_SZ);
    if (response == NULL) {
        ERR("send_rpc_message: alloc failed -> response");
        return -2;
    }

    const char* post_body = jvalue_tostring_simple(post_body_jval);

    if ((ret = do_http_post(host, rpc_port, "/json-rpc", post_body, response, MAX_RESPONSE_BUF_SZ)) != 0) {
        WARN("send_rpc_message: HTTP POST request failed, ret: %d", ret);
        ret = -3;
        goto exit;
    }

    jschema_info_init(&schema, jschema_all(), NULL, NULL);
    *response_body_jval = jdom_parse(j_cstr_to_buffer(response), DOMOPT_NOOPT, &schema);

    // Parsing failed
    if (!jis_valid(*response_body_jval) || jis_null(*response_body_jval)) {
        ERR("Failed to parse RPC response from %s:%d", host, rpc_port);
        ret = -4;
        goto exit;
    }

    // Check boolean field "success" in response
    success_field = jobject_get(*response_body_jval, j_cstr_to_buffer("success"));
    if (!jis_boolean(success_field) || jboolean_get(success_field, &command_success) || !command_success) {
        ERR("Invalid success status from %s:%d", host, rpc_port);
        ret = -5;
        goto exit;
    }

exit:
    // Callers only own the response on success
    if (ret != 0)
        j_release(response_body_jval);
    free(response);

    return ret;
}

int get_daemon_flavor(char* host, ushort rpc_port, AmbientLightingDaemon* flavor)
{
    int ret = 0;

    jvalue_ref info_node;
    jvalue_ref daemon_node;
    jvalue_ref response_body_jval;
    jvalue_ref post_body = jobject_create();
    jobject_put(post_body, J_CSTR_TO_JVAL("command"), jstring_create("sysinfo"));

    if ((ret = send_rpc_message(host, rpc_port, post_body, &response_body_jval)) != 0) {
        WARN("get_daemon_flavor: Failed to send RPC message, code: %d", ret);
        j_release(&post_body);
        return -1;
    }

    /*
     * Response difference between Hyperion.NG and HyperHDR
     * - Hyperion.NG: { "command" "info": { "hyperion" : {...} }, "success": true }
     * - HyperHDR: { "command" "info": { "hyperhdr" : {...} }, "success": true }
     */
    if ((info_node = jobject_get(response_body_jval, j_cstr_to_buffer("info"))) && jis_valid(info_node)) {
        if ((daemon_node = jobject_get(info_node, j_cstr_to_buffer("hyperion"))) && jis_valid(daemon_node)) {
            DBG("get_daemon_flavor: Detected Hyperion.NG");
            *flavor = DAEMON_HYPERION_NG;
        } else if ((daemon_node = jobject_get(info_node, j_cstr_to_buffer("hyperhdr"))) && jis_valid(daemon_node)) {
            DBG("get_daemon_flavor: Detected HyperHDR");
            *flavor = DAEMON_HYPERHDR;
        } else {
            WARN("get_daemon_flavor: Unknown daemon flavor!");
            *flavor = DAEMON_INVALID;
            ret = -2;
        }
    } else {
        WARN("Failed to fetch daemon flavor from JSON-RPC, no 'info' node");
        ret = -3;
    }

    j_release(&response_body_jval);
    j_release(&post_body);
    return ret;
}

int set_hdr_state(char* host, ushort rpc_port, DynamicRange range)
{
    int ret = 0;

    if (daemon_flavor == DAEMON_NOT_SET || daemon_flavor == DAEMON_INVALID) {
        DBG("set_hdr_state: Currently known daemon flavor: %d (%s) -> Fetching new state",
            daemon_flavor, daemon_to_string(daemon_flavor));
        if ((ret = get_daemon_flavor(host, rpc_port, &daemon_flavor)) != 0) {
            ERR("set_hdr_state: Failed to fetch daemon flavor, ret: %d", ret);
            return -1;
        }
        INFO("Detected daemon flavor: %d (%s)", daemon_flavor, daemon_to_string(daemon_flavor));
    }

    if (daemon_flavor != DAEMON_HYPERHDR) {
        WARN("set_hdr_state: Daemon is not HyperHDR -> Not submitting HDR state!");
        return -2;
    }

    jvalue_ref response_body_jval;
    jvalue_ref post_body = jobject_create();

    jobject_put(post_body, J_CSTR_TO_JVAL("command"), jstring_create("videomodehdr"));
    jobject_put(post_body, J_CSTR_TO_JVAL("HDR"), jnumber_create_i32(1));

    const char* lut_filename = "";
    switch (range) {
    case SDR:
        lut_filename = LUT_FILENAME_SDR;
        break;
    case HDR10:
        lut_filename = LUT_FILENAME_HDR;
        break;
    case DOLBYVISION:
        lut_filename = LUT_FILENAME_DOLBYVISION;
        break;
    }
    jobject_put(post_body, J_CSTR_TO_JVAL("flatbuffers_user_lut_filename"), jstring_create(lut_filename));

    if ((ret = send_rpc_message(host, rpc_port, post_body, &response_body_jval)) != 0) {
        WARN("set_hdr_state: Failed to send RPC message, code: %d", ret);
        ret = -3;
    } else {
        j_release(&response_body_jval);
    }

    j_release(&post_body);
    return ret;
}