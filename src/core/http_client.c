#define _POSIX_C_SOURCE 200809L
#include "rd/http_client.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>

int rd_http_parse_url(const char *text, rd_http_url *url) {
    static const char scheme[] = "http://";
    const char *authority, *authority_end, *path, *fragment;
    size_t host_length, path_length;
    unsigned long port = 80;
    char *port_end;
    const char *colon;
    size_t i;

    if (!text || !url || strncasecmp(text, scheme, sizeof(scheme) - 1U) != 0) return -1;
    for (i = 0; text[i]; ++i)
        if ((unsigned char)text[i] <= 0x20 || (unsigned char)text[i] == 0x7f) return -1;
    authority = text + sizeof(scheme) - 1U;
    authority_end = authority + strcspn(authority, "/?#");
    if (authority_end == authority || memchr(authority, '@', (size_t)(authority_end - authority)) ||
        memchr(authority, '[', (size_t)(authority_end - authority)) ||
        memchr(authority, ']', (size_t)(authority_end - authority))) return -1;

    colon = memchr(authority, ':', (size_t)(authority_end - authority));
    host_length = colon ? (size_t)(colon - authority) : (size_t)(authority_end - authority);
    if (host_length == 0 || host_length > RD_HTTP_HOST_MAX) return -1;
    for (i = 0; i < host_length; ++i) {
        unsigned char c = (unsigned char)authority[i];
        if (isspace(c) || c < 0x21 || c == '/' || c == '\\') return -1;
    }
    if (colon) {
        if (colon + 1 == authority_end) return -1;
        errno = 0;
        port = strtoul(colon + 1, &port_end, 10);
        if (errno || port_end != authority_end || port == 0 || port > 65535) return -1;
    }

    path = authority_end;
    fragment = strchr(path, '#');
    if (fragment) path_length = (size_t)(fragment - path);
    else path_length = strlen(path);
    if (path_length == 0) {
        path = "/";
        path_length = 1;
    } else if (*path == '?') {
        if (path_length + 1U >= sizeof(url->path)) return -1;
        memmove(url->path + 1, path, path_length);
        url->path[0] = '/';
        url->path[path_length + 1U] = '\0';
        memcpy(url->host, authority, host_length);
        url->host[host_length] = '\0';
        url->port = (unsigned short)port;
        return 0;
    }
    if (path_length >= sizeof(url->path)) return -1;
    memcpy(url->path, path, path_length);
    url->path[path_length] = '\0';
    memcpy(url->host, authority, host_length);
    url->host[host_length] = '\0';
    url->port = (unsigned short)port;
    return 0;
}

static int parse_decimal_u64(const char *value, size_t length,
                             unsigned long long *out) {
    unsigned long long result = 0;
    size_t i;
    if (length == 0) return -1;
    for (i = 0; i < length; ++i) {
        unsigned int digit;
        if (value[i] < '0' || value[i] > '9') return -1;
        digit = (unsigned int)(value[i] - '0');
        if (result > (ULLONG_MAX - digit) / 10ULL) return -1;
        result = result * 10ULL + digit;
    }
    *out = result;
    return 0;
}

int rd_http_parse_response_headers(const char *text, size_t length,
                                   rd_http_response_headers *headers) {
    const char *line_end, *cursor, *header_end;
    unsigned long long content_length = 0;
    int has_content_length = 0, chunked = 0;
    if (!text || !headers || length < 12U || text[length - 1U] != '\0') return -1;
    header_end = strstr(text, "\r\n\r\n");
    if (!header_end || (size_t)(header_end - text) + 4U >= length) return -1;
    line_end = strstr(text, "\r\n");
    if (!line_end || (size_t)(line_end - text) < 12U ||
        (strncmp(text, "HTTP/1.0 ", 9U) && strncmp(text, "HTTP/1.1 ", 9U)) ||
        !isdigit((unsigned char)text[9]) || !isdigit((unsigned char)text[10]) ||
        !isdigit((unsigned char)text[11]) || (text[12] != ' ' && text[12] != '\r')) return -1;
    memset(headers, 0, sizeof(*headers));
    headers->status_code = (text[9] - '0') * 100 + (text[10] - '0') * 10 + (text[11] - '0');

    cursor = line_end + 2;
    while (cursor < header_end) {
        const char *end = strstr(cursor, "\r\n");
        const char *colon;
        const char *value;
        size_t name_length, value_length;
        if (!end || end > header_end || (colon = memchr(cursor, ':', (size_t)(end - cursor))) == NULL)
            return -1;
        name_length = (size_t)(colon - cursor);
        value = colon + 1;
        while (value < end && (*value == ' ' || *value == '\t')) ++value;
        value_length = (size_t)(end - value);
        while (value_length && (value[value_length - 1U] == ' ' || value[value_length - 1U] == '\t')) --value_length;
        if (name_length == 14U && strncasecmp(cursor, "Content-Length", name_length) == 0) {
            if (has_content_length || parse_decimal_u64(value, value_length, &content_length) != 0)
                return -1;
            has_content_length = 1;
        } else if (name_length == 17U && strncasecmp(cursor, "Transfer-Encoding", name_length) == 0) {
            if (value_length != 7U || strncasecmp(value, "chunked", 7U) != 0) return -1;
            chunked = 1;
        } else if (name_length == 8U && strncasecmp(cursor, "Location", name_length) == 0) {
            if (value_length == 0 || value_length >= sizeof(headers->location)) return -1;
            memcpy(headers->location, value, value_length);
            headers->location[value_length] = '\0';
        }
        cursor = end + 2;
    }
    if (has_content_length && chunked) return -1;
    headers->has_content_length = has_content_length;
    headers->content_length = content_length;
    headers->chunked = chunked;
    return 0;
}

#define RD_HTTP_HEADER_LIMIT 16384U
#define RD_HTTP_IO_SIZE 65536U
#define RD_HTTP_REDIRECT_LIMIT 5U
#define RD_HTTP_SPACE_MARGIN (16ULL * 1024ULL * 1024ULL)

typedef struct rd_socket_reader {
    int fd;
    unsigned char buffer[RD_HTTP_IO_SIZE];
    size_t position;
    size_t available;
} rd_socket_reader;

static void set_error(char *error, size_t capacity, const char *message) {
    if (error && capacity) {
        snprintf(error, capacity, "%s", message);
        error[capacity - 1U] = '\0';
    }
}

static int socket_send_all(int fd, const char *data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
#ifdef MSG_NOSIGNAL
        ssize_t n = send(fd, data + sent, length - sent, MSG_NOSIGNAL);
#else
        ssize_t n = send(fd, data + sent, length - sent, 0);
#endif
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int connect_http(const rd_http_url *url) {
    struct addrinfo hints, *addresses = NULL, *address;
    char service[8];
    int fd = -1;
    struct timeval timeout = { .tv_sec = 30, .tv_usec = 0 };
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof(service), "%u", (unsigned int)url->port);
    if (getaddrinfo(url->host, service, &hints, &addresses) != 0) return -1;
    for (address = addresses; address; address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) continue;
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#if defined(__FreeBSD__) && !defined(MSG_NOSIGNAL)
        {
            int one = 1;
            (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        }
#endif
        if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    return fd;
}

static int reader_fill(rd_socket_reader *reader) {
    ssize_t n;
    do {
        n = recv(reader->fd, reader->buffer, sizeof(reader->buffer), 0);
    } while (n < 0 && errno == EINTR);
    if (n <= 0) return (int)n;
    reader->position = 0;
    reader->available = (size_t)n;
    return 1;
}

static ssize_t reader_read(rd_socket_reader *reader, void *buffer, size_t capacity) {
    size_t copied = 0;
    while (copied < capacity) {
        size_t available, take;
        if (reader->position == reader->available) {
            int result = reader_fill(reader);
            if (result < 0) return copied ? (ssize_t)copied : -1;
            if (result == 0) return (ssize_t)copied;
        }
        available = reader->available - reader->position;
        take = capacity - copied < available ? capacity - copied : available;
        memcpy((unsigned char *)buffer + copied, reader->buffer + reader->position, take);
        reader->position += take;
        copied += take;
    }
    return (ssize_t)copied;
}

static int reader_exact(rd_socket_reader *reader, void *buffer, size_t length) {
    size_t received = 0;
    while (received < length) {
        ssize_t n = reader_read(reader, (unsigned char *)buffer + received, length - received);
        if (n <= 0) return -1;
        received += (size_t)n;
    }
    return 0;
}

static int reader_line(rd_socket_reader *reader, char *line, size_t capacity) {
    size_t used = 0;
    while (used + 1U < capacity) {
        unsigned char c;
        if (reader_exact(reader, &c, 1U) != 0) return -1;
        line[used++] = (char)c;
        if (used >= 2U && line[used - 2U] == '\r' && line[used - 1U] == '\n') {
            line[used - 2U] = '\0';
            return 0;
        }
        if (c == '\n' && (used < 2U || line[used - 2U] != '\r')) return -1;
    }
    return -1;
}

static int write_all(int fd, const unsigned char *data, size_t length) {
    size_t written = 0;
    while (written < length) {
        ssize_t n = write(fd, data + written, length - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        written += (size_t)n;
    }
    return 0;
}

static int append_redirect(const char *current, const char *location,
                           char *next, size_t capacity) {
    rd_http_url parsed;
    char origin[RD_HTTP_HOST_MAX + 16U];
    char path[RD_HTTP_PATH_MAX];
    const char *path_end;
    size_t path_length;
    int n;
    if (strncasecmp(location, "http://", 7U) == 0) {
        if (strlen(location) >= capacity) return -1;
        strcpy(next, location);
        return 0;
    }
    if (strncasecmp(location, "https://", 8U) == 0) return -2;
    if (strncmp(location, "//", 2U) == 0) {
        int absolute_length = snprintf(next, capacity, "http:%s", location);
        return absolute_length < 0 || (size_t)absolute_length >= capacity ? -1 : 0;
    }
    if (rd_http_parse_url(current, &parsed) != 0) return -1;
    if (parsed.port == 80) n = snprintf(origin, sizeof(origin), "http://%s", parsed.host);
    else n = snprintf(origin, sizeof(origin), "http://%s:%u", parsed.host, (unsigned int)parsed.port);
    if (n < 0 || (size_t)n >= sizeof(origin)) return -1;
    if (location[0] == '/') {
        n = snprintf(next, capacity, "%s%s", origin, location);
        return n < 0 || (size_t)n >= capacity ? -1 : 0;
    }
    path_end = strpbrk(parsed.path, "?#");
    path_length = path_end ? (size_t)(path_end - parsed.path) : strlen(parsed.path);
    if (path_length >= sizeof(path)) return -1;
    memcpy(path, parsed.path, path_length);
    path[path_length] = '\0';
    if (location[0] == '?') {
        n = snprintf(next, capacity, "%s%s%s", origin, path, location);
        return n < 0 || (size_t)n >= capacity ? -1 : 0;
    }
    path_end = strrchr(path, '/');
    if (path_end) path_length = (size_t)(path_end - path) + 1U;
    else { path[0] = '/'; path[1] = '\0'; path_length = 1U; }
    path[path_length] = '\0';
    n = snprintf(next, capacity, "%s%s%s", origin, path, location);
    return n < 0 || (size_t)n >= capacity ? -1 : 0;
}

static int make_filename(const rd_http_url *url, unsigned long id,
                         char *name, size_t capacity) {
    char decoded[RD_HTTP_PATH_MAX];
    const char *end = strpbrk(url->path, "?#");
    const char *base;
    size_t length = end ? (size_t)(end - url->path) : strlen(url->path);
    size_t used = 0, i;
    int n;
    if (length >= sizeof(decoded)) return -1;
    memcpy(decoded, url->path, length);
    decoded[length] = '\0';
    base = strrchr(decoded, '/');
    base = base ? base + 1 : decoded;
    for (i = 0; base[i] && used < 120U; ++i) {
        unsigned char c = (unsigned char)base[i];
        if (c == '%' && isxdigit((unsigned char)base[i + 1U]) &&
            isxdigit((unsigned char)base[i + 2U])) {
            char hex[3] = { base[i + 1U], base[i + 2U], '\0' };
            c = (unsigned char)strtoul(hex, NULL, 16);
            i += 2U;
        }
        if (c == '/' || c == '\\' || c == ':' || c < 0x20U || c == 0x7fU) c = '_';
        if (c < 0x80U && !(isalnum(c) || c == '.' || c == '_' || c == '-')) c = '_';
        name[used++] = (char)c;
    }
    while (used && name[used - 1U] == '.') --used;
    if (!used || (used == 1U && name[0] == '.') ||
        (used == 2U && name[0] == '.' && name[1] == '.')) {
        memcpy(name, "download.bin", 12U);
        used = 12U;
    }
    name[used] = '\0';
    n = snprintf(decoded, sizeof(decoded), "rd-%lu-%s", id, name);
    if (n < 0 || (size_t)n >= sizeof(decoded) || (size_t)n >= capacity) return -1;
    strcpy(name, decoded);
    return 0;
}

static int read_headers(rd_socket_reader *reader, char *buffer, size_t capacity,
                        size_t *header_length) {
    size_t used = 0;
    while (used + 1U < capacity) {
        unsigned char c;
        if (reader_exact(reader, &c, 1U) != 0) return -1;
        buffer[used++] = (char)c;
        if (used >= 4U && memcmp(buffer + used - 4U, "\r\n\r\n", 4U) == 0) {
            buffer[used] = '\0';
            *header_length = used + 1U;
            return 0;
        }
    }
    return -1;
}

static int receive_to_file(rd_socket_reader *reader, int file_fd,
                           const rd_http_response_headers *headers,
                           unsigned long long *received,
                           rd_http_progress_fn progress, void *context) {
    unsigned char buffer[RD_HTTP_IO_SIZE];
    unsigned long long total = 0;
    if (headers->chunked) {
        for (;;) {
            char line[128], *end;
            unsigned long long chunk_size;
            if (reader_line(reader, line, sizeof(line)) != 0 || line[0] == '-') return -1;
            errno = 0;
            chunk_size = strtoull(line, &end, 16);
            if (errno || end == line || !isxdigit((unsigned char)line[0]) ||
                (*end != '\0' && *end != ';') || total > ULLONG_MAX - chunk_size) return -1;
            if (chunk_size == 0) {
                unsigned int trailer_bytes = 0;
                do {
                    if (reader_line(reader, line, sizeof(line)) != 0) return -1;
                    trailer_bytes += (unsigned int)strlen(line) + 2U;
                    if (trailer_bytes > RD_HTTP_HEADER_LIMIT) return -1;
                } while (line[0]);
                break;
            }
            while (chunk_size) {
                size_t take = chunk_size < sizeof(buffer) ? (size_t)chunk_size : sizeof(buffer);
                if (reader_exact(reader, buffer, take) != 0 || write_all(file_fd, buffer, take) != 0) return -1;
                total += take;
                chunk_size -= take;
                if (progress && progress(total, headers->has_content_length ? headers->content_length : 0, context) != 0) return -1;
            }
            {
                char crlf[2];
                if (reader_exact(reader, crlf, sizeof(crlf)) != 0 || crlf[0] != '\r' || crlf[1] != '\n') return -1;
            }
        }
    } else if (headers->has_content_length) {
        unsigned long long remaining = headers->content_length;
        while (remaining) {
            size_t take = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
            if (reader_exact(reader, buffer, take) != 0 || write_all(file_fd, buffer, take) != 0) return -1;
            total += take;
            remaining -= take;
            if (progress && progress(total, headers->content_length, context) != 0) return -1;
        }
    } else {
        for (;;) {
            ssize_t n = reader_read(reader, buffer, sizeof(buffer));
            if (n < 0) return -1;
            if (n == 0) break;
            if (write_all(file_fd, buffer, (size_t)n) != 0) return -1;
            total += (unsigned long long)n;
            if (progress && progress(total, 0, context) != 0) return -1;
        }
    }
    *received = total;
    return 0;
}

int rd_http_download(const char *input_url, const char *directory, unsigned long item_id,
                     rd_http_progress_fn progress, void *context,
                     char *error, size_t error_capacity) {
    char url_text[RD_HTTP_PATH_MAX + RD_HTTP_HOST_MAX + 32U];
    char redirect_url[sizeof(url_text)];
    char header_buffer[RD_HTTP_HEADER_LIMIT + 1U];
    char filename[192], final_path[768], part_path[784];
    rd_http_url url;
    unsigned int redirects;
    int fd = -1, file_fd = -1, result = -1;
    struct stat st;
    struct statvfs space;
    unsigned long long received = 0;

    set_error(error, error_capacity, "download failed");
    if (!input_url || !directory || item_id == 0 || strlen(input_url) >= sizeof(url_text)) {
        set_error(error, error_capacity, "invalid URL or destination");
        return -1;
    }
    if (rd_http_parse_url(input_url, &url) != 0) {
        set_error(error, error_capacity, "only valid plain HTTP URLs are supported by this MVP transport");
        return -1;
    }
    strcpy(url_text, input_url);
    if (stat(directory, &st) != 0 || !S_ISDIR(st.st_mode) || access(directory, W_OK) != 0) {
        set_error(error, error_capacity, "destination is missing or not writable");
        return -1;
    }

    for (redirects = 0; redirects <= RD_HTTP_REDIRECT_LIMIT; ++redirects) {
        rd_socket_reader reader;
        rd_http_response_headers headers;
        char request[RD_HTTP_HOST_MAX + RD_HTTP_PATH_MAX + 512U];
        char host_header[RD_HTTP_HOST_MAX + 16U];
        size_t header_length;
        int request_length;
        fd = connect_http(&url);
        if (fd < 0) { set_error(error, error_capacity, "DNS lookup or TCP connection failed"); goto done; }
        if (url.port == 80) snprintf(host_header, sizeof(host_header), "%s", url.host);
        else snprintf(host_header, sizeof(host_header), "%s:%u", url.host, (unsigned int)url.port);
        request_length = snprintf(request, sizeof(request),
            "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: RDDownloader/0.1\r\n"
            "Accept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",
            url.path, host_header);
        if (request_length < 0 || (size_t)request_length >= sizeof(request) ||
            socket_send_all(fd, request, (size_t)request_length) != 0) {
            set_error(error, error_capacity, "failed to send HTTP request");
            close(fd); fd = -1; goto done;
        }
        reader.fd = fd; reader.position = 0; reader.available = 0;
        if (read_headers(&reader, header_buffer, sizeof(header_buffer), &header_length) != 0 ||
            rd_http_parse_response_headers(header_buffer, header_length, &headers) != 0) {
            set_error(error, error_capacity, "invalid or oversized HTTP response headers");
            close(fd); fd = -1; goto done;
        }
        if (headers.status_code == 301 || headers.status_code == 302 ||
            headers.status_code == 303 || headers.status_code == 307 || headers.status_code == 308) {
            int redirect_result;
            close(fd); fd = -1;
            if (redirects == RD_HTTP_REDIRECT_LIMIT || !headers.location[0]) {
                set_error(error, error_capacity, "too many redirects or missing Location header");
                goto done;
            }
            redirect_result = append_redirect(url_text, headers.location,
                                              redirect_url, sizeof(redirect_url));
            if (redirect_result != 0 || rd_http_parse_url(redirect_url, &url) != 0) {
                set_error(error, error_capacity,
                          redirect_result == -2 ? "HTTPS redirect refused: safe TLS transport is not available" :
                          "redirect target is invalid or unsupported");
                goto done;
            }
            strcpy(url_text, redirect_url);
            continue;
        }
        if (headers.status_code != 200) {
            char message[64];
            snprintf(message, sizeof(message), "server returned HTTP %d", headers.status_code);
            set_error(error, error_capacity, message);
            close(fd); fd = -1; goto done;
        }
        if (make_filename(&url, item_id, filename, sizeof(filename)) != 0 ||
            snprintf(final_path, sizeof(final_path), "%s/%s", directory, filename) >= (int)sizeof(final_path) ||
            snprintf(part_path, sizeof(part_path), "%s.part", final_path) >= (int)sizeof(part_path)) {
            set_error(error, error_capacity, "filename or output path is too long");
            close(fd); fd = -1; goto done;
        }
        if (headers.has_content_length && headers.content_length > 0) {
            unsigned long long block_size, available;
            if (statvfs(directory, &space) != 0) {
                set_error(error, error_capacity, "cannot verify destination free space");
                close(fd); fd = -1; goto done;
            }
            block_size = space.f_frsize ? (unsigned long long)space.f_frsize :
                         (unsigned long long)space.f_bsize;
            if (block_size == 0 || space.f_bavail > ULLONG_MAX / block_size) {
                set_error(error, error_capacity, "destination free-space value is invalid");
                close(fd); fd = -1; goto done;
            }
            available = (unsigned long long)space.f_bavail * block_size;
            if (available < headers.content_length ||
                available - headers.content_length < RD_HTTP_SPACE_MARGIN) {
                set_error(error, error_capacity, "not enough free space for file and safety margin");
                close(fd); fd = -1; goto done;
            }
        }
        if (lstat(final_path, &st) == 0 || errno != ENOENT) {
            set_error(error, error_capacity, "output file already exists; refusing to overwrite it");
            close(fd); fd = -1; goto done;
        }
        file_fd = open(part_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (file_fd < 0) {
            set_error(error, error_capacity, errno == EEXIST ?
                      "partial file already exists; preserving it" : "cannot create partial output file");
            close(fd); fd = -1; goto done;
        }
        if (progress && progress(0, headers.has_content_length ? headers.content_length : 0,
                                 context) != 0) {
            set_error(error, error_capacity, "download was stopped; partial file kept");
            close(fd); fd = -1;
            close(file_fd); file_fd = -1;
            goto done;
        }
        if (receive_to_file(&reader, file_fd, &headers, &received, progress, context) != 0 ||
            (headers.has_content_length && received != headers.content_length)) {
            set_error(error, error_capacity, "transfer interrupted or byte count did not match Content-Length; partial file kept");
            close(fd); fd = -1;
            close(file_fd); file_fd = -1;
            goto done;
        }
        {
            int sync_error = fsync(file_fd);
            int close_error = close(file_fd);
            file_fd = -1;
            if (sync_error != 0 || close_error != 0) {
            set_error(error, error_capacity, "could not safely flush downloaded file; partial file kept");
            close(fd); fd = -1; goto done;
            }
        }
        close(fd); fd = -1;
        if (lstat(final_path, &st) == 0 || errno != ENOENT || rename(part_path, final_path) != 0) {
            set_error(error, error_capacity, "final file exists or could not be atomically published; partial file kept");
            goto done;
        }
        result = 0;
        goto done;
    }
done:
    if (fd >= 0) close(fd);
    if (file_fd >= 0) close(file_fd);
    return result;
}
