#pragma once

#include <stddef.h>

// Tries a TCP connection to address:port within timeout_ms. Returns 0 when
// it connects, otherwise -errno with a short reason in err.
int receiver_test_tcp(const char* address, int port, int timeout_ms, char* err, size_t err_size);
