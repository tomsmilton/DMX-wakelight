#pragma once

#include <stdbool.h>

// Connect to WiFi STA (blocking until first connect or timeout) and start SNTP.
// Initialises NVS + netif + event loop the first time it's called.
// Returns true on successful WiFi association.
bool wifi_sntp_start(void);

// True once system time has been set from SNTP at least once.
bool wifi_sntp_time_valid(void);
