#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#include <dile_vt.h>

#include "log.h"
#include "quirks.h"
#include "unicapture.h"
#include "utils.h"

int capture_release_frame(void* state, frame_info_t* frame);

typedef struct _dile_vt_backend_state {
    DILE_VT_HANDLE vth;
    DILE_OUTPUTDEVICE_STATE output_state;
    DILE_VT_FRAMEBUFFER_PROPERTY vfbprop;
    DILE_VT_FRAMEBUFFER_CAPABILITY vfbcap;
    uint8_t*** vfbs;
    int mem_fd;
    bool no_freeze_to_acquire_frame;
} dile_vt_backend_state_t;

/*
 * DILE_VT_Create (HAL_VT_Init) registers the VT capture unit with the kernel
 * display driver under the caller's pid; only DILE_VT_Destroy (HAL_VT_Finalize)
 * unregisters it, again by pid. A process that dies in between leaves the unit
 * registered to a dead pid and every later DILE_VT_Create fails until reboot
 * (seen on webOS 3.5). A DILE_VT_Create that fails part way leaves its own
 * registration behind too. So remember who owns it, and if the recorded owner
 * is gone, unregister on its behalf the way HAL_VT_Finalize does. The file
 * lives in /tmp, which a reboot clears along with the driver.
 */
#define DILE_VT_OWNER_FILE "/tmp/piccap-dilevt.pid"

typedef enum {
    OWNER_NONE, // nothing recorded, or released now
    OWNER_ALIVE, // another running process holds VT capture
} owner_state_t;

static void owner_record(void)
{
    FILE* f = fopen(DILE_VT_OWNER_FILE, "w");
    if (f == NULL) {
        WARN("[DILE_VT] Unable to record capture owner: %s", strerror(errno));
        return;
    }
    fprintf(f, "%d\n", (int)getpid());
    fclose(f);
}

static void owner_clear(void)
{
    unlink(DILE_VT_OWNER_FILE);
}

static owner_state_t release_stale_owner(void)
{
    int pid = 0;
    FILE* f = fopen(DILE_VT_OWNER_FILE, "r");
    if (f == NULL) {
        return OWNER_NONE;
    }
    if (fscanf(f, "%d", &pid) != 1) {
        pid = 0;
    }
    fclose(f);

    if (pid <= 0 || pid == (int)getpid()) {
        return OWNER_NONE;
    }
    if (kill(pid, 0) == 0 || errno != ESRCH) {
        WARN("[DILE_VT] VT capture is held by running process %d", pid);
        return OWNER_ALIVE;
    }

    // libhal_vt already pulled libkadaptor in; look it up instead of linking it
    void* kadp = dlopen("libkadaptor.so.2", RTLD_NOW | RTLD_LOCAL);
    if (kadp == NULL) {
        WARN("[DILE_VT] Unable to load libkadaptor: %s", dlerror());
        return OWNER_NONE;
    }

    int (*de_open)(void) = NULL;
    int (*de_close)(void) = NULL;
    int (*de_set_vtm)(void*) = NULL;
    *(void**)(&de_open) = dlsym(kadp, "KADP_DE_Open");
    *(void**)(&de_close) = dlsym(kadp, "KADP_DE_Close");
    *(void**)(&de_set_vtm) = dlsym(kadp, "KADP_DE_SetVTM");
    int ret = -1;

    if (de_open != NULL && de_close != NULL && de_set_vtm != NULL && de_open() == 0) {
        // Same request HAL_VT_Finalize sends: 0x70 bytes, pid at 4, enable = 0 at 8
        uint32_t vtm[0x70 / sizeof(uint32_t)] = { 0 };
        vtm[1] = (uint32_t)pid;
        ret = de_set_vtm(vtm);
        de_close();
    }
    dlclose(kadp);

    if (ret != 0) {
        WARN("[DILE_VT] Releasing VT capture left by dead process %d failed: %d", pid, ret);
        return OWNER_NONE;
    }

    INFO("[DILE_VT] Released VT capture left registered by dead process %d", pid);
    owner_clear();
    return OWNER_NONE;
}

// Releases everything capture_init() may have set up; safe on partial init
static void dile_vt_free(dile_vt_backend_state_t* this)
{
    if (this->vfbs != NULL) {
        size_t map_size = (size_t)this->vfbprop.stride * this->vfbprop.height;
        for (uint32_t vfb = 0; vfb < this->vfbcap.numVfbs; vfb++) {
            if (this->vfbs[vfb] == NULL)
                continue;
            for (uint32_t plane = 0; plane < this->vfbcap.numPlanes; plane++) {
                if (this->vfbs[vfb][plane] != NULL && this->vfbs[vfb][plane] != MAP_FAILED) {
                    munmap(this->vfbs[vfb][plane], map_size);
                }
            }
            free(this->vfbs[vfb]);
        }
        free(this->vfbs);
    }

    if (this->vfbprop.ptr != NULL) {
        for (uint32_t vfb = 0; vfb < this->vfbcap.numVfbs; vfb++) {
            free(this->vfbprop.ptr[vfb]);
        }
        free(this->vfbprop.ptr);
    }

    if (this->mem_fd >= 0) {
        close(this->mem_fd);
    }

    if (this->vth != NULL) {
        DILE_VT_Destroy(this->vth);
        owner_clear();
    }

    free(this);
}

static DILE_VT_HANDLE create_handle(cap_backend_config_t* config)
{
    // DILE_VT_CreateEx is a weak symbol that older libdile_vt builds lack;
    // calling it there would jump to NULL
    if (HAS_QUIRK(config->quirks, QUIRK_DILE_VT_CREATE_EX) && DILE_VT_CreateEx == NULL) {
        WARN("[QUIRK_DILE_VT_CREATE_EX]: DILE_VT_CreateEx not available on this TV, using DILE_VT_Create");
    }

    if (HAS_QUIRK(config->quirks, QUIRK_DILE_VT_CREATE_EX) && DILE_VT_CreateEx != NULL) {
        INFO("[QUIRK_DILE_VT_CREATE_EX]: Attempting DILE_VT_CreateEx...");
        return DILE_VT_CreateEx(0, 1);
    }

    INFO("Attempting DILE_VT_Create...");
    return DILE_VT_Create(0);
}

int capture_init(cap_backend_config_t* config, void** state_p)
{
    int ret = 0;

    INFO("Capture start called.");

    // Release before trying: a DILE_VT_Create that failed leaves libdile_vt in
    // a state where retrying in this process fails too. A recorded owner that
    // is gone can only have died without DILE_VT_Destroy. Never try while a
    // running process holds it: a failed attempt would take its registration.
    if (release_stale_owner() == OWNER_ALIVE) {
        return -1;
    }

    // Recorded first, so even dying inside DILE_VT_Create stays recoverable
    owner_record();
    DILE_VT_HANDLE vth = create_handle(config);
    if (vth == NULL) {
        WARN("Failed to get DILE_VT context!");
        // DILE_VT_Create doesn't undo HAL_VT_Init when it fails later on
        if (DILE_VT_Finalize != NULL) {
            DILE_VT_Finalize();
        }
        owner_clear();
        return -1;
    }

    dile_vt_backend_state_t* this = calloc(1, sizeof(dile_vt_backend_state_t));
    if (this == NULL) {
        DILE_VT_Destroy(vth);
        owner_clear();
        return -1;
    }
    this->vth = vth;
    this->mem_fd = -1;

    DBG("Got DILE_VT context!");

    DILE_VT_VIDEO_FRAME_OUTPUT_DEVICE_LIMITATION limitation;
    if (DILE_VT_GetVideoFrameOutputDeviceLimitation(vth, &limitation) != 0) {
        ret = -11;
        goto err_destroy;
    }

    DBG("supportScaleUp: %d; (%dx%d)", limitation.supportScaleUp, limitation.scaleUpLimitWidth, limitation.scaleUpLimitHeight);
    DBG("supportScaleDown: %d; (%dx%d)", limitation.supportScaleDown, limitation.scaleDownLimitWidth, limitation.scaleDownLimitHeight);
    DBG("maxResolution: %dx%d", limitation.maxResolution.width, limitation.maxResolution.height);
    DBG("input deinterlace: %d; display deinterlace: %d", limitation.supportInputVideoDeInterlacing, limitation.supportDisplayVideoDeInterlacing);

    DILE_VT_DUMP_LOCATION_TYPE_T dump_location;
    if (HAS_QUIRK(config->quirks, QUIRK_ALTERNATIVE_DUMP_LOCATION)) {
        // Quirk for WebOS 3.4 initialization
        INFO("[QUIRK_DILE_VT_DUMP_LOCATION_2]: Attempting UNDOCUMENTED dump location 2...");
        dump_location = 2;
        if (DILE_VT_SetVideoFrameOutputDeviceDumpLocation(vth, dump_location) != 0) {
            ERR("[DILE_VT] SetVideoFrameOutputDeviceDumpLocation (2) failed!");
            ret = -2;
            goto err_destroy;
        }
        INFO("[QUIRK_DILE_VT_DUMP_LOCATION_2]: SetVideoFrameOutputDeviceDumpLocation success");
    } else {
        // Regular operation: No quirk set
        dump_location = DILE_VT_DISPLAY_OUTPUT;
        if (DILE_VT_SetVideoFrameOutputDeviceDumpLocation(vth, dump_location) != 0) {
            WARN("[DILE_VT] DISPLAY dump location failed, attempting SCALER...");
            dump_location = DILE_VT_SCALER_OUTPUT;
            if (DILE_VT_SetVideoFrameOutputDeviceDumpLocation(vth, dump_location) != 0) {
                ERR("[DILE_VT] SCALER dump location failed, Exiting!");
                ret = -2;
                goto err_destroy;
            }
        }
    }

    DILE_VT_RECT region = { 0, 0, config->resolution_width, config->resolution_height };

    bool below_limit = region.width < limitation.scaleDownLimitWidth || region.height < limitation.scaleDownLimitHeight;

    if (DILE_VT_SetVideoFrameOutputDeviceOutputRegion(vth, dump_location, &region) != 0) {
        ERR("[DILE_VT] SetVideoFrameOutputDeviceOutputRegion failed!");
        if (below_limit) {
            WARN("[DILE_VT] %dx%d is below the reported scale-down limit %dx%d; try a larger resolution",
                region.width, region.height, limitation.scaleDownLimitWidth, limitation.scaleDownLimitHeight);
        }
        ret = -3;
        goto err_destroy;
    }

    // Drivers report a scale-down limit (320x240 on a webOS 3.5 C7) but
    // accept smaller regions and deliver correct frames, e.g. 320x180
    if (below_limit) {
        INFO("[DILE_VT] %dx%d is below the reported scale-down limit %dx%d; the driver accepted it",
            region.width, region.height, limitation.scaleDownLimitWidth, limitation.scaleDownLimitHeight);
    }

    this->output_state.enabled = 0;
    this->output_state.freezed = 0;
    this->output_state.appliedPQ = 0;

    // Framerate provided by libdile_vt is dependent on content currently played
    // (50/60fps). We don't have any better way of limiting FPS, so let's just
    // average it out - if someone sets their preferred framerate to 30 and
    // content was 50fps, we'll just end up feeding 25 frames per second.
    this->output_state.framerate = config->fps == 0 ? 1 : 60 / config->fps;
    INFO("[DILE_VT] framerate divider: %d", this->output_state.framerate);

    DILE_VT_WaitVsync(vth);
    uint64_t t1 = getticks_us();
    DILE_VT_WaitVsync(vth);
    uint64_t t2 = getticks_us();

    double fps = 1000000.0 / (t2 - t1);
    INFO("[DILE_VT] frametime: %d; estimated fps before divider: %.5f", (uint32_t)(t2 - t1), fps);

    // Set framerate divider
    if (DILE_VT_SetVideoFrameOutputDeviceState(vth, DILE_VT_VIDEO_FRAME_OUTPUT_DEVICE_STATE_FRAMERATE_DIVIDE, &this->output_state) != 0) {
        ERR("[DILE_VT] SetVideoFrameOutputDeviceState FRAMERATE_DIVIDE failed!");
        ret = -4;
        goto err_destroy;
    }

    DILE_VT_WaitVsync(vth);
    t1 = getticks_us();
    DILE_VT_WaitVsync(vth);
    t2 = getticks_us();

    fps = 1000000.0 / (t2 - t1);
    INFO("[DILE_VT] frametime: %d; estimated fps after divider: %.5f", (uint32_t)(t2 - t1), fps);

    // Set freeze
    if (DILE_VT_SetVideoFrameOutputDeviceState(vth, DILE_VT_VIDEO_FRAME_OUTPUT_DEVICE_STATE_FREEZED, &this->output_state) != 0) {
        ERR("[DILE_VT] SetVideoFrameOutputDeviceState FREEZE failed!");
        ret = -5;
        goto err_destroy;
    }

    // Prepare offsets table (needs to be ptr[vfbcap.numVfs][vbcap.numPlanes])
    if (DILE_VT_GetVideoFrameBufferCapability(vth, &this->vfbcap) != 0) {
        ERR("[DILE_VT] GetVideoFrameBufferCapability failed!");
        ret = -9;
        goto err_destroy;
    }

    INFO("[DILE_VT] vfbs: %d; planes: %d", this->vfbcap.numVfbs, this->vfbcap.numPlanes);
    uint32_t** ptr = calloc(this->vfbcap.numVfbs, sizeof(uint32_t*));
    if (ptr == NULL) {
        ret = -12;
        goto err_destroy;
    }
    this->vfbprop.ptr = ptr;
    for (uint32_t vfb = 0; vfb < this->vfbcap.numVfbs; vfb++) {
        if ((ptr[vfb] = calloc(this->vfbcap.numPlanes, sizeof(uint32_t))) == NULL) {
            ret = -12;
            goto err_destroy;
        }
    }

    if (DILE_VT_GetAllVideoFrameBufferProperty(vth, &this->vfbcap, &this->vfbprop) != 0) {
        ERR("[DILE_VT] GetAllVideoFrameBufferProperty failed!");
        ret = -10;
        goto err_destroy;
    }

    // TODO: check out if /dev/gfx is something we should rather use
    // Frames are only read, so don't ask for write access to physical memory
    this->mem_fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (this->mem_fd == -1) {
        ERR("[DILE_VT] Creating handle to /dev/mem failed!");
        ret = -6;
        goto err_destroy;
    }

    INFO("[DILE_VT] pixelFormat: %d; width: %d; height: %d; stride: %d...", this->vfbprop.pixelFormat, this->vfbprop.width, this->vfbprop.height, this->vfbprop.stride);
    if ((this->vfbs = calloc(this->vfbcap.numVfbs, sizeof(uint8_t**))) == NULL) {
        ret = -12;
        goto err_destroy;
    }
    for (uint32_t vfb = 0; vfb < this->vfbcap.numVfbs; vfb++) {
        if ((this->vfbs[vfb] = calloc(this->vfbcap.numPlanes, sizeof(uint8_t*))) == NULL) {
            ret = -12;
            goto err_destroy;
        }
        for (uint32_t plane = 0; plane < this->vfbcap.numPlanes; plane++) {
            DBG("[DILE_VT] vfb[%d][%d] = 0x%08x", vfb, plane, this->vfbprop.ptr[vfb][plane]);
            this->vfbs[vfb][plane] = (uint8_t*)mmap(0, this->vfbprop.stride * this->vfbprop.height, PROT_READ, MAP_SHARED, this->mem_fd, this->vfbprop.ptr[vfb][plane]);
            if (this->vfbs[vfb][plane] == MAP_FAILED) {
                ERR("[DILE_VT] mmap for vfb[%d][%d] failed!", vfb, plane);
                ret = -7;
                goto err_destroy;
            }
        }
    }

    if (HAS_QUIRK(config->quirks, QUIRK_DILE_VT_NO_FREEZE_CAPTURE)) {
        INFO("[QUIRK_DILE_VT_NO_FREEZE_CAPTURE]: Won't freeze for acquiring frames");
        this->no_freeze_to_acquire_frame = true;
    }

    *state_p = this;
    return 0;

err_destroy:
    dile_vt_free(this);
    return ret;
}

int capture_cleanup(void* state)
{
    dile_vt_free((dile_vt_backend_state_t*)state);
    return 0;
}

int capture_start(void* state)
{
    (void)state;
    // DILE_VT_Start is libdile_vt's cross-process VT lock, held while
    // capturing. This used to pass the handle as pWaitTime, whose first
    // field (the video window, 0) meant "don't wait"; keep that.
    unsigned int wait_ms = 0;
    if (DILE_VT_Start(&wait_ms) != 0) {
        return -1;
    }
    return 0;
}

int capture_terminate(void* state)
{
    (void)state;
    DILE_VT_Stop();
    return 0;
}

int capture_acquire_frame(void* state, frame_info_t* frame)
{
    dile_vt_backend_state_t* this = (dile_vt_backend_state_t*)state;

    uint32_t idx = 0;

    if (!this->no_freeze_to_acquire_frame) {
        this->output_state.freezed = 1;
        DILE_VT_SetVideoFrameOutputDeviceState(this->vth, DILE_VT_VIDEO_FRAME_OUTPUT_DEVICE_STATE_FREEZED, &this->output_state);
    }

    DILE_VT_GetCurrentVideoFrameBufferProperty(this->vth, NULL, &idx);
    if (idx >= this->vfbcap.numVfbs) {
        ERR("[DILE_VT] Invalid framebuffer index: %u", idx);
        capture_release_frame(state, frame);
        return -1;
    }

    frame->width = this->vfbprop.width;
    frame->height = this->vfbprop.height;

    if (this->vfbprop.pixelFormat == DILE_VT_VIDEO_FRAME_BUFFER_PIXEL_FORMAT_RGB) {
        frame->pixel_format = PIXFMT_RGB;
        // Note: vfbprop.width is equal to stride for some reason.
        frame->width = this->vfbprop.stride / 3;
        frame->planes[0].buffer = this->vfbs[idx][0];
        frame->planes[0].stride = this->vfbprop.stride;
    } else if (this->vfbprop.pixelFormat == DILE_VT_VIDEO_FRAME_BUFFER_PIXEL_FORMAT_YUV420_SEMI_PLANAR) {
        frame->pixel_format = PIXFMT_YUV420_SEMI_PLANAR;
        frame->planes[0].buffer = this->vfbs[idx][0];
        frame->planes[0].stride = this->vfbprop.stride;
        frame->planes[1].buffer = this->vfbs[idx][1];
        frame->planes[1].stride = this->vfbprop.stride;
    } else if (this->vfbprop.pixelFormat == DILE_VT_VIDEO_FRAME_BUFFER_PIXEL_FORMAT_YUV422_SEMI_PLANAR) {
        frame->pixel_format = PIXFMT_YUV422_SEMI_PLANAR;
        frame->planes[0].buffer = this->vfbs[idx][0];
        frame->planes[0].stride = this->vfbprop.stride;
        frame->planes[1].buffer = this->vfbs[idx][1];
        frame->planes[1].stride = this->vfbprop.stride;
    } else {
        ERR("[DILE_VT] Unhandled pixelformat: 0x%x!", this->vfbprop.pixelFormat);
        capture_release_frame(state, frame);
        return -1;
    }

    return 0;
}

int capture_release_frame(void* state, frame_info_t* frame __attribute__((unused)))
{
    dile_vt_backend_state_t* this = (dile_vt_backend_state_t*)state;
    if (!this->no_freeze_to_acquire_frame) {
        this->output_state.freezed = 0;
        DILE_VT_SetVideoFrameOutputDeviceState(this->vth, DILE_VT_VIDEO_FRAME_OUTPUT_DEVICE_STATE_FREEZED, &this->output_state);
    }

    return 0;
}

int capture_wait(void* state)
{
    dile_vt_backend_state_t* this = (dile_vt_backend_state_t*)state;
    DILE_VT_WaitVsync(this->vth);
    return 0;
}
