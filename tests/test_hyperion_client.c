// hyperion_client against an in-process fake receiver: registration, frames,
// idle connections, bad replies, receiver restarts and reconnects. The client
// calls block, but small messages fit in the socket buffers, so one thread
// can play both sides.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "check.h"
#include "hyperion_client.h"

#include "hyperion_reply_builder.h"
#include "hyperion_request_reader.h"
#include "hyperion_request_verifier.h"

#define PRIORITY 150
#define WIDTH 16
#define HEIGHT 9

static int listener = -1;
static int port = 0;
static unsigned char msg_buf[64 * 1024];

static int listen_loopback(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    if (fd < 0 || bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 || listen(fd, 4) != 0
        || getsockname(fd, (struct sockaddr*)&addr, &len) != 0) {
        return -1;
    }
    port = ntohs(addr.sin_port);
    return fd;
}

// Connects the client (the kernel completes it from the backlog) and returns
// the receiver's end, or -1
static int connect_pair(void)
{
    if (hyperion_client("test", "127.0.0.1", port, false, PRIORITY) != 0) {
        return -1;
    }
    return accept(listener, NULL, NULL);
}

static int read_all(int fd, void* buf, size_t size)
{
    size_t done = 0;
    while (done < size) {
        ssize_t n = read(fd, (unsigned char*)buf + done, size - done);
        if (n <= 0) {
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

// Reads one framed request from the client and returns it, verified, or NULL
static hyperionnet_Request_table_t receive_request(int peer)
{
    unsigned char head[4];
    if (read_all(peer, head, sizeof(head)) != 0) {
        return NULL;
    }
    size_t size = ((size_t)head[0] << 24) | ((size_t)head[1] << 16) | ((size_t)head[2] << 8) | head[3];
    if (size == 0 || size > sizeof(msg_buf) || read_all(peer, msg_buf, size) != 0) {
        return NULL;
    }
    if (hyperionnet_Request_verify_as_root(msg_buf, size) != 0) {
        return NULL;
    }
    return hyperionnet_Request_as_root(msg_buf);
}

static void send_raw(int peer, const void* data, size_t size)
{
    if (write(peer, data, size) != (ssize_t)size) {
        perror("write");
    }
}

static void send_framed(int peer, const void* body, size_t size)
{
    unsigned char head[4] = { (unsigned char)(size >> 24), (unsigned char)(size >> 16), (unsigned char)(size >> 8),
        (unsigned char)size };
    send_raw(peer, head, sizeof(head));
    send_raw(peer, body, size);
}

// error == NULL: a success reply carrying registered/video
static void send_reply(int peer, const char* error, int registered, int video)
{
    flatcc_builder_t B;
    flatcc_builder_init(&B);
    hyperionnet_Reply_start_as_root(&B);
    if (error != NULL) {
        hyperionnet_Reply_error_create_str(&B, error);
    }
    hyperionnet_Reply_registered_add(&B, registered);
    hyperionnet_Reply_video_add(&B, video);
    hyperionnet_Reply_end_as_root(&B);
    size_t size;
    void* buf = flatcc_builder_finalize_buffer(&B, &size);
    send_framed(peer, buf, size);
    free(buf);
    flatcc_builder_clear(&B);
}

static int is_register(hyperionnet_Request_table_t req, const char* origin, int priority)
{
    if (req == NULL || hyperionnet_Request_command_type(req) != hyperionnet_Command_Register) {
        return 0;
    }
    hyperionnet_Register_table_t reg = (hyperionnet_Register_table_t)hyperionnet_Request_command(req);
    return strcmp(hyperionnet_Register_origin(reg), origin) == 0 && hyperionnet_Register_priority(reg) == priority;
}

static int is_frame_sized(hyperionnet_Request_table_t req, const unsigned char* frame, int width, int height)
{
    if (req == NULL || hyperionnet_Request_command_type(req) != hyperionnet_Command_Image) {
        return 0;
    }
    hyperionnet_Image_table_t img = (hyperionnet_Image_table_t)hyperionnet_Request_command(req);
    if (hyperionnet_Image_data_type(img) != hyperionnet_ImageType_RawImage) {
        return 0;
    }
    hyperionnet_RawImage_table_t raw = (hyperionnet_RawImage_table_t)hyperionnet_Image_data(img);
    flatbuffers_uint8_vec_t data = hyperionnet_RawImage_data(raw);
    const size_t size = (size_t)width * height * 3;
    return hyperionnet_RawImage_width(raw) == width && hyperionnet_RawImage_height(raw) == height
        && flatbuffers_uint8_vec_len(data) == size && memcmp(data, frame, size) == 0;
}

static int is_frame(hyperionnet_Request_table_t req, const unsigned char* frame)
{
    return is_frame_sized(req, frame, WIDTH, HEIGHT);
}

// Connects and completes the registration handshake; returns the peer or -1
static int registered_pair(const unsigned char* frame)
{
    int peer = connect_pair();
    if (peer < 0) {
        return -1;
    }
    hyperion_set_image(frame, WIDTH, HEIGHT);
    if (!is_register(receive_request(peer), "test", PRIORITY)) {
        close(peer);
        return -1;
    }
    send_reply(peer, NULL, PRIORITY, -1);
    hyperion_read();
    return peer;
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void bad_reply_drops(const unsigned char* frame, const void* data, size_t size, const char* msg)
{
    int peer = registered_pair(frame);
    send_raw(peer, data, size);
    CHECK(peer >= 0 && hyperion_read() < 0, msg);
    hyperion_destroy();
    close(peer);
}

int main(void)
{
    // A failed write must come back as an error, never kill the service
    signal(SIGPIPE, SIG_DFL);

    unsigned char frame[WIDTH * HEIGHT * 3];
    for (size_t i = 0; i < sizeof(frame); i++) {
        frame[i] = (unsigned char)(i * 7);
    }

    listener = listen_loopback();
    CHECK(listener >= 0, "fake receiver listening");

    // Connection failures
    CHECK(hyperion_client("test", "not-an-ip", port, false, PRIORITY) != 0, "hostname that isn't an IPv4 refused");
    char long_path[200];
    memset(long_path, 'a', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';
    CHECK(hyperion_client("test", long_path, 0, true, PRIORITY) != 0, "too long unix socket path refused");
    CHECK(hyperion_set_image(frame, WIDTH, HEIGHT) == 0, "frame without a connection is dropped quietly");
    CHECK(hyperion_read() < 0, "read without a connection reports it");

    // Registration comes first; the frame that triggered it is not sent
    int peer = connect_pair();
    CHECK(peer >= 0, "client connects");
    CHECK(hyperion_set_image(frame, WIDTH, HEIGHT) == 0, "first frame accepted");
    CHECK(is_register(receive_request(peer), "test", PRIORITY), "first message registers origin and priority");
    CHECK(hyperion_set_image(frame, WIDTH, HEIGHT) == 0, "frame before the registered reply accepted");
    CHECK(is_register(receive_request(peer), "test", PRIORITY), "still unregistered: registers again instead of sending");

    // A silent receiver is idle, not gone
    double t0 = now();
    int ret = hyperion_read();
    double waited = now() - t0;
    CHECK(ret == 0, "quiet receiver reads as idle");
    CHECK(waited > 0.5 && waited < 3.0, "idle read returns after the receive timeout");

    send_reply(peer, NULL, PRIORITY + 1, -1);
    CHECK(hyperion_read() == 0, "reply for another priority read");
    hyperion_set_image(frame, WIDTH, HEIGHT);
    CHECK(is_register(receive_request(peer), "test", PRIORITY), "registration for another priority ignored");

    send_reply(peer, NULL, PRIORITY, -1);
    CHECK(hyperion_read() == 0, "registered reply read");
    CHECK(hyperion_set_image(frame, WIDTH, HEIGHT) == 0, "frame sent");
    CHECK(is_frame(receive_request(peer), frame), "frame arrives with its size and pixels");

    // Frames reuse one builder and buffer: nothing may carry over between them
    unsigned char small[8 * 4 * 3];
    for (size_t i = 0; i < sizeof(small); i++) {
        small[i] = (unsigned char)(255 - i);
    }
    CHECK(hyperion_set_image(small, 8, 4) == 0, "smaller frame sent");
    CHECK(is_frame_sized(receive_request(peer), small, 8, 4), "smaller frame arrives intact after a larger one");
    hyperion_set_image(frame, WIDTH, HEIGHT);
    CHECK(is_frame(receive_request(peer), frame), "larger frame arrives intact after a smaller one");

    send_reply(peer, NULL, -1, 2);
    CHECK(hyperion_read() == 0, "video mode reply read");
    send_reply(peer, "Priority out of range", -1, -1);
    CHECK(hyperion_read() == 0, "error reply logged, connection kept");
    hyperion_set_image(frame, WIDTH, HEIGHT);
    CHECK(is_frame(receive_request(peer), frame), "frames continue after an error reply");

    CHECK(hyperion_set_register("test", 120) == 0, "priority change sent");
    CHECK(is_register(receive_request(peer), "test", 120), "priority change re-registers");
    hyperion_set_register("test", PRIORITY);
    receive_request(peer);

    // Receiver restarts: EOF, then a fresh connection must register again
    close(peer);
    CHECK(hyperion_read() < 0, "receiver closing is reported");
    hyperion_destroy();
    peer = connect_pair();
    CHECK(peer >= 0, "client reconnects");
    hyperion_set_image(frame, WIDTH, HEIGHT);
    CHECK(is_register(receive_request(peer), "test", PRIORITY), "reconnected client registers again");
    hyperion_destroy();
    close(peer);

    // Receiver gone while frames are being sent: reset, then closed normally
    // (the send after its RST gets EPIPE, which would raise SIGPIPE)
    for (int graceful = 0; graceful <= 1; graceful++) {
        peer = registered_pair(frame);
        CHECK(peer >= 0, "registered for the receiver-gone test");
        if (!graceful) {
            struct linger lg = { 1, 0 };
            setsockopt(peer, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        }
        close(peer);
        int failed = 0;
        for (int i = 0; i < 50 && !failed; i++) {
            failed = hyperion_set_image(frame, WIDTH, HEIGHT) != 0;
            struct timespec ts = { 0, 10000000 };
            nanosleep(&ts, NULL);
        }
        CHECK(failed, graceful ? "send after the receiver closed fails without SIGPIPE" : "send to a reset connection fails");
        CHECK(hyperion_set_image(frame, WIDTH, HEIGHT) == 0, "later frames are dropped quietly");
        CHECK(hyperion_read() < 0, "failed send makes the connection thread reconnect");
        hyperion_destroy();
    }

    // Receiver alive but stuck: once a send times out half way, the stream is
    // unframed, so the connection must be dropped rather than look idle
    peer = registered_pair(frame);
    CHECK(peer >= 0, "registered for the stalled receiver test");
    size_t big_size = 640 * 360 * 3;
    unsigned char* big = calloc(1, big_size);
    int timed_out = 0;
    for (int i = 0; i < 200 && !timed_out; i++) {
        timed_out = hyperion_set_image(big, 640, 360) != 0;
    }
    free(big);
    CHECK(timed_out, "send to a receiver that stopped reading times out");
    CHECK(hyperion_read() < 0, "timed-out send drops the connection instead of reading as idle");
    hyperion_destroy();
    close(peer);

    // Replies that must drop the connection
    const unsigned char zero_size[] = { 0, 0, 0, 0 };
    bad_reply_drops(frame, zero_size, sizeof(zero_size), "zero-size reply drops the connection");
    // Header and a full body, so a missing size check would overflow the buffer
    static unsigned char huge[4 + 1025] = { 0, 0, 4, 1 };
    bad_reply_drops(frame, huge, sizeof(huge), "reply larger than the buffer drops the connection");
    const unsigned char garbage[] = { 0, 0, 0, 8, 0xff, 0xff, 0xff, 0x7f, 1, 2, 3, 4 };
    bad_reply_drops(frame, garbage, sizeof(garbage), "malformed reply drops the connection");

    peer = registered_pair(frame);
    const unsigned char truncated[] = { 0, 0, 0, 40, 1, 2, 3 };
    send_raw(peer, truncated, sizeof(truncated));
    close(peer);
    CHECK(hyperion_read() < 0, "reply cut short by the receiver closing drops the connection");
    hyperion_destroy();

    // Unix socket receivers
    char path[64];
    snprintf(path, sizeof(path), "/tmp/hwclient-%d.sock", (int)getpid());
    int ufd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un ua;
    memset(&ua, 0, sizeof(ua));
    ua.sun_family = AF_UNIX;
    strncpy(ua.sun_path, path, sizeof(ua.sun_path) - 1);
    unlink(path);
    CHECK(ufd >= 0 && bind(ufd, (struct sockaddr*)&ua, sizeof(ua)) == 0 && listen(ufd, 1) == 0, "unix receiver listening");
    CHECK(hyperion_client("test", path, 0, true, PRIORITY) == 0, "client connects over a unix socket");
    peer = accept(ufd, NULL, NULL);
    hyperion_set_image(frame, WIDTH, HEIGHT);
    CHECK(is_register(receive_request(peer), "test", PRIORITY), "registers over a unix socket");
    hyperion_destroy();
    close(peer);
    close(ufd);
    unlink(path);

    close(listener);
    CHECK(hyperion_client("test", "127.0.0.1", port, false, PRIORITY) != 0, "closed receiver port refused");

    return check_summary();
}
