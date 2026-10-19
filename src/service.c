#include "service.h"
#include "hyperion_client.h"
#include "json_rpc_client.h"
#include "log.h"
#include "pthread.h"
#include "receiver_test.h"
#include "settings.h"
#include "unicapture.h"
#include "utils.h"
#include "version.h"
#include <errno.h>
#include <luna-service2/lunaservice.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// This is a deprecated symbol present in meta-lg-webos-ndk but missing in
// latest buildroot NDK. It is required for proper public service registration
// before webOS 3.5.
//
// SECURITY_COMPATIBILITY flag present in CMakeList disables deprecation notices, see:
// https://github.com/webosose/luna-service2/blob/b74b1859372597fcd6f0f7d9dc3f300acbf6ed6c/include/public/luna-service2/lunaservice.h#L49-L53
extern bool LSRegisterPubPriv(const char* name, LSHandle** sh,
    bool public_bus,
    LSError* lserror) __attribute__((weak));

/*
 * HDR state is pushed to HyperHDR over HTTP, which can take up to the client
 * timeout. Luna callbacks only queue the latest request; a worker thread sends
 * it, so the main loop (and with it every Luna method) never blocks.
 */
static pthread_mutex_t hdr_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hdr_cond = PTHREAD_COND_INITIALIZER;
static bool hdr_thread_started = false;
static bool hdr_pending = false;
static DynamicRange hdr_range = SDR;
static char hdr_host[64];

static void* hdr_worker(void* data __attribute__((unused)))
{
    char host[sizeof(hdr_host)];

    while (true) {
        pthread_mutex_lock(&hdr_lock);
        while (!hdr_pending) {
            pthread_cond_wait(&hdr_cond, &hdr_lock);
        }
        memcpy(host, hdr_host, sizeof(host));
        DynamicRange range = hdr_range;
        hdr_pending = false;
        pthread_mutex_unlock(&hdr_lock);

        int ret = set_hdr_state(host, RPC_PORT, range);
        if (ret != 0) {
            ERR("hdr_worker: set_hdr_state failed, ret: %d", ret);
        }
    }

    return NULL;
}

static void queue_hdr_state(service_t* service, DynamicRange range)
{
    // Nothing to tell HyperHDR until a receiver address is configured
    if (!service->settings->unix_socket && (service->settings->address == NULL || service->settings->address[0] == '\0')) {
        DBG("No receiver address set, not sending HDR state");
        return;
    }

    pthread_mutex_lock(&hdr_lock);

    // Copied here, on the main loop, because setSettings may free the address later
    snprintf(hdr_host, sizeof(hdr_host), "%s", service->settings->unix_socket ? "127.0.0.1" : service->settings->address);
    hdr_range = range;
    hdr_pending = true;

    if (!hdr_thread_started) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, hdr_worker, NULL) == 0) {
            pthread_detach(thread);
            hdr_thread_started = true;
        } else {
            ERR("Unable to start HDR worker thread");
            hdr_pending = false;
        }
    }

    pthread_cond_signal(&hdr_cond);
    pthread_mutex_unlock(&hdr_lock);
}

void* connection_loop(void* data)
{
    service_t* service = (service_t*)data;
    DBG("Starting connection loop");
    while (service->connection_loop_running) {
        INFO("Connecting hyperion-client..");
        if ((hyperion_client("webos", service->settings->address, service->settings->port,
                service->settings->unix_socket, service->settings->priority))
            != 0) {
            ERR("Error! hyperion_client.");
        } else {
            INFO("hyperion-client connected!");
            service->connected = true;
            __atomic_add_fetch(&service->connects, 1, __ATOMIC_RELAXED);
            while (service->connection_loop_running) {
                if (hyperion_read() < 0) {
                    ERR("Receiver connection lost.");
                    break;
                }
            }
            service->connected = false;
        }

        hyperion_destroy();

        if (service->connection_loop_running) {
            INFO("Connection destroyed, waiting...");
            sleep(1);
        }
    }

    INFO("Ending connection loop");
    DBG("Connection loop exiting");
    return 0;
}

/*
 * Live preview for the app: a small copy of the frames being sent, kept only
 * while someone asked for it in the last few seconds (getPreview), and
 * refreshed every 10th frame so it costs next to nothing.
 */
#define PREVIEW_WIDTH 64
#define PREVIEW_HEIGHT 36
#define PREVIEW_KEEP_US (5 * 1000000ULL)

static pthread_mutex_t preview_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t preview_rgb[PREVIEW_WIDTH * PREVIEW_HEIGHT * 3];
static bool preview_valid = false;
static uint64_t preview_wanted_until = 0;
static unsigned int preview_frame = 0;

static bool preview_wanted(void)
{
    uint64_t until = __atomic_load_n(&preview_wanted_until, __ATOMIC_RELAXED);
    return until != 0 && getticks_us() < until && preview_frame++ % 10 == 0;
}

static void preview_from_rgb(const uint8_t* rgb, int width, int height)
{
    pthread_mutex_lock(&preview_lock);
    for (size_t y = 0; y < PREVIEW_HEIGHT; y++) {
        size_t sy = y * (size_t)height / PREVIEW_HEIGHT;
        for (size_t x = 0; x < PREVIEW_WIDTH; x++) {
            size_t sx = x * (size_t)width / PREVIEW_WIDTH;
            memcpy(&preview_rgb[(y * PREVIEW_WIDTH + x) * 3], &rgb[(sy * (size_t)width + sx) * 3], 3);
        }
    }
    preview_valid = true;
    pthread_mutex_unlock(&preview_lock);
}

// NV12 frames: the luma plane is enough for a recognisable thumbnail
static void preview_from_luma(const uint8_t* luma, int width, int height, int stride)
{
    pthread_mutex_lock(&preview_lock);
    for (size_t y = 0; y < PREVIEW_HEIGHT; y++) {
        size_t sy = y * (size_t)height / PREVIEW_HEIGHT;
        for (size_t x = 0; x < PREVIEW_WIDTH; x++) {
            size_t sx = x * (size_t)width / PREVIEW_WIDTH;
            uint8_t v = luma[sy * (size_t)stride + sx];
            memset(&preview_rgb[(y * PREVIEW_WIDTH + x) * 3], v, 3);
        }
    }
    preview_valid = true;
    pthread_mutex_unlock(&preview_lock);
}

static void preview_invalidate(void)
{
    pthread_mutex_lock(&preview_lock);
    preview_valid = false;
    pthread_mutex_unlock(&preview_lock);
}

int service_feed_frame(void* data, int width, int height, uint8_t* rgb_data)
{
    service_t* service = (service_t*)data;
    int ret;
    if (preview_wanted()) {
        preview_from_rgb(rgb_data, width, height);
    }
    if ((ret = hyperion_set_image(rgb_data, width, height)) != 0) {
        __atomic_add_fetch(&service->send_errors, 1, __ATOMIC_RELAXED);
        WARN("Frame sending failed: %d", ret);
    }

    return 0;
}

int service_feed_nv12_frame(void* data, int width, int height, uint8_t* y, uint8_t* uv, int stride_y, int stride_uv)
{
    service_t* service = (service_t*)data;
    int ret;
    if (preview_wanted()) {
        preview_from_luma(y, width, height, stride_y);
    }
    if ((ret = hyperion_set_nv12_image(y, uv, width, height, stride_y, stride_uv)) != 0) {
        __atomic_add_fetch(&service->send_errors, 1, __ATOMIC_RELAXED);
        WARN("Frame sending failed: %d", ret);
    }

    return 0;
}

int service_init(service_t* service, settings_t* settings)
{
    service->settings = settings;
    service->started_us = getticks_us();

    unicapture_init(&service->unicapture);
    service->unicapture.vsync = settings->vsync;
    service->unicapture.fps = settings->fps;
    service->unicapture.target_format = settings->send_nv12 ? PIXFMT_YUV420_SEMI_PLANAR : PIXFMT_RGB;
    service->unicapture.callback = &service_feed_frame;
    service->unicapture.callback_nv12 = &service_feed_nv12_frame;
    service->unicapture.callback_data = (void*)service;
    service->unicapture.dump_frames = settings->dump_frames;

    // Without root the capture libraries can't work, and some crash outright
    // (libgm in libkadaptor on webOS 4). Stay alive instead so the app can
    // ask for our status and elevate us; backends load on the first start.
    if (getuid() != 0) {
        INFO("Not running as root, skipping capture backend detection");
        return 0;
    }

    service_init_backends(service);

    return 0;
}

static void service_backend_config(service_t* service, cap_backend_config_t* config)
{
    config->resolution_width = service->settings->width;
    config->resolution_height = service->settings->height;
    config->fps = service->settings->fps;
    config->quirks = service->settings->quirks;
}

// Loads the configured video backend into service->video_backend and records
// why it failed in service->video_error. Doesn't attach it to unicapture.
static int service_open_video_backend(service_t* service, cap_backend_config_t* config)
{
    settings_t* settings = service->settings;
    const char* const video_backends[] = { "libvtcapture_backend.so", "libdile_vt_backend.so", NULL };
    char backend_name[FILENAME_MAX] = { 0 };
    int ret;

    if (settings->no_video) {
        INFO("Video capture disabled");
        ret = -ENOENT;
    } else if (!settings_backend_allowed(settings->video_backend, settings_video_backends)) {
        ERR("Refusing unknown video backend: %s", settings->video_backend);
        ret = -EINVAL;
    } else if (settings->video_backend == NULL || settings->video_backend[0] == '\0' || strcmp(settings->video_backend, "auto") == 0) {
        INFO("Autodetecting video backend...");
        ret = unicapture_try_backends(config, &service->video_backend, video_backends);
    } else {
        snprintf(backend_name, sizeof(backend_name), "%s_backend.so", settings->video_backend);
        ret = unicapture_init_backend(config, &service->video_backend, backend_name);
    }

    service->video_backend_initialized = ret == 0;
    service->video_error = ret;
    return ret;
}

void service_init_backends(service_t* service)
{
    settings_t* settings = service->settings;

    cap_backend_config_t config;
    service_backend_config(service, &config);

    const char* const ui_backends[] = { "libgm_backend.so", "libhalgal_backend.so", NULL };
    char backend_name[FILENAME_MAX] = { 0 };

    // unicapture_init() clears these pointers, so re-attach backends that are
    // still loaded from an earlier init
    if (service->ui_backend_initialized) {
        service->unicapture.ui_capture = &service->ui_backend;
    } else {
        service->unicapture.ui_capture = NULL;

        if (settings->no_gui) {
            INFO("UI capture disabled");
        } else if (!settings_backend_allowed(settings->ui_backend, settings_ui_backends)) {
            // The name ends up in dlopen(), so never pass through anything unknown
            ERR("Refusing unknown UI backend: %s", settings->ui_backend);
        } else {
            if (settings->ui_backend == NULL || settings->ui_backend[0] == '\0' || strcmp(settings->ui_backend, "auto") == 0) {
                INFO("Autodetecting UI backend...");
                if (unicapture_try_backends(&config, &service->ui_backend, ui_backends) == 0) {
                    service->unicapture.ui_capture = &service->ui_backend;
                    service->ui_backend_initialized = true;
                }
            } else {
                snprintf(backend_name, sizeof(backend_name), "%s_backend.so", settings->ui_backend);
                if (unicapture_init_backend(&config, &service->ui_backend, backend_name) == 0) {
                    service->unicapture.ui_capture = &service->ui_backend;
                    service->ui_backend_initialized = true;
                }
            }
        }
    }

    if (service->video_backend_initialized) {
        service->unicapture.video_capture = &service->video_backend;
    } else {
        service->unicapture.video_capture = NULL;
        if (service_open_video_backend(service, &config) == 0) {
            service->unicapture.video_capture = &service->video_backend;
        }
    }
}

/*
 * The video backend can fail to load while the UI one works, e.g. while
 * another process holds VT capture. While capture runs, try again now and
 * then and hand a backend that loads to the running capture loop, instead
 * of staying without video until the next restart.
 */
static gboolean service_retry_video(gpointer data)
{
    service_t* service = (service_t*)data;
    if (!service->running || service->video_backend_initialized || service->settings->no_video || getuid() != 0) {
        return G_SOURCE_CONTINUE;
    }

    INFO("Video capture isn't available, trying again...");
    cap_backend_config_t config;
    service_backend_config(service, &config);
    if (service_open_video_backend(service, &config) == 0) {
        INFO("Video backend loaded, attaching it to the running capture");
        __atomic_store_n(&service->unicapture.video_capture, &service->video_backend, __ATOMIC_RELEASE);
    }
    return G_SOURCE_CONTINUE;
}

static const char* service_video_error_text(service_t* service)
{
    if (service->video_backend_initialized) {
        return NULL;
    }
    if (getuid() != 0) {
        return "Needs root";
    }
    switch (service->video_error) {
    case 0:
        return "Not loaded yet";
    case -ENOENT:
        return "Disabled in settings";
    case -EINVAL:
        return "Unknown backend in settings";
    case -EBUSY:
        return "Held by another process";
    case -EIO:
        // Busy (another app) or stuck (needs a reboot) look the same from here
        return "Capture driver busy or refused, retrying";
    default:
        return "No video backend could be opened";
    }
}

void service_destroy_backends(service_t* service)
{
    if (service->ui_backend_initialized && service->ui_backend.cleanup) {
        DBG("Cleaning up UI backend...");
        DBG("Result: %d", service->ui_backend.cleanup(service->ui_backend.state));
        service->ui_backend_initialized = false;
    }
    free(service->ui_backend.name);
    service->ui_backend.name = NULL;
    service->unicapture.ui_capture = NULL;

    if (service->video_backend_initialized && service->video_backend.cleanup) {
        DBG("Cleaning up video backend...");
        DBG("Result: %d", service->video_backend.cleanup(service->video_backend.state));
        service->video_backend_initialized = false;
    }
    free(service->video_backend.name);
    service->video_backend.name = NULL;
    service->unicapture.video_capture = NULL;
}

int service_destroy(service_t* service)
{
    service_stop(service);
    // Backends are loaded at init even when capture never started
    service_destroy_backends(service);
    return 0;
}

int service_start(service_t* service)
{
    if (service->running) {
        return 1;
    }

    if (getuid() != 0) {
        ERR("Not running as root, refusing to start capture");
        return -2;
    }

    service_init_backends(service);

    service->running = true;
    int res = unicapture_start(&service->unicapture);

    if (res != 0) {
        service->running = false;
        return res;
    }

    service->connection_loop_running = true;
    if (pthread_create(&service->connection_thread, NULL, connection_loop, service) != 0) {
        unicapture_stop(&service->unicapture);
        service->connection_loop_running = false;
        service->running = false;
        return -1;
    }
    return 0;
}

int service_stop(service_t* service)
{
    if (!service->running) {
        return 1;
    }

    service->connection_loop_running = false;
    unicapture_stop(&service->unicapture);
    pthread_join(service->connection_thread, NULL);
    service->running = false;
    preview_invalidate();

    // Backends stay loaded: on some TVs (seen on webOS 4) DILE_VT can't be
    // created again after DILE_VT_Destroy, so stop/start and power pauses
    // reuse the open handle. service_destroy() releases them on exit.

    return 0;
}

static bool same_string(const char* a, const char* b)
{
    return strcmp(a ? a : "", b ? b : "") == 0;
}

/*
 * start/stop/setSettings change what the TV does, and the service's role
 * accepts calls from everyone ("inbound": ["*"] after elevation). Only let
 * the PicCap app and command-line callers through:
 * - The PicCap app: its application ID, which the app manager sets for web
 *   apps, so another installed app can't claim it.
 * - Callers without any identity: luna-send from a shell, as used by the
 *   autostart script, Homebrew Channel exec and tools like Glasshouse. That
 *   already needs root, and root can do anything anyway (luna-send -a can
 *   claim any app ID), so this is no protection against root.
 * Anything else, e.g. another installed app, is refused. status and
 * getSettings stay open.
 */
#define PICCAP_APP_ID "org.webosbrew.piccap"

static bool service_caller_allowed(LSHandle* sh, LSMessage* msg)
{
    const char* app_id = LSMessageGetApplicationID(msg);
    const char* service_name = LSMessageGetSenderServiceName(msg);

    if (app_id != NULL && strcmp(app_id, PICCAP_APP_ID) == 0) {
        return true;
    }
    if (app_id == NULL && service_name == NULL) {
        return true;
    }

    WARN("Refusing %s from app %s / service %s", LSMessageGetMethod(msg),
        app_id ? app_id : "(none)", service_name ? service_name : "(none)");

    LSError lserror;
    LSErrorInit(&lserror);
    jvalue_ref jobj = jobject_create();
    jobject_put(jobj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(false));
    jobject_put(jobj, J_CSTR_TO_JVAL("errorText"), jstring_create("Not allowed for this caller"));
    LSMessageReply(sh, msg, jvalue_tostring_simple(jobj), &lserror);
    j_release(&jobj);
    LSErrorFree(&lserror);
    return false;
}

bool service_method_start(LSHandle* sh, LSMessage* msg, void* data)
{
    service_t* service = (service_t*)data;
    if (!service_caller_allowed(sh, msg)) {
        return true;
    }

    LSError lserror;
    LSErrorInit(&lserror);

    jvalue_ref jobj = jobject_create();

    jobject_put(jobj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(service_start(service) == 0));

    LSMessageReply(sh, msg, jvalue_tostring_simple(jobj), &lserror);

    j_release(&jobj);
    // true = handled; false made luna-service2 log "Method wasn't handled"
    return true;
}

bool service_method_stop(LSHandle* sh, LSMessage* msg, void* data)
{
    service_t* service = (service_t*)data;
    if (!service_caller_allowed(sh, msg)) {
        return true;
    }

    LSError lserror;
    LSErrorInit(&lserror);

    jvalue_ref jobj = jobject_create();

    jobject_put(jobj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(service_stop(service) == 0));

    LSMessageReply(sh, msg, jvalue_tostring_simple(jobj), &lserror);

    j_release(&jobj);
    // true = handled; false made luna-service2 log "Method wasn't handled"
    return true;
}

#define LOAD32(x) jnumber_create_i64(__atomic_load_n(&(x), __ATOMIC_RELAXED))

// Counters for spotting stalls and slow stages without debug logs. Capture
// totals restart with each start; timings are per frame, averaged over the
// last 60 frames. vsyncMissed counts vsync signals the capture thread was
// too busy to see, so a high value means frames are lost to slow stages.
static jvalue_ref service_health_json(service_t* service)
{
    unicapture_state_t* u = &service->unicapture;
    jvalue_ref health = jobject_create();
    jobject_put(health, J_CSTR_TO_JVAL("uptime"), jnumber_create_i64((int64_t)((getticks_us() - service->started_us) / 1000000)));
    jobject_put(health, J_CSTR_TO_JVAL("connects"), LOAD32(service->connects));
    jobject_put(health, J_CSTR_TO_JVAL("sendErrors"), LOAD32(service->send_errors));
    jobject_put(health, J_CSTR_TO_JVAL("frames"), LOAD32(u->metrics.frames));
    uint32_t vsyncs = __atomic_load_n(&u->metrics.vsyncs, __ATOMIC_RELAXED);
    uint32_t wakeups = __atomic_load_n(&u->metrics.wakeups, __ATOMIC_RELAXED);
    jobject_put(health, J_CSTR_TO_JVAL("vsyncMissed"), jnumber_create_i64(vsyncs > wakeups ? vsyncs - wakeups : 0));
    jobject_put(health, J_CSTR_TO_JVAL("videoRestarts"), LOAD32(u->metrics.video_restarts));

    jvalue_ref timing = jobject_create();
    jobject_put(timing, J_CSTR_TO_JVAL("waitUs"), LOAD32(u->metrics.wait_us));
    jobject_put(timing, J_CSTR_TO_JVAL("acquireUs"), LOAD32(u->metrics.acquire_us));
    jobject_put(timing, J_CSTR_TO_JVAL("copyUs"), LOAD32(u->metrics.copy_us));
    jobject_put(timing, J_CSTR_TO_JVAL("convertUs"), LOAD32(u->metrics.convert_us));
    jobject_put(timing, J_CSTR_TO_JVAL("processUs"), LOAD32(u->metrics.process_us));
    jobject_put(timing, J_CSTR_TO_JVAL("sendUs"), LOAD32(u->metrics.send_us));
    jobject_put(health, J_CSTR_TO_JVAL("timing"), timing);
    jvalue_ref exp = jobject_create();
    jobject_put(exp, J_CSTR_TO_JVAL("uiAcquireUs"), jnumber_create_i64(u->metrics.exp_ui_acquire_us));
    jobject_put(exp, J_CSTR_TO_JVAL("uiCopyMemcpyUs"), jnumber_create_i64(u->metrics.exp_ui_copy_us[0]));
    jobject_put(exp, J_CSTR_TO_JVAL("uiCopyNeonUs"), jnumber_create_i64(u->metrics.exp_ui_copy_us[1]));
    jobject_put(exp, J_CSTR_TO_JVAL("videoCopyMemcpyUs"), jnumber_create_i64(u->metrics.exp_video_copy_us[0]));
    jobject_put(exp, J_CSTR_TO_JVAL("videoCopyNeonUs"), jnumber_create_i64(u->metrics.exp_video_copy_us[1]));
    jobject_put(health, J_CSTR_TO_JVAL("exp"), exp);
    return health;
}

bool service_method_status(LSHandle* sh, LSMessage* msg, void* data)
{
    service_t* service = (service_t*)data;
    jvalue_ref jobj = jobject_create();
    LSError lserror;
    LSErrorInit(&lserror);

    jobject_put(jobj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
    jobject_put(jobj, J_CSTR_TO_JVAL("elevated"), jboolean_create(getuid() == 0));
    jobject_put(jobj, J_CSTR_TO_JVAL("version"), jstring_create(HYPERION_WEBOS_VERSION));
    jobject_put(jobj, J_CSTR_TO_JVAL("isRunning"), jboolean_create(service->running));
    jobject_put(jobj, J_CSTR_TO_JVAL("connected"), jboolean_create(service->connected));
    jobject_put(jobj, J_CSTR_TO_JVAL("videoBackend"), service->video_backend.name ? jstring_create(service->video_backend.name) : jnull());
    jobject_put(jobj, J_CSTR_TO_JVAL("videoRunning"), jboolean_create(service->unicapture.video_capture_running));
    const char* video_error = service_video_error_text(service);
    jobject_put(jobj, J_CSTR_TO_JVAL("videoError"), video_error ? jstring_create(video_error) : jnull());
    jobject_put(jobj, J_CSTR_TO_JVAL("uiBackend"), service->ui_backend.name ? jstring_create(service->ui_backend.name) : jnull());
    jobject_put(jobj, J_CSTR_TO_JVAL("uiRunning"), jboolean_create(service->unicapture.ui_capture_running));
    jobject_put(jobj, J_CSTR_TO_JVAL("framerate"), jnumber_create_f64(service->unicapture.metrics.framerate));
    jobject_put(jobj, J_CSTR_TO_JVAL("health"), service_health_json(service));

    LSMessageReply(sh, msg, jvalue_tostring_simple(jobj), &lserror);

    j_release(&jobj);

    return true;
}

bool service_method_get_settings(LSHandle* sh, LSMessage* msg, void* data)
{
    service_t* service = (service_t*)data;
    LSError lserror;
    LSErrorInit(&lserror);

    jvalue_ref jobj = jobject_create();

    jobject_put(jobj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
    settings_save_json(service->settings, jobj);

    LSMessageReply(sh, msg, jvalue_tostring_simple(jobj), &lserror);

    j_release(&jobj);

    return true;
}

bool service_method_set_settings(LSHandle* sh, LSMessage* msg, void* data)
{
    service_t* service = (service_t*)data;
    if (!service_caller_allowed(sh, msg)) {
        return true;
    }
    LSError lserror;
    LSErrorInit(&lserror);

    JSchemaInfo schema;
    jvalue_ref parsed;

    jschema_info_init(&schema, jschema_all(), NULL, NULL);
    parsed = jdom_parse(j_cstr_to_buffer(LSMessageGetPayload(msg)), DOMOPT_NOOPT, &schema);

    if (jis_null(parsed) || !jis_object(parsed)) {
        j_release(&parsed);
        jvalue_ref jerr = jobject_create();
        jobject_put(jerr, J_CSTR_TO_JVAL("returnValue"), jboolean_create(false));
        jobject_put(jerr, J_CSTR_TO_JVAL("errorText"), jstring_create("Invalid settings payload"));
        LSMessageReply(sh, msg, jvalue_tostring_simple(jerr), &lserror);
        j_release(&jerr);
        return true;
    }

    // Stop capture and the connection thread before touching settings: they
    // read settings->address and friends, which settings_load_json() frees.
    bool was_running = service->running;
    service_stop(service);

    // Remember what the backends were set up with: they are only reloaded
    // when one of these changes (reloading DILE_VT breaks it on some TVs)
    settings_t* s = service->settings;
    char* old_video_backend = s->video_backend ? strdup(s->video_backend) : NULL;
    char* old_ui_backend = s->ui_backend ? strdup(s->ui_backend) : NULL;
    const int old_width = s->width, old_height = s->height, old_fps = s->fps, old_quirks = s->quirks;
    const bool old_no_video = s->no_video, old_no_gui = s->no_gui;

    int res = settings_load_json(service->settings, parsed);

    if (res == 0
        && (s->width != old_width || s->height != old_height || s->fps != old_fps || s->quirks != old_quirks
            || s->no_video != old_no_video || s->no_gui != old_no_gui
            || !same_string(s->video_backend, old_video_backend) || !same_string(s->ui_backend, old_ui_backend))) {
        INFO("Capture settings changed, reloading backends");
        service_destroy_backends(service);
    }
    free(old_video_backend);
    free(old_ui_backend);
    if (res == 0) {
        if (settings_save_file(service->settings, SETTINGS_PERSISTENCE_PATH) != 0) {
            WARN("Settings save failed");
        }

        const char* startup_directory = "/var/lib/webosbrew/init.d";
        const char* startup_symlink = "/var/lib/webosbrew/init.d/piccapautostart";
        const char* startup_script = "/media/developer/apps/usr/palm/services/org.webosbrew.piccap.service/piccapautostart";

        if (unlink(startup_symlink) != 0 && errno != ENOENT) {
            WARN("Startup symlink removal failed: %s", strerror(errno));
        }

        if (service->settings->autostart) {
            mkdir(startup_directory, 0755);
            if (symlink(startup_script, startup_symlink) != 0) {
                WARN("Startup symlink creation failed: %s", strerror(errno));
            }
        }
    }

    service_init(service, service->settings);

    if (was_running) {
        service_start(service);
    }

    jvalue_ref jobj = jobject_create();
    jobject_put(jobj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(res == 0));

    LSMessageReply(sh, msg, jvalue_tostring_simple(jobj), &lserror);

    j_release(&parsed);
    j_release(&jobj);

    return true;
}

/*
 * testReceiver can take seconds, so it runs in a worker thread and
 * replies from the main loop, which keeps serving other calls meanwhile.
 */
typedef struct {
    LSHandle* sh;
    LSMessage* msg;
    jvalue_ref reply;
    char address[64];
    int port;
} async_call_t;

static gboolean async_call_reply(gpointer data)
{
    async_call_t* call = (async_call_t*)data;
    LSError lserror;
    LSErrorInit(&lserror);
    LSMessageReply(call->sh, call->msg, jvalue_tostring_simple(call->reply), &lserror);
    LSErrorFree(&lserror);
    LSMessageUnref(call->msg);
    j_release(&call->reply);
    free(call);
    return G_SOURCE_REMOVE;
}

static void reply_error(LSHandle* sh, LSMessage* msg, const char* text)
{
    LSError lserror;
    LSErrorInit(&lserror);
    jvalue_ref jobj = jobject_create();
    jobject_put(jobj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(false));
    jobject_put(jobj, J_CSTR_TO_JVAL("errorText"), jstring_create(text));
    LSMessageReply(sh, msg, jvalue_tostring_simple(jobj), &lserror);
    j_release(&jobj);
    LSErrorFree(&lserror);
}

static bool start_async(LSHandle* sh, LSMessage* msg, async_call_t* call, void* (*worker)(void*))
{
    call->sh = sh;
    call->msg = msg;
    LSMessageRef(msg);
    pthread_t thread;
    if (pthread_create(&thread, NULL, worker, call) != 0) {
        LSMessageUnref(msg);
        free(call);
        reply_error(sh, msg, "Unable to start worker thread");
        return false;
    }
    pthread_detach(thread);
    return true;
}

static void* test_receiver_worker(void* data)
{
    async_call_t* call = (async_call_t*)data;
    char err[96];
    int ret = receiver_test_tcp(call->address, call->port, 2000, err, sizeof(err));

    call->reply = jobject_create();
    jobject_put(call->reply, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
    jobject_put(call->reply, J_CSTR_TO_JVAL("reachable"), jboolean_create(ret == 0));
    if (ret != 0) {
        jobject_put(call->reply, J_CSTR_TO_JVAL("errorText"), jstring_create(err));
    } else {
        // Optional: which receiver it is, from its JSON-RPC web port
        AmbientLightingDaemon flavor = DAEMON_NOT_SET;
        if (get_daemon_flavor(call->address, RPC_PORT, &flavor) == 0 && flavor > DAEMON_NOT_SET) {
            jobject_put(call->reply, J_CSTR_TO_JVAL("flavor"), jstring_create(daemon_to_string(flavor)));
        }
    }
    g_idle_add(async_call_reply, call);
    return NULL;
}

bool service_method_test_receiver(LSHandle* sh, LSMessage* msg, void* data __attribute__((unused)))
{
    if (!service_caller_allowed(sh, msg)) {
        return true;
    }

    JSchemaInfo schema;
    jschema_info_init(&schema, jschema_all(), NULL, NULL);
    jvalue_ref parsed = jdom_parse(j_cstr_to_buffer(LSMessageGetPayload(msg)), DOMOPT_NOOPT, &schema);
    jvalue_ref address = jobject_get(parsed, j_cstr_to_buffer("address"));
    jvalue_ref port = jobject_get(parsed, j_cstr_to_buffer("port"));
    if (jis_null(parsed) || !jis_string(address)) {
        j_release(&parsed);
        reply_error(sh, msg, "address missing");
        return true;
    }

    async_call_t* call = calloc(1, sizeof(async_call_t));
    if (call == NULL) {
        j_release(&parsed);
        reply_error(sh, msg, "Out of memory");
        return true;
    }
    raw_buffer str = jstring_get(address);
    snprintf(call->address, sizeof(call->address), "%s", str.m_str);
    jstring_free_buffer(str);
    call->port = 19400;
    if (jis_number(port)) {
        jnumber_get_i32(port, &call->port);
    }
    j_release(&parsed);

    if (!settings_address_valid(call->address, false)) {
        free(call);
        reply_error(sh, msg, "address must be an IPv4 address");
        return true;
    }
    start_async(sh, msg, call, test_receiver_worker);
    return true;
}

bool service_method_get_preview(LSHandle* sh, LSMessage* msg, void* data __attribute__((unused)))
{
    // The preview shows what is on screen: same callers as start/stop
    if (!service_caller_allowed(sh, msg)) {
        return true;
    }
    __atomic_store_n(&preview_wanted_until, getticks_us() + PREVIEW_KEEP_US, __ATOMIC_RELAXED);

    jvalue_ref jobj = jobject_create();
    jobject_put(jobj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
    pthread_mutex_lock(&preview_lock);
    jobject_put(jobj, J_CSTR_TO_JVAL("available"), jboolean_create(preview_valid));
    if (preview_valid) {
        gchar* b64 = g_base64_encode(preview_rgb, sizeof(preview_rgb));
        jobject_put(jobj, J_CSTR_TO_JVAL("width"), jnumber_create_i32(PREVIEW_WIDTH));
        jobject_put(jobj, J_CSTR_TO_JVAL("height"), jnumber_create_i32(PREVIEW_HEIGHT));
        jobject_put(jobj, J_CSTR_TO_JVAL("rgb"), jstring_create(b64));
        g_free(b64);
    }
    pthread_mutex_unlock(&preview_lock);

    LSError lserror;
    LSErrorInit(&lserror);
    LSMessageReply(sh, msg, jvalue_tostring_simple(jobj), &lserror);
    j_release(&jobj);
    LSErrorFree(&lserror);
    return true;
}

LSMethod methods[] = {
    { "start", service_method_start, LUNA_METHOD_FLAGS_NONE },
    { "stop", service_method_stop, LUNA_METHOD_FLAGS_NONE },
    { "status", service_method_status, LUNA_METHOD_FLAGS_NONE },
    { "getSettings", service_method_get_settings, LUNA_METHOD_FLAGS_NONE },
    { "setSettings", service_method_set_settings, LUNA_METHOD_FLAGS_NONE },
    { "testReceiver", service_method_test_receiver, LUNA_METHOD_FLAGS_NONE },
    { "getPreview", service_method_get_preview, LUNA_METHOD_FLAGS_NONE },
    { 0, 0, 0 }
};

static bool power_callback(LSHandle* sh __attribute__((unused)), LSMessage* msg, void* data)
{
    JSchemaInfo schema;
    jvalue_ref parsed;
    service_t* service = (service_t*)data;

    if (service->settings->no_powerstate) {
        return false;
    }

    INFO("Power status callback message: %s", LSMessageGetPayload(msg));

    jschema_info_init(&schema, jschema_all(), NULL, NULL);
    parsed = jdom_parse(j_cstr_to_buffer(LSMessageGetPayload(msg)), DOMOPT_NOOPT, &schema);

    // Parsing failed
    if (jis_null(parsed)) {
        j_release(&parsed);
        return true;
    }

    jvalue_ref state_ref = jobject_get(parsed, j_cstr_to_buffer("state"));
    if (!jis_valid(state_ref)) {
        DBG("power_callback: luna-reply does not contain 'state'");
        j_release(&parsed);
        return true;
    }

    raw_buffer state_buf = jstring_get(state_ref);
    const char* state_str = state_buf.m_str;
    bool target_state = strcmp(state_str, "Active") == 0;

    if (!service->running && target_state && service->power_paused) {
        INFO("Resuming after power pause...");
        service->power_paused = false;
        service_start(service);
    }

    if (service->running && !target_state && !service->power_paused) {
        INFO("Pausing due to power event...");
        service->power_paused = true;
        service_stop(service);
    }

    jstring_free_buffer(state_buf);
    j_release(&parsed);

    return true;
}

static bool videooutput_callback(LSHandle* sh __attribute__((unused)), LSMessage* msg, void* data)
{
    JSchemaInfo schema;
    jvalue_ref parsed;
    service_t* service = (service_t*)data;

    if (service->settings->no_hdr) {
        return false;
    }

    // INFO("Videooutput status callback message: %s", LSMessageGetPayload(msg));

    jschema_info_init(&schema, jschema_all(), NULL, NULL);
    parsed = jdom_parse(j_cstr_to_buffer(LSMessageGetPayload(msg)), DOMOPT_NOOPT, &schema);

    // Parsing failed
    if (jis_null(parsed)) {
        j_release(&parsed);
        return false; // was true; why?
    }

    // Get to the information we want (hdrType)
    jvalue_ref video_ref = jobject_get(parsed, j_cstr_to_buffer("video"));
    if (jis_null(video_ref) || !jis_valid(video_ref)) {
        j_release(&parsed);
        return false;
    }
    jvalue_ref video_0_ref = jarray_get(video_ref, 0); // should always be index 0 = main screen ?!
    if (jis_null(video_0_ref) || !jis_valid(video_0_ref)) {
        j_release(&parsed);
        return false;
    }
    jvalue_ref video_info_ref = jobject_get(video_0_ref, j_cstr_to_buffer("videoInfo"));
    if (jis_null(video_info_ref) || !jis_valid(video_info_ref)) {
        j_release(&parsed);
        return false;
    }
    jvalue_ref hdr_type_ref = jobject_get(video_info_ref, j_cstr_to_buffer("hdrType"));
    if (jis_null(hdr_type_ref) || !jis_valid(hdr_type_ref) || !jis_string(hdr_type_ref)) {
        j_release(&parsed);
        return false;
    }

    raw_buffer hdr_type_buf = jstring_get(hdr_type_ref);
    const char* hdr_type_str = hdr_type_buf.m_str;
    INFO("videooutput_callback: hdrType: %s", hdr_type_str);

    queue_hdr_state(service, get_dynamic_range(hdr_type_str));

    jstring_free_buffer(hdr_type_buf);
    j_release(&parsed);

    return true;
}

static bool picture_callback(LSHandle* sh __attribute__((unused)), LSMessage* msg, void* data)
{
    JSchemaInfo schema;
    jvalue_ref parsed;
    service_t* service = (service_t*)data;

    if (service->settings->no_hdr) {
        return false;
    }

    // INFO("getSystemSettings/picture status callback message: %s", LSMessageGetPayload(msg));

    jschema_info_init(&schema, jschema_all(), NULL, NULL);
    parsed = jdom_parse(j_cstr_to_buffer(LSMessageGetPayload(msg)), DOMOPT_NOOPT, &schema);

    // Parsing failed
    if (jis_null(parsed)) {
        j_release(&parsed);
        return false; // was true; why?
    }

    // Get to the information we want (dynamicRange)
    jvalue_ref dimension_ref = jobject_get(parsed, j_cstr_to_buffer("dimension"));
    if (jis_null(dimension_ref) || !jis_valid(dimension_ref)) {
        j_release(&parsed);
        return false;
    }
    jvalue_ref dynamic_range_ref = jobject_get(dimension_ref, j_cstr_to_buffer("dynamicRange"));
    if (jis_null(dynamic_range_ref) || !jis_valid(dynamic_range_ref) || !jis_string(dynamic_range_ref)) {
        j_release(&parsed);
        return false;
    }

    raw_buffer dynamic_range_buf = jstring_get(dynamic_range_ref);
    const char* dynamic_range_str = dynamic_range_buf.m_str;
    INFO("picture_callback: dynamicRange: %s", dynamic_range_str);

    queue_hdr_state(service, get_dynamic_range(dynamic_range_str));

    jstring_free_buffer(dynamic_range_buf);
    j_release(&parsed);

    return true;
}

int service_register(service_t* service, GMainLoop* loop)
{
    LSHandle* handle = NULL;
    LSHandle* handlelegacy = NULL;
    LSError lserror;

    LSErrorInit(&lserror);

    DBG("Try register with LSRegister");
    bool registered = LSRegister(SERVICE_NAME, &handle, &lserror);

    bool registeredLegacy = false;

    if (&LSRegisterPubPriv != NULL) {
        DBG("Try legacy register with LSRegisterPubPriv");
        registeredLegacy = LSRegisterPubPriv(SERVICE_NAME, &handlelegacy, true, &lserror);
    }

    if (!registered && !registeredLegacy) {
        ERR("Unable to register on Luna bus: %s", lserror.message);
        LSErrorFree(&lserror);
        return -1;
    }

    LSRegisterCategory(handle, "/", methods, NULL, NULL, &lserror);
    LSCategorySetData(handle, "/", service, &lserror);

    LSGmainAttach(handle, loop, &lserror);

    g_timeout_add_seconds(15, service_retry_video, service);

    if (!LSCall(handle, "luna://com.webos.service.tvpower/power/getPowerState", "{\"subscribe\":true}", power_callback, (void*)service, NULL, &lserror)) {
        WARN("Power state monitoring call failed: %s", lserror.message);
    }

    if (!LSCall(handle, "luna://com.webos.service.videooutput/getStatus", "{\"subscribe\":true}", videooutput_callback, (void*)service, NULL, &lserror)) {
        WARN("videooutput/getStatus call failed: %s", lserror.message);
    }

    if (!LSCall(handle, "luna://com.webos.settingsservice/getSystemSettings", "{\"category\":\"picture\",\"subscribe\":true}", picture_callback, (void*)service, NULL, &lserror)) {
        WARN("settingsservice/getSystemSettings call failed: %s", lserror.message);
    }

    if (registeredLegacy) {
        LSRegisterCategory(handlelegacy, "/", methods, NULL, NULL, &lserror);
        LSCategorySetData(handlelegacy, "/", service, &lserror);
        LSGmainAttach(handlelegacy, loop, &lserror);

        if (!LSCall(handlelegacy, "luna://com.webos.service.tvpower/power/getPowerState", "{\"subscribe\":true}", power_callback, (void*)service, NULL, &lserror)) {
            WARN("Power state monitoring call failed: %s", lserror.message);
        }

        if (!LSCall(handlelegacy, "luna://com.webos.service.videooutput/getStatus", "{\"subscribe\":true}", videooutput_callback, (void*)service, NULL, &lserror)) {
            WARN("videooutput/getStatus call failed: %s", lserror.message);
        }

        if (!LSCall(handlelegacy, "luna://com.webos.settingsservice/getSystemSettings", "{\"category\":\"picture\",\"subscribe\":true}", picture_callback, (void*)service, NULL, &lserror)) {
            WARN("settingsservice/getSystemSettings call failed: %s", lserror.message);
        }
    }

    LSErrorFree(&lserror);
    return 0;
}
