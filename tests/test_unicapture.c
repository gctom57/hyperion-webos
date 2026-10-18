// unicapture's capture loop with a fake video backend that has a 30 Hz vsync
// (like DILE_VT with framerate divider 2) and a receiver that is slow to take
// frames. A frame that takes longer than one vsync period must not cost an
// extra period: 40 ms per frame should give about 25 fps, not 15.
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "check.h"
#include "unicapture.h"

#define WIDTH 64
#define HEIGHT 36
#define VSYNC_US 33333

static uint8_t pixels[WIDTH * HEIGHT * 4];
static struct timespec next_vsync;
static int send_delay_us = 0;
static int acquired = 0, released = 0;

static void add_us(struct timespec* ts, long us)
{
    ts->tv_nsec += us * 1000;
    while (ts->tv_nsec >= 1000000000) {
        ts->tv_nsec -= 1000000000;
        ts->tv_sec++;
    }
}

static int fake_start(void* state)
{
    (void)state;
    clock_gettime(CLOCK_MONOTONIC, &next_vsync);
    return 0;
}

static int fake_terminate(void* state)
{
    (void)state;
    return 0;
}

// Returns on the next vsync tick, like the driver's wait call
static int fake_wait(void* state)
{
    (void)state;
    add_us(&next_vsync, VSYNC_US);
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_vsync, NULL);
    return 0;
}

static int fake_acquire(void* state, frame_info_t* frame)
{
    (void)state;
    frame->pixel_format = PIXFMT_ARGB;
    frame->width = WIDTH;
    frame->height = HEIGHT;
    frame->planes[0].buffer = pixels;
    frame->planes[0].stride = WIDTH * 4;
    __atomic_add_fetch(&acquired, 1, __ATOMIC_RELAXED);
    return 0;
}

static int fake_release(void* state, frame_info_t* frame)
{
    (void)state;
    (void)frame;
    __atomic_add_fetch(&released, 1, __ATOMIC_RELAXED);
    return 0;
}

static int frames_received = 0;

static int slow_receiver(void* data, int width, int height, uint8_t* rgb)
{
    (void)data;
    if (width == WIDTH && height == HEIGHT && rgb != NULL) {
        frames_received++;
    }
    if (send_delay_us > 0) {
        usleep(send_delay_us);
    }
    return 0;
}

// Captures for `seconds` with the given send delay and returns the frame rate
static double run(unicapture_state_t* u, capture_backend_t* video, int delay_us, double seconds, uint32_t* missed)
{
    send_delay_us = delay_us;
    frames_received = 0;
    unicapture_init(u);
    u->video_capture = video;
    u->target_format = PIXFMT_RGB;
    u->callback = slow_receiver;
    unicapture_start(u);
    // Skip the first frames: backend start and thread creation
    usleep(500000);
    uint32_t start_frames = __atomic_load_n(&u->metrics.frames, __ATOMIC_RELAXED);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    usleep((useconds_t)(seconds * 1000000));
    uint32_t frames = __atomic_load_n(&u->metrics.frames, __ATOMIC_RELAXED) - start_frames;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    *missed = __atomic_load_n(&u->metrics.vsyncs, __ATOMIC_RELAXED) - __atomic_load_n(&u->metrics.wakeups, __ATOMIC_RELAXED);
    unicapture_stop(u);
    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    return frames / elapsed;
}

int main(void)
{
    memset(pixels, 0x80, sizeof(pixels));
    capture_backend_t video = {
        .name = "fake",
        .start = fake_start,
        .terminate = fake_terminate,
        .wait = fake_wait,
        .acquire_frame = fake_acquire,
        .release_frame = fake_release,
    };
    unicapture_state_t u;
    uint32_t missed;

    double fast = run(&u, &video, 2000, 2.0, &missed);
    printf("fast receiver: %.1f fps, %u vsyncs missed\n", fast, missed);
    CHECK(fast > 26 && fast < 32, "fast receiver keeps the 30 Hz vsync rate");
    CHECK(frames_received > 0, "frames reach the receiver callback as RGB");

    double slow = run(&u, &video, 40000, 3.0, &missed);
    printf("40 ms receiver: %.1f fps, %u vsyncs missed\n", slow, missed);
    CHECK(slow > 21, "a frame slower than a vsync period doesn't lose the next vsync (~25 fps, not 15)");
    CHECK(slow < 26.5, "frame rate is bounded by the send time");

    double slower = run(&u, &video, 80000, 3.0, &missed);
    printf("80 ms receiver: %.1f fps, %u vsyncs missed\n", slower, missed);
    CHECK(slower > 11 && slower < 13.5, "80 ms per frame gives ~12.5 fps, not 10");
    CHECK(missed > 0, "vsyncs that pass during a slow frame are counted as missed");

    CHECK(acquired == released, "every acquired frame is released");

    return check_summary();
}
