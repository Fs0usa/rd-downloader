#include "rd/http_client.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    rd_http_url url;
    rd_http_response_headers headers;
    const char response[] = "HTTP/1.1 200 OK\r\nContent-Length: 12345\r\n\r\n";
    const char redirect[] = "HTTP/1.1 302 Found\r\nLocation: /new/file.bin\r\n\r\n";
    const char chunked[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    const char duplicate[] = "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n";
    char error[128];

    assert(rd_http_parse_url("http://example.com", &url) == 0);
    assert(strcmp(url.host, "example.com") == 0 && url.port == 80 && strcmp(url.path, "/") == 0);
    assert(rd_http_parse_url("http://cdn.example:8081/a.zip?token=abc#frag", &url) == 0);
    assert(strcmp(url.host, "cdn.example") == 0 && url.port == 8081);
    assert(strcmp(url.path, "/a.zip?token=abc") == 0);
    assert(rd_http_parse_url("http://cdn.example?token=abc", &url) == 0);
    assert(strcmp(url.path, "/?token=abc") == 0);
    assert(rd_http_parse_url("https://example.com/file", &url) != 0);
    assert(rd_http_parse_url("http://user:pass@example.com/file", &url) != 0);
    assert(rd_http_parse_url("http://example.com:0/file", &url) != 0);
    assert(rd_http_parse_url("http://example.com/a\nb", &url) != 0);

    assert(rd_http_parse_response_headers(response, sizeof(response), &headers) == 0);
    assert(headers.status_code == 200 && headers.has_content_length);
    assert(headers.content_length == 12345 && !headers.chunked);
    assert(rd_http_parse_response_headers(redirect, sizeof(redirect), &headers) == 0);
    assert(headers.status_code == 302 && strcmp(headers.location, "/new/file.bin") == 0);
    assert(rd_http_parse_response_headers(chunked, sizeof(chunked), &headers) == 0);
    assert(headers.status_code == 200 && headers.chunked && !headers.has_content_length);
    assert(rd_http_parse_response_headers(duplicate, sizeof(duplicate), &headers) != 0);
    assert(rd_http_download("https://example.com/file", "/tmp", "file", 5, 1, NULL, NULL,
                            error, sizeof(error)) != 0);
    assert(strstr(error, "plain HTTP") != NULL);
    rd_http_begin(99);
    rd_http_cancel(99);
    assert(rd_http_set_handle(99, -1) != 0);
    rd_http_begin(99);
    assert(rd_http_set_handle(99, -1) == 0);
    puts("HTTP parser tests: OK");
    return 0;
}
