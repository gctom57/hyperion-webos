#include "settings.h"
#include "log.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/un.h>
#include <unistd.h>

#define SETTINGS_MAX_FILE_SIZE (64 * 1024)

const char* const settings_ui_backends[] = { "libgm", "libhalgal", NULL };
const char* const settings_video_backends[] = { "libvtcapture", "libdile_vt", NULL };

bool settings_backend_allowed(const char* name, const char* const* allowed)
{
    if (name == NULL || name[0] == '\0' || strcmp(name, "auto") == 0) {
        return true;
    }

    for (int i = 0; allowed[i] != NULL; i++) {
        if (strcmp(name, allowed[i]) == 0) {
            return true;
        }
    }

    return false;
}

bool settings_address_valid(const char* address, bool unix_socket)
{
    if (address == NULL || address[0] == '\0') {
        return true;
    }

    if (unix_socket) {
        // Must fit sockaddr_un.sun_path including the terminator
        return address[0] == '/' && strlen(address) < sizeof(((struct sockaddr_un*)0)->sun_path);
    }

    struct in_addr addr;
    return inet_pton(AF_INET, address, &addr) == 1;
}

static bool int_in_range(const char* key, int value, int min, int max)
{
    if (value < min || value > max) {
        WARN("Settings: %s out of range (%d..%d)", key, min, max);
        return false;
    }
    return true;
}

bool settings_valid(settings_t* settings)
{
    bool valid = true;

    if (!settings_backend_allowed(settings->video_backend, settings_video_backends)) {
        WARN("Settings: unknown video backend: %s", settings->video_backend);
        valid = false;
    }
    if (!settings_backend_allowed(settings->ui_backend, settings_ui_backends)) {
        WARN("Settings: unknown UI backend: %s", settings->ui_backend);
        valid = false;
    }
    if (!settings_address_valid(settings->address, settings->unix_socket)) {
        WARN("Settings: invalid %s address", settings->unix_socket ? "unix socket" : "IPv4");
        valid = false;
    }

    valid = int_in_range("port", settings->port, 1, 65535) && valid;
    valid = int_in_range("priority", settings->priority, 0, 255) && valid;
    valid = int_in_range("fps", settings->fps, 0, 240) && valid;
    valid = int_in_range("width", settings->width, 16, 3840) && valid;
    valid = int_in_range("height", settings->height, 16, 2160) && valid;

    return valid;
}

static bool json_string_field(jvalue_ref source, const char* key, raw_buffer* out)
{
    jvalue_ref value = jobject_get(source, j_cstr_to_buffer(key));
    if (!value || !jis_string(value)) {
        return false;
    }

    *out = jstring_get(value);
    return true;
}

static bool json_int_in_range(jvalue_ref source, const char* key, int min, int max)
{
    jvalue_ref value = jobject_get(source, j_cstr_to_buffer(key));
    if (!value || !jis_number(value)) {
        return true;
    }

    int number;
    if (jnumber_get_i32(value, &number) != CONV_OK) {
        WARN("Settings: %s is not an integer", key);
        return false;
    }

    return int_in_range(key, number, min, max);
}

static bool settings_validate_json(settings_t* settings, jvalue_ref source)
{
    bool valid = true;
    raw_buffer str;

    if (json_string_field(source, "backend", &str)) {
        if (!settings_backend_allowed(str.m_str, settings_video_backends)) {
            WARN("Settings: unknown video backend: %s", str.m_str);
            valid = false;
        }
        jstring_free_buffer(str);
    }

    if (json_string_field(source, "uibackend", &str)) {
        if (!settings_backend_allowed(str.m_str, settings_ui_backends)) {
            WARN("Settings: unknown UI backend: %s", str.m_str);
            valid = false;
        }
        jstring_free_buffer(str);
    }

    // Validate the address against the unix-socket flag it will be used with
    bool unix_socket = settings->unix_socket;
    jvalue_ref value = jobject_get(source, j_cstr_to_buffer("unix-socket"));
    if (value && jis_boolean(value)) {
        jboolean_get(value, &unix_socket);
    }

    if (json_string_field(source, "address", &str)) {
        if (!settings_address_valid(str.m_str, unix_socket)) {
            WARN("Settings: invalid %s address", unix_socket ? "unix socket" : "IPv4");
            valid = false;
        }
        jstring_free_buffer(str);
    } else if (value && !settings_address_valid(settings->address, unix_socket)) {
        WARN("Settings: current address is not valid for the new unix-socket setting");
        valid = false;
    }

    valid = json_int_in_range(source, "port", 1, 65535) && valid;
    valid = json_int_in_range(source, "priority", 0, 255) && valid;
    valid = json_int_in_range(source, "fps", 0, 240) && valid;
    valid = json_int_in_range(source, "width", 16, 3840) && valid;
    valid = json_int_in_range(source, "height", 16, 2160) && valid;

    return valid;
}

void settings_init(settings_t* settings)
{
    settings->ui_backend = strdup("auto");
    settings->video_backend = strdup("auto");

    settings->address = strdup("");
    settings->port = 19400;
    settings->priority = 150;
    settings->unix_socket = false;

    settings->fps = 30;
    settings->width = 320;
    settings->height = 180;
    settings->quirks = 0;

    settings->send_nv12 = false;
    settings->no_video = false;
    settings->no_gui = false;
    settings->autostart = false;
    settings->vsync = true;

    settings->no_hdr = false;
    settings->no_powerstate = false;

    settings->dump_frames = false;
}

int settings_load_json(settings_t* settings, jvalue_ref source)
{
    jvalue_ref value;

    // All-or-nothing: reject the whole payload before touching any field
    if (!settings_validate_json(settings, source)) {
        return -1;
    }

    if ((value = jobject_get(source, j_cstr_to_buffer("backend"))) && jis_string(value)) {
        free(settings->video_backend);
        raw_buffer str = jstring_get(value);
        settings->video_backend = strdup(str.m_str);
        jstring_free_buffer(str);
    }

    if ((value = jobject_get(source, j_cstr_to_buffer("uibackend"))) && jis_string(value)) {
        free(settings->ui_backend);
        raw_buffer str = jstring_get(value);
        settings->ui_backend = strdup(str.m_str);
        jstring_free_buffer(str);
    }

    if ((value = jobject_get(source, j_cstr_to_buffer("address"))) && jis_string(value)) {
        free(settings->address);
        raw_buffer str = jstring_get(value);
        settings->address = strdup(str.m_str);
        jstring_free_buffer(str);
    }
    if ((value = jobject_get(source, j_cstr_to_buffer("port"))) && jis_number(value))
        jnumber_get_i32(value, &settings->port);
    if ((value = jobject_get(source, j_cstr_to_buffer("priority"))) && jis_number(value))
        jnumber_get_i32(value, &settings->priority);
    if ((value = jobject_get(source, j_cstr_to_buffer("unix-socket"))) && jis_boolean(value))
        jboolean_get(value, &settings->unix_socket);

    if ((value = jobject_get(source, j_cstr_to_buffer("fps"))) && jis_number(value))
        jnumber_get_i32(value, &settings->fps);
    if ((value = jobject_get(source, j_cstr_to_buffer("width"))) && jis_number(value))
        jnumber_get_i32(value, &settings->width);
    if ((value = jobject_get(source, j_cstr_to_buffer("height"))) && jis_number(value))
        jnumber_get_i32(value, &settings->height);
    if ((value = jobject_get(source, j_cstr_to_buffer("nv12"))) && jis_boolean(value))
        jboolean_get(value, &settings->send_nv12);
    if ((value = jobject_get(source, j_cstr_to_buffer("quirks"))) && jis_number(value))
        jnumber_get_i32(value, &settings->quirks);

    if ((value = jobject_get(source, j_cstr_to_buffer("vsync"))) && jis_boolean(value))
        jboolean_get(value, &settings->vsync);
    if ((value = jobject_get(source, j_cstr_to_buffer("novideo"))) && jis_boolean(value))
        jboolean_get(value, &settings->no_video);
    if ((value = jobject_get(source, j_cstr_to_buffer("nogui"))) && jis_boolean(value))
        jboolean_get(value, &settings->no_gui);
    if ((value = jobject_get(source, j_cstr_to_buffer("autostart"))) && jis_boolean(value))
        jboolean_get(value, &settings->autostart);

    if ((value = jobject_get(source, j_cstr_to_buffer("nohdr"))) && jis_boolean(value))
        jboolean_get(value, &settings->no_hdr);
    if ((value = jobject_get(source, j_cstr_to_buffer("nopowerstate"))) && jis_boolean(value))
        jboolean_get(value, &settings->no_powerstate);

    return 0;
}

int settings_save_json(settings_t* settings, jvalue_ref target)
{
    jobject_put(target, J_CSTR_TO_JVAL("backend"), jstring_create(settings->video_backend));
    jobject_put(target, J_CSTR_TO_JVAL("uibackend"), jstring_create(settings->ui_backend));

    jobject_put(target, J_CSTR_TO_JVAL("address"), jstring_create(settings->address));
    jobject_put(target, J_CSTR_TO_JVAL("port"), jnumber_create_i32(settings->port));
    jobject_put(target, J_CSTR_TO_JVAL("priority"), jnumber_create_i32(settings->priority));
    jobject_put(target, J_CSTR_TO_JVAL("unix-socket"), jboolean_create(settings->unix_socket));

    jobject_put(target, J_CSTR_TO_JVAL("fps"), jnumber_create_i32(settings->fps));
    jobject_put(target, J_CSTR_TO_JVAL("width"), jnumber_create_i32(settings->width));
    jobject_put(target, J_CSTR_TO_JVAL("height"), jnumber_create_i32(settings->height));
    jobject_put(target, J_CSTR_TO_JVAL("nv12"), jboolean_create(settings->send_nv12));
    jobject_put(target, J_CSTR_TO_JVAL("quirks"), jnumber_create_i32(settings->quirks));

    jobject_put(target, J_CSTR_TO_JVAL("vsync"), jboolean_create(settings->vsync));
    jobject_put(target, J_CSTR_TO_JVAL("novideo"), jboolean_create(settings->no_video));
    jobject_put(target, J_CSTR_TO_JVAL("nogui"), jboolean_create(settings->no_gui));
    jobject_put(target, J_CSTR_TO_JVAL("autostart"), jboolean_create(settings->autostart));

    jobject_put(target, J_CSTR_TO_JVAL("nohdr"), jboolean_create(settings->no_hdr));
    jobject_put(target, J_CSTR_TO_JVAL("nopowerstate"), jboolean_create(settings->no_powerstate));

    return 0;
}

int settings_load_file(settings_t* settings, char* source)
{
    int ret = 0;

    JSchemaInfo schema;
    jvalue_ref parsed;

    FILE* f = fopen(source, "rb");
    // File read failed
    if (f == NULL) {
        return -1;
    }

    long fsize = -1;
    if (fseek(f, 0, SEEK_END) == 0) {
        fsize = ftell(f);
    }
    if (fsize < 0 || fsize > SETTINGS_MAX_FILE_SIZE || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -3;
    }

    char* json = calloc(fsize + 1, 1);
    if (json == NULL) {
        fclose(f);
        return -3;
    }
    if (fsize > 0 && fread(json, fsize, 1, f) != 1) {
        free(json);
        fclose(f);
        return -3;
    }
    fclose(f);

    jschema_info_init(&schema, jschema_all(), NULL, NULL);
    parsed = jdom_parse(j_cstr_to_buffer(json), DOMOPT_NOOPT, &schema);

    // Parsing failed
    if (jis_null(parsed)) {
        free(json);
        j_release(&parsed);
        return -2;
    }

    ret = settings_load_json(settings, parsed);

    free(json);
    j_release(&parsed);
    return ret;
}

int settings_save_file(settings_t* settings, char* target)
{
    jvalue_ref jobj = jobject_create();
    int ret = settings_save_json(settings, jobj);

    if (ret != 0) {
        j_release(&jobj);
        return -2;
    }

    // Write to a temporary file and rename it over the target, so a crash or
    // power loss mid-write never leaves a truncated config behind
    char tmp_path[FILENAME_MAX];
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", target) >= (int)sizeof(tmp_path)) {
        j_release(&jobj);
        return -1;
    }

    FILE* fd = fopen(tmp_path, "wb");
    if (fd == NULL) {
        j_release(&jobj);
        return -1;
    }

    bool ok = fputs(jvalue_tostring_simple(jobj), fd) >= 0;
    ok = fflush(fd) == 0 && ok;
    ok = fsync(fileno(fd)) == 0 && ok;
    ok = fclose(fd) == 0 && ok;
    j_release(&jobj);

    if (!ok || rename(tmp_path, target) != 0) {
        unlink(tmp_path);
        return -1;
    }

    return 0;
}
