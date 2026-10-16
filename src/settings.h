#pragma once

#include <pbnjson.h>
#include <stdbool.h>

#define SETTINGS_PERSISTENCE_PATH "/media/developer/apps/usr/palm/services/org.webosbrew.piccap.service/config.json"

// Settings stored in config.json file

typedef struct _settings_t {
    char* video_backend;
    char* ui_backend;

    char* address;
    int port;
    int priority;
    bool unix_socket;

    int fps;
    bool send_nv12;
    int width;
    int height;
    bool vsync;
    int quirks;

    bool no_video;
    bool no_gui;

    bool autostart;

    bool dump_frames;

    bool no_hdr;
    bool no_powerstate;
} settings_t;

extern const char* const settings_ui_backends[];
extern const char* const settings_video_backends[];

void settings_init(settings_t*);

bool settings_backend_allowed(const char* name, const char* const* allowed);
bool settings_address_valid(const char* address, bool unix_socket);

int settings_load_json(settings_t* settings, jvalue_ref source);
int settings_save_json(settings_t* settings, jvalue_ref target);

int settings_load_file(settings_t* settings, char* source);
int settings_save_file(settings_t* settings, char* target);
