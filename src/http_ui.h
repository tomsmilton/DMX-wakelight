#pragma once

// Starts mDNS (wakelight.local) and the HTTP server that serves the UI and
// JSON API. Must be called after wifi_sntp_start() so the netif exists.
void http_ui_start(void);
