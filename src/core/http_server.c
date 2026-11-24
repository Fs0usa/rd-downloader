#include "rd/http_server.h"
#include "rd/queue.h"
#include "rd/http_client.h"
#include "rd/archive.h"
#include "rd/output.h"
#include "rd/wfm_client.h"
#include "rd/version.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RD_HTTP_MAX_REQUEST 32768U
#define RD_HTTP_MAX_BODY 16384U
#define RD_HTTP_MAX_CLIENTS 8
#define RD_HTTP_CLIENT_STACK (256U * 1024U)
#define RD_HTTP_REQUEST_TIMEOUT_MS 15000ULL
#define RD_HTTP_QUEUE_RESPONSE (RD_MAX_QUEUE_ITEMS * 8192U + 32U)
#define RD_PROGRESS_SAVE_INTERVAL (16ULL * 1024ULL * 1024ULL)
#define RD_DOWNLOAD_WORKER_STACK (1024U * 1024U)
#define RD_PASSWORD_HEADER_MAX 256U
#define RD_MAX_NESTED_ARCHIVES 64U
#define RD_MAX_NESTED_DEPTH 3U

#ifdef RD_HOST_BUILD
#define RD_DOWNLOAD_DIRECTORY "/tmp/rd-downloader-host-downloads/downloads"
#define RD_EXTRACTION_DIRECTORY "/tmp/rd-downloader-host-downloads"
#else
#define RD_DOWNLOAD_DIRECTORY RD_DEFAULT_DOWNLOAD_DIRECTORY
#define RD_EXTRACTION_DIRECTORY RD_DEFAULT_EXTRACTION_DIRECTORY
#endif

#define RD_LOG_FILENAME "rd-downloader.log"

static rd_queue server_queue;
static int queue_ready;
static int download_directory_ready;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t worker_condition = PTHREAD_COND_INITIALIZER;
static int download_requested = 1;
static int worker_started;
static unsigned long active_download_id;
static unsigned long active_extraction_id;
static pthread_mutex_t client_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned int active_clients;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

static void write_log(const char *format, ...) {
    char path[640], message[1536], line[1792];
    struct timespec now;
    struct tm calendar;
    va_list arguments;
    size_t used, i;
    int fd, count;
    if (snprintf(path, sizeof(path), "%s/%s", RD_DOWNLOAD_DIRECTORY,
                 RD_LOG_FILENAME) >= (int)sizeof(path)) return;
    va_start(arguments, format);
    count = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (count < 0) return;
    message[sizeof(message) - 1U] = '\0';
    for (i = 0; message[i]; ++i)
        if (message[i] == '\r' || message[i] == '\n' ||
            (unsigned char)message[i] < 0x20U) message[i] = ' ';
    (void)clock_gettime(CLOCK_REALTIME, &now);
    (void)localtime_r(&now.tv_sec, &calendar);
    count = snprintf(line, sizeof(line), "%04d-%02d-%02dT%02d:%02d:%02d.%03ld %s\n",
        calendar.tm_year + 1900, calendar.tm_mon + 1, calendar.tm_mday,
        calendar.tm_hour, calendar.tm_min, calendar.tm_sec, now.tv_nsec / 1000000L,
        message);
    if (count < 0 || (size_t)count >= sizeof(line)) return;
    pthread_mutex_lock(&log_mutex);
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        used = 0;
        while (used < (size_t)count) {
            ssize_t written = write(fd, line + used, (size_t)count - used);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) break;
            used += (size_t)written;
        }
        close(fd);
    }
    pthread_mutex_unlock(&log_mutex);
}

static int same_archive_set(const rd_queue_item *candidate, const rd_queue_item *other,
                            const rd_archive_name *candidate_name);

#include "index_html.inc"

static int send_response(int fd, int status, const char *reason,
                         const char *content_type, const char *body,
                         size_t body_length);

static int send_health(int fd) {
    char body[320];
    size_t i;
    int active = 0, length, ready;
    int queue_available, download_directory_available;
    pthread_mutex_lock(&queue_mutex);
    for (i = 0; i < server_queue.count; ++i)
        if (server_queue.items[i].state == RD_JOB_DOWNLOADING) active = 1;
    ready = worker_started;
    queue_available = queue_ready;
    download_directory_available = download_directory_ready;
    pthread_mutex_unlock(&queue_mutex);
    length = snprintf(body, sizeof(body),
        "{\"status\":\"ok\",\"application\":\"rd-downloader\","
        "\"version\":\"%s\",\"transport\":\"http-https\","
        "\"worker_ready\":%s,\"queue_ready\":%s,\"download_directory_ready\":%s,"
        "\"downloader_active\":%s}\n",
        RD_DOWNLOADER_VERSION, ready ? "true" : "false", queue_available ? "true" : "false",
        download_directory_available ? "true" : "false", active ? "true" : "false");
    if (length < 0 || (size_t)length >= sizeof(body)) return -1;
    return send_response(fd, 200, "OK", "application/json; charset=utf-8", body, (size_t)length);
}

static int send_all(int fd, const char *data, size_t length) {
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

static unsigned long long monotonic_ms(void);

static ssize_t recv_before_deadline(int fd, void *buffer, size_t capacity,
                                    unsigned long long deadline_ms) {
    struct timeval timeout;
    unsigned long long now = monotonic_ms();
    unsigned long long remaining;
    if (now >= deadline_ms) { errno = ETIMEDOUT; return -1; }
    remaining = deadline_ms - now;
    timeout.tv_sec = (time_t)(remaining / 1000ULL);
    timeout.tv_usec = (suseconds_t)((remaining % 1000ULL) * 1000ULL);
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0)
        return -1;
    return recv(fd, buffer, capacity, 0);
}

static int send_response(int fd, int status, const char *reason,
                         const char *content_type, const char *body,
                         size_t body_length) {
    char headers[512];
    int header_length = snprintf(headers, sizeof(headers),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lu\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; "
        "script-src 'unsafe-inline'; connect-src 'self'\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "Connection: close\r\n\r\n",
        status, reason, content_type, (unsigned long)body_length);
    if (header_length < 0 || (size_t)header_length >= sizeof(headers)) return -1;
    if (send_all(fd, headers, (size_t)header_length) != 0) return -1;
    return send_all(fd, body, body_length);
}

static const char *find_header(const char *request, const char *name) {
    const char *line = strstr(request, "\r\n");
    size_t name_len = strlen(name);
    if (!line) return NULL;
    line += 2;
    while (*line && !(line[0] == '\r' && line[1] == '\n')) {
        const char *end = strstr(line, "\r\n");
        if (!end) return NULL;
        if ((size_t)(end - line) > name_len &&
            strncasecmp(line, name, name_len) == 0 && line[name_len] == ':') {
            const char *value = line + name_len + 1;
            while (value < end && (*value == ' ' || *value == '\t')) ++value;
            return value;
        }
        line = end + 2;
    }
    return NULL;
}

static int content_length_of(const char *request, size_t *length) {
    const char *value = find_header(request, "Content-Length");
    char *end;
    unsigned long parsed;
    if (!value) { *length = 0; return 0; }
    parsed = strtoul(value, &end, 10);
    if (end == value || parsed > RD_HTTP_MAX_BODY ||
        (*end != '\r' && *end != '\n' && *end != ' ' && *end != '\t')) return -1;
    *length = (size_t)parsed;
    return 0;
}

static int command_header_ok(const char *request) {
    const char *value = find_header(request, "X-RD-Command");
    return value && strncmp(value, "1\r\n", 3) == 0;
}

static int copy_header_value(const char *request, const char *name, char *out, size_t capacity) {
    const char *value = find_header(request, name);
    const char *end;
    size_t length, i;
    if (!value) { if (capacity) out[0] = '\0'; return 0; }
    end = strstr(value, "\r\n");
    if (!end) return -1;
    length = (size_t)(end - value);
    if (length >= capacity) return -1;
    for (i = 0; i < length; ++i)
        if ((unsigned char)value[i] < 0x20 || (unsigned char)value[i] == 0x7f) return -1;
    memcpy(out, value, length);
    out[length] = '\0';
    return 1;
}

static int form_hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int decode_form_value(const char *src, size_t length, char *out, size_t capacity,
                             int allow_newlines) {
    size_t i, used = 0;
    for (i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c == '+') c = ' ';
        else if (c == '%') {
            int hi, lo;
            if (i + 2 >= length || (hi = form_hex((unsigned char)src[i + 1])) < 0 ||
                (lo = form_hex((unsigned char)src[i + 2])) < 0) return -1;
            c = (unsigned char)((hi << 4) | lo);
            i += 2;
        }
        if (c == 0 || (c < 0x20 && !(allow_newlines && (c == '\n' || c == '\r'))) || c == 0x7f ||
            used + 1 >= capacity) return -1;
        out[used++] = (char)c;
    }
    out[used] = '\0';
    return 0;
}

static int parse_add_form(const char *body, size_t length, char *urls, size_t urls_capacity,
                          char *extract, size_t extract_capacity,
                          char *nested, size_t nested_capacity,
                          char *password, size_t password_capacity) {
    size_t offset = 0;
    int have_urls = 0, have_extract = 0, have_nested = 0, have_password = 0;
    while (offset < length) {
        size_t end = offset, equal;
        char key[16];
        while (end < length && body[end] != '&') ++end;
        equal = offset;
        while (equal < end && body[equal] != '=') ++equal;
        if (decode_form_value(body + offset, equal - offset, key, sizeof(key), 0) != 0) return -1;
        if (strcmp(key, "urls") == 0) {
            if (have_urls++ || equal == end || decode_form_value(body + equal + 1, end - equal - 1,
                urls, urls_capacity, 1) != 0) return -1;
        } else if (strcmp(key, "extract") == 0) {
            if (have_extract++ || equal == end || decode_form_value(body + equal + 1, end - equal - 1,
                extract, extract_capacity, 0) != 0) return -1;
        } else if (strcmp(key, "extractNested") == 0) {
            if (have_nested++ || equal == end || decode_form_value(body + equal + 1, end - equal - 1,
                nested, nested_capacity, 0) != 0) return -1;
        } else if (strcmp(key, "password") == 0) {
            if (have_password++ || equal == end || decode_form_value(body + equal + 1, end - equal - 1,
                password, password_capacity, 0) != 0) return -1;
        } else return -1;
        offset = end + 1;
    }
    return have_urls && have_extract && have_nested && have_password ? 0 : -1;
}

static int json_escape(const char *input, char *output, size_t capacity) {
    static const char hex[] = "0123456789abcdef";
    size_t i, used = 0;
    if (!capacity) return -1;
    for (i = 0; input && input[i]; ++i) {
        unsigned char c = (unsigned char)input[i];
        if (c == '"' || c == '\\') {
            if (used + 2U >= capacity) return -1;
            output[used++] = '\\';
            output[used++] = (char)c;
        } else if (c < 0x20U) {
            if (used + 6U >= capacity) return -1;
            output[used++] = '\\'; output[used++] = 'u';
            output[used++] = '0'; output[used++] = '0';
            output[used++] = hex[c >> 4]; output[used++] = hex[c & 15U];
        } else {
            if (used + 1U >= capacity) return -1;
            output[used++] = (char)c;
        }
    }
    output[used] = '\0';
    return (int)used;
}

static size_t related_file_count_for_item(const rd_queue *queue, unsigned long id) {
    size_t i, count = 0;
    for (i = 0; i < queue->related_file_count; ++i)
        if (queue->related_files[i].item_id == id) ++count;
    return count;
}

static int send_queue(int fd) {
    static const char unavailable[] = "{\"error\":\"queue storage unavailable\"}\n";
    static const char no_memory[] = "{\"error\":\"out of memory\"}\n";
    char *body;
    size_t used = 0, i;
    int n;
    if (!queue_ready) return send_response(fd, 503, "Service Unavailable",
        "application/json; charset=utf-8", unavailable, sizeof(unavailable) - 1U);
    body = (char *)malloc(RD_HTTP_QUEUE_RESPONSE);
    if (!body) return send_response(fd, 503, "Service Unavailable",
        "application/json; charset=utf-8", no_memory, sizeof(no_memory) - 1U);
    pthread_mutex_lock(&queue_mutex);
    n = snprintf(body, RD_HTTP_QUEUE_RESPONSE,
                 "{\"online\":true,\"dir\":\"%s\",\"extractDir\":\"%s\",\"downloadDirReady\":%s,\"canExtract\":true,\"items\":[",
                 RD_DOWNLOAD_DIRECTORY, RD_EXTRACTION_DIRECTORY,
                 download_directory_ready ? "true" : "false");
    if (n < 0 || (size_t)n >= RD_HTTP_QUEUE_RESPONSE) goto serialization_failed;
    used = (size_t)n;
    for (i = 0; i < server_queue.count; ++i) {
        const rd_queue_item *item = &server_queue.items[i];
        rd_archive_name archive_name;
        char filename_json[3072], current_file_json[3072], error_json[1537];
        int is_archive = rd_classify_archive_name(item->filename, &archive_name) == 0 &&
                         archive_name.kind != RD_ARCHIVE_NONE;
        if (json_escape(item->filename, filename_json, sizeof(filename_json)) < 0 ||
            json_escape(item->extraction_current_file, current_file_json,
                        sizeof(current_file_json)) < 0 ||
            json_escape(item->error, error_json, sizeof(error_json)) < 0) goto serialization_failed;
        n = snprintf(body + used, RD_HTTP_QUEUE_RESPONSE - used,
            "%s{\"id\":%lu,\"filename\":\"%s\",\"state\":\"%s\",\"downloaded_bytes\":%llu,\"expected_bytes\":%llu,\"speed_bps\":%llu,\"average_bps\":%llu,\"eta_seconds\":%llu,\"extract\":%s,\"extractNested\":%s,\"relatedFiles\":%lu,\"retryAttempts\":%u,\"retryAfter\":%llu,\"archive\":%s,\"xDone\":%llu,\"xTotal\":%llu,\"xSpeed\":%llu,\"xAverage\":%llu,\"xETA\":%llu,\"xFile\":\"%s\",\"error\":\"%s\"}",
            i ? "," : "", item->id, filename_json, rd_job_state_name(item->state),
            item->downloaded_bytes, item->expected_bytes, item->speed_bps,
            item->average_bps, item->eta_seconds,
            item->extract_after_download ? "true" : "false",
            item->extract_nested_archives ? "true" : "false",
            (unsigned long)related_file_count_for_item(&server_queue, item->id),
            item->retry_attempts, item->retry_after_epoch,
            is_archive ? "true" : "false",
            item->extraction_done_bytes, item->extraction_total_bytes,
            item->extraction_speed_bps, item->extraction_average_bps,
            item->extraction_eta_seconds, current_file_json, error_json);
        if (n < 0 || (size_t)n >= RD_HTTP_QUEUE_RESPONSE - used) goto serialization_failed;
        used += (size_t)n;
    }
    if (used + 3U >= RD_HTTP_QUEUE_RESPONSE) goto serialization_failed;
    memcpy(body + used, "]}\n", 3U); used += 3U;
    pthread_mutex_unlock(&queue_mutex);
    n = send_response(fd, 200, "OK", "application/json; charset=utf-8", body, used);
    free(body);
    return n;
serialization_failed:
    pthread_mutex_unlock(&queue_mutex);
    free(body);
    return -1;
}

typedef struct rd_progress_context {
    unsigned long id;
    unsigned long long last_persisted;
    unsigned long long started_ms;
    unsigned long long previous_ms;
    unsigned long long previous_bytes;
} rd_progress_context;

static unsigned long long monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (unsigned long long)ts.tv_sec * 1000ULL +
           (unsigned long long)ts.tv_nsec / 1000000ULL;
}

static void copy_display_text(char *out, size_t capacity, const char *text) {
    size_t i;
    if (!capacity) return;
    for (i = 0; text && text[i] && i + 1U < capacity; ++i) {
        unsigned char c = (unsigned char)text[i];
        out[i] = c < 0x20U || c == 0x7fU || c == '"' || c == '\\' ? '_' : (char)c;
    }
    out[i] = '\0';
}

static int record_progress(unsigned long long downloaded, unsigned long long expected,
                           void *opaque) {
    rd_progress_context *context = (rd_progress_context *)opaque;
    size_t i;
    int found = 0, persist;
    unsigned long long now = monotonic_ms();
    pthread_mutex_lock(&queue_mutex);
    for (;;) {
        rd_queue_item *item = NULL;
        for (i = 0; i < server_queue.count; ++i)
            if (server_queue.items[i].id == context->id) { item = &server_queue.items[i]; break; }
        if (!item) break;
        if (item->state == RD_JOB_PAUSED) {
            item->speed_bps = 0;
            pthread_cond_wait(&worker_condition, &queue_mutex);
            now = monotonic_ms();
            continue;
        }
        if (item->state != RD_JOB_DOWNLOADING) break;
        item->downloaded_bytes = downloaded;
        item->expected_bytes = expected;
        if (context->previous_ms && now > context->previous_ms && downloaded >= context->previous_bytes)
            item->speed_bps = (downloaded - context->previous_bytes) * 1000ULL / (now - context->previous_ms);
        item->average_bps = context->started_ms && now > context->started_ms ?
            downloaded * 1000ULL / (now - context->started_ms) : 0;
        item->eta_seconds = expected > downloaded && item->average_bps ?
            (expected - downloaded) / item->average_bps : 0;
        context->previous_ms = now;
        context->previous_bytes = downloaded;
        found = 1;
        break;
    }
    persist = found && (downloaded >= context->last_persisted) &&
              downloaded - context->last_persisted >= RD_PROGRESS_SAVE_INTERVAL;
    if (persist) {
        if (rd_queue_update(&server_queue, context->id, RD_JOB_DOWNLOADING,
                            downloaded, expected, NULL) != 0) found = 0;
        else context->last_persisted = downloaded;
    }
    pthread_mutex_unlock(&queue_mutex);
    return found ? 0 : -1;
}

static const char *download_directory(const rd_queue_item *item) {
#ifdef RD_HOST_BUILD
    (void)item;
    return RD_DOWNLOAD_DIRECTORY;
#else
    return item->destination;
#endif
}

static int discard_output(const rd_queue_item *item, int include_final) {
    const char *directory = download_directory(item);
    rd_output output;
    if (item->legacy_partial)
        return rd_http_reset_legacy_partial(item->url, directory, item->id);
    if (rd_output_paths(&output, directory, item->url, item->id)) return -1;
    return rd_output_discard(&output, include_final);
}

static int finish_removal(rd_queue_item *item, int delete_downloads) {
    size_t i;
    unsigned long item_id = item->id;
    rd_output output;
    const char *directory = download_directory(item);
    if (delete_downloads && active_extraction_id && active_extraction_id != item_id) {
        size_t active_index;
        for (active_index = 0; active_index < server_queue.count; ++active_index) {
            rd_queue_item *active_item = &server_queue.items[active_index];
            rd_archive_name selected_name;
            if (active_item->id != active_extraction_id ||
                rd_classify_archive_name(item->filename, &selected_name) != 0 ||
                !same_archive_set(item, active_item, &selected_name)) continue;
            errno = EBUSY;
            return -1;
        }
    }
    if (item->legacy_partial) {
        if (rd_http_reset_legacy_partial(item->url, directory, item->id) != 0) return -1;
    } else if (rd_output_paths(&output, directory, item->url, item->id) != 0 ||
               rd_output_discard(&output, !!delete_downloads) != 0) return -1;
    if (delete_downloads) {
        for (i = 0; i < server_queue.related_file_count; ++i) {
            char path[768];
            struct stat st;
            int n;
            if (server_queue.related_files[i].item_id != item->id) continue;
            n = snprintf(path, sizeof(path), "%s/%s", RD_DOWNLOAD_DIRECTORY,
                         server_queue.related_files[i].filename);
            if (n < 0 || (size_t)n >= sizeof(path)) return -1;
            if (lstat(path, &st) == 0) {
                if (!S_ISREG(st.st_mode) || unlink(path) != 0) return -1;
            } else if (errno != ENOENT) return -1;
        }
    }
    if (delete_downloads) {
        rd_archive_name archive_name;
        if (rd_classify_archive_name(item->filename, &archive_name) == 0 &&
            archive_name.kind != RD_ARCHIVE_NONE) {
            unsigned long sibling_ids[RD_MAX_QUEUE_ITEMS];
            size_t sibling_count = 0, j;
            for (i = 0; i < server_queue.count; ++i) {
                rd_queue_item *sibling = &server_queue.items[i];
                if (sibling->id == item_id || !same_archive_set(item, sibling, &archive_name) ||
                    (sibling->state != RD_JOB_COMPLETED && sibling->state != RD_JOB_FAILED &&
                     sibling->state != RD_JOB_READY_TO_EXTRACT) ||
                    active_download_id == sibling->id || active_extraction_id == sibling->id) continue;
                sibling_ids[sibling_count++] = sibling->id;
            }
            for (j = 0; j < sibling_count; ++j) {
                rd_queue_item *sibling = NULL;
                for (i = 0; i < server_queue.count; ++i)
                    if (server_queue.items[i].id == sibling_ids[j]) { sibling = &server_queue.items[i]; break; }
                if (!sibling) continue;
                if (sibling->legacy_partial) {
                    if (rd_http_reset_legacy_partial(sibling->url,
                            download_directory(sibling), sibling->id) != 0) return -1;
                } else {
                    rd_output sibling_output;
                    if (rd_output_paths(&sibling_output, download_directory(sibling),
                                        sibling->url, sibling->id) != 0 ||
                        rd_output_discard(&sibling_output, 1) != 0) return -1;
                }
                for (i = 0; i < server_queue.related_file_count; ++i) {
                    char path[768];
                    struct stat st;
                    int n;
                    if (server_queue.related_files[i].item_id != sibling_ids[j]) continue;
                    n = snprintf(path, sizeof(path), "%s/%s", RD_DOWNLOAD_DIRECTORY,
                                 server_queue.related_files[i].filename);
                    if (n < 0 || (size_t)n >= sizeof(path)) return -1;
                    if (lstat(path, &st) == 0) {
                        if (!S_ISREG(st.st_mode) || unlink(path) != 0) return -1;
                    } else if (errno != ENOENT) return -1;
                }
                if (rd_queue_remove(&server_queue, sibling_ids[j]) != 0) return -1;
            }
        }
    }
    return rd_queue_remove(&server_queue, item_id);
}

typedef struct rd_extraction_context {
    unsigned long id;
    unsigned int source_scope_hash;
    unsigned long long started_ms;
    unsigned long long previous_ms;
    unsigned long long previous_done;
    size_t package_count;
    int package_overflow;
    char package_paths[64][512];
    size_t archive_count;
    int archive_overflow;
    char archive_paths[RD_MAX_NESTED_ARCHIVES][512];
} rd_extraction_context;

typedef struct rd_nested_job {
    char path[768];
    unsigned int depth;
} rd_nested_job;

static int safe_relative_member(const char *path);
static int copy_package_without_overwrite(const char *source, const char *target,
                                          rd_extraction_context *context);

static int is_package_path(const char *path) {
    const char *base = strrchr(path, '/');
    size_t length;
    base = base ? base + 1 : path;
    length = strlen(base);
    return length >= 4U && base[length - 4U] == '.' &&
        tolower((unsigned char)base[length - 3U]) == 'p' &&
        tolower((unsigned char)base[length - 2U]) == 'k' &&
        tolower((unsigned char)base[length - 1U]) == 'g';
}

static void remember_package_path(rd_extraction_context *context, const char *path) {
    size_t i;
    if (!path || !is_package_path(path)) return;
    for (i = 0; i < context->package_count; ++i)
        if (strcmp(context->package_paths[i], path) == 0) return;
    if (context->package_count >= sizeof(context->package_paths) / sizeof(context->package_paths[0])) {
        context->package_overflow = 1;
        return;
    }
    if (strlen(path) >= sizeof(context->package_paths[0])) {
        context->package_overflow = 1;
        return;
    }
    snprintf(context->package_paths[context->package_count++],
             sizeof(context->package_paths[0]), "%s", path);
}

static void remember_archive_path(rd_extraction_context *context, const char *path) {
    rd_archive_name parsed;
    size_t i;
    if (!path || !*path || rd_classify_archive_name(path, &parsed) != 0 ||
        parsed.kind == RD_ARCHIVE_NONE) return;
    for (i = 0; i < context->archive_count; ++i)
        if (strcmp(context->archive_paths[i], path) == 0) return;
    if (context->archive_count >= RD_MAX_NESTED_ARCHIVES ||
        strlen(path) >= sizeof(context->archive_paths[0])) {
        context->archive_overflow = 1;
        return;
    }
    snprintf(context->archive_paths[context->archive_count++],
             sizeof(context->archive_paths[0]), "%s", path);
}

static int record_extraction_progress(uint64_t done, uint64_t total,
                                      const char *current_file, void *opaque) {
    rd_extraction_context *context = (rd_extraction_context *)opaque;
    size_t i;
    unsigned long long now = monotonic_ms();
    int found = 0;
    pthread_mutex_lock(&queue_mutex);
    for (i = 0; i < server_queue.count; ++i) {
        rd_queue_item *item = &server_queue.items[i];
        if (item->id != context->id) continue;
        if (item->state == RD_JOB_CANCELLED) break;
        if (item->state != RD_JOB_EXTRACTING) break;
        if (current_file) {
            copy_display_text(item->extraction_current_file,
                              sizeof(item->extraction_current_file), current_file);
            remember_package_path(context, current_file);
            remember_archive_path(context, current_file);
        }
        else {
            item->extraction_done_bytes = done;
            item->extraction_total_bytes = total;
            if (context->previous_ms && now > context->previous_ms && done >= context->previous_done)
                item->extraction_speed_bps =
                    (done - context->previous_done) * 1000ULL / (now - context->previous_ms);
            else item->extraction_speed_bps = 0;
            item->extraction_average_bps = context->started_ms && now > context->started_ms ?
                done * 1000ULL / (now - context->started_ms) : 0;
            item->extraction_eta_seconds = total > done && item->extraction_speed_bps ?
                (total - done) / item->extraction_speed_bps : 0;
            context->previous_ms = now;
            context->previous_done = done;
        }
        found = 1;
        break;
    }
    pthread_mutex_unlock(&queue_mutex);
    return found ? 0 : -1;
}

static unsigned int relative_path_hash(const char *path) {
    unsigned int hash = 2166136261U;
    while (*path) { hash ^= (unsigned char)*path++; hash *= 16777619U; }
    return hash;
}

static int move_extracted_archives(rd_extraction_context *context,
                                   const char *destination,
                                   rd_nested_job *jobs, size_t *job_count,
                                   unsigned int depth,
                                   char *warning, size_t warning_capacity) {
    size_t i;
    unsigned int failed = context->archive_overflow ? 1U : 0U;
    for (i = 0; i < context->archive_count; ++i) {
        const char *relative = context->archive_paths[i];
        const char *base;
        rd_archive_name archive_name;
        char source[1024], filename[256], target[768], safe_base[180], group_path[512];
        const char *relative_slash;
        size_t j, used = 0;
        int n, known, target_existed = 0;
        if (!safe_relative_member(relative) ||
            rd_classify_archive_name(relative, &archive_name) != 0 ||
            archive_name.kind == RD_ARCHIVE_NONE) { ++failed; continue; }
        relative_slash = strrchr(relative, '/');
        n = snprintf(group_path, sizeof(group_path), "%.*s%s",
                     relative_slash ? (int)(relative_slash + 1 - relative) : 0,
                     relative, archive_name.first_volume);
        if (n < 0 || (size_t)n >= sizeof(group_path)) { ++failed; continue; }
        base = strrchr(relative, '/');
        base = base ? base + 1 : relative;
        for (j = 0; base[j] && used + 1U < sizeof(safe_base); ++j) {
            unsigned char c = (unsigned char)base[j];
            safe_base[used++] = (isalnum(c) || c == '.' || c == '_' || c == '-') ? (char)c : '_';
        }
        safe_base[used] = '\0';
        if (!used) { ++failed; continue; }
        n = snprintf(source, sizeof(source), "%s/%s", destination, relative);
        if (n < 0 || (size_t)n >= sizeof(source)) { ++failed; continue; }
        n = snprintf(filename, sizeof(filename), "rd-%lu-%08x-%s",
                     context->id,
                     relative_path_hash(group_path) ^ context->source_scope_hash,
                     safe_base);
        if (n < 0 || (size_t)n >= sizeof(filename)) { ++failed; continue; }
        n = snprintf(target, sizeof(target), "%s/%s", RD_DOWNLOAD_DIRECTORY, filename);
        if (n < 0 || (size_t)n >= sizeof(target)) { ++failed; continue; }
        pthread_mutex_lock(&queue_mutex);
        known = rd_queue_has_related_file(&server_queue, context->id, filename);
        pthread_mutex_unlock(&queue_mutex);
        {
            struct stat st;
            if (lstat(target, &st) == 0) {
                if (!known || !S_ISREG(st.st_mode)) { ++failed; continue; }
                target_existed = 1;
            } else if (errno != ENOENT) { ++failed; continue; }
            else {
                if (copy_package_without_overwrite(source, target, context) != 0) {
                    if (errno == ECANCELED) return -1;
                    ++failed;
                    continue;
                }
                pthread_mutex_lock(&queue_mutex);
                n = rd_queue_record_related_file(&server_queue, context->id, filename);
                pthread_mutex_unlock(&queue_mutex);
                if (n != 0) { unlink(target); ++failed; continue; }
            }
            /* If recovery finds a previously recorded copy, preserve the source
               too: its contents may have changed since that copy was made. */
            if (!target_existed && unlink(source) != 0 && errno != ENOENT) {
                ++failed;
                continue;
            }
        }
        if (jobs && job_count && depth < RD_MAX_NESTED_DEPTH &&
            archive_name.kind != RD_ARCHIVE_RAR_LEGACY_VOLUME &&
            !(archive_name.kind == RD_ARCHIVE_RAR_MULTIPART_PART && archive_name.part_number > 1U) &&
            !(archive_name.kind == RD_ARCHIVE_SPLIT_001 && archive_name.part_number > 1U) &&
            *job_count < RD_MAX_NESTED_ARCHIVES) {
            snprintf(jobs[*job_count].path, sizeof(jobs[*job_count].path), "%s", target);
            jobs[*job_count].depth = depth + 1U;
            ++*job_count;
        } else if (jobs && job_count && depth < RD_MAX_NESTED_DEPTH) {
            ++failed;
        }
    }
    if (failed && warning && warning_capacity)
        snprintf(warning, warning_capacity,
                 "%u nested archive file(s) could not be moved into downloads",
                 failed);
    return failed ? -1 : 0;
}

static int extraction_cancel_requested(void *opaque) {
    rd_extraction_context *context = (rd_extraction_context *)opaque;
    size_t i;
    int cancelled = 1;
    pthread_mutex_lock(&queue_mutex);
    for (i = 0; i < server_queue.count; ++i) {
        if (server_queue.items[i].id != context->id) continue;
        cancelled = server_queue.items[i].state == RD_JOB_CANCELLED;
        break;
    }
    pthread_mutex_unlock(&queue_mutex);
    return cancelled;
}

static int safe_relative_member(const char *path) {
    const char *segment;
    if (!path || !*path || path[0] == '/') return 0;
    segment = path;
    for (;;) {
        const char *slash = strchr(segment, '/');
        size_t length = slash ? (size_t)(slash - segment) : strlen(segment);
        if (!length || (length == 1U && segment[0] == '.') ||
            (length == 2U && segment[0] == '.' && segment[1] == '.')) return 0;
        if (!slash) return 1;
        segment = slash + 1;
    }
}

static int copy_package_without_overwrite(const char *source, const char *target,
                                          rd_extraction_context *context) {
    struct stat before, opened;
    unsigned char buffer[64U * 1024U];
    int input = -1, output = -1, created = 0, result = -1;
    if (lstat(source, &before) != 0 || !S_ISREG(before.st_mode)) return -1;
    input = open(source, O_RDONLY
#ifdef O_NOFOLLOW
                 | O_NOFOLLOW
#endif
    );
    if (input < 0 || fstat(input, &opened) != 0 || !S_ISREG(opened.st_mode) ||
        before.st_dev != opened.st_dev || before.st_ino != opened.st_ino) goto done;
    output = open(target, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (output < 0) goto done;
    created = 1;
    for (;;) {
        ssize_t count;
        size_t written = 0;
        if (extraction_cancel_requested(context)) { errno = ECANCELED; goto done; }
        count = read(input, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) goto done;
        if (count == 0) break;
        while (written < (size_t)count) {
            ssize_t n = write(output, buffer + written, (size_t)count - written);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) goto done;
            written += (size_t)n;
        }
    }
    if (fchmod(output, before.st_mode & 0777) != 0 || fsync(output) != 0) goto done;
    if (close(output) != 0) { output = -1; goto done; }
    output = -1;
    result = 0;
done:
    if (input >= 0) close(input);
    if (output >= 0) close(output);
    if (result != 0 && created) unlink(target);
    return result;
}

static void promote_extracted_packages(rd_extraction_context *context,
                                       const char *destination,
                                       char *warning, size_t warning_capacity) {
    size_t i;
    unsigned int failed = context->package_overflow ? 1U : 0U;
    warning[0] = '\0';
    for (i = 0; i < context->package_count; ++i) {
        const char *relative = context->package_paths[i];
        const char *base;
        char source[1024], target[1024];
        int n;
        if (!safe_relative_member(relative)) { ++failed; continue; }
        base = strrchr(relative, '/');
        base = base ? base + 1 : relative;
        n = snprintf(source, sizeof(source), "%s/%s", destination, relative);
        if (n < 0 || (size_t)n >= sizeof(source)) { ++failed; continue; }
        n = snprintf(target, sizeof(target), "%s/%s", destination, base);
        if (n < 0 || (size_t)n >= sizeof(target)) { ++failed; continue; }
        if (strcmp(source, target) == 0) continue;
        if (copy_package_without_overwrite(source, target, context) != 0) {
            if (errno == ECANCELED) return;
            ++failed;
        }
    }
    if (failed)
        snprintf(warning, warning_capacity,
                 "%u .pkg file(s) could not be copied to the destination folder; extracted originals were preserved",
                 failed);
}

static int queue_has_pending_downloads(void) {
    size_t i;
    for (i = 0; i < server_queue.count; ++i) {
        rd_job_state state = server_queue.items[i].state;
        if (state == RD_JOB_QUEUED || state == RD_JOB_DOWNLOADING || state == RD_JOB_PAUSED ||
            state == RD_JOB_WAITING_FOR_NETWORK)
            return 1;
    }
    return 0;
}

static int is_retryable_download_error(const char *error) {
    const char *status;
    int code;
    if (!error) return 0;
    if (strstr(error, "timeout") || strstr(error, "DNS") || strstr(error, "connection") ||
        strstr(error, "connect") || strstr(error, "interrupted") || strstr(error, "TLS") ||
        strstr(error, "verification failed") || strstr(error, "stream")) return 1;
    status = strstr(error, "server returned HTTP status ");
    if (status) {
        code = atoi(status + strlen("server returned HTTP status "));
        return code == 408 || code == 425 || code == 429 || (code >= 500 && code <= 599);
    }
    return 0;
}

static int same_archive_set(const rd_queue_item *candidate, const rd_queue_item *other,
                            const rd_archive_name *candidate_name) {
    rd_archive_name other_name;
    if (strcmp(candidate->destination, other->destination) != 0 ||
        rd_classify_archive_name(other->filename, &other_name) != 0 ||
        other_name.kind == RD_ARCHIVE_NONE) return 0;
    return strcmp(candidate_name->first_volume, other_name.first_volume) == 0;
}

static rd_queue_item *find_extract_candidate(void) {
    size_t i;
    if (queue_has_pending_downloads()) return NULL;
    for (i = 0; i < server_queue.count; ++i) {
        rd_queue_item *item = &server_queue.items[i];
        rd_archive_name archive_name;
        size_t j;
        int group_ready = 1;
        if (item->state != RD_JOB_READY_TO_EXTRACT || !item->extract_after_download ||
            (item->error[0] && strncmp(item->error,
             "Extraction interrupted by payload restart", 42U) != 0) ||
            rd_classify_archive_name(item->filename, &archive_name) != 0 ||
            archive_name.kind == RD_ARCHIVE_NONE) continue;
        if ((archive_name.kind == RD_ARCHIVE_RAR_MULTIPART_PART ||
             archive_name.kind == RD_ARCHIVE_SPLIT_001) && archive_name.part_number > 1U)
            continue;
        if (archive_name.kind == RD_ARCHIVE_RAR_LEGACY_VOLUME) continue;
        for (j = 0; j < server_queue.count; ++j) {
            rd_queue_item *volume = &server_queue.items[j];
            if (!same_archive_set(item, volume, &archive_name)) continue;
            if (volume->state != RD_JOB_READY_TO_EXTRACT && volume->state != RD_JOB_COMPLETED) {
                group_ready = 0;
                break;
            }
        }
        if (!group_ready) continue;
        return item;
    }
    return NULL;
}

static void complete_archive_set(rd_queue_item *primary) {
    rd_archive_name archive_name;
    size_t i;
    if (rd_classify_archive_name(primary->filename, &archive_name) != 0) return;
    for (i = 0; i < server_queue.count; ++i) {
        rd_queue_item *volume = &server_queue.items[i];
        if (!same_archive_set(primary, volume, &archive_name) ||
            (volume->state != RD_JOB_READY_TO_EXTRACT && volume->id != primary->id)) continue;
        if (rd_queue_update(&server_queue, volume->id, RD_JOB_COMPLETED,
                            volume->downloaded_bytes, volume->expected_bytes, "") == 0) {
            volume = &server_queue.items[i];
            volume->extraction_done_bytes = volume->extraction_total_bytes = 0;
            volume->extraction_speed_bps = volume->extraction_average_bps =
                volume->extraction_eta_seconds = 0;
            volume->extraction_current_file[0] = '\0';
            memset(volume->extraction_password, 0, sizeof(volume->extraction_password));
        }
    }
}

static void *download_worker(void *unused) {
    (void)unused;
    for (;;) {
        char url[RD_MAX_URL_LENGTH];
        char destination[512];
        char filename[512];
        char archive_path[1024];
        char extraction_destination[512];
        char password[RD_MAX_PASSWORD_LENGTH];
        unsigned long id = 0;
        size_t i;
        int result, extracting = 0;
        rd_progress_context progress;
        rd_extraction_context extraction_progress;
        char error[256];
        char package_warning[256] = "";
        char nested_warning[256] = "";
        rd_nested_job nested_jobs[RD_MAX_NESTED_ARCHIVES];
        size_t nested_job_count = 0, nested_cursor = 0;
        int extract_nested = 0;
        unsigned int nested_failures = 0;

        pthread_mutex_lock(&queue_mutex);
        for (;;) {
            time_t wall_now = time(NULL);
            for (i = 0; i < server_queue.count; ++i) {
                rd_queue_item *retry = &server_queue.items[i];
                if (retry->state == RD_JOB_WAITING_FOR_NETWORK &&
                    retry->retry_after_epoch <= (unsigned long long)wall_now) {
                    if (rd_queue_update(&server_queue, retry->id, RD_JOB_QUEUED, 0, 0, "") == 0)
                        download_requested = 1;
                }
            }
            if (download_requested && download_directory_ready) {
                for (i = 0; i < server_queue.count; ++i) {
                    rd_queue_item *item = &server_queue.items[i];
                    if (item->state != RD_JOB_QUEUED) continue;
                    if (rd_queue_update(&server_queue, item->id, RD_JOB_DOWNLOADING, 0, 0, "") != 0) {
                        item->state = RD_JOB_FAILED;
                        continue;
                    }
                    id = item->id;
                    rd_http_begin(id);
                    active_download_id = id;
                    memcpy(url, item->url, sizeof(url));
                    snprintf(destination, sizeof(destination), "%s", download_directory(item));
                    snprintf(filename, sizeof(filename), "%s", item->filename);
                    break;
                }
            }
            if (id) break;
            download_requested = 0;
            if (!queue_has_pending_downloads()) {
                rd_queue_item *item = find_extract_candidate();
                if (item) {
                    rd_archive_name archive_name;
                    (void)rd_classify_archive_name(item->filename, &archive_name);
                    i = (size_t)(item - server_queue.items);
                    id = item->id;
                    snprintf(archive_path, sizeof(archive_path), "%s/%s",
                             download_directory(item), item->filename);
                    snprintf(extraction_destination, sizeof(extraction_destination), "%s",
                             RD_EXTRACTION_DIRECTORY);
                    snprintf(password, sizeof(password), "%s", item->extraction_password);
                    extract_nested = item->extract_nested_archives;
                    (void)rd_queue_update(&server_queue, id, RD_JOB_EXTRACTING,
                                          item->downloaded_bytes, item->expected_bytes, "");
                    item = &server_queue.items[i];
                    item->extraction_done_bytes = item->extraction_total_bytes = 0;
                    item->extraction_speed_bps = item->extraction_average_bps =
                        item->extraction_eta_seconds = 0;
                    item->extraction_current_file[0] = '\0';
                    active_extraction_id = id;
                    extracting = 1;
                    break;
                }
            }
            {
                unsigned long long next_retry = 0;
                struct timespec wake_at;
                wall_now = time(NULL);
                for (i = 0; i < server_queue.count; ++i) {
                    const rd_queue_item *retry = &server_queue.items[i];
                    if (retry->state == RD_JOB_WAITING_FOR_NETWORK &&
                        retry->retry_after_epoch > (unsigned long long)wall_now &&
                        (!next_retry || retry->retry_after_epoch < next_retry))
                        next_retry = retry->retry_after_epoch;
                }
                if (next_retry) {
                    wake_at.tv_sec = (time_t)next_retry;
                    wake_at.tv_nsec = 0;
                    (void)pthread_cond_timedwait(&worker_condition, &queue_mutex, &wake_at);
                    download_requested = 1;
                } else pthread_cond_wait(&worker_condition, &queue_mutex);
            }
        }
        pthread_mutex_unlock(&queue_mutex);

        if (extracting) {
            memset(&extraction_progress, 0, sizeof(extraction_progress));
            extraction_progress.id = id;
            extraction_progress.source_scope_hash = relative_path_hash(archive_path);
            extraction_progress.started_ms = monotonic_ms();
            error[0] = '\0';
            result = rd_wfm_extract(id, archive_path, extraction_destination, password,
                                    record_extraction_progress, extraction_cancel_requested,
                                    &extraction_progress,
                                    error, sizeof(error));
            if (result == 0) {
                promote_extracted_packages(&extraction_progress, extraction_destination,
                                           package_warning, sizeof(package_warning));
                if (move_extracted_archives(&extraction_progress, extraction_destination,
                        extract_nested ? nested_jobs : NULL,
                        extract_nested ? &nested_job_count : NULL, 0,
                        nested_warning, sizeof(nested_warning)) != 0 && result == 0)
                    nested_failures++;
            }
            while (result == 0 && extract_nested && nested_cursor < nested_job_count) {
                rd_nested_job job = nested_jobs[nested_cursor++];
                rd_extraction_context child_progress;
                char child_error[256] = "";
                memset(&child_progress, 0, sizeof(child_progress));
                child_progress.id = id;
                child_progress.source_scope_hash = relative_path_hash(job.path);
                child_progress.started_ms = monotonic_ms();
                if (rd_wfm_extract(id, job.path, extraction_destination, password,
                        record_extraction_progress, extraction_cancel_requested,
                        &child_progress, child_error, sizeof(child_error)) == 0) {
                    char child_warning[256] = "";
                    promote_extracted_packages(&child_progress, extraction_destination,
                                               child_warning, sizeof(child_warning));
                    if (child_warning[0] && !package_warning[0])
                        snprintf(package_warning, sizeof(package_warning), "%s", child_warning);
                    if (move_extracted_archives(&child_progress, extraction_destination,
                            nested_jobs, &nested_job_count, job.depth,
                            nested_warning, sizeof(nested_warning)) != 0)
                        nested_failures++;
                } else {
                    nested_failures++;
                    fprintf(stderr, "RD Downloader: nested archive extraction failed for queue item %lu: %s\n",
                            id, child_error);
                    if (move_extracted_archives(&child_progress, extraction_destination,
                            NULL, NULL, job.depth, nested_warning,
                            sizeof(nested_warning)) != 0)
                        nested_failures++;
                }
            }
            if (nested_failures && !nested_warning[0])
                snprintf(nested_warning, sizeof(nested_warning),
                         "%u nested archive(s) were kept in downloads because extraction failed",
                         nested_failures);
            memset(password, 0, sizeof(password));
            pthread_mutex_lock(&queue_mutex);
            active_extraction_id = 0;
            for (i = 0; i < server_queue.count; ++i) {
                rd_queue_item *item = &server_queue.items[i];
                if (item->id != id) continue;
                if (item->state == RD_JOB_CANCELLED) {
                    if (finish_removal(item, item->remove_downloads_on_cancel) != 0)
                        (void)rd_queue_update(&server_queue, id, RD_JOB_FAILED,
                            item->downloaded_bytes, item->expected_bytes,
                            "Cleanup failed. Remove again to retry cleanup.");
                    break;
                }
                if (result == 0) {
                    complete_archive_set(item);
                    if (package_warning[0] || nested_warning[0]) {
                        char completion_warning[256];
                        if (package_warning[0] && nested_warning[0])
                            snprintf(completion_warning, sizeof(completion_warning), "%.120s; %.120s",
                                     package_warning, nested_warning);
                        else snprintf(completion_warning, sizeof(completion_warning), "%s",
                                      package_warning[0] ? package_warning : nested_warning);
                        (void)rd_queue_update(&server_queue, id, RD_JOB_COMPLETED,
                            item->downloaded_bytes, item->expected_bytes, completion_warning);
                    }
                }
                else {
                    (void)rd_queue_update(&server_queue, id, RD_JOB_READY_TO_EXTRACT,
                        item->downloaded_bytes, item->expected_bytes,
                        error[0] ? error : "archive extraction failed");
                    item = &server_queue.items[i];
                    item->extraction_done_bytes = item->extraction_total_bytes = 0;
                    item->extraction_speed_bps = item->extraction_average_bps =
                        item->extraction_eta_seconds = 0;
                    item->extraction_current_file[0] = '\0';
                }
                if (item->state == RD_JOB_COMPLETED)
                    memset(item->extraction_password, 0, sizeof(item->extraction_password));
                break;
            }
            pthread_mutex_unlock(&queue_mutex);
            continue;
        }

        progress.id = id;
        progress.last_persisted = 0;
        progress.started_ms = monotonic_ms();
        progress.previous_ms = 0;
        progress.previous_bytes = 0;
        write_log("download started id=%lu file=%s destination=%s", id, filename, destination);
        result = rd_http_download(url, destination, id, record_progress,
                                  &progress, error, sizeof(error));
        write_log("download %s id=%lu file=%s destination=%s%s%s", result == 0 ? "completed" : "failed",
                  id, filename, destination, result == 0 ? "" : " error=",
                  result == 0 ? "" : error);

        pthread_mutex_lock(&queue_mutex);
        active_download_id = 0;
        for (i = 0; i < server_queue.count; ++i) {
            rd_queue_item *item = &server_queue.items[i];
            if (item->id != id) continue;
            if (item->state == RD_JOB_CANCELLED) {
                if (finish_removal(item, item->remove_downloads_on_cancel))
                    (void)rd_queue_update(&server_queue, id, RD_JOB_FAILED,
                        item->downloaded_bytes, item->expected_bytes,
                        "Cleanup failed. Remove again to retry cleanup.");
                break;
            }
            rd_archive_name archive_name;
            int is_archive = rd_classify_archive_name(item->filename, &archive_name) == 0 &&
                             archive_name.kind != RD_ARCHIVE_NONE;
            rd_job_state final_state = result == 0 ?
                ((item->extract_after_download && is_archive) ? RD_JOB_READY_TO_EXTRACT : RD_JOB_COMPLETED) :
                RD_JOB_FAILED;
            const char *final_error = result != 0 ? error : "";
            int state_result;
            if (result != 0 && is_retryable_download_error(error) && item->retry_attempts < 5U) {
                unsigned int attempts = item->retry_attempts + 1U;
                unsigned int delay_seconds = 5U << (attempts - 1U);
                char retry_message[sizeof(item->error)];
                if (delay_seconds > 300U) delay_seconds = 300U;
                if (discard_output(item, 0) != 0) {
                    state_result = rd_queue_set_retry(&server_queue, id, attempts, 0,
                        RD_JOB_FAILED, "Download failed; partial file cleanup failed");
                } else {
                    unsigned long long retry_at = (unsigned long long)time(NULL) + delay_seconds;
                    snprintf(retry_message, sizeof(retry_message),
                             "Retry %u/5 in %u seconds after a temporary connection error",
                             attempts, delay_seconds);
                    state_result = rd_queue_set_retry(&server_queue, id, attempts, retry_at,
                                                       RD_JOB_WAITING_FOR_NETWORK, retry_message);
                }
            } else if (result == 0) {
                state_result = rd_queue_set_retry(&server_queue, id, 0, 0, final_state, "");
            } else {
                state_result = rd_queue_set_retry(&server_queue, id, item->retry_attempts, 0,
                                                   RD_JOB_FAILED, final_error);
            }
            if (state_result != 0) {
                item->state = RD_JOB_FAILED;
            }
            break;
        }
        pthread_mutex_unlock(&queue_mutex);
    }
    return NULL;
}

static void handle_client(int fd) {
    char request[RD_HTTP_MAX_REQUEST + 1U];
    size_t used = 0;
    const char *end_headers = NULL;
    size_t body_length = 0, body_offset;
    struct timeval send_timeout = { .tv_sec = 5, .tv_usec = 0 };
    unsigned long long deadline_ms = monotonic_ms() + RD_HTTP_REQUEST_TIMEOUT_MS;

    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    while (used < RD_HTTP_MAX_REQUEST) {
        ssize_t n = recv_before_deadline(fd, request + used,
                                         RD_HTTP_MAX_REQUEST - used, deadline_ms);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (n == 0) return;
        used += (size_t)n;
        request[used] = '\0';
        end_headers = strstr(request, "\r\n\r\n");
        if (end_headers) break;
    }

    if (!end_headers) {
        static const char error_body[] = "request headers too large\n";
        (void)send_response(fd, 431, "Request Header Fields Too Large",
                            "text/plain; charset=utf-8", error_body,
                            sizeof(error_body) - 1U);
        return;
    }

    {
        char method[8];
        char path[256];
        char version[16];
        int fields = sscanf(request, "%7s %255s %15s", method, path, version);
        if (fields != 3 || (strcmp(version, "HTTP/1.1") != 0 &&
                            strcmp(version, "HTTP/1.0") != 0)) {
            static const char error_body[] = "bad request\n";
            (void)send_response(fd, 400, "Bad Request", "text/plain; charset=utf-8",
                                error_body, sizeof(error_body) - 1U);
        } else if (content_length_of(request, &body_length) != 0) {
            static const char error_body[] = "invalid content length\n";
            (void)send_response(fd, 400, "Bad Request", "text/plain; charset=utf-8",
                                error_body, sizeof(error_body) - 1U);
        } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/start") == 0) {
            if (!command_header_ok(request)) {
                static const char error_body[] = "command header required\n";
                (void)send_response(fd, 403, "Forbidden", "text/plain; charset=utf-8",
                                    error_body, sizeof(error_body) - 1U);
            } else if (!queue_ready || !worker_started || body_length != 0) {
                static const char error_body[] = "download worker unavailable or invalid body\n";
                (void)send_response(fd, 503, "Service Unavailable", "text/plain; charset=utf-8",
                                    error_body, sizeof(error_body) - 1U);
            } else {
                pthread_mutex_lock(&queue_mutex);
                download_requested = 1;
                pthread_cond_signal(&worker_condition);
                pthread_mutex_unlock(&queue_mutex);
                (void)send_queue(fd);
            }
        } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/clear") == 0) {
            int cleared;
            if (!command_header_ok(request) || body_length != 0) {
                static const char error_body[] = "invalid clear request\n";
                (void)send_response(fd, 400, "Bad Request", "text/plain; charset=utf-8",
                                    error_body, sizeof(error_body) - 1U);
            } else if (!queue_ready) {
                static const char error_body[] = "queue storage unavailable\n";
                (void)send_response(fd, 503, "Service Unavailable", "text/plain; charset=utf-8",
                                    error_body, sizeof(error_body) - 1U);
            } else {
                pthread_mutex_lock(&queue_mutex);
                for (size_t i = 0; i < server_queue.count; ++i)
                    if (server_queue.items[i].state == RD_JOB_COMPLETED)
                        (void)discard_output(&server_queue.items[i], 0);
                cleared = rd_queue_clear_completed(&server_queue);
                pthread_mutex_unlock(&queue_mutex);
                if (cleared < 0) {
                    static const char error_body[] = "could not persist queue update\n";
                    (void)send_response(fd, 503, "Service Unavailable", "text/plain; charset=utf-8",
                                        error_body, sizeof(error_body) - 1U);
                } else (void)send_queue(fd);
            }
        } else if (strcmp(method, "POST") == 0 &&
                   (strcmp(path, "/api/queue") == 0 || strcmp(path, "/api/add") == 0)) {
            char urls[4][RD_MAX_URL_LENGTH];
            char password[RD_PASSWORD_HEADER_MAX] = "";
            char extract_setting[8] = "1";
            char nested_setting[8] = "0";
            char url_batch[RD_HTTP_MAX_BODY + 1U];
            int extract_enabled = 1;
            int form_request = strcmp(path, "/api/add") == 0;
            int count;
            size_t received;
            body_offset = (size_t)((end_headers + 4) - request);
            if (!command_header_ok(request) || body_length == 0 ||
                (!form_request && (copy_header_value(request, "X-RD-Extract", extract_setting, sizeof(extract_setting)) < 0 ||
                copy_header_value(request, "X-RD-Password", password, sizeof(password)) < 0)) ||
                body_offset + body_length > RD_HTTP_MAX_REQUEST) {
                static const char error_body[] = "invalid queue options or URL body\n";
                (void)send_response(fd, 400, "Bad Request", "text/plain; charset=utf-8",
                                    error_body, sizeof(error_body) - 1U);
                return;
            }
            received = used > body_offset ? used - body_offset : 0;
            if (received > body_length) received = body_length;
            used = body_offset + received;
            while (received < body_length) {
                ssize_t got = recv_before_deadline(fd, request + used,
                                                   body_length - received, deadline_ms);
                if (got <= 0) return;
                used += (size_t)got;
                received += (size_t)got;
            }
            if (memchr(request + body_offset, '\0', body_length) != NULL) {
                static const char error_body[] = "request body contains a NUL byte\n";
                (void)send_response(fd, 400, "Bad Request", "text/plain; charset=utf-8",
                                    error_body, sizeof(error_body) - 1U);
                return;
            }
            request[body_offset + body_length] = '\0';
            if (form_request) {
                if (parse_add_form(request + body_offset, body_length, url_batch, sizeof(url_batch),
                                   extract_setting, sizeof(extract_setting), nested_setting,
                                   sizeof(nested_setting), password, sizeof(password)) != 0) {
                    static const char error_body[] = "invalid add form\n";
                    (void)send_response(fd, 400, "Bad Request", "text/plain; charset=utf-8",
                                        error_body, sizeof(error_body) - 1U);
                    return;
                }
            } else {
                if (body_length >= sizeof(url_batch)) {
                    static const char error_body[] = "URL batch is too large\n";
                    (void)send_response(fd, 400, "Bad Request", "text/plain; charset=utf-8",
                                        error_body, sizeof(error_body) - 1U);
                    return;
                }
                memcpy(url_batch, request + body_offset, body_length + 1U);
            }
            if (strcmp(extract_setting, "0") != 0 && strcmp(extract_setting, "1") != 0) {
                static const char error_body[] = "invalid extraction option\n";
                (void)send_response(fd, 400, "Bad Request", "text/plain; charset=utf-8",
                                    error_body, sizeof(error_body) - 1U);
                return;
            }
            if (strcmp(nested_setting, "0") != 0 && strcmp(nested_setting, "1") != 0) {
                static const char error_body[] = "invalid nested extraction option\n";
                (void)send_response(fd, 400, "Bad Request", "text/plain; charset=utf-8",
                                    error_body, sizeof(error_body) - 1U);
                return;
            }
            if (!queue_ready || !worker_started) {
                static const char error_body[] = "download worker or queue unavailable\n";
                (void)send_response(fd, 503, "Service Unavailable", "text/plain", error_body, sizeof(error_body)-1);
                return;
            }
            count = rd_parse_url_batch(url_batch, urls, 4);
            extract_enabled = strcmp(extract_setting, "1") == 0;
            if (count <= 0) {
                static const char error_body[] = "invalid URL batch (maximum 4 URLs per request)\n";
                (void)send_response(fd, 400, "Bad Request", "text/plain; charset=utf-8",
                                    error_body, sizeof(error_body) - 1U);
            } else {
                int add_result;
                pthread_mutex_lock(&queue_mutex);
                add_result = rd_queue_add_batch_preferences(&server_queue,
                    (const char (*)[RD_MAX_URL_LENGTH])urls, (size_t)count,
                    extract_enabled, extract_enabled && strcmp(nested_setting, "1") == 0,
                    password);
                if (add_result >= 0) {
                    download_requested = 1;
                    pthread_cond_signal(&worker_condition);
                }
                pthread_mutex_unlock(&queue_mutex);
                if (add_result < 0) {
                    static const char error_body[] = "queue persistence failed or queue is full\n";
                    (void)send_response(fd, 503, "Service Unavailable", "text/plain; charset=utf-8",
                                        error_body, sizeof(error_body) - 1U);
                } else {
                    (void)send_queue(fd);
                }
            }
        } else if (strcmp(method, "POST") == 0 && strncmp(path, "/api/queue/", 11) == 0) {
            char *end;
            unsigned long id = strtoul(path + 11, &end, 10);
            const char *action = *end == '/' ? end + 1 : "";
            char retry_password[RD_PASSWORD_HEADER_MAX] = "";
            int password_header = copy_header_value(request, "X-RD-Password",
                                                   retry_password, sizeof(retry_password));
            size_t i;
            int found = 0, action_result = -1;
            if (!command_header_ok(request) || !id || *end != '/' || !*action ||
                body_length != 0 || password_header < 0) {
                static const char error_body[] = "invalid queue action\n";
                (void)send_response(fd, 400, "Bad Request", "text/plain; charset=utf-8",
                                    error_body, sizeof(error_body) - 1U);
            } else {
                pthread_mutex_lock(&queue_mutex);
                for (i = 0; i < server_queue.count; ++i) {
                    rd_queue_item *item = &server_queue.items[i];
                    if (item->id != id) continue;
                    found = 1;
                    if (strcmp(action, "pause") == 0 &&
                        (item->state == RD_JOB_QUEUED || item->state == RD_JOB_DOWNLOADING ||
                         item->state == RD_JOB_WAITING_FOR_NETWORK)) {
                        action_result = rd_queue_set_retry(&server_queue, id,
                            item->retry_attempts, 0, RD_JOB_PAUSED, "");
                    } else if (strcmp(action, "resume") == 0 && item->state == RD_JOB_PAUSED) {
                        rd_job_state next = active_download_id == id ? RD_JOB_DOWNLOADING : RD_JOB_QUEUED;
                        action_result = rd_queue_set_retry(&server_queue, id,
                            0, 0, next, "");
                        if (next == RD_JOB_QUEUED) download_requested = 1;
                        pthread_cond_broadcast(&worker_condition);
                    } else if (strcmp(action, "retry") == 0 && item->state == RD_JOB_FAILED &&
                               !active_download_id && !active_extraction_id) {
                        if (discard_output(item, 0) == 0) {
                            item->legacy_partial = 0;
                            action_result = rd_queue_set_retry(&server_queue, id, 0, 0,
                                                               RD_JOB_QUEUED, "");
                            if (action_result == 0) {
                                item = &server_queue.items[i];
                                item->speed_bps = item->average_bps = item->eta_seconds = 0;
                                if (password_header > 0)
                                    snprintf(item->extraction_password,
                                             sizeof(item->extraction_password), "%s", retry_password);
                                download_requested = 1;
                                pthread_cond_signal(&worker_condition);
                            }
                        }
                    } else if (strcmp(action, "retry") == 0 &&
                               item->state == RD_JOB_READY_TO_EXTRACT && item->error[0] &&
                               !active_extraction_id) {
                        action_result = rd_queue_update(&server_queue, id, RD_JOB_READY_TO_EXTRACT,
                            item->downloaded_bytes, item->expected_bytes, "");
                        if (action_result == 0) {
                            item = &server_queue.items[i];
                            if (password_header > 0)
                                snprintf(item->extraction_password, sizeof(item->extraction_password),
                                         "%s", retry_password);
                            download_requested = 1;
                            pthread_cond_signal(&worker_condition);
                        }
                    }
                    break;
                }
                pthread_mutex_unlock(&queue_mutex);
                if (!found) {
                    static const char error_body[] = "queue item not found\n";
                    (void)send_response(fd, 404, "Not Found", "text/plain; charset=utf-8",
                                        error_body, sizeof(error_body) - 1U);
                } else if (action_result != 0) {
                    static const char error_body[] = "action is invalid for the current state or could not be saved\n";
                    (void)send_response(fd, 409, "Conflict", "text/plain; charset=utf-8",
                                        error_body, sizeof(error_body) - 1U);
                } else (void)send_queue(fd);
            }
        } else if (strcmp(method, "DELETE") == 0 && strncmp(path, "/api/queue/", 11) == 0) {
            char *end;
            char *query = strchr(path, '?');
            unsigned long id;
            int delete_files = 0;
            if (query) {
                *query++ = '\0';
                delete_files = strcmp(query, "deleteFiles=1") == 0;
            }
            if (!command_header_ok(request)) {
                static const char error_body[] = "command header required\n";
                (void)send_response(fd, 403, "Forbidden", "text/plain; charset=utf-8",
                                    error_body, sizeof(error_body) - 1U);
            } else if (query && !delete_files) {
                static const char error_body[] = "invalid delete option\n";
                (void)send_response(fd, 400, "Bad Request", "text/plain; charset=utf-8",
                                    error_body, sizeof(error_body) - 1U);
            } else {
                id = strtoul(path + 11, &end, 10);
                if (path[11] == '\0' || *end || id == 0) {
                    static const char error_body[] = "invalid queue id\n";
                    (void)send_response(fd, 400, "Bad Request", "text/plain; charset=utf-8",
                                        error_body, sizeof(error_body) - 1U);
                } else if (!queue_ready) {
                    static const char error_body[] = "queue storage unavailable\n";
                    (void)send_response(fd, 503, "Service Unavailable", "text/plain; charset=utf-8",
                                        error_body, sizeof(error_body) - 1U);
                } else {
                    int remove_result = -2;
                    size_t i;
                    pthread_mutex_lock(&queue_mutex);
                    for (i = 0; i < server_queue.count; ++i) {
                        rd_queue_item *item = &server_queue.items[i];
                        if (item->id != id) continue;
                        item->remove_downloads_on_cancel = delete_files;
                        if (active_download_id == id) {
                            remove_result = rd_queue_update(&server_queue, id, RD_JOB_CANCELLED,
                                item->downloaded_bytes, item->expected_bytes, "Stopping download");
                            if (remove_result == 0) rd_http_cancel(id);
                            pthread_cond_broadcast(&worker_condition);
                        } else if (active_extraction_id == id) {
                            remove_result = rd_queue_update(&server_queue, id, RD_JOB_CANCELLED,
                                item->downloaded_bytes, item->expected_bytes, "Stopping extraction");
                            pthread_cond_broadcast(&worker_condition);
                        } else {
                            remove_result = finish_removal(item, delete_files);
                        }
                        break;
                    }
                    pthread_mutex_unlock(&queue_mutex);
                    if (remove_result == -2) {
                        static const char error_body[] = "queue item not found\n";
                        (void)send_response(fd, 404, "Not Found", "text/plain; charset=utf-8",
                                            error_body, sizeof(error_body) - 1U);
                    } else if (remove_result != 0) {
                        static const char error_body[] = "could not clean up download or save queue\n";
                        (void)send_response(fd, 503, "Service Unavailable", "text/plain; charset=utf-8",
                                            error_body, sizeof(error_body) - 1U);
                    } else {
                        (void)send_queue(fd);
                    }
                }
            }
        } else if (strcmp(method, "GET") != 0) {
            static const char error_body[] = "method not allowed\n";
            (void)send_response(fd, 405, "Method Not Allowed",
                                "text/plain; charset=utf-8", error_body,
                                sizeof(error_body) - 1U);
        } else if (strcmp(path, "/") == 0) {
            (void)send_response(fd, 200, "OK", "text/html; charset=utf-8",
                                index_body, sizeof(index_body) - 1U);
        } else if (strcmp(path, "/api/health") == 0) {
            (void)send_health(fd);
        } else if (strcmp(path, "/api/queue") == 0) {
            (void)send_queue(fd);
        } else {
            static const char error_body[] = "not found\n";
            (void)send_response(fd, 404, "Not Found", "text/plain; charset=utf-8",
                                error_body, sizeof(error_body) - 1U);
        }
    }
}

static void *client_thread(void *argument) {
    int fd = *(int *)argument;
    free(argument);
    handle_client(fd);
    close(fd);
    pthread_mutex_lock(&client_mutex);
    --active_clients;
    pthread_mutex_unlock(&client_mutex);
    return NULL;
}

static int prepare_download_directories(void) {
    struct stat st;
#ifdef RD_HOST_BUILD
    if (mkdir(RD_EXTRACTION_DIRECTORY, 0700) != 0 && errno != EEXIST) return -1;
#endif
    if (mkdir(RD_DOWNLOAD_DIRECTORY, 0755) != 0 && errno != EEXIST) return -1;
    if (lstat(RD_DOWNLOAD_DIRECTORY, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
    if (chmod(RD_DOWNLOAD_DIRECTORY, 0755) != 0) return -1;
    return 0;
}

int rd_http_server_run(const char *bind_address, unsigned short port) {
    struct sockaddr_in address;
    int server_fd;
    int reuse = 1;

#ifdef RD_HOST_BUILD
    queue_ready = rd_queue_init(&server_queue, "/tmp/rd-downloader-host-queue.db") == 0;
#else
    queue_ready = rd_queue_init(&server_queue, "/data/rd-downloader-queue.db") == 0;
#endif
    download_directory_ready = queue_ready && prepare_download_directories() == 0;
    if (download_directory_ready) {
        struct stat directory_stat;
        if (stat(RD_DOWNLOAD_DIRECTORY, &directory_stat) == 0)
            write_log("service started version=%s download_directory=%s mode=%04o uid=%u gid=%u",
                      RD_DOWNLOADER_VERSION, RD_DOWNLOAD_DIRECTORY,
                      (unsigned int)(directory_stat.st_mode & 07777),
                      (unsigned int)directory_stat.st_uid, (unsigned int)directory_stat.st_gid);
        else write_log("service started version=%s download_directory=%s stat_errno=%d",
                       RD_DOWNLOADER_VERSION, RD_DOWNLOAD_DIRECTORY, errno);
    } else if (queue_ready) {
        write_log("downloads directory setup failed path=%s errno=%d",
                  RD_DOWNLOAD_DIRECTORY, errno);
        fprintf(stderr, "RD Downloader: cannot prepare downloads directory %s: %s\n",
                RD_DOWNLOAD_DIRECTORY, strerror(errno));
    }

    if (queue_ready) {
        for (size_t i = server_queue.count; i > 0; --i) {
            rd_queue_item *item = &server_queue.items[i - 1];
            if (item->state == RD_JOB_CANCELLED &&
                finish_removal(item, item->remove_downloads_on_cancel))
                (void)rd_queue_update(&server_queue, item->id, RD_JOB_FAILED,
                    item->downloaded_bytes, item->expected_bytes,
                    "Cleanup failed. Remove again to retry cleanup.");
        }
    }

    {
        pthread_t worker;
        pthread_attr_t worker_attributes;
        int attributes_ready = pthread_attr_init(&worker_attributes) == 0;
        int stack_ready = attributes_ready &&
            pthread_attr_setstacksize(&worker_attributes, RD_DOWNLOAD_WORKER_STACK) == 0;
        if (stack_ready && pthread_create(&worker, &worker_attributes,
                                          download_worker, NULL) == 0) {
            (void)pthread_detach(worker);
            worker_started = 1;
            fprintf(stderr, "RD Downloader: worker started with %u-byte stack\n",
                    (unsigned int)RD_DOWNLOAD_WORKER_STACK);
        } else {
            fprintf(stderr, "RD Downloader: worker startup failed (thread or stack setup)\n");
        }
        if (attributes_ready) (void)pthread_attr_destroy(&worker_attributes);
    }

    if (!bind_address || port == 0) return -1;
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return -1;
    }
    (void)setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#if defined(__FreeBSD__)
    (void)setsockopt(server_fd, SOL_SOCKET, SO_NOSIGPIPE, &reuse, sizeof(reuse));
#endif
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_address, &address.sin_addr) != 1) {
        fprintf(stderr, "invalid bind address: %s\n", bind_address);
        close(server_fd);
        return -1;
    }
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        perror("bind");
        close(server_fd);
        return -1;
    }
    if (listen(server_fd, RD_HTTP_MAX_CLIENTS) != 0) {
        perror("listen");
        close(server_fd);
        return -1;
    }

    fprintf(stderr, "RD Downloader health service listening on %s:%u\n",
            bind_address, (unsigned int)port);
    for (;;) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            close(server_fd);
            return -1;
        }
        pthread_mutex_lock(&client_mutex);
        if (active_clients >= RD_HTTP_MAX_CLIENTS) {
            pthread_mutex_unlock(&client_mutex);
            close(client_fd);
            continue;
        }
        ++active_clients;
        pthread_mutex_unlock(&client_mutex);
        {
            pthread_t thread;
            pthread_attr_t attributes;
            int *client_argument = (int *)malloc(sizeof(*client_argument));
            int attributes_ready = pthread_attr_init(&attributes) == 0;
            int stack_ready = attributes_ready &&
                pthread_attr_setstacksize(&attributes, RD_HTTP_CLIENT_STACK) == 0;
            int create_result = -1;
            if (client_argument) *client_argument = client_fd;
            if (stack_ready && client_argument)
                create_result = pthread_create(&thread, &attributes, client_thread,
                                               client_argument);
            if (attributes_ready) (void)pthread_attr_destroy(&attributes);
            if (create_result == 0) {
                (void)pthread_detach(thread);
            } else {
                free(client_argument);
                pthread_mutex_lock(&client_mutex);
                --active_clients;
                pthread_mutex_unlock(&client_mutex);
                close(client_fd);
            }
        }
    }
}
