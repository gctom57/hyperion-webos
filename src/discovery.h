#pragma once

#include <stdbool.h>
#include <stddef.h>

// A Hyperion.NG or HyperHDR instance found on the network
typedef struct {
    char address[16]; // IPv4 the reply came from
    char name[64]; // instance name, may be empty
    char flavor[16]; // "HyperHDR" or "Hyperion"
    int flatbuffer_port; // 0 if not announced
    int json_port; // 0 if not announced
} receiver_info_t;

// Parses one SSDP M-SEARCH reply. Returns true for a Hyperion.NG/HyperHDR
// reply and fills `out` (address taken from sender_ip).
bool discovery_parse_reply(const char* reply, size_t len, const char* sender_ip, receiver_info_t* out);

// Searches the local network for receivers with SSDP for timeout_ms.
// Returns the number found (at most max, one per address), or -1 on error.
int discovery_search(receiver_info_t* out, int max, int timeout_ms);

// Tries a TCP connection to address:port within timeout_ms. Returns 0 when
// it connects, otherwise -errno with a short reason in err.
int discovery_test_tcp(const char* address, int port, int timeout_ms, char* err, size_t err_size);
