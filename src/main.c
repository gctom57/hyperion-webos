#include <errno.h>
#include <getopt.h>
#include <glib-unix.h>
#include <glib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "service.h"
#include "settings.h"
#include "version.h"

GMainLoop* loop = NULL;

settings_t settings;
service_t service;
bool using_cli = false;
int print_version = 0;

// Runs from the main loop (g_unix_signal_add), not in signal context
static gboolean quit_handler(gpointer user_data)
{
    INFO("%s detected, stopping...", (const char*)user_data);
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static const struct option long_options[] = {
    { "width", required_argument, 0, 'x' },
    { "height", required_argument, 0, 'y' },
    { "address", required_argument, 0, 'a' },
    { "port", required_argument, 0, 'p' },
    { "unix-socket", no_argument, 0, 'l' },
    { "fps", required_argument, 0, 'f' },
    { "no-video", no_argument, 0, 'V' },
    { "no-gui", no_argument, 0, 'G' },
    { "no-vsync", no_argument, 0, 'n' },
    { "no-hdr", no_argument, 0, 'r' },
    { "no-powerstate", no_argument, 0, 's' },
    { "backend", required_argument, 0, 'b' },
    { "ui-backend", required_argument, 0, 'u' },
    { "quirks", required_argument, 0, 'q' },
    { "help", no_argument, 0, 'h' },
    { "verbose", no_argument, 0, 'v' },
    { "version", no_argument, &print_version, 1 },
    { "config", required_argument, 0, 'c' },
    { "dump-frames", no_argument, 0, 'd' },
    { "nv12", no_argument, 0, 't' },
    { 0, 0, 0, 0 },
};

static void print_usage()
{
    printf("Usage: hyperion-webos -a ADDRESS -S [OPTION]...\n");
    printf("\n");
    printf("Grab screen content continously and send to Hyperion via flatbuffers server.\n");
    printf("Application has to be named to hyperion-webos to avoid bugs!\n");
    printf("\n");
    printf("  -x, --width=WIDTH     Width of video frame (default 192)\n");
    printf("  -y, --height=HEIGHT   Height of video frame (default 108)\n");
    printf("  -a, --address=ADDR    IP address of Hyperion server\n");
    printf("  -p, --port=PORT       Port of Hyperion flatbuffers server (default 19400)\n");
    printf("  -l, --unix-socket     Connect through unix socket\n");
    printf("  -f, --fps=FPS         Framerate for sending video frames (default 0 = unlimited)\n");
    printf("  -b, --backend=BE      Use specific video capture backend (default auto)\n");
    printf("  -u, --ui-backend=BE   Use specific ui capture backend (default auto)\n");
    printf("  -V, --no-video        Video will not be captured\n");
    printf("  -G, --no-gui          GUI/UI will not be captured\n");
    printf("  -n, --no-vsync        Disable vsync (may increase framerate at the cost of tearing/artifacts)\n");
    printf("  -r, --no-hdr          Disable automatic HDR mode switching\n");
    printf("  -s, --no-powerstate   Disable automatic powerstate switching\n");
    printf("  -q, --quirks=QUIRKS   Enable certain handling for per-device quirks\n");
    printf("  -c, --config=PATH     Absolute path for configfile to load settings. Giving additional runtime arguments will overwrite loaded ones.\n");
    printf("  -d, --dump-frames     Dump raw video frames to /tmp/.\n");
    printf("  -t, --nv12            Send NV12 frames\n");
    printf("\n");
    printf("  -v, --verbose         Enable verbose logging.\n");
    printf("  -h, --help            Print this list of arguments.\n");
    printf("  --version             Print program version.\n");
}

static int parse_options(int argc, char* argv[])
{
    int opt, longindex;
    int ret;

    while ((opt = getopt_long(argc, argv, "x:y:a:p:f:b:u:q:c:lvnhdVGrst", long_options, &longindex)) != -1) {
        switch (opt) {
        case 'x':
            settings.width = atoi(optarg);
            break;
        case 'y':
            settings.height = atoi(optarg);
            break;
        case 'a':
            free(settings.address);
            settings.address = strdup(optarg);
            break;
        case 'p':
            settings.port = atol(optarg);
            break;
        case 'l':
            settings.unix_socket = true;
            break;
        case 'f':
            settings.fps = atoi(optarg);
            break;
        case 'V':
            settings.no_video = 1;
            break;
        case 'G':
            settings.no_gui = 1;
            break;
        case 's':
            settings.no_powerstate = 1;
            break;
        case 'r':
            settings.no_hdr = 1;
            break;
        case 'n':
            settings.vsync = false;
            break;
        case 'd':
            settings.dump_frames = true;
            break;
        case 't':
            settings.send_nv12 = true;
            break;
        case 'v':
            log_set_level(Debug);
            break;
        case 'b':
            free(settings.video_backend);
            settings.video_backend = strdup(optarg);
            break;
        case 'u':
            free(settings.ui_backend);
            settings.ui_backend = strdup(optarg);
            break;
        case 'q':
            settings.quirks = atoi(optarg);
            break;
        case 'c':
            DBG("Loading config file %s...", optarg);
            if ((ret = settings_load_file(&settings, optarg)) != 0) {
                WARN("Config load failed: %d", ret);
            }
            break;
        case 'h':
            print_usage();
            return 1;
        case 0:
            /* getopt_long() set a variable, just keep going */
            break;
        default:
            WARN("Unknown option: %c", opt);
            print_usage();
            return 1;
        }
    }

    return 0;
}

void print_version_string()
{
    fprintf(stderr, "%s\n", HYPERION_WEBOS_VERSION);
}

int main(int argc, char* argv[])
{
    int ret;

    log_init();
    INFO("Starting up (version: %s)...", HYPERION_WEBOS_VERSION);

    using_cli = !getenv("LS_SERVICE_NAMES");

    settings_init(&settings);

    if (using_cli) {
        INFO("Running via CLI");
    } else {
        INFO("Running as a service");
        settings_load_file(&settings, SETTINGS_PERSISTENCE_PATH);
    }

    if ((ret = parse_options(argc, argv)) != 0) {
        return ret;
    }

    if (print_version > 0) {
        print_version_string();
        return 0;
    }

    if (!settings_valid(&settings)) {
        ERR("Invalid settings, refusing to start");
        return 1;
    }

    // Socket errors are handled where they happen; never die from SIGPIPE
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        WARN("Ignoring SIGPIPE failed: %s", strerror(errno));
    }

    loop = g_main_loop_new(NULL, false);

    // webOS stops services with SIGTERM; both paths shut capture down cleanly
    g_unix_signal_add(SIGINT, quit_handler, "SIGINT");
    g_unix_signal_add(SIGTERM, quit_handler, "SIGTERM");

    if ((ret = service_init(&service, &settings)) != 0) {
        ERR("Service init failed: %d", ret);
        return -1;
    }

    if ((ret = service_register(&service, loop)) != 0) {
        WARN("Service register failed: %d", ret);
    }

    if (using_cli || settings.autostart) {
        if ((ret = service_start(&service)) != 0) {
            ERR("Service startup failed: %d", ret);
            // g_main_loop_quit() before g_main_loop_run() has no effect. As a
            // service keep running so the app can still reconfigure us.
            if (using_cli) {
                service_destroy(&service);
                g_main_loop_unref(loop);
                return -1;
            }
        }
    }

    DBG("Going into main loop..");

    g_main_loop_run(loop);

    DBG("Main loop quit...");

    DBG("Cleaning up service...");
    service_destroy(&service);

    g_main_loop_unref(loop);

    DBG("Service main finished");
    return 0;
}
