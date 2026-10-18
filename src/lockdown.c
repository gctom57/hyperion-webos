#include "lockdown.h"
#include "log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool is_config_file(const char* name)
{
    return strcmp(name, "config.json") == 0 || strcmp(name, "config.json.tmp") == 0;
}

// Applies owner root:root and mode to an open file. Returns 1 if anything
// changed, 0 if it was already right, -1 on error.
static int fix_fd(int fd, const struct stat* st, mode_t mode, const char* name)
{
    int changed = 0;

    // Only root can give files to root; tests run unprivileged and skip it
    if (geteuid() == 0 && (st->st_uid != 0 || st->st_gid != 0)) {
        if (fchown(fd, 0, 0) != 0) {
            WARN("lockdown: chown %s failed: %s", name, strerror(errno));
            return -1;
        }
        changed = 1;
    }
    if ((st->st_mode & 07777) != mode) {
        if (fchmod(fd, mode) != 0) {
            WARN("lockdown: chmod %s failed: %s", name, strerror(errno));
            return -1;
        }
        INFO("lockdown: %s %04o -> %04o", name, (unsigned)(st->st_mode & 07777), (unsigned)mode);
        changed = 1;
    }
    return changed;
}

int lockdown_dir(const char* dir)
{
    int dir_fd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (dir_fd < 0) {
        WARN("lockdown: cannot open %s: %s", dir, strerror(errno));
        return -1;
    }

    int changed = 0;
    struct stat st;
    if (fstat(dir_fd, &st) == 0 && fix_fd(dir_fd, &st, 0755, ".") > 0) {
        changed++;
    }

    int list_fd = dup(dir_fd);
    DIR* d = list_fd >= 0 ? fdopendir(list_fd) : NULL;
    if (d == NULL) {
        if (list_fd >= 0) {
            close(list_fd);
        }
        close(dir_fd);
        return changed;
    }

    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        const char* name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }

        // O_NOFOLLOW: never chmod the target of a symlink planted here
        int fd = openat(dir_fd, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }
        if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
            mode_t mode;
            if (is_config_file(name)) {
                mode = 0600;
            } else if (st.st_mode & 0111) {
                mode = 0755;
            } else {
                mode = 0644;
            }
            if (fix_fd(fd, &st, mode, name) > 0) {
                changed++;
            }
        }
        close(fd);
    }

    closedir(d);
    close(dir_fd);
    return changed;
}
