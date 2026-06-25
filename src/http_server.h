#pragma once

// Start the setup wizard. networks_html must remain valid for the lifetime of the server
// (point it at the static buffer returned by wifi_start_setup()).
void http_server_start_setup(const char *networks_html);

// Start the captive portal server.
void http_server_start_portal(void);
