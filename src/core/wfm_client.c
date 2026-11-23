#define _POSIX_C_SOURCE 200809L
#include "rd/wfm_client.h"

#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifdef RD_HOST_BUILD
#define RD_WFM_SOCKET_PATH "/tmp/wfm-7zip-helper.sock"
#else
#define RD_WFM_SOCKET_PATH "/system_tmp/wfm-7zip-helper.sock"
#endif

#define WFM_HEADER 20U
#define WFM_MAX_PAYLOAD (1024U * 1024U)
#define WFM_PING 1U
#define WFM_PONG 2U
#define WFM_EXTRACT 10U
#define WFM_CANCEL 11U
#define WFM_LIST_TASKS 13U
#define WFM_ATTACH_TASK 14U
#define WFM_ACK_TASK 15U
#define WFM_ACCEPTED 20U
#define WFM_PROGRESS 21U
#define WFM_CURRENT_FILE 22U
#define WFM_DONE 24U
#define WFM_ERROR 25U
#define WFM_TASK_SNAPSHOT 26U
#define WFM_LIST_DONE 27U
#define WFM_HELPER_ELF_PATH "/data/wfm/wfm-7zip-helper.elf"
#define WFM_ELFLDR_PORT 9021U
#define WFM_MAX_ELF_SIZE (128U * 1024U * 1024U)

typedef struct rd_wfm_frame {
    unsigned short type;
    unsigned int payload_size;
    unsigned long long request_id;
} rd_wfm_frame;

static unsigned short get16(const unsigned char *p) {
    return (unsigned short)(((unsigned short)p[0] << 8) | p[1]);
}

static unsigned int get32(const unsigned char *p) {
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
           ((unsigned int)p[2] << 8) | p[3];
}

static unsigned long long get64(const unsigned char *p) {
    return ((unsigned long long)get32(p) << 32) | get32(p + 4);
}

static void put16(unsigned char *p, unsigned short n) {
    p[0] = (unsigned char)(n >> 8);
    p[1] = (unsigned char)n;
}

static void put32(unsigned char *p, unsigned int n) {
    p[0] = (unsigned char)(n >> 24);
    p[1] = (unsigned char)(n >> 16);
    p[2] = (unsigned char)(n >> 8);
    p[3] = (unsigned char)n;
}

static void put64(unsigned char *p, unsigned long long n) {
    put32(p, (unsigned int)(n >> 32));
    put32(p + 4, (unsigned int)n);
}

static int send_all(int fd, const void *data, size_t size) {
    const unsigned char *p = (const unsigned char *)data;
    size_t sent = 0;
    while (sent < size) {
#ifdef MSG_NOSIGNAL
        ssize_t n = send(fd, p + sent, size - sent, MSG_NOSIGNAL);
#else
        ssize_t n = send(fd, p + sent, size - sent, 0);
#endif
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, void *data, size_t size) {
    unsigned char *p = (unsigned char *)data;
    size_t received = 0;
    while (received < size) {
        ssize_t n = recv(fd, p + received, size - received, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        received += (size_t)n;
    }
    return 0;
}

static int send_frame(int fd, unsigned short type, unsigned long long id,
                      const void *payload, unsigned int payload_size) {
    unsigned char header[WFM_HEADER];
    memcpy(header, "W7HP", 4);
    put16(header + 4, type);
    put16(header + 6, 0);
    put32(header + 8, payload_size);
    put64(header + 12, id);
    return send_all(fd, header, sizeof(header)) == 0 &&
           (!payload_size || send_all(fd, payload, payload_size) == 0) ? 0 : -1;
}

static int recv_frame(int fd, rd_wfm_frame *frame, unsigned char *payload) {
    unsigned char header[WFM_HEADER];
    if (recv_all(fd, header, sizeof(header)) != 0 || memcmp(header, "W7HP", 4) != 0)
        return -1;
    frame->type = get16(header + 4);
    if (get16(header + 6) != 0) return -1;
    frame->payload_size = get32(header + 8);
    frame->request_id = get64(header + 12);
    if (frame->payload_size > WFM_MAX_PAYLOAD) return -1;
    if (frame->payload_size && recv_all(fd, payload, frame->payload_size) != 0) return -1;
    payload[frame->payload_size] = 0;
    return 0;
}

static int wait_readable(int fd, int timeout_ms) {
    struct pollfd pfd;
    int result;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    do { result = poll(&pfd, 1, timeout_ms); } while (result < 0 && errno == EINTR);
    if (result <= 0) return result;
    return (pfd.revents & (POLLIN | POLLPRI)) ? 1 : -1;
}

static int connect_helper(void) {
    struct sockaddr_un address;
    struct timeval timeout = { .tv_sec = 30, .tv_usec = 0 };
    size_t length = strlen(RD_WFM_SOCKET_PATH);
    int fd;
    if (length >= sizeof(address.sun_path)) { errno = ENAMETOOLONG; return -1; }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, RD_WFM_SOCKET_PATH, length + 1U);
#ifndef RD_HOST_BUILD
    address.sun_len = (unsigned char)(offsetof(struct sockaddr_un, sun_path) + length + 1U);
#endif
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifndef RD_HOST_BUILD
    if (connect(fd, (struct sockaddr *)&address, address.sun_len) != 0) {
#else
    if (connect(fd, (struct sockaddr *)&address,
                (socklen_t)(offsetof(struct sockaddr_un, sun_path) + length + 1U)) != 0) {
#endif
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static unsigned long long make_task_id(unsigned long queue_id) {
    struct timespec ts;
    unsigned long long id;
    (void)clock_gettime(CLOCK_REALTIME, &ts);
    id = ((unsigned long long)ts.tv_sec << 32) ^
         ((unsigned long long)ts.tv_nsec << 1) ^ (unsigned long long)queue_id;
    return id ? id : (unsigned long long)queue_id + 1U;
}

static int helper_probe(void) {
    rd_wfm_frame frame;
    unsigned char *payload = (unsigned char *)malloc(WFM_MAX_PAYLOAD + 1U);
    struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
    unsigned long long id = make_task_id(0);
    int fd;
    int result = -1;
    if (!payload) return -2;
    fd = connect_helper();
    if (fd < 0) { free(payload); return 0; }
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (send_frame(fd, WFM_PING, id, NULL, 0) == 0 &&
        recv_frame(fd, &frame, payload) == 0 && frame.type == WFM_PONG &&
        frame.request_id == id && frame.payload_size == 0) result = 1;
    close(fd);
    free(payload);
    return result;
}

#ifndef RD_HOST_BUILD
static int send_helper_elf(char *error, size_t error_capacity) {
    struct sockaddr_in address;
    struct timeval timeout = { .tv_sec = 10, .tv_usec = 0 };
    unsigned char buffer[16384];
    struct stat st;
    unsigned char magic[4];
    int file_fd = -1, socket_fd = -1, result = -1;
    off_t remaining;
    file_fd = open(WFM_HELPER_ELF_PATH, O_RDONLY);
    if (file_fd < 0) {
        snprintf(error, error_capacity, "WFM helper ELF missing at %s; install the helper first",
                 WFM_HELPER_ELF_PATH);
        goto done;
    }
    if (fstat(file_fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 4 ||
        st.st_size > (off_t)WFM_MAX_ELF_SIZE ||
        read(file_fd, magic, sizeof(magic)) != (ssize_t)sizeof(magic) ||
        memcmp(magic, "\x7f" "ELF", sizeof(magic)) != 0 || lseek(file_fd, 0, SEEK_SET) < 0) {
        snprintf(error, error_capacity, "WFM helper ELF is invalid or too large");
        goto done;
    }
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        snprintf(error, error_capacity, "could not connect to PS5 ELF loader");
        goto done;
    }
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(WFM_ELFLDR_PORT);
    address.sin_addr.s_addr = htonl(0x7f000001U);
    if (connect(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        snprintf(error, error_capacity, "PS5 ELF loader is unavailable on port %u",
                 WFM_ELFLDR_PORT);
        goto done;
    }
    remaining = st.st_size;
    while (remaining > 0) {
        size_t amount = remaining < (off_t)sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
        ssize_t count = read(file_fd, buffer, amount);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0 || send_all(socket_fd, buffer, (size_t)count) != 0) {
            snprintf(error, error_capacity, "could not send WFM helper ELF to the PS5 loader");
            goto done;
        }
        remaining -= count;
    }
    (void)shutdown(socket_fd, SHUT_WR);
    result = 0;
done:
    if (socket_fd >= 0) close(socket_fd);
    if (file_fd >= 0) close(file_fd);
    return result;
}
#endif

static int ensure_helper_running(char *error, size_t error_capacity) {
    int probe = helper_probe();
    if (probe == 1) return 0;
#ifdef RD_HOST_BUILD
    (void)error;
    (void)error_capacity;
    return 0;
#else
    if (probe == -2) {
        snprintf(error, error_capacity, "not enough memory to probe WFM helper");
        return -1;
    }
    if (probe < 0) {
        snprintf(error, error_capacity,
                 "WFM helper socket is active but did not answer PING; keeping existing process");
        return -1;
    }
    if (send_helper_elf(error, error_capacity) != 0) return -1;
    for (int i = 0; i < 50; ++i) {
        struct timespec delay = { .tv_sec = 0, .tv_nsec = 100000000L };
        (void)nanosleep(&delay, NULL);
        probe = helper_probe();
        if (probe == 1) return 0;
        if (probe < 0) break;
    }
    snprintf(error, error_capacity,
             "WFM helper ELF was sent, but the helper did not become ready");
    return -1;
#endif
}

static int snapshot_matches(const unsigned char *p, unsigned int size,
                            const char *source, const char *destination,
                            unsigned int *state, char *error, size_t error_capacity) {
    unsigned int current_len, code_len, message_len, source_len, dest_len;
    size_t offset;
    if (size < 44U) return 0;
    *state = get32(p);
    current_len = get32(p + 32);
    code_len = get32(p + 36);
    message_len = get32(p + 40);
    offset = 44U + (size_t)current_len + code_len + message_len;
    if (offset > size || size - offset < 8U) return 0;
    source_len = get32(p + offset);
    dest_len = get32(p + offset + 4U);
    offset += 8U;
    if ((size_t)source_len + dest_len > size - offset) return 0;
    if (strlen(source) != source_len || memcmp(p + offset, source, source_len) != 0)
        return 0;
    offset += source_len;
    if (strlen(destination) != dest_len || memcmp(p + offset, destination, dest_len) != 0)
        return 0;
    if (error_capacity && code_len + message_len) {
        size_t take = code_len + message_len;
        if (take >= error_capacity) take = error_capacity - 1U;
        memcpy(error, p + 44U + current_len, take);
        error[take] = '\0';
    }
    return 1;
}

static int read_error(const unsigned char *p, unsigned int size,
                      char *error, size_t capacity) {
    unsigned int code_len, message_len;
    size_t used = 0, take;
    if (size < 8 || !capacity) return -1;
    code_len = get32(p);
    message_len = get32(p + 4);
    if ((unsigned long long)code_len + message_len > size - 8U) return -1;
    error[0] = '\0';
    if (code_len) {
        take = code_len < capacity - 1U ? code_len : capacity - 1U;
        memcpy(error, p + 8, take);
        used = take;
    }
    if (message_len && used + 1U < capacity) {
        if (used && used + 2U < capacity) {
            error[used++] = ':';
            error[used++] = ' ';
        }
        take = message_len < capacity - used - 1U ? message_len : capacity - used - 1U;
        memcpy(error + used, p + 8U + code_len, take);
        used += take;
    }
    error[used] = '\0';
    return 0;
}

static int acknowledge(int fd, unsigned long long task_id,
                       rd_wfm_frame *frame, unsigned char *payload) {
    if (send_frame(fd, WFM_ACK_TASK, task_id, NULL, 0) != 0 ||
        recv_frame(fd, frame, payload) != 0) return -1;
    return frame->type == WFM_DONE && frame->request_id == task_id ? 0 : -1;
}

static int collect_result(int fd, unsigned long long task_id, int attached,
                          rd_wfm_progress_fn progress, void *context,
                          rd_wfm_cancel_fn cancel,
                          rd_wfm_frame *frame, unsigned char *payload,
                          char *error, size_t error_capacity) {
    int accepted = attached;
    int cancel_sent = 0;
    if (attached) {
        if (recv_frame(fd, frame, payload) != 0 || frame->type != WFM_TASK_SNAPSHOT ||
            frame->request_id != task_id || frame->payload_size < 44U) {
            snprintf(error, error_capacity, "helper could not attach to extraction task");
            return -1;
        }
        if (get32(payload) != 1U) {
            int success = get32(payload) == 2U;
            if (!success)
                snprintf(error, error_capacity, "%.240s", "previous extraction task failed");
            (void)acknowledge(fd, task_id, frame, payload);
            return success ? 0 : -1;
        }
        if (progress && frame->payload_size >= 44U) {
            unsigned int current_len = get32(payload + 32);
            const char *current = NULL;
            char path[512];
            if (current_len <= frame->payload_size - 44U) {
                size_t take = current_len < sizeof(path) - 1U ? current_len : sizeof(path) - 1U;
                memcpy(path, payload + 44U, take);
                path[take] = '\0';
                if (take) current = path;
            }
            if (progress(get64(payload + 24), get64(payload + 16), NULL, context) != 0 &&
                send_frame(fd, WFM_CANCEL, task_id, NULL, 0) == 0) cancel_sent = 1;
            if (!cancel_sent && current && progress(0, 0, current, context) != 0 &&
                send_frame(fd, WFM_CANCEL, task_id, NULL, 0) == 0) cancel_sent = 1;
        }
    }
    for (;;) {
        unsigned long long done, total;
        int ready = wait_readable(fd, 250);
        if (!cancel_sent && cancel && cancel(context)) {
            if (send_frame(fd, WFM_CANCEL, task_id, NULL, 0) == 0) cancel_sent = 1;
            else {
                snprintf(error, error_capacity, "could not cancel extraction helper task");
                return -1;
            }
        }
        if (ready == 0) continue;
        if (ready < 0) {
            snprintf(error, error_capacity, "connection to extraction helper was lost");
            return -1;
        }
        if (recv_frame(fd, frame, payload) != 0) {
            snprintf(error, error_capacity, "connection to extraction helper was lost");
            return -1;
        }
        if (!accepted) {
            if (frame->type == WFM_ERROR) {
                if (read_error(payload, frame->payload_size, error, error_capacity) != 0)
                    snprintf(error, error_capacity, "extraction helper rejected the request");
                return -1;
            }
            if (frame->type != WFM_ACCEPTED || frame->request_id != task_id) {
                snprintf(error, error_capacity, "unexpected response from extraction helper");
                return -1;
            }
            accepted = 1;
            continue;
        }
        if (frame->request_id != task_id) continue;
        if (frame->type == WFM_PROGRESS && frame->payload_size >= 16U) {
            done = get64(payload + 8);
            total = get64(payload);
            if (!cancel_sent && progress && progress(done, total, NULL, context) != 0 &&
                send_frame(fd, WFM_CANCEL, task_id, NULL, 0) == 0) cancel_sent = 1;
        } else if (frame->type == WFM_CURRENT_FILE && frame->payload_size >= 4U) {
            unsigned int path_len = get32(payload);
            char path[512];
            const char *current = NULL;
            if (path_len <= frame->payload_size - 4U) {
                size_t take = path_len < sizeof(path) - 1U ? path_len : sizeof(path) - 1U;
                memcpy(path, payload + 4, take);
                path[take] = '\0';
                current = path;
            }
            if (!cancel_sent && progress && progress(0, 0, current, context) != 0 &&
                send_frame(fd, WFM_CANCEL, task_id, NULL, 0) == 0) cancel_sent = 1;
        } else if (frame->type == WFM_DONE) {
            return acknowledge(fd, task_id, frame, payload) == 0 ? 0 : -1;
        } else if (frame->type == WFM_ERROR) {
            (void)read_error(payload, frame->payload_size, error, error_capacity);
            (void)acknowledge(fd, task_id, frame, payload);
            return -1;
        }
    }
}

int rd_wfm_extract(unsigned long queue_id, const char *archive_path,
                   const char *destination, const char *password,
                   rd_wfm_progress_fn progress, rd_wfm_cancel_fn cancel, void *context,
                   char *error, size_t error_capacity) {
    rd_wfm_frame frame;
    unsigned char *payload;
    unsigned long long task_id, list_id;
    size_t source_len, destination_len, password_len, payload_size;
    int fd, attached = 0, result = -1;
    unsigned int found_state = 0;
    char found_error[256] = "";

    if (!archive_path || !*archive_path || !destination || !*destination ||
        !error || !error_capacity) return -1;
    error[0] = '\0';
    source_len = strlen(archive_path);
    destination_len = strlen(destination);
    password_len = password ? strlen(password) : 0;
    if (password_len >= 256U || source_len > 4096U || destination_len > 4096U) {
        snprintf(error, error_capacity, "archive path or password is too long");
        return -1;
    }
    payload = (unsigned char *)malloc(WFM_MAX_PAYLOAD + 1U);
    if (!payload) { snprintf(error, error_capacity, "not enough memory for helper IPC"); return -1; }
    if (ensure_helper_running(error, error_capacity) != 0) {
        free(payload);
        return -1;
    }
    fd = connect_helper();
    if (fd < 0) {
        snprintf(error, error_capacity, "extraction helper unavailable (%.190s)", strerror(errno));
        free(payload);
        return -1;
    }
    task_id = make_task_id(queue_id);
    list_id = task_id ^ 0xa5a5a5a55a5a5a5aULL;
    if (!list_id) list_id = task_id + 1U;
    if (send_frame(fd, WFM_PING, list_id, NULL, 0) != 0 ||
        recv_frame(fd, &frame, payload) != 0 || frame.type != WFM_PONG ||
        frame.request_id != list_id) {
        snprintf(error, error_capacity, "extraction helper did not respond to PING");
        goto done;
    }
    if (send_frame(fd, WFM_LIST_TASKS, list_id, NULL, 0) != 0) {
        snprintf(error, error_capacity, "could not list helper tasks");
        goto done;
    }
    for (;;) {
        if (recv_frame(fd, &frame, payload) != 0) {
            snprintf(error, error_capacity, "could not read helper task list");
            goto done;
        }
        if (frame.type == WFM_LIST_DONE && frame.request_id == list_id) break;
        if (frame.type == WFM_TASK_SNAPSHOT) {
            unsigned int state = 0;
            char task_error[256] = "";
            if (snapshot_matches(payload, frame.payload_size, archive_path, destination,
                                 &state, task_error, sizeof(task_error)) &&
                (!attached || (found_state != 1U && state == 1U))) {
                task_id = frame.request_id;
                found_state = state;
                snprintf(found_error, sizeof(found_error), "%s", task_error);
                attached = 1;
            }
        }
        if (frame.type == WFM_ERROR) {
            (void)read_error(payload, frame.payload_size, error, error_capacity);
            goto done;
        }
    }
    if (attached && found_state != 1U) {
        (void)acknowledge(fd, task_id, &frame, payload);
        if (found_state == 2U) {
            result = 0;
            goto done;
        }
        /* Retire a terminal failed task before retrying with the current password. */
        attached = 0;
        task_id = make_task_id(queue_id);
    }
    if (attached) {
        if (send_frame(fd, WFM_ATTACH_TASK, task_id, NULL, 0) != 0) {
            snprintf(error, error_capacity, "could not attach to previous extraction task");
            goto done;
        }
        result = collect_result(fd, task_id, 1, progress, context, cancel,
                                &frame, payload, error, error_capacity);
        goto done;
    }
    payload_size = 16U + password_len + 8U + source_len + destination_len;
    if (payload_size > WFM_MAX_PAYLOAD) {
        snprintf(error, error_capacity, "extraction request is too large");
        goto done;
    }
    put32(payload, 0);
    put32(payload + 4, (unsigned int)password_len);
    put32(payload + 8, 1);
    put32(payload + 12, 0);
    if (password_len) memcpy(payload + 16, password, password_len);
    put32(payload + 16 + password_len, (unsigned int)source_len);
    put32(payload + 20 + password_len, (unsigned int)destination_len);
    memcpy(payload + 24 + password_len, archive_path, source_len);
    memcpy(payload + 24 + password_len + source_len, destination, destination_len);
    if (send_frame(fd, WFM_EXTRACT, task_id, payload, (unsigned int)payload_size) != 0) {
        snprintf(error, error_capacity, "could not send archive to extraction helper");
        goto done;
    }
    result = collect_result(fd, task_id, 0, progress, context, cancel,
                            &frame, payload, error, error_capacity);
done:
    close(fd);
    free(payload);
    return result;
}
