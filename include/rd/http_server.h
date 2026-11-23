#ifndef RD_HTTP_SERVER_H
#define RD_HTTP_SERVER_H

#define RD_HTTP_DEFAULT_PORT 8090

/* Runs a small read-only HTTP health service until the process is stopped. */
int rd_http_server_run(const char *bind_address, unsigned short port);

#endif
