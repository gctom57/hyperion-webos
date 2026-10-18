#pragma once

// Minimal test helpers: each test binary prints one PASS/FAIL line per check
// and exits non-zero when any check failed.
#include <stdio.h>

static int check_failures = 0;

#define CHECK(cond, msg)                                          \
    do {                                                          \
        if (cond) {                                               \
            printf("PASS %s\n", msg);                             \
        } else {                                                  \
            printf("FAIL %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            check_failures++;                                     \
        }                                                         \
    } while (0)

static inline int check_summary(void)
{
    if (check_failures) {
        printf("%d check(s) failed\n", check_failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
