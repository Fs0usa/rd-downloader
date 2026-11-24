#ifndef RD_HTTP_CLIENT_H
#define RD_HTTP_CLIENT_H

#include <stddef.h>

#define RD_HTTP_HOST_MAX 255U
#define RD_HTTP_PATH_MAX 4096U
#define RD_HTTP_LOCATION_MAX 2048U

typedef struct rd_http_url {
    char host[RD_HTTP_HOST_MAX + 1U];
    unsigned short port;
    char path[RD_HTTP_PATH_MAX];
} rd_http_url;

typedef struct rd_http_response_headers {
    int status_code;
    int has_content_length;
    unsigned long long content_length;
    int chunked;
    char location[RD_HTTP_LOCATION_MAX];
    char content_disposition[1024];
    char content_type[128];
} rd_http_response_headers;

typedef struct rd_http_probe_result {
    int status_code;
    int verified;
    int is_html;
    int has_content_length;
    unsigned long long content_length;
    char filename[512];
    char content_type[128];
} rd_http_probe_result;

typedef int (*rd_http_progress_fn)(unsigned long long downloaded,
                                   unsigned long long total, void *context);

/* Host-socket transport parser: plain HTTP only; rejects userinfo and IPv6 literals. */
int rd_http_parse_url(const char *text, rd_http_url *url);

/* Parse a complete, NUL-terminated header block (including CRLF CRLF). */
int rd_http_parse_response_headers(const char *text, size_t length,
                                   rd_http_response_headers *headers);

/* Sequential download; PS5 builds use libSceHttp/libSceSsl, host tests use sockets. */
int rd_http_download(const char *url, const char *directory, char *filename, size_t filename_capacity, unsigned long item_id,
                     rd_http_progress_fn progress, void *context,
                     char *error, size_t error_capacity);
int rd_http_probe(const char *url, rd_http_probe_result *result,
                  char *error, size_t error_capacity);
void rd_http_begin(unsigned long id);
int rd_http_set_handle(unsigned long id, int handle);
void rd_http_cancel(unsigned long id);
int rd_http_reset_legacy_partial(const char *url, const char *directory, unsigned long item_id);

#endif
