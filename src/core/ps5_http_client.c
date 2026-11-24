#define _POSIX_C_SOURCE 200809L
#include "rd/http_client.h"
#include "rd/output.h"
#include "rd/log.h"

#ifndef RD_HOST_BUILD

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "ca_bundle_pem.inc"

#define RD_SCE_HTTP_VERSION_1_1 2
#define RD_SCE_HTTP_POOL_SIZE (256U * 1024U)
#define RD_SCE_NET_POOL_SIZE (32U * 1024U)
#define RD_SCE_HTTP_TIMEOUT_US (30U * 1000U * 1000U)
#define RD_SCE_HTTP_HEADER_LIMIT (32U * 1024U)
#define RD_SCE_IO_SIZE (64U * 1024U)
#define RD_SCE_SPACE_MARGIN (16ULL * 1024ULL * 1024ULL)
#define RD_SCE_REDIRECT_LIMIT 5U

static int rd_sce_net_initialized;

typedef struct rd_sce_ssl_data {
    char *ptr;
    size_t size;
} rd_sce_ssl_data;

int sceNetInit(void);
int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetPoolDestroy(int pool_id);
int sceSslInit(size_t pool_size);
int sceSslTerm(int ssl_id);
int sceSslLoadCert(int ssl_id, int ca_count, const rd_sce_ssl_data **ca_list,
                   const rd_sce_ssl_data *cert, const rd_sce_ssl_data *private_key);
int sceSslUnloadCert(int ssl_id);
int sceHttpInit(int net_pool_id, int ssl_id, size_t pool_size);
int sceHttpTerm(int http_id);
int sceHttpCreateTemplate(int http_id, const char *user_agent,
                          int http_version, int auto_proxy_config);
int sceHttpDeleteTemplate(int template_id);
int sceHttpCreateConnectionWithURL(int template_id, const char *url,
                                   int enable_keepalive);
int sceHttpDeleteConnection(int connection_id);
int sceHttpCreateRequestWithURL2(int connection_id, const char *method,
                                 const char *url, uint64_t content_length);
int sceHttpDeleteRequest(int request_id);
int sceHttpAddRequestHeader(int request_id, const char *name,
                            const char *value, int mode);
int sceHttpSendRequest(int request_id, const void *data, size_t size);
int sceHttpGetStatusCode(int request_id, int *status_code);
int sceHttpGetAllResponseHeaders(int request_id, char **headers,
                                 size_t *headers_size);
int sceHttpReadData(int request_id, void *buffer, size_t size);
int sceHttpSetAutoRedirect(int id, int enabled);
int sceHttpSetResponseHeaderMaxSize(int id, size_t size);
int sceHttpSetResolveTimeOut(int id, uint32_t timeout_us);
int sceHttpSetConnectTimeOut(int id, uint32_t timeout_us);
int sceHttpSetSendTimeOut(int id, uint32_t timeout_us);
int sceHttpSetRecvTimeOut(int id, uint32_t timeout_us);
int sceHttpsEnableOption(int id, uint32_t flags);

static void set_error(char *error, size_t capacity, const char *message) {
    if (error && capacity) {
        snprintf(error, capacity, "%s", message);
        error[capacity - 1U] = '\0';
    }
}

static int probe_directory_write(const char *directory, unsigned long item_id,
                                 char *error, size_t error_capacity) {
    char path[RD_HTTP_PATH_MAX];
    unsigned char byte = 0;
    int fd, saved_errno = 0;
    int length = snprintf(path, sizeof(path), "%s/.rd-write-probe-%lu-XXXXXX",
                          directory, item_id);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        set_error(error, error_capacity, "destination probe path is too long");
        return -1;
    }
    fd = mkstemp(path);
    if (fd < 0) {
        if (error && error_capacity)
            snprintf(error, error_capacity,
                     "cannot create destination write probe (errno=%d mode/owner checked at startup)",
                     errno);
        return -1;
    }
    errno = 0;
    if (write(fd, &byte, sizeof(byte)) != (ssize_t)sizeof(byte)) saved_errno = errno ? errno : EIO;
    if (close(fd) != 0 && !saved_errno) saved_errno = errno;
    if (unlink(path) != 0 && !saved_errno) saved_errno = errno;
    if (saved_errno) {
        if (error && error_capacity)
            snprintf(error, error_capacity, "destination write probe failed (errno=%d)", saved_errno);
        errno = saved_errno;
        return -1;
    }
    return 0;
}

static int ensure_download_directory(const char *directory,
                                     char *error, size_t error_capacity) {
    struct stat st;
    int created = 0;
    if (mkdir(directory, 0755) == 0) created = 1;
    else if (errno != EEXIST) {
        if (error && error_capacity)
            snprintf(error, error_capacity, "cannot create destination directory (errno=%d)", errno);
        return -1;
    }
    if (lstat(directory, &st) != 0) {
        if (error && error_capacity)
            snprintf(error, error_capacity, "destination stat failed (errno=%d)", errno);
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        set_error(error, error_capacity, "destination is not a directory");
        errno = ENOTDIR;
        return -1;
    }
    if (chmod(directory, 0755) != 0) {
        if (error && error_capacity)
            snprintf(error, error_capacity, "cannot set destination directory mode (errno=%d)", errno);
        return -1;
    }
    if (created)
        rd_log_write("INFO", "download engine recreated destination directory=%s mode=0755",
                     directory);
    return 0;
}

static int write_all(int fd, const void *data, size_t length) {
    const unsigned char *cursor = (const unsigned char *)data;
    while (length) {
        ssize_t written = write(fd, cursor, length);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        cursor += (size_t)written;
        length -= (size_t)written;
    }
    return 0;
}

static int valid_http_url(const char *url) {
    const char *authority, *end;
    size_t i;
    if (!url || (strncasecmp(url, "http://", 7U) &&
                 strncasecmp(url, "https://", 8U))) return 0;
    authority = strstr(url, "://") + 3;
    end = authority + strcspn(authority, "/?#");
    if (end == authority || memchr(authority, '@', (size_t)(end - authority))) return 0;
    for (i = 0; url[i]; ++i)
        if ((unsigned char)url[i] <= 0x20U || (unsigned char)url[i] == 0x7fU)
            return 0;
    return 1;
}

static int resolve_redirect(const char *current, const char *location,
                            char *resolved, size_t capacity) {
    const char *scheme_end, *authority_end, *path_start, *query, *slash;
    size_t origin_length, path_length;
    int n;
    while (*location == ' ' || *location == '\t') ++location;
    if (!*location || strpbrk(location, "\r\n")) return -1;
    if (!strncasecmp(location, "http://", 7U) ||
        !strncasecmp(location, "https://", 8U)) {
        if (strlen(location) >= capacity) return -1;
        strcpy(resolved, location);
        return valid_http_url(resolved) ? 0 : -1;
    }
    scheme_end = strstr(current, "://");
    if (!scheme_end) return -1;
    authority_end = strpbrk(scheme_end + 3, "/?#");
    origin_length = authority_end ? (size_t)(authority_end - current) : strlen(current);
    if (location[0] == '/' && location[1] == '/') {
        n = snprintf(resolved, capacity, "%.*s:%s", (int)(scheme_end - current),
                     current, location);
    } else if (location[0] == '/') {
        n = snprintf(resolved, capacity, "%.*s%s", (int)origin_length,
                     current, location);
    } else if (location[0] == '?') {
        path_start = authority_end ? authority_end : "/";
        query = strpbrk(path_start, "?#");
        path_length = query ? (size_t)(query - path_start) : strlen(path_start);
        if (!path_length) { path_start = "/"; path_length = 1U; }
        n = snprintf(resolved, capacity, "%.*s%.*s%s", (int)origin_length,
                     current, (int)path_length, path_start, location);
    } else {
        path_start = authority_end ? authority_end : "/";
        query = strpbrk(path_start, "?#");
        path_length = query ? (size_t)(query - path_start) : strlen(path_start);
        slash = NULL;
        if (path_length) {
            const char *p;
            for (p = path_start; p < path_start + path_length; ++p)
                if (*p == '/') slash = p;
        }
        if (slash) path_length = (size_t)(slash - path_start) + 1U;
        else { path_start = "/"; path_length = 1U; }
        n = snprintf(resolved, capacity, "%.*s%.*s%s", (int)origin_length,
                     current, (int)path_length, path_start, location);
    }
    return n < 0 || (size_t)n >= capacity || !valid_http_url(resolved) ? -1 : 0;
}

static int response_location(int request_id, char *location, size_t capacity) {
    char *headers = NULL;
    size_t header_size = 0;
    const char *cursor, *end;
    if (sceHttpGetAllResponseHeaders(request_id, &headers, &header_size) != 0 ||
        !headers || header_size > RD_SCE_HTTP_HEADER_LIMIT) return -1;
    cursor = headers;
    end = headers + header_size;
    while (cursor < end) {
        const char *line_end = cursor;
        const char *colon;
        const char *value;
        size_t name_length, value_length;
        while ((size_t)(end - line_end) >= 2U &&
               !(line_end[0] == '\r' && line_end[1] == '\n')) ++line_end;
        if ((size_t)(end - line_end) < 2U) break;
        if (line_end == cursor) break;
        colon = memchr(cursor, ':', (size_t)(line_end - cursor));
        if (!colon) { cursor = line_end + 2; continue; }
        name_length = (size_t)(colon - cursor);
        value = colon + 1;
        while (value < line_end && (*value == ' ' || *value == '\t')) ++value;
        if (name_length == 8U && !strncasecmp(cursor, "Location", 8U)) {
            value_length = (size_t)(line_end - value);
            while (value_length && (value[value_length - 1U] == ' ' ||
                                     value[value_length - 1U] == '\t')) --value_length;
            if (!value_length || value_length >= capacity) return -1;
            memcpy(location, value, value_length);
            location[value_length] = '\0';
            return 0;
        }
        cursor = line_end + 2;
    }
    return -1;
}

static int response_header_value(int request_id, const char *header_name,
                                 char *value_out, size_t capacity) {
    char *headers = NULL;
    size_t header_size = 0, wanted = strlen(header_name);
    const char *cursor, *end;
    if (sceHttpGetAllResponseHeaders(request_id, &headers, &header_size) != 0 ||
        !headers || header_size > RD_SCE_HTTP_HEADER_LIMIT) return -1;
    cursor = headers;
    end = headers + header_size;
    while (cursor < end) {
        const char *line_end = cursor, *colon, *value;
        size_t n;
        while ((size_t)(end - line_end) >= 2U &&
               !(line_end[0] == '\r' && line_end[1] == '\n')) ++line_end;
        if ((size_t)(end - line_end) < 2U || line_end == cursor) break;
        colon = memchr(cursor, ':', (size_t)(line_end - cursor));
        if (colon && (size_t)(colon - cursor) == wanted &&
            !strncasecmp(cursor, header_name, wanted)) {
            value = colon + 1;
            while (value < line_end && (*value == ' ' || *value == '\t')) ++value;
            n = (size_t)(line_end - value);
            while (n && (value[n - 1U] == ' ' || value[n - 1U] == '\t')) --n;
            if (!n || n >= capacity) return -1;
            memcpy(value_out, value, n);
            value_out[n] = '\0';
            return 0;
        }
        cursor = line_end + 2;
    }
    return -1;
}

static int response_content_length(int request_id,
                                   unsigned long long *content_length) {
    char *headers = NULL;
    size_t header_size = 0;
    const char *cursor, *end;
    if (sceHttpGetAllResponseHeaders(request_id, &headers, &header_size) != 0 ||
        !headers || header_size > RD_SCE_HTTP_HEADER_LIMIT) return -1;
    cursor = headers;
    end = headers + header_size;
    while (cursor < end) {
        const char *line_end = cursor;
        const char *colon, *value;
        char value_buffer[32], *parse_end;
        size_t value_length;
        unsigned long long parsed;
        while ((size_t)(end - line_end) >= 2U &&
               !(line_end[0] == '\r' && line_end[1] == '\n')) ++line_end;
        if ((size_t)(end - line_end) < 2U || line_end == cursor) break;
        colon = memchr(cursor, ':', (size_t)(line_end - cursor));
        if (!colon) { cursor = line_end + 2; continue; }
        if ((size_t)(colon - cursor) != 14U ||
            strncasecmp(cursor, "Content-Length", 14U)) {
            cursor = line_end + 2;
            continue;
        }
        value = colon + 1;
        while (value < line_end && (*value == ' ' || *value == '\t')) ++value;
        value_length = (size_t)(line_end - value);
        while (value_length && (value[value_length - 1U] == ' ' ||
                                 value[value_length - 1U] == '\t')) --value_length;
        if (!value_length || value_length >= sizeof(value_buffer)) return -1;
        memcpy(value_buffer, value, value_length);
        value_buffer[value_length] = '\0';
        errno = 0;
        parsed = strtoull(value_buffer, &parse_end, 10);
        if (errno || parse_end == value_buffer || *parse_end) return -1;
        *content_length = parsed;
        return 0;
    }
    return -1;
}

static int load_ca_bundle(int ssl_id, size_t *certificate_count) {
    const char begin[] = "-----BEGIN CERTIFICATE-----";
    const char finish[] = "-----END CERTIFICATE-----";
    const char *cursor = rd_ca_bundle_pem;
    size_t count = 0, i = 0;
    rd_sce_ssl_data *certificates;
    const rd_sce_ssl_data **list;
    while ((cursor = strstr(cursor, begin)) != NULL) { ++count; cursor += sizeof(begin) - 1U; }
    if (!count || count > INT_MAX || count != RD_CA_BUNDLE_CERT_COUNT) return -1;
    certificates = (rd_sce_ssl_data *)calloc(count, sizeof(*certificates));
    list = (const rd_sce_ssl_data **)calloc(count, sizeof(*list));
    if (!certificates || !list) { free(certificates); free(list); return -1; }
    cursor = rd_ca_bundle_pem;
    while ((cursor = strstr(cursor, begin)) != NULL && i < count) {
        const char *end = strstr(cursor, finish);
        size_t length;
        if (!end) { free(certificates); free(list); return -1; }
        end += sizeof(finish) - 1U;
        if (*end == '\r') ++end;
        if (*end == '\n') ++end;
        length = (size_t)(end - cursor);
        certificates[i].ptr = (char *)cursor;
        certificates[i].size = length;
        list[i] = &certificates[i];
        ++i;
        cursor = end;
    }
    {
        int result = sceSslLoadCert(ssl_id, (int)count, list, NULL, NULL);
        free(certificates);
        free(list);
        if (result != 0) return result;
    }
    *certificate_count = count;
    return 0;
}

static int make_output_path(const char *directory, const char *url,
                             unsigned long id, char *final_path,
                             size_t final_capacity, char *part_path,
                             size_t part_capacity) {
    char filename[192], base[128];
    const char *path = strstr(url, "://") + 3;
    const char *slash = strchr(path, '/');
    const char *name, *end;
    size_t used = 0, i;
    int n;
    name = slash ? slash + 1 : "download.bin";
    end = strpbrk(name, "?#");
    if (end) {
        char *truncated = strndup(name, (size_t)(end - name));
        if (!truncated) return -1;
        name = truncated;
    }
    for (i = 0; name[i] && used < sizeof(base) - 1U; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (c == '%' && name[i + 1U] && name[i + 2U] &&
            isxdigit((unsigned char)name[i + 1U]) && isxdigit((unsigned char)name[i + 2U])) {
            char hex[3] = { name[i + 1U], name[i + 2U], '\0' };
            c = (unsigned char)strtoul(hex, NULL, 16);
            i += 2U;
        }
        if (c == '/' || c == '\\' || c == ':' || c < 0x20U || c == 0x7fU ||
            !(isalnum(c) || c == '.' || c == '_' || c == '-')) c = '_';
        base[used++] = (char)c;
    }
    if (end) free((void *)name);
    while (used && base[used - 1U] == '.') --used;
    if (!used || (used == 1U && base[0] == '.') ||
        (used == 2U && base[0] == '.' && base[1] == '.')) {
        memcpy(base, "download.bin", 12U);
        used = 12U;
    }
    base[used] = '\0';
    n = snprintf(filename, sizeof(filename), "rd-%lu-%s", id, base);
    if (n < 0 || (size_t)n >= sizeof(filename)) return -1;
    n = snprintf(final_path, final_capacity, "%s/%s", directory, filename);
    if (n < 0 || (size_t)n >= final_capacity) return -1;
    n = snprintf(part_path, part_capacity, "%s.part", final_path);
    return n < 0 || (size_t)n >= part_capacity ? -1 : 0;
}

int rd_ps5_http_download(const char *input_url, const char *directory,
                         char *filename, size_t filename_capacity, unsigned long item_id, rd_http_progress_fn progress,
                         void *context, char *error, size_t error_capacity) {
    char current_url[RD_HTTP_PATH_MAX + RD_HTTP_HOST_MAX + 64U];
    char next_url[sizeof(current_url)];
    char location[RD_HTTP_LOCATION_MAX];
    rd_output output;
    unsigned int redirect_count;
    size_t ca_count = 0;
    int net_pool = -1, ssl = -1, http = -1, template_id = -1;
    int connection = -1, request = -1, output_fd = -1, result = -1;
    int ca_loaded = 0;
    struct stat st;
    struct statvfs space;
    unsigned long long expected = 0, received = 0;
    char url_filename[512];
    int use_server_name;
    unsigned char *buffer = NULL;

    set_error(error, error_capacity, "HTTPS transfer failed");
    if (!input_url || !directory || !filename || !filename_capacity || !item_id || strlen(input_url) >= sizeof(current_url) ||
        !valid_http_url(input_url)) {
        set_error(error, error_capacity, "only valid HTTP and HTTPS URLs are supported");
        return -1;
    }
    rd_download_filename(input_url, url_filename, sizeof(url_filename));
    use_server_name = strcmp(filename, url_filename) == 0;
    if (ensure_download_directory(directory, error, error_capacity) != 0) return -1;
    if (stat(directory, &st) != 0) {
        if (error && error_capacity)
            snprintf(error, error_capacity, "destination stat failed after setup (errno=%d)", errno);
        return -1;
    }
    if (probe_directory_write(directory, item_id, error, error_capacity) != 0) {
        if (error && error_capacity) {
            size_t used = strlen(error);
            if (used < error_capacity)
                snprintf(error + used, error_capacity - used,
                         " mode=%04o uid=%u gid=%u ruid=%u euid=%u",
                         (unsigned int)(st.st_mode & 07777),
                         (unsigned int)st.st_uid, (unsigned int)st.st_gid,
                         (unsigned int)getuid(), (unsigned int)geteuid());
        }
        return -1;
    }
    strcpy(current_url, input_url);

    if (!rd_sce_net_initialized) {
        if (sceNetInit() != 0) {
            set_error(error, error_capacity, "PS5 network initialization failed");
            goto done;
        }
        rd_sce_net_initialized = 1;
    }
    net_pool = sceNetPoolCreate("rd-downloader", (int)RD_SCE_NET_POOL_SIZE, 0);
    if (net_pool < 0) { set_error(error, error_capacity, "PS5 network pool creation failed"); goto done; }
    ssl = sceSslInit((256U + 4U * RD_CA_BUNDLE_CERT_COUNT) * 1024U);
    if (ssl < 0) { set_error(error, error_capacity, "PS5 TLS initialization failed"); goto done; }
    {
        int rc = load_ca_bundle(ssl, &ca_count);
        if (rc != 0) { snprintf(error, error_capacity, "TLS CA bundle load failed (0x%08x)", (unsigned int)rc); goto done; }
        ca_loaded = 1;
    }
    /* Size the TLS pool for the embedded Mozilla-derived root bundle. */
    if (ca_count != RD_CA_BUNDLE_CERT_COUNT) {
        set_error(error, error_capacity, "embedded TLS CA bundle count mismatch");
        goto done;
    }
    http = sceHttpInit(net_pool, ssl, RD_SCE_HTTP_POOL_SIZE);
    if (http < 0) { set_error(error, error_capacity, "PS5 HTTP initialization failed"); goto done; }
    template_id = sceHttpCreateTemplate(http, "RDDownloader/0.2", RD_SCE_HTTP_VERSION_1_1, 1);
    if (template_id < 0) { set_error(error, error_capacity, "PS5 HTTP template creation failed"); goto done; }
    if (sceHttpSetAutoRedirect(template_id, 0) != 0 ||
        sceHttpSetResponseHeaderMaxSize(template_id, RD_SCE_HTTP_HEADER_LIMIT) != 0 ||
        sceHttpSetResolveTimeOut(template_id, RD_SCE_HTTP_TIMEOUT_US) != 0 ||
        sceHttpSetConnectTimeOut(template_id, RD_SCE_HTTP_TIMEOUT_US) != 0 ||
        sceHttpSetSendTimeOut(template_id, RD_SCE_HTTP_TIMEOUT_US) != 0 ||
        sceHttpSetRecvTimeOut(template_id, RD_SCE_HTTP_TIMEOUT_US) != 0 ||
        sceHttpsEnableOption(template_id, 0xA5U) != 0) {
        set_error(error, error_capacity, "PS5 HTTP TLS verification or timeout setup failed");
        goto done;
    }

    for (redirect_count = 0; redirect_count <= RD_SCE_REDIRECT_LIMIT; ++redirect_count) {
        int status = 0;
        connection = sceHttpCreateConnectionWithURL(template_id, current_url, 1);
        if (connection < 0) { set_error(error, error_capacity, "PS5 HTTP connection creation failed"); goto done; }
        request = sceHttpCreateRequestWithURL2(connection, "GET", current_url, 0);
        if (request < 0) { set_error(error, error_capacity, "PS5 HTTP request creation failed"); goto done; }
        if (rd_http_set_handle(item_id, request) != 0) goto done;
        if (sceHttpAddRequestHeader(request, "Accept-Encoding", "identity", 0) != 0) {
            set_error(error, error_capacity, "could not request the original uncompressed file bytes");
            goto done;
        }
        {
            int rc = sceHttpSendRequest(request, NULL, 0);
            if (rc != 0) {
                snprintf(error, error_capacity,
                         "PS5 HTTP request/TLS verification failed (0x%08x)",
                         (unsigned int)rc);
                goto done;
            }
            rc = sceHttpGetStatusCode(request, &status);
            if (rc != 0) {
                snprintf(error, error_capacity,
                         "PS5 HTTP status request failed (0x%08x)",
                         (unsigned int)rc);
                goto done;
            }
        }
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            if (redirect_count == RD_SCE_REDIRECT_LIMIT ||
                response_location(request, location, sizeof(location)) != 0 ||
                resolve_redirect(current_url, location, next_url, sizeof(next_url)) != 0) {
                set_error(error, error_capacity, "redirect limit reached or invalid Location header");
                goto done;
            }
            if (!strncasecmp(current_url, "https://", 8U) &&
                !strncasecmp(next_url, "http://", 7U)) {
                set_error(error, error_capacity, "refused HTTPS to HTTP redirect downgrade");
                goto done;
            }
            rd_http_set_handle(item_id, -1);
            (void)sceHttpDeleteRequest(request); request = -1;
            (void)sceHttpDeleteConnection(connection); connection = -1;
            strcpy(current_url, next_url);
            continue;
        }
        if (status != 200) {
            snprintf(error, error_capacity, "server returned HTTP status %d", status);
            goto done;
        }
        if (use_server_name) {
            char *headers = NULL;
            size_t header_size = 0;
            if (sceHttpGetAllResponseHeaders(request, &headers, &header_size) == 0 &&
                headers && header_size <= RD_SCE_HTTP_HEADER_LIMIT) {
                const char *cursor = headers, *end = headers + header_size;
                while (cursor < end) {
                    const char *line_end = cursor, *colon;
                    while ((size_t)(end - line_end) >= 2U &&
                           !(line_end[0] == '\r' && line_end[1] == '\n')) ++line_end;
                    if ((size_t)(end - line_end) < 2U || line_end == cursor) break;
                    colon = memchr(cursor, ':', (size_t)(line_end - cursor));
                    if (colon && (size_t)(colon - cursor) == 19U &&
                        !strncasecmp(cursor, "Content-Disposition", 19U)) {
                        char disposition[1024], candidate[512];
                        const char *value = colon + 1;
                        size_t value_len;
                        while (value < line_end && (*value == ' ' || *value == '\t')) ++value;
                        value_len = (size_t)(line_end - value);
                        if (value_len < sizeof(disposition)) {
                            memcpy(disposition, value, value_len); disposition[value_len] = '\0';
                            if (rd_filename_from_content_disposition(disposition, candidate,
                                    sizeof(candidate)) == 0 && strlen(candidate) < filename_capacity)
                                snprintf(filename, filename_capacity, "%s", candidate);
                        }
                        break;
                    }
                    cursor = line_end + 2;
                }
            }
        }
        {
            char *headers = NULL;
            size_t header_size = 0;
            if (sceHttpGetAllResponseHeaders(request, &headers, &header_size) == 0 &&
                headers && header_size <= RD_SCE_HTTP_HEADER_LIMIT) {
                const char *p = headers;
                while ((size_t)(headers + header_size - p) > 10U) {
                    if (!strncasecmp(p, "Content-Type:", 13U)) {
                        const char *v = p + 13;
                        while (*v == ' ' || *v == '\t') ++v;
                        if (!strncasecmp(v, "text/html", 9U) ||
                            !strncasecmp(v, "application/xhtml+xml", 21U)) {
                            set_error(error, error_capacity, "server returned a web page, not a downloadable file");
                            goto done;
                        }
                        break;
                    }
                    ++p;
                }
            }
        }
        if (response_content_length(request, &expected) != 0) expected = 0;
        if (expected) {
            unsigned long long block_size;
            unsigned long long available;
            if (statvfs(directory, &space) != 0) {
                set_error(error, error_capacity, "cannot verify destination free space");
                goto done;
            }
            block_size = space.f_frsize ? space.f_frsize : space.f_bsize;
            if (!block_size || space.f_bavail > ULLONG_MAX / block_size) {
                set_error(error, error_capacity, "cannot safely calculate destination free space");
                goto done;
            }
            available = (unsigned long long)space.f_bavail * block_size;
            if (available < expected || available - expected < RD_SCE_SPACE_MARGIN) {
                set_error(error, error_capacity, "insufficient destination space for file and safety margin");
                goto done;
            }
        }
        if (rd_output_paths(&output, directory, filename, item_id) != 0) {
            set_error(error, error_capacity, "download filename or destination path is too long");
            goto done;
        }
        if (lstat(output.final, &st) == 0 || errno != ENOENT ||
            lstat(output.partial, &st) == 0 || errno != ENOENT) {
            set_error(error, error_capacity, "download output or partial file already exists");
            goto done;
        }
        output_fd = rd_output_create(&output);
        if (output_fd < 0) {
            if (errno == EEXIST)
                set_error(error, error_capacity,
                          "partial output or ownership marker already exists; retry or remove the stale partial");
            else
                snprintf(error, error_capacity, "cannot create partial output file: %s", strerror(errno));
            goto done;
        }
        buffer = (unsigned char *)malloc(RD_SCE_IO_SIZE);
        if (!buffer) { set_error(error, error_capacity, "out of memory for download buffer"); goto done; }
        if (progress && progress(0, expected, context) != 0) {
            set_error(error, error_capacity, "download stopped; partial file kept");
            goto done;
        }
        for (;;) {
            int amount = sceHttpReadData(request, buffer, RD_SCE_IO_SIZE);
            if (amount < 0) { set_error(error, error_capacity, "HTTPS stream interrupted; partial file kept"); goto done; }
            if (!amount) break;
            if (received > ULLONG_MAX - (unsigned int)amount ||
                (expected && received + (unsigned int)amount > expected) ||
                write_all(output_fd, buffer, (size_t)amount) != 0) {
                set_error(error, error_capacity, "download write failed or server exceeded declared file size");
                goto done;
            }
            received += (unsigned int)amount;
            if (progress && progress(received, expected, context) != 0) {
                set_error(error, error_capacity, "download stopped; partial file kept");
                goto done;
            }
        }
        if (expected && received != expected) {
            set_error(error, error_capacity, "received byte count did not match Content-Length");
            goto done;
        }
        if (fsync(output_fd) != 0) {
            set_error(error, error_capacity, "could not safely flush completed download");
            goto done;
        }
        if (close(output_fd) != 0) {
            output_fd = -1;
            set_error(error, error_capacity, "could not safely flush completed download");
            goto done;
        }
        output_fd = -1;
        if (lstat(output.final, &st) == 0 || errno != ENOENT || rename(output.partial, output.final) != 0) {
            set_error(error, error_capacity, "final file exists or could not be published atomically");
            goto done;
        }
        result = 0;
        goto done;
    }
done:
    rd_http_set_handle(item_id, -1);
    free(buffer);
    if (output_fd >= 0) close(output_fd);
    if (request >= 0) sceHttpDeleteRequest(request);
    if (connection >= 0) sceHttpDeleteConnection(connection);
    if (template_id >= 0) sceHttpDeleteTemplate(template_id);
    if (http >= 0) sceHttpTerm(http);
    if (ca_loaded) sceSslUnloadCert(ssl);
    if (ssl >= 0) sceSslTerm(ssl);
    if (net_pool >= 0) sceNetPoolDestroy(net_pool);
    return result;
}

int rd_ps5_http_probe(const char *input_url, rd_http_probe_result *result,
                      char *error, size_t error_capacity) {
    char current_url[RD_HTTP_PATH_MAX + RD_HTTP_HOST_MAX + 64U];
    char next_url[sizeof(current_url)], location[RD_HTTP_LOCATION_MAX];
    size_t ca_count = 0;
    int net_pool = -1, ssl = -1, http = -1, template_id = -1;
    int connection = -1, request = -1, ca_loaded = 0, rc = -1;
    unsigned int redirects;
    if (result) memset(result, 0, sizeof(*result));
    set_error(error, error_capacity, "server response could not be checked");
    if (!input_url || !result || strlen(input_url) >= sizeof(current_url) ||
        !valid_http_url(input_url)) {
        set_error(error, error_capacity, "invalid HTTP or HTTPS URL");
        return -1;
    }
    strcpy(current_url, input_url);
    if (!rd_sce_net_initialized) {
        if (sceNetInit() != 0) { set_error(error, error_capacity, "PS5 network initialization failed"); goto done; }
        rd_sce_net_initialized = 1;
    }
    net_pool = sceNetPoolCreate("rd-probe", (int)RD_SCE_NET_POOL_SIZE, 0);
    if (net_pool < 0) { set_error(error, error_capacity, "PS5 network pool creation failed"); goto done; }
    ssl = sceSslInit((256U + 4U * RD_CA_BUNDLE_CERT_COUNT) * 1024U);
    if (ssl < 0) { set_error(error, error_capacity, "PS5 TLS initialization failed"); goto done; }
    if (load_ca_bundle(ssl, &ca_count) != 0 || ca_count != RD_CA_BUNDLE_CERT_COUNT) {
        set_error(error, error_capacity, "TLS CA bundle load failed"); goto done;
    }
    ca_loaded = 1;
    http = sceHttpInit(net_pool, ssl, RD_SCE_HTTP_POOL_SIZE);
    if (http < 0) { set_error(error, error_capacity, "PS5 HTTP initialization failed"); goto done; }
    template_id = sceHttpCreateTemplate(http, "RDDownloader/1.1", RD_SCE_HTTP_VERSION_1_1, 1);
    if (template_id < 0 || sceHttpSetAutoRedirect(template_id, 0) != 0 ||
        sceHttpSetResponseHeaderMaxSize(template_id, RD_SCE_HTTP_HEADER_LIMIT) != 0 ||
        sceHttpSetResolveTimeOut(template_id, 8000000U) != 0 ||
        sceHttpSetConnectTimeOut(template_id, 8000000U) != 0 ||
        sceHttpSetSendTimeOut(template_id, 8000000U) != 0 ||
        sceHttpSetRecvTimeOut(template_id, 8000000U) != 0 ||
        sceHttpsEnableOption(template_id, 0xA5U) != 0) {
        set_error(error, error_capacity, "PS5 HTTP probe TLS or timeout setup failed"); goto done;
    }
    for (redirects = 0; redirects <= RD_SCE_REDIRECT_LIMIT; ++redirects) {
        int status = 0;
        connection = sceHttpCreateConnectionWithURL(template_id, current_url, 1);
        if (connection < 0) { set_error(error, error_capacity, "PS5 HTTP probe connection failed"); goto done; }
        request = sceHttpCreateRequestWithURL2(connection, "GET", current_url, 0);
        if (request < 0) { set_error(error, error_capacity, "PS5 HTTP probe request failed"); goto done; }
        if (sceHttpAddRequestHeader(request, "Range", "bytes=0-0", 0) != 0 ||
            sceHttpAddRequestHeader(request, "Accept-Encoding", "identity", 0) != 0) {
            set_error(error, error_capacity, "PS5 HTTP probe headers could not be set"); goto done;
        }
        {
            int send_rc = sceHttpSendRequest(request, NULL, 0);
            if (send_rc != 0) {
                snprintf(error, error_capacity, "PS5 metadata request/TLS verification failed (0x%08x)",
                         (unsigned int)send_rc);
                goto done;
            }
        }
        if (sceHttpGetStatusCode(request, &status) != 0) {
            set_error(error, error_capacity, "PS5 metadata response status unavailable"); goto done;
        }
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            if (redirects == RD_SCE_REDIRECT_LIMIT ||
                response_location(request, location, sizeof(location)) != 0 ||
                resolve_redirect(current_url, location, next_url, sizeof(next_url)) != 0 ||
                (!strncasecmp(current_url, "https://", 8U) &&
                 !strncasecmp(next_url, "http://", 7U))) {
                set_error(error, error_capacity, "metadata redirect invalid or unsafe"); goto done;
            }
            (void)sceHttpDeleteRequest(request); request = -1;
            (void)sceHttpDeleteConnection(connection); connection = -1;
            strcpy(current_url, next_url);
            continue;
        }
        result->status_code = status;
        result->verified = (status == 200 || status == 206);
        if (response_content_length(request, &result->content_length) == 0)
            result->has_content_length = 1;
        (void)response_header_value(request, "Content-Type", result->content_type,
                                    sizeof(result->content_type));
        result->is_html = !strncasecmp(result->content_type, "text/html", 9U) ||
                          !strncasecmp(result->content_type, "application/xhtml+xml", 21U);
        {
            char disposition[1024];
            if (response_header_value(request, "Content-Disposition", disposition,
                    sizeof(disposition)) == 0)
                (void)rd_filename_from_content_disposition(disposition, result->filename,
                                                            sizeof(result->filename));
        }
        rc = 0;
        break;
    }
done:
    if (request >= 0) sceHttpDeleteRequest(request);
    if (connection >= 0) sceHttpDeleteConnection(connection);
    if (template_id >= 0) sceHttpDeleteTemplate(template_id);
    if (http >= 0) sceHttpTerm(http);
    if (ca_loaded) sceSslUnloadCert(ssl);
    if (ssl >= 0) sceSslTerm(ssl);
    if (net_pool >= 0) sceNetPoolDestroy(net_pool);
    return rc;
}

int rd_ps5_http_reset_legacy_partial(const char *input_url, const char *directory,
                              unsigned long item_id) {
    char final_path[768], part_path[784];
    struct stat st;
    if (!input_url || !directory || !item_id ||
        make_output_path(directory, input_url, item_id, final_path, sizeof(final_path),
                         part_path, sizeof(part_path)) != 0) return -1;
    if (lstat(part_path, &st) != 0) return errno == ENOENT ? 0 : -1;
    return S_ISREG(st.st_mode) ? unlink(part_path) : -1;
}

#endif
