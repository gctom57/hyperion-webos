// Steps through LG's DILE_VT capture API one call at a time so we can see
// which lifecycle step leaves the capture hardware unusable.
// Usage: dvtest <steps>   c=create s=start t=stop d=destroy w=wait 1s
//                         k=exit now without cleanup (like SIGKILL)
// s/t are DILE_VT_Start/Stop, the cross-process VT lock, taken like
// hyperion-webos does (no waiting).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dile_vt.h"

int main(int argc, char** argv)
{
    DILE_VT_HANDLE vth = NULL;
    unsigned int wait_ms = 0;
    const char* steps = argc > 1 ? argv[1] : "cstd";

    setvbuf(stdout, NULL, _IONBF, 0);
    for (const char* p = steps; *p; p++) {
        switch (*p) {
        case 'c':
            vth = DILE_VT_Create(0);
            printf("create: %s\n", vth ? "OK" : "FAILED");
            if (!vth)
                return 1;
            break;
        case 's':
            printf("start: %d\n", DILE_VT_Start(&wait_ms));
            break;
        case 't':
            printf("stop: %d\n", DILE_VT_Stop());
            break;
        case 'd':
            DILE_VT_Destroy(vth);
            vth = NULL;
            printf("destroy: done\n");
            break;
        case 'w':
            sleep(1);
            break;
        case 'k':
            printf("exit without cleanup\n");
            _exit(0);
        }
    }
    return 0;
}
