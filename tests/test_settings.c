// settings.c: validation, all-or-nothing loading, file handling, and a
// deterministic mutation pass checking that whatever gets accepted is valid.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "settings.h"

static void settings_free(settings_t* s)
{
    free(s->video_backend);
    free(s->ui_backend);
    free(s->address);
}

// Loads a JSON text into s. Returns the settings_load_json result, or -2 when
// the text is not JSON.
static int load_text(settings_t* s, const char* text)
{
    JSchemaInfo schema;
    jschema_info_init(&schema, jschema_all(), NULL, NULL);
    jvalue_ref parsed = jdom_parse(j_cstr_to_buffer(text), DOMOPT_NOOPT, &schema);
    int ret = jis_null(parsed) ? -2 : settings_load_json(s, parsed);
    j_release(&parsed);
    return ret;
}

static int load_fresh(const char* text, settings_t* out)
{
    settings_init(out);
    return load_text(out, text);
}

static void write_file(const char* path, const char* data, size_t len)
{
    FILE* f = fopen(path, "wb");
    if (f) {
        fwrite(data, 1, len, f);
        fclose(f);
    }
}

static void test_defaults_and_full_payload(void)
{
    settings_t s;
    settings_init(&s);
    CHECK(settings_valid(&s), "defaults are valid");

    int ret = load_text(&s, "{\"backend\":\"libvtcapture\",\"uibackend\":\"libhalgal\",\"address\":\"192.168.1.19\","
                            "\"port\":19401,\"priority\":100,\"unix-socket\":false,\"fps\":60,\"width\":640,\"height\":360,"
                            "\"nv12\":true,\"quirks\":3,\"vsync\":false,\"novideo\":true,\"nogui\":true,\"autostart\":true,"
                            "\"nohdr\":true,\"nopowerstate\":true}");
    CHECK(ret == 0, "full payload accepted");
    CHECK(strcmp(s.video_backend, "libvtcapture") == 0 && strcmp(s.ui_backend, "libhalgal") == 0, "backends loaded");
    CHECK(strcmp(s.address, "192.168.1.19") == 0 && s.port == 19401 && s.priority == 100, "receiver loaded");
    CHECK(s.fps == 60 && s.width == 640 && s.height == 360 && s.quirks == 3, "capture numbers loaded");
    CHECK(s.send_nv12 && !s.vsync && s.no_video && s.no_gui && s.autostart && s.no_hdr && s.no_powerstate, "flags loaded");
    CHECK(settings_valid(&s), "loaded settings valid");
    settings_free(&s);
}

static void test_rejections(void)
{
    static const struct {
        const char* json;
        const char* what;
    } bad[] = {
        { "{\"port\":0}", "port 0" },
        { "{\"port\":65536}", "port 65536" },
        { "{\"port\":1.5}", "fractional port" },
        { "{\"port\":1e12}", "huge port" },
        { "{\"priority\":-1}", "negative priority" },
        { "{\"priority\":256}", "priority 256" },
        { "{\"fps\":241}", "fps 241" },
        { "{\"width\":15}", "width 15" },
        { "{\"width\":3841}", "width 3841" },
        { "{\"height\":2161}", "height 2161" },
        { "{\"backend\":\"../../tmp/evil\"}", "video backend outside the allow list" },
        { "{\"uibackend\":\"libvtcapture\"}", "video backend as UI backend" },
        { "{\"address\":\"192.168.1\"}", "short IPv4" },
        { "{\"address\":\"example.com\"}", "host name" },
        { "{\"address\":\"192.168.1.19\",\"unix-socket\":true}", "IPv4 with unix socket" },
        { "{\"address\":\"relative/path\",\"unix-socket\":true}", "relative socket path" },
    };
    char msg[128];
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        settings_t s;
        snprintf(msg, sizeof(msg), "rejects %s", bad[i].what);
        CHECK(load_fresh(bad[i].json, &s) == -1, msg);
        settings_free(&s);
    }

    // sun_path is 108 bytes including the terminator
    char json[400];
    char path[200];
    memset(path, 'a', sizeof(path));
    path[0] = '/';
    path[108] = '\0';
    snprintf(json, sizeof(json), "{\"unix-socket\":true,\"address\":\"%s\"}", path);
    settings_t s;
    CHECK(load_fresh(json, &s) == -1, "rejects a socket path that doesn't fit sun_path");
    settings_free(&s);
    path[107] = '\0';
    snprintf(json, sizeof(json), "{\"unix-socket\":true,\"address\":\"%s\"}", path);
    CHECK(load_fresh(json, &s) == 0 && s.unix_socket, "accepts the longest socket path that fits");
    settings_free(&s);

    char* big = malloc(20000);
    memcpy(big, "{\"backend\":\"", 12);
    memset(big + 12, 'x', 19000);
    strcpy(big + 19012, "\"}");
    CHECK(load_fresh(big, &s) == -1 && strcmp(s.video_backend, "auto") == 0, "rejects a very long backend name");
    settings_free(&s);
    free(big);
}

static void test_all_or_nothing(void)
{
    settings_t s;
    settings_init(&s);
    CHECK(load_text(&s, "{\"address\":\"10.0.0.2\",\"port\":19400}") == 0, "first load");
    CHECK(load_text(&s, "{\"address\":\"10.0.0.3\",\"fps\":999}") == -1, "payload with one bad field rejected");
    CHECK(strcmp(s.address, "10.0.0.2") == 0 && s.fps == 30, "nothing applied from the rejected payload");

    CHECK(load_text(&s, "{\"unix-socket\":true}") == -1, "unix-socket without a matching address rejected");
    CHECK(!s.unix_socket, "unix-socket flag unchanged");

    CHECK(load_text(&s, "{\"port\":\"19444\",\"autostart\":\"yes\",\"address\":5}") == 0, "wrong types are ignored");
    CHECK(s.port == 19400 && !s.autostart && strcmp(s.address, "10.0.0.2") == 0, "ignored fields keep their value");

    CHECK(load_text(&s, "{\"address\":\"\"}") == 0 && s.address[0] == '\0', "empty address allowed (not configured)");
    settings_free(&s);
}

static void test_files(void)
{
    char dir[] = "/tmp/hwtest-XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "temp dir");
    char path[128];
    snprintf(path, sizeof(path), "%s/config.json", dir);

    settings_t a;
    settings_init(&a);
    free(a.address);
    a.address = strdup("192.168.1.19");
    a.port = 19401;
    a.fps = 0;
    a.autostart = true;
    a.quirks = 0x15;
    CHECK(settings_save_file(&a, path) == 0, "save");
    char tmp[160];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    CHECK(access(tmp, F_OK) != 0, "no temp file left after save");
    struct stat st;
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600, "saved config readable by its owner only");

    settings_t b;
    settings_init(&b);
    CHECK(settings_load_file(&b, path) == 0, "load what was saved");
    CHECK(strcmp(b.address, a.address) == 0 && b.port == a.port && b.fps == a.fps && b.autostart && b.quirks == a.quirks, "round trip keeps values");
    settings_free(&b);

    settings_init(&b);
    char missing[160];
    snprintf(missing, sizeof(missing), "%s/missing.json", dir);
    CHECK(settings_load_file(&b, missing) == -1, "missing file");
    write_file(path, "{not json", 9);
    CHECK(settings_load_file(&b, path) == -2, "invalid JSON");
    write_file(path, "", 0);
    CHECK(settings_load_file(&b, path) == -2, "empty file");
    char* huge = malloc(70 * 1024);
    memset(huge, ' ', 70 * 1024);
    huge[0] = '{';
    huge[70 * 1024 - 1] = '}';
    write_file(path, huge, 70 * 1024);
    free(huge);
    CHECK(settings_load_file(&b, path) == -3, "file over 64 KiB refused");
    CHECK(strcmp(b.address, "") == 0, "failed loads leave settings untouched");
    settings_free(&b);

    char nodir[160];
    snprintf(nodir, sizeof(nodir), "%s/nope/config.json", dir);
    CHECK(settings_save_file(&a, nodir) == -1, "save into a missing directory fails cleanly");
    settings_free(&a);

    unlink(path);
    rmdir(dir);
}

static uint32_t rng_state = 0x12345678u;
static uint32_t rnd(void)
{
    // xorshift32: fixed seed, so a failure reproduces on every run
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void test_mutations(void)
{
    static const char* seeds[] = {
        "{\"backend\":\"libvtcapture\",\"uibackend\":\"auto\",\"address\":\"192.168.1.19\",\"port\":19400,\"priority\":150,"
        "\"unix-socket\":false,\"fps\":30,\"width\":320,\"height\":180,\"nv12\":false,\"quirks\":0,\"vsync\":true}",
        "{\"unix-socket\":true,\"address\":\"/tmp/hyperhdr-domain\",\"width\":16,\"height\":2160,\"fps\":0}",
        "{\"address\":\"\",\"port\":65535,\"priority\":0,\"backend\":\"auto\",\"autostart\":true,\"nohdr\":false}",
    };
    static const char tokens[] = "{}[]\",:0123456789.-e/ tfnul\\";
    char buf[512];
    int accepted = 0;
    int invalid_accepted = 0;
    const int rounds = 20000;

    for (int i = 0; i < rounds; i++) {
        const char* seed = seeds[rnd() % 3];
        size_t len = strlen(seed);
        memcpy(buf, seed, len + 1);
        int edits = 1 + (int)(rnd() % 4);
        for (int e = 0; e < edits && len > 0; e++) {
            size_t pos = rnd() % len;
            switch (rnd() % 4) {
            case 0: // replace with a JSON-ish character
                buf[pos] = tokens[rnd() % (sizeof(tokens) - 1)];
                break;
            case 1: // replace with any byte except NUL
                buf[pos] = (char)(1 + rnd() % 255);
                break;
            case 2: // delete
                memmove(buf + pos, buf + pos + 1, len - pos);
                len--;
                break;
            default: // insert
                if (len + 2 < sizeof(buf)) {
                    memmove(buf + pos + 1, buf + pos, len - pos + 1);
                    buf[pos] = tokens[rnd() % (sizeof(tokens) - 1)];
                    len++;
                }
                break;
            }
        }

        settings_t s;
        if (load_fresh(buf, &s) == 0) {
            accepted++;
            if (!settings_valid(&s)) {
                invalid_accepted++;
                fprintf(stderr, "accepted but invalid: %s\n", buf);
            }
        }
        settings_free(&s);
    }

    printf("mutation rounds: %d, accepted: %d\n", rounds, accepted);
    CHECK(accepted > 0, "some mutated payloads are still accepted (the pass exercises loading)");
    CHECK(invalid_accepted == 0, "every accepted payload gives valid settings");
}

int main(void)
{
    test_defaults_and_full_payload();
    test_rejections();
    test_all_or_nothing();
    test_files();
    test_mutations();
    return check_summary();
}
