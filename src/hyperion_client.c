#include "hyperion_client.h"
#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#include "hyperion_reply_reader.h"
#include "hyperion_reply_verifier.h"
#include "hyperion_request_builder.h"

static int _connect_unix_socket(int fd, const char* hostname);
static int _connect_inet_socket(int fd, const char* hostname, int port);
static int _send_message(const void* buffer, size_t size);
static int _write_message_locked(const void* buffer, size_t size);
static int _register_locked(void);
static bool _parse_reply(hyperionnet_Reply_table_t reply);

/*
 * The connection thread owns the socket lifecycle (hyperion_client,
 * hyperion_read, hyperion_destroy) while the capture thread sends frames.
 * _lock guards the socket state and serializes writes so a frame is never
 * written to a closed or reused file descriptor.
 */
static pthread_mutex_t _lock = PTHREAD_MUTEX_INITIALIZER;
static int sockfd = -1;
static bool _registered = false;
static int _priority = 0;
static const char* _origin = NULL;
static bool _connected = false;
static unsigned char recvBuff[1024];

int hyperion_client(const char* origin, const char* hostname, int port, bool unix_socket, int priority)
{
    int fd = socket(unix_socket ? AF_UNIX : AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        WARN("Could not create socket: %s", strerror(errno));
        return 1;
    }

    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout)) < 0) {
        WARN("setsockopt(SO_SNDTIMEO) failed: %s", strerror(errno));
        close(fd);
        return 1;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout)) < 0) {
        WARN("setsockopt(SO_RCVTIMEO) failed: %s", strerror(errno));
        close(fd);
        return 1;
    }

    int ret = unix_socket ? _connect_unix_socket(fd, hostname) : _connect_inet_socket(fd, hostname, port);
    if (ret != 0) {
        close(fd);
        return ret;
    }

    pthread_mutex_lock(&_lock);
    _origin = origin;
    _priority = priority;
    _registered = false;
    sockfd = fd;
    _connected = true;
    pthread_mutex_unlock(&_lock);

    return 0;
}

static int _read_full(int fd, void* buffer, size_t size)
{
    size_t done = 0;
    while (done < size) {
        ssize_t n = read(fd, (uint8_t*)buffer + done, size - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            // Peer closed the connection
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

int hyperion_read()
{
    // Only the connection thread changes sockfd, and it is the one calling us
    int fd = sockfd;
    if (fd < 0)
        return -1;

    uint8_t headbuff[4];
    if (_read_full(fd, headbuff, sizeof(headbuff)) != 0)
        return -1;

    uint32_t messageSize = ((uint32_t)headbuff[0] << 24) | ((uint32_t)headbuff[1] << 16) | ((uint32_t)headbuff[2] << 8) | (uint32_t)headbuff[3];
    if (messageSize == 0 || messageSize > sizeof(recvBuff)) {
        WARN("Invalid reply size from server: %u", messageSize);
        return -1;
    }

    if (_read_full(fd, recvBuff, messageSize) != 0)
        return -1;

    int verify = hyperionnet_Reply_verify_as_root(recvBuff, messageSize);
    if (verify != 0) {
        WARN("Malformed reply from server: %s", flatcc_verify_error_string(verify));
        return -1;
    }

    _parse_reply(hyperionnet_Reply_as_root(recvBuff));
    return 0;
}

int hyperion_destroy()
{
    pthread_mutex_lock(&_lock);
    if (sockfd >= 0) {
        close(sockfd);
    }
    sockfd = -1;
    _connected = false;
    _registered = false;
    pthread_mutex_unlock(&_lock);
    return 0;
}

int hyperion_set_image(const unsigned char* image, int width, int height)
{
    flatbuffers_builder_t B;
    flatcc_builder_init(&B);
    flatbuffers_uint8_vec_ref_t imgData = flatcc_builder_create_type_vector(&B, image, width * height * 3);
    hyperionnet_RawImage_ref_t rawImg = hyperionnet_RawImage_create(&B, imgData, width, height);
    hyperionnet_Image_ref_t imageReq = hyperionnet_Image_create(&B, hyperionnet_ImageType_as_RawImage(rawImg), -1);
    hyperionnet_Request_create_as_root(&B, hyperionnet_Command_as_Image(imageReq));
    size_t size;
    void* buf = flatcc_builder_finalize_buffer(&B, &size);
    int ret = _send_message(buf, size);
    free(buf);
    flatcc_builder_clear(&B);
    return ret;
}

int hyperion_set_nv12_image(const uint8_t* y, const uint8_t* uv, int width, int height, int stride_y, int stride_uv)
{
    flatbuffers_builder_t B;
    flatcc_builder_init(&B);
    flatbuffers_uint8_vec_ref_t yData = flatcc_builder_create_type_vector(&B, y, stride_y * height);
    flatbuffers_uint8_vec_ref_t uvData = flatcc_builder_create_type_vector(&B, uv, stride_uv * height / 2);
    hyperionnet_RawImage_ref_t nv12Img = hyperionnet_NV12Image_create(&B, yData, uvData, width, height, stride_y, stride_uv);
    hyperionnet_Image_ref_t imageReq = hyperionnet_Image_create(&B, hyperionnet_ImageType_as_NV12Image(nv12Img), -1);
    hyperionnet_Request_create_as_root(&B, hyperionnet_Command_as_Image(imageReq));
    size_t size;
    void* buf = flatcc_builder_finalize_buffer(&B, &size);
    int ret = _send_message(buf, size);
    free(buf);
    flatcc_builder_clear(&B);
    return ret;
}

int hyperion_set_register(const char* origin, int priority)
{
    pthread_mutex_lock(&_lock);
    _origin = origin;
    _priority = priority;
    int ret = sockfd >= 0 ? _register_locked() : 0;
    pthread_mutex_unlock(&_lock);
    return ret;
}

static int _register_locked(void)
{
    flatbuffers_builder_t B;
    flatcc_builder_init(&B);
    hyperionnet_Register_ref_t registerReq = hyperionnet_Register_create(&B, flatcc_builder_create_string_str(&B, _origin), _priority);
    hyperionnet_Request_create_as_root(&B, hyperionnet_Command_as_Register(registerReq));

    size_t size;
    void* buf = flatcc_builder_finalize_buffer(&B, &size);
    int ret = _write_message_locked(buf, size);

    free(buf);
    flatcc_builder_clear(&B);
    return ret;
}

static int _write_message_locked(const void* buffer, size_t size)
{
    const uint8_t header[] = {
        (uint8_t)((size >> 24) & 0xFF),
        (uint8_t)((size >> 16) & 0xFF),
        (uint8_t)((size >> 8) & 0xFF),
        (uint8_t)(size & 0xFF)
    };
    struct iovec iov[] = {
        { (void*)header, sizeof(header) },
        { (void*)buffer, size }
    };
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;

    // MSG_NOSIGNAL: a restarted Hyperion server must not kill us with SIGPIPE
    return sendmsg(sockfd, &msg, MSG_NOSIGNAL) == (ssize_t)(sizeof(header) + size) ? 0 : -1;
}

static int _send_message(const void* buffer, size_t size)
{
    int ret = 0;

    pthread_mutex_lock(&_lock);
    if (sockfd >= 0 && _connected) {
        ret = _registered ? _write_message_locked(buffer, size) : _register_locked();
        if (ret != 0) {
            // A partial write leaves the stream unframed: stop sending and make
            // the connection thread see EOF so it reconnects
            _connected = false;
            shutdown(sockfd, SHUT_RDWR);
        }
    }
    pthread_mutex_unlock(&_lock);

    return ret;
}

static bool _parse_reply(hyperionnet_Reply_table_t reply)
{
    if (!hyperionnet_Reply_error(reply)) {
        // no error set must be a success or registered or video
        int32_t videoMode = hyperionnet_Reply_video(reply);
        int32_t registered = hyperionnet_Reply_registered(reply);
        if (videoMode != -1) {
            // We got a video reply.
            // printf("set video mode %d\n", videoMode);
            return true;
        }

        // We got a registered reply.
        pthread_mutex_lock(&_lock);
        if (registered == _priority) {
            _registered = true;
        }
        pthread_mutex_unlock(&_lock);

        return true;
    } else {
        flatbuffers_string_t error = hyperionnet_Reply_error(reply);
        WARN("Error received from server: %s", error);
    }

    return false;
}

static int _connect_inet_socket(int fd, const char* hostname, int port)
{
    struct sockaddr_in serv_addr_in;

    memset(&serv_addr_in, 0, sizeof(serv_addr_in));

    serv_addr_in.sin_family = AF_INET;
    serv_addr_in.sin_port = htons(port);

    if (inet_pton(AF_INET, hostname, &serv_addr_in.sin_addr) <= 0) {
        WARN("inet_pton error occured (hostname: %s): %s", hostname, strerror(errno));
        return 1;
    }

    if (connect(fd, (struct sockaddr*)&serv_addr_in, sizeof(serv_addr_in)) < 0) {
        WARN("connect() to %s:%d failed: %s", hostname, port, strerror(errno));
        return 1;
    }

    return 0;
}

static int _connect_unix_socket(int fd, const char* hostname)
{
    struct sockaddr_un serv_addr_un;

    memset(&serv_addr_un, 0, sizeof(serv_addr_un));

    size_t path_len = strlen(hostname);
    if (path_len == 0 || path_len >= sizeof(serv_addr_un.sun_path)) {
        WARN("Unix socket path length %zu is invalid (max %zu)", path_len, sizeof(serv_addr_un.sun_path) - 1);
        return 1;
    }

    serv_addr_un.sun_family = AF_LOCAL;
    memcpy(serv_addr_un.sun_path, hostname, path_len + 1);

    if (connect(fd, (struct sockaddr*)&serv_addr_un, sizeof(serv_addr_un)) < 0) {
        WARN("connect() to unix socket %s failed: %s", hostname, strerror(errno));
        return 1;
    }

    return 0;
}
