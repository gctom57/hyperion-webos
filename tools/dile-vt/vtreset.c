// Releases a DILE_VT capture registration left behind by a dead process.
//
// DILE_VT_Create (HAL_VT_Init) registers the VT capture unit with the kernel
// display driver under the caller's pid; only DILE_VT_Destroy
// (HAL_VT_Finalize) unregisters it, by the same pid. If that process died in
// between, every later DILE_VT_Create fails with "HAL_VT_Init error" until the
// TV reboots. This sends the same request HAL_VT_Finalize does, on behalf of
// the dead pid. hyperion-webos does this by itself for its own processes
// (/tmp/piccap-dilevt.pid); this tool is for other cases and for testing.
//
// Usage: vtreset <pid>    (the pid of the process that had capture open)
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <pid of the dead capture owner>\n", argv[0]);
        return 2;
    }
    long pid = strtol(argv[1], NULL, 10);
    if (pid <= 0) {
        fprintf(stderr, "invalid pid: %s\n", argv[1]);
        return 2;
    }
    if (kill((pid_t)pid, 0) == 0 || errno != ESRCH) {
        fprintf(stderr, "process %ld is still running; stop it instead\n", pid);
        return 1;
    }

    void* kadp = dlopen("libkadaptor.so.2", RTLD_NOW | RTLD_LOCAL);
    if (kadp == NULL) {
        fprintf(stderr, "can't load libkadaptor: %s\n", dlerror());
        return 1;
    }
    int (*de_open)(void) = NULL;
    int (*de_close)(void) = NULL;
    int (*de_set_vtm)(void*) = NULL;
    *(void**)(&de_open) = dlsym(kadp, "KADP_DE_Open");
    *(void**)(&de_close) = dlsym(kadp, "KADP_DE_Close");
    *(void**)(&de_set_vtm) = dlsym(kadp, "KADP_DE_SetVTM");
    if (de_open == NULL || de_close == NULL || de_set_vtm == NULL) {
        fprintf(stderr, "libkadaptor lacks KADP_DE_Open/Close/SetVTM\n");
        return 1;
    }

    int ret = de_open();
    if (ret != 0) {
        fprintf(stderr, "KADP_DE_Open failed: %d\n", ret);
        return 1;
    }
    // HAL_VT_Finalize's request: 0x70 bytes, pid at offset 4, enable = 0 at 8
    uint32_t vtm[0x70 / sizeof(uint32_t)] = { 0 };
    vtm[1] = (uint32_t)pid;
    ret = de_set_vtm(vtm);
    de_close();
    dlclose(kadp);

    printf("KADP_DE_SetVTM(enable=0, pid=%ld) -> %d\n", pid, ret);
    return ret == 0 ? 0 : 1;
}
