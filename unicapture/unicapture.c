#include "unicapture.h"
#include "converter.h"
#include "log.h"
#include "utils.h"
#include <dlfcn.h>
#include <inttypes.h>
#include <libyuv.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DLSYM_ERROR_CHECK()                         \
    if ((error = dlerror()) != NULL) {              \
        ERR("Error! dlsym failed, msg: %s", error); \
        return -2;                                  \
    }

static int reserve_buffer(uint8_t** buffer, size_t* capacity, size_t size)
{
    if (*capacity >= size) {
        return 0;
    }

    uint8_t* new_buffer = realloc(*buffer, size);
    if (!new_buffer) {
        return -1;
    }

    *buffer = new_buffer;
    *capacity = size;
    return 0;
}

static void borrow_frame(frame_info_t* output, const frame_info_t* input)
{
    *output = *input;
}

int unicapture_init_backend(cap_backend_config_t* config, capture_backend_t* backend, const char* name)
{
    char* error;
    void* handle = dlopen(name, RTLD_LAZY);

    DBG("%s: loading...", name);

    if (handle == NULL) {
        WARN("Unable to load %s: %s", name, dlerror());
        return -1;
    }

    dlerror();

    *(void**)(&backend->init) = dlsym(handle, "capture_init");
    DLSYM_ERROR_CHECK();
    *(void**)(&backend->cleanup) = dlsym(handle, "capture_cleanup");
    DLSYM_ERROR_CHECK();

    *(void**)(&backend->start) = dlsym(handle, "capture_start");
    DLSYM_ERROR_CHECK();
    *(void**)(&backend->terminate) = dlsym(handle, "capture_terminate");
    DLSYM_ERROR_CHECK();

    *(void**)(&backend->acquire_frame) = dlsym(handle, "capture_acquire_frame");
    DLSYM_ERROR_CHECK();
    *(void**)(&backend->release_frame) = dlsym(handle, "capture_release_frame");
    DLSYM_ERROR_CHECK();

    *(void**)(&backend->wait) = dlsym(handle, "capture_wait");
    DLSYM_ERROR_CHECK();

    DBG("%s: loaded, initializing...", name);

    int ret = backend->init(config, &backend->state);

    if (ret == 0) {
        backend->name = strdup(name);
        DBG("%s: success", name);
    } else {
        ERR("%s: init failure, code: %d", name, ret);
    }

    return ret;
}

int unicapture_try_backends(cap_backend_config_t* config, capture_backend_t* backend, const char* const* candidates)
{
    int ret = 0;
    for (int i = 0; candidates[i] != NULL; i++) {
        ret = unicapture_init_backend(config, backend, candidates[i]);
        if (ret == 0) {
            DBG("try_backends: %s succeeded", candidates[i]);
            return 0;
        } else {
            WARN("try_backends: backend: %s failed with code: %d", candidates[i], ret);
        }
    }

    // The last backend's error is the most telling (candidates are ordered
    // newest first, and missing libraries fail with -1 before the real one)
    ERR("Try backends failed!");
    return ret != 0 ? ret : -1;
}

void* unicapture_vsync_handler(void* data)
{
    unicapture_state_t* this = (unicapture_state_t*)data;

    INFO("vsync thread starting...");

    while (this->vsync_thread_running) {
        if (this->vsync && this->video_capture_running && !__atomic_load_n(&this->video_restart_requested, __ATOMIC_ACQUIRE) && this->video_capture->wait) {
            if ((this->video_capture->wait(this->video_capture->state)) == -99) {
                // Only flag it: the capture thread may be inside acquire_frame()
                // right now, so it terminates (and later restarts) the backend
                INFO("Video capture requested a restart.");
                __atomic_store_n(&this->video_restart_requested, true, __ATOMIC_RELEASE);
            }
        } else {
            usleep(1000000 / (this->fps == 0 ? 30 : this->fps));
        }
        pthread_mutex_lock(&this->vsync_lock);
        __atomic_add_fetch(&this->metrics.vsyncs, 1, __ATOMIC_RELAXED);
        pthread_cond_signal(&this->vsync_cond);
        pthread_mutex_unlock(&this->vsync_lock);
    }

    INFO("vsync thread finished");

    return NULL;
}

void* unicapture_run(void* data)
{
    unicapture_state_t* this = (unicapture_state_t*)data;
    capture_backend_t* ui_capture = this->ui_capture;
    capture_backend_t* video_capture = this->video_capture;

    uint64_t framecounter = 0;
    uint64_t framecounter_start = getticks_us();

    uint64_t last_video_start = 0;
    uint64_t last_ui_start = 0;

    converter_t ui_converter;
    converter_t video_converter;
    converter_t final_converter;

    converter_init(&ui_converter);
    converter_init(&video_converter);
    converter_init(&final_converter);

    pthread_mutex_init(&this->vsync_lock, NULL);
    pthread_cond_init(&this->vsync_cond, NULL);

    frame_info_t blended_frame = { PIXFMT_INVALID };
    size_t blended_frame_capacities[MAX_PLANES] = { 0 };
    uint8_t* blend_alpha_y = NULL;
    size_t blend_alpha_y_capacity = 0;
    uint8_t* ui_scaled = NULL;
    size_t ui_scaled_capacity = 0;
    unsigned int video_fail_count = 0;
    unsigned int ui_mismatch_count = 0;
    // Stage times summed over the current 60-frame window
    uint64_t sum_wait = 0, sum_acquire = 0, sum_convert = 0, sum_process = 0, sum_send = 0;
    uint32_t vsync_seen = 0;

    __atomic_store_n(&this->video_restart_requested, false, __ATOMIC_RELEASE);
    this->vsync_thread_running = true;
    if (pthread_create(&this->vsync_thread, NULL, unicapture_vsync_handler, this) != 0) {
        ERR("Unable to start vsync thread");
        this->vsync_thread_running = false;
        this->running = false;
    }

    while (this->running) {
        uint64_t now = getticks_us();

        // The service may attach a video backend that loaded only later
        if (video_capture == NULL) {
            video_capture = __atomic_load_n(&this->video_capture, __ATOMIC_ACQUIRE);
        }

        if ((now - last_ui_start) > 1000000 && ui_capture != NULL && !this->ui_capture_running) {
            last_ui_start = now;
            DBG("Attempting UI capture init...");
            if (ui_capture->start(ui_capture->state) == 0) {
                INFO("UI capture started");
                this->ui_capture_running = true;
            }
        }

        if ((now - last_video_start) > 1000000 && video_capture != NULL && !this->video_capture_running) {
            last_video_start = now;
            DBG("Attempting video capture init...");
            if (video_capture->start(video_capture->state) == 0) {
                INFO("Video capture started");
                this->video_capture_running = true;
            }
        }

        int ret = 0;
        uint64_t frame_start = getticks_us();

        // Wait on the vsync count, not just the signal: a vsync that came
        // while the previous frame was still being processed is used right
        // away instead of being lost (which cost a whole extra period)
        pthread_mutex_lock(&this->vsync_lock);
        while (__atomic_load_n(&this->metrics.vsyncs, __ATOMIC_RELAXED) == vsync_seen && this->running) {
            pthread_cond_wait(&this->vsync_cond, &this->vsync_lock);
        }
        vsync_seen = __atomic_load_n(&this->metrics.vsyncs, __ATOMIC_RELAXED);
        pthread_mutex_unlock(&this->vsync_lock);
        __atomic_add_fetch(&this->metrics.wakeups, 1, __ATOMIC_RELAXED);
        uint64_t frame_wait = getticks_us();

        frame_info_t ui_frame = { PIXFMT_INVALID };
        frame_info_t ui_frame_converted = { PIXFMT_INVALID };
        frame_info_t video_frame = { PIXFMT_INVALID };
        frame_info_t video_frame_converted = { PIXFMT_INVALID };
        frame_info_t final_frame = { PIXFMT_INVALID };
        bool video_restart = __atomic_load_n(&this->video_restart_requested, __ATOMIC_ACQUIRE);

        // Capture frames
        if (this->ui_capture_running) {
            if ((ret = ui_capture->acquire_frame(ui_capture->state, &ui_frame)) != 0) {
                ui_frame.pixel_format = PIXFMT_INVALID;
            }
        }

        if (this->video_capture_running && !video_restart) {
            if ((ret = video_capture->acquire_frame(video_capture->state, &video_frame)) != 0) {
                // Can fail on every frame for a while; don't flood the log
                if (video_fail_count++ % 300 == 0) {
                    DBG("video_capture acquire_frame failed: %d (%u times)", ret, video_fail_count);
                }
                video_frame.pixel_format = PIXFMT_INVALID;
            } else {
                video_fail_count = 0;
            }
        }

        uint64_t frame_acquired = getticks_us();
        pixel_format_t target_format = this->target_format;
        pixel_format_t blend_format = (target_format == PIXFMT_RGB || target_format == PIXFMT_ARGB) ? PIXFMT_ARGB : PIXFMT_YUV420_SEMI_PLANAR;

        // Blending reads both frames with the video frame's geometry, so bring
        // the UI layer to the same size first (ui_frame itself stays untouched
        // because it has to be handed back to release_frame)
        frame_info_t ui_source = ui_frame;
        if (ui_frame.pixel_format != PIXFMT_INVALID && video_frame.pixel_format != PIXFMT_INVALID
            && (ui_frame.width != video_frame.width || ui_frame.height != video_frame.height)) {
            const int width = video_frame.width;
            const int height = video_frame.height;

            if ((ui_frame.pixel_format == PIXFMT_ARGB || ui_frame.pixel_format == PIXFMT_ABGR)
                && width > 0 && height > 0
                && reserve_buffer(&ui_scaled, &ui_scaled_capacity, (size_t)width * height * 4) == 0) {
                ARGBScale(ui_frame.planes[0].buffer, ui_frame.planes[0].stride, ui_frame.width, ui_frame.height,
                    ui_scaled, width * 4, width, height, kFilterBilinear);
                ui_source.width = width;
                ui_source.height = height;
                ui_source.planes[0].buffer = ui_scaled;
                ui_source.planes[0].stride = width * 4;
            } else {
                if (ui_mismatch_count++ % 300 == 0) {
                    DBG("UI frame %dx%d can't be matched to video %dx%d, sending video only",
                        ui_frame.width, ui_frame.height, width, height);
                }
                ui_source.pixel_format = PIXFMT_INVALID;
            }
        }

        // Convert frame to suitable video formats
        if (ui_source.pixel_format != PIXFMT_INVALID) {
            if (ui_source.pixel_format == blend_format) {
                borrow_frame(&ui_frame_converted, &ui_source);
            } else if (converter_run(&ui_converter, &ui_source, &ui_frame_converted, blend_format) != 0) {
                ui_frame_converted.pixel_format = PIXFMT_INVALID;
            }
        }

        if (video_frame.pixel_format != PIXFMT_INVALID) {
            if (video_frame.pixel_format == blend_format) {
                borrow_frame(&video_frame_converted, &video_frame);
            } else if (converter_run(&video_converter, &video_frame, &video_frame_converted, blend_format) != 0) {
                video_frame_converted.pixel_format = PIXFMT_INVALID;
            }
        }

        uint64_t frame_converted = getticks_us();

        bool got_frame = true;

        if (video_frame_converted.pixel_format != PIXFMT_INVALID && ui_frame_converted.pixel_format != PIXFMT_INVALID
            && (video_frame_converted.width != ui_frame_converted.width || video_frame_converted.height != ui_frame_converted.height)) {
            ui_frame_converted.pixel_format = PIXFMT_INVALID;
        }

        // Blend frames and prepare for sending
        if (video_frame_converted.pixel_format != PIXFMT_INVALID && ui_frame_converted.pixel_format != PIXFMT_INVALID) {
            const int width = video_frame_converted.width;
            const int height = video_frame_converted.height;

            if (target_format == PIXFMT_RGB || target_format == PIXFMT_ARGB) {
                if (reserve_buffer(&blended_frame.planes[0].buffer, &blended_frame_capacities[0], width * height * 4) != 0) {
                    got_frame = false;
                    WARN("Unable to allocate ARGB blend buffer");
                } else {
                    blended_frame.planes[0].stride = width * 4;
                    blended_frame.pixel_format = PIXFMT_ARGB;
                    blended_frame.width = width;
                    blended_frame.height = height;

                    ARGBBlend(ui_frame_converted.planes[0].buffer,
                        ui_frame_converted.planes[0].stride,
                        video_frame_converted.planes[0].buffer,
                        video_frame_converted.planes[0].stride,
                        blended_frame.planes[0].buffer,
                        4 * width,
                        width,
                        height);
                }
            }

            if (got_frame && target_format == PIXFMT_YUV420_SEMI_PLANAR) {
                const int uv_height = height / 2;

                if (reserve_buffer(&blended_frame.planes[0].buffer, &blended_frame_capacities[0], width * height) != 0
                    || reserve_buffer(&blended_frame.planes[1].buffer, &blended_frame_capacities[1], width * height / 2) != 0
                    || reserve_buffer(&blend_alpha_y, &blend_alpha_y_capacity, width * height) != 0) {
                    got_frame = false;
                    WARN("Unable to allocate NV12 blend buffers");
                } else {
                    blended_frame.planes[0].stride = width;
                    blended_frame.planes[1].stride = width;
                    blended_frame.height = height;
                    blended_frame.width = width;
                    blended_frame.pixel_format = PIXFMT_YUV420_SEMI_PLANAR;

                    ARGBExtractAlpha(ui_source.planes[0].buffer,
                        ui_source.planes[0].stride,
                        blend_alpha_y,
                        width,
                        width,
                        height);

                    BlendPlane(ui_frame_converted.planes[0].buffer,
                        ui_frame_converted.planes[0].stride,
                        video_frame_converted.planes[0].buffer,
                        video_frame_converted.planes[0].stride,
                        blend_alpha_y,
                        width,
                        blended_frame.planes[0].buffer,
                        blended_frame.planes[0].stride,
                        width,
                        height);

                    BlendPlane(ui_frame_converted.planes[1].buffer,
                        ui_frame_converted.planes[1].stride,
                        video_frame_converted.planes[1].buffer,
                        video_frame_converted.planes[1].stride,
                        blend_alpha_y,
                        width * 2,
                        blended_frame.planes[1].buffer,
                        blended_frame.planes[1].stride,
                        width,
                        uv_height);
                }
            }
            if (got_frame) {
                if (blended_frame.pixel_format == target_format) {
                    final_frame = blended_frame;
                } else if (converter_run(&final_converter, &blended_frame, &final_frame, target_format) != 0) {
                    final_frame.pixel_format = PIXFMT_INVALID;
                }
            }
        } else if (ui_frame_converted.pixel_format != PIXFMT_INVALID) {
            if (ui_frame_converted.pixel_format == target_format) {
                borrow_frame(&final_frame, &ui_frame_converted);
            } else if (converter_run(&final_converter, &ui_frame_converted, &final_frame, target_format) != 0) {
                final_frame.pixel_format = PIXFMT_INVALID;
            }
        } else if (video_frame_converted.pixel_format != PIXFMT_INVALID) {
            if (video_frame_converted.pixel_format == target_format) {
                borrow_frame(&final_frame, &video_frame_converted);
            } else if (converter_run(&final_converter, &video_frame_converted, &final_frame, target_format) != 0) {
                final_frame.pixel_format = PIXFMT_INVALID;
            }
        } else {
            WARN("No valid frame to send...");
        }

        // Never send a frame that wasn't produced in this iteration: earlier
        // buffers may already have been handed back to the backend
        if (final_frame.pixel_format != target_format) {
            got_frame = false;
        }

        uint64_t frame_processed = getticks_us();

        if (this->dump_frames && got_frame && framecounter % 30 == 0) {
            char filename[256];
            snprintf(filename, sizeof(filename), "/tmp/hyperion-webos-dump.%03d.data", (int)(framecounter / 30) % 10);
            FILE* fd = fopen(filename, "wb");
            if (fd == NULL) {
                WARN("Unable to open %s for frame dump", filename);
            } else {
                if (target_format == PIXFMT_RGB)
                    fwrite(final_frame.planes[0].buffer, 3 * final_frame.width * final_frame.height, 1, fd);
                if (target_format == PIXFMT_ARGB)
                    fwrite(final_frame.planes[0].buffer, 4 * final_frame.width * final_frame.height, 1, fd);
                if (target_format == PIXFMT_YUV420_SEMI_PLANAR) {
                    fwrite(final_frame.planes[0].buffer, final_frame.width * final_frame.height, 1, fd);
                    fwrite(final_frame.planes[1].buffer, final_frame.width * final_frame.height / 2, 1, fd);
                }
                fclose(fd);
                INFO("Buffer dumped to: %s", filename);
            }
        }

        if (got_frame && this->callback != NULL && (target_format == PIXFMT_RGB || target_format == PIXFMT_ARGB)) {
            this->callback(this->callback_data, final_frame.width, final_frame.height, final_frame.planes[0].buffer);
        }

        if (got_frame && this->callback_nv12 != NULL && target_format == PIXFMT_YUV420_SEMI_PLANAR) {
            this->callback_nv12(this->callback_data, final_frame.width, final_frame.height, final_frame.planes[0].buffer,
                final_frame.planes[1].buffer, final_frame.planes[0].stride, final_frame.planes[1].stride);
        }

        uint64_t frame_sent = getticks_us();

        if (ui_frame.pixel_format != PIXFMT_INVALID) {
            ui_capture->release_frame(ui_capture->state, &ui_frame);
        }

        if (video_frame.pixel_format != PIXFMT_INVALID) {
            video_capture->release_frame(video_capture->state, &video_frame);
        }

        if (video_restart) {
            if (this->video_capture_running) {
                INFO("Stopping video capture.");
                video_capture->terminate(video_capture->state);
                this->video_capture_running = false;
                __atomic_add_fetch(&this->metrics.video_restarts, 1, __ATOMIC_RELAXED);
            }
            // Started again at the top of the loop
            __atomic_store_n(&this->video_restart_requested, false, __ATOMIC_RELEASE);
        }

        uint64_t frame_released = getticks_us();

        if (got_frame) {
            framecounter += 1;
            __atomic_add_fetch(&this->metrics.frames, 1, __ATOMIC_RELAXED);
            sum_wait += frame_wait - frame_start;
            sum_acquire += frame_acquired - frame_wait;
            sum_convert += frame_converted - frame_acquired;
            sum_process += frame_processed - frame_converted;
            sum_send += frame_sent - frame_processed;

            if (framecounter % 60 == 0) {
                double fps = (60 * 1000000.0) / (getticks_us() - framecounter_start);
                this->metrics.framerate = fps;
                __atomic_store_n(&this->metrics.wait_us, (uint32_t)(sum_wait / 60), __ATOMIC_RELAXED);
                __atomic_store_n(&this->metrics.acquire_us, (uint32_t)(sum_acquire / 60), __ATOMIC_RELAXED);
                __atomic_store_n(&this->metrics.convert_us, (uint32_t)(sum_convert / 60), __ATOMIC_RELAXED);
                __atomic_store_n(&this->metrics.process_us, (uint32_t)(sum_process / 60), __ATOMIC_RELAXED);
                __atomic_store_n(&this->metrics.send_us, (uint32_t)(sum_send / 60), __ATOMIC_RELAXED);
                sum_wait = sum_acquire = sum_convert = sum_process = sum_send = 0;
                // One line every ~20 s at 30 fps: with "Setup logging" debug
                // output goes to the TV's RAM-backed /var/log
                if (framecounter % 600 == 0) {
                    DBG("%.1f fps; us wait %" PRIu64 " acquire %" PRIu64 " convert %" PRIu64 " process %" PRIu64 " send %" PRIu64
                        " release %" PRIu64 "; "
                        "UI fmt %d %dx%d -> %dx%d; video fmt %d %dx%d -> %dx%d",
                        fps, frame_wait - frame_start, frame_acquired - frame_wait, frame_converted - frame_acquired,
                        frame_processed - frame_converted, frame_sent - frame_processed, frame_released - frame_sent,
                        ui_frame.pixel_format, ui_frame.width, ui_frame.height, ui_frame_converted.width, ui_frame_converted.height,
                        video_frame.pixel_format, video_frame.width, video_frame.height, video_frame_converted.width, video_frame_converted.height);
                }

                framecounter_start = getticks_us();
            }
        }
    }

    INFO("Shutting down...");

    if (this->vsync_thread_running) {
        DBG("Waiting for vsync thread to finish...");
        this->vsync_thread_running = false;
        pthread_join(this->vsync_thread, NULL);
    }

    if (this->ui_capture_running) {
        DBG("Terminating UI capture...");
        ui_capture->terminate(ui_capture->state);
        this->ui_capture_running = false;
    }

    if (this->video_capture_running) {
        DBG("Terminating Video capture...");
        video_capture->terminate(video_capture->state);
        this->video_capture_running = false;
    }

    for (int i = 0; i < MAX_PLANES; i++) {
        if (blended_frame.planes[i].buffer != NULL) {
            free(blended_frame.planes[i].buffer);
            blended_frame.planes[i].buffer = NULL;
            blended_frame_capacities[i] = 0;
        }
    }
    free(blend_alpha_y);
    free(ui_scaled);

    pthread_cond_destroy(&this->vsync_cond);
    pthread_mutex_destroy(&this->vsync_lock);

    converter_release(&ui_converter);
    converter_release(&video_converter);
    converter_release(&final_converter);
    DBG("Done!");

    return NULL;
}

void unicapture_init(unicapture_state_t* this)
{
    memset(this, 0, sizeof(unicapture_state_t));
    this->vsync = true;
}

int unicapture_start(unicapture_state_t* this)
{
    if (this->running) {
        return 1;
    }

    memset(&this->metrics, 0, sizeof(this->metrics));
    this->running = true;
    if (pthread_create(&this->main_thread, NULL, unicapture_run, this) != 0) {
        ERR("Unable to start capture thread");
        this->running = false;
        return -1;
    }

    return 0;
}

int unicapture_stop(unicapture_state_t* this)
{
    if (!this->running) {
        return 1;
    }

    this->running = false;
    pthread_join(this->main_thread, NULL);

    return 0;
}

void unicapture_emergency_stop(unicapture_state_t* this)
{
    // A process that dies with a DILE_VT handle open leaves the capture unit
    // registered to its pid until the TV reboots, so release the video backend
    // (not just stop it) before dying. The handle exists whenever the backend
    // is loaded, even while capture is stopped.
    capture_backend_t* video = this->video_capture;
    if (video != NULL) {
        this->video_capture = NULL;
        if (this->video_capture_running && video->terminate != NULL) {
            video->terminate(video->state);
        }
        this->video_capture_running = false;
        if (video->cleanup != NULL) {
            video->cleanup(video->state);
        }
    }

    if (this->ui_capture_running && this->ui_capture != NULL && this->ui_capture->terminate != NULL) {
        this->ui_capture_running = false;
        this->ui_capture->terminate(this->ui_capture->state);
    }
}
