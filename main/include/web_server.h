#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <esp_http_server.h>

// Starts the web server and registers all URI handler
httpd_handle_t start_webserver(void);

// Stops the web server
void stop_webserver(httpd_handle_t server);

#endif // WEB_SERVER_H