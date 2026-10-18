// lockdown_dir on a directory laid out like a fresh webOS install (0777 dir,
// 0666 files), plus the things it must not touch.
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "lockdown.h"

static char base[] = "/tmp/hwlock-XXXXXX";
static char dir[256];

static void make_file(const char* name, mode_t mode)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        if (write(fd, "x", 1) != 1) {
            perror("write");
        }
        close(fd);
    }
    chmod(path, mode);
}

static mode_t mode_of(const char* path)
{
    struct stat st;
    return lstat(path, &st) == 0 ? (st.st_mode & 07777) : (mode_t)-1;
}

static mode_t mode_in(const char* name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return mode_of(path);
}

int main(void)
{
    // umask would otherwise hide the world-writable modes we need to set up
    umask(0);
    CHECK(mkdtemp(base) != NULL, "temp dir");
    snprintf(dir, sizeof(dir), "%s/org.webosbrew.piccap.service", base);
    mkdir(dir, 0777);
    chmod(dir, 0777);

    make_file("hyperion-webos", 0777);
    make_file("libvtcapture_backend.so", 0666);
    make_file("libgm_backend.so", 0755);
    make_file("services.json", 0666);
    make_file("config.json", 0666);
    make_file("config.json.tmp", 0644);

    // Outside the service dir, reachable through a symlink inside it
    char outside[512];
    snprintf(outside, sizeof(outside), "%s/outside", base);
    int fd = open(outside, O_WRONLY | O_CREAT, 0666);
    close(fd);
    chmod(outside, 0666);
    char link[512];
    snprintf(link, sizeof(link), "%s/link", dir);
    CHECK(symlink(outside, link) == 0, "symlink planted");

    char sub[512];
    snprintf(sub, sizeof(sub), "%s/subdir", dir);
    mkdir(sub, 0777);
    chmod(sub, 0777);
    char fifo[512];
    snprintf(fifo, sizeof(fifo), "%s/fifo", dir);
    CHECK(mkfifo(fifo, 0666) == 0, "fifo created");

    // As root, hand some entries to nobody so the ownership fix is exercised
    char bin[512];
    snprintf(bin, sizeof(bin), "%s/hyperion-webos", dir);
    if (geteuid() == 0) {
        CHECK(chown(dir, 65534, 65534) == 0 && chown(bin, 65534, 65534) == 0, "entries given to nobody");
    }

    int changed = lockdown_dir(dir);
    CHECK(changed == 6, "six entries changed (dir, binary, .so, services.json, both config files)");
    CHECK(mode_of(dir) == 0755, "service dir 0755");
    CHECK(mode_in("hyperion-webos") == 0755, "binary 0755");
    CHECK(mode_in("libvtcapture_backend.so") == 0644, "non-executable library 0644");
    CHECK(mode_in("libgm_backend.so") == 0755, "executable library stays 0755");
    CHECK(mode_in("services.json") == 0644, "services.json 0644");
    CHECK(mode_in("config.json") == 0600, "config.json 0600");
    CHECK(mode_in("config.json.tmp") == 0600, "config.json.tmp 0600");
    CHECK(mode_of(outside) == 0666, "symlink target untouched");
    CHECK(mode_of(sub) == 0777, "subdirectory untouched");
    CHECK(mode_of(fifo) == 0666, "fifo untouched (and opening it didn't block)");

    if (geteuid() == 0) {
        struct stat st;
        CHECK(stat(dir, &st) == 0 && st.st_uid == 0 && st.st_gid == 0, "service dir owned by root again");
        CHECK(stat(bin, &st) == 0 && st.st_uid == 0 && st.st_gid == 0, "binary owned by root again");
    }

    CHECK(lockdown_dir(dir) == 0, "second run changes nothing");

    char missing[512];
    snprintf(missing, sizeof(missing), "%s/missing", base);
    CHECK(lockdown_dir(missing) == -1, "missing dir reported");
    char dir_link[512];
    snprintf(dir_link, sizeof(dir_link), "%s/dirlink", base);
    CHECK(symlink(dir, dir_link) == 0 && lockdown_dir(dir_link) == -1, "symlinked service dir refused");

    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", base);
    if (system(cmd) != 0) {
        perror("cleanup");
    }
    return check_summary();
}
