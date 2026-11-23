#include "rd/http_server.h"
#include "rd/queue.h"
#include "rd/http_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>

#define RD_HTTP_MAX_REQUEST 32768U
#define RD_HTTP_MAX_BODY 16384U
#define RD_HTTP_MAX_CLIENTS 8
#define RD_HTTP_QUEUE_RESPONSE (RD_MAX_QUEUE_ITEMS * 448U + 32U)
#define RD_PROGRESS_SAVE_INTERVAL (16ULL * 1024ULL * 1024ULL)

#ifdef RD_HOST_BUILD
#define RD_DOWNLOAD_DIRECTORY "/tmp/rd-downloader-host-downloads"
#else
#define RD_DOWNLOAD_DIRECTORY RD_DEFAULT_DESTINATION
#endif

static rd_queue server_queue;
static int queue_ready;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t worker_condition = PTHREAD_COND_INITIALIZER;
static int download_requested;
static int worker_started;

static const char index_body[] =
    "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<meta name=\"theme-color\" content=\"#171421\"><title>RD Downloader</title>"
    "<style>"
    ":root{color-scheme:dark;font:16px system-ui,sans-serif;background:#171421;color:#f5f1ff}"
    "*{box-sizing:border-box}body{margin:0 auto;padding:24px 16px;max-width:760px}"
    "h1{font-size:1.65rem;margin:0 0 4px}.muted{color:#b9b0ca}"
    ".card{background:#211d2d;border:1px solid #393148;border-radius:14px;padding:16px;margin:16px 0}"
    "textarea{width:100%;min-height:130px;background:#15121c;color:#fff;border:1px solid #504663;"
    "border-radius:10px;padding:12px;font:inherit;resize:vertical}"
    "button{background:#8c5be8;color:white;border:0;border-radius:9px;padding:11px 15px;"
    "font:600 1rem system-ui;cursor:pointer}button:disabled{opacity:.55}"
    "ul{list-style:none;padding:0;margin:8px 0}li{display:flex;align-items:center;gap:12px;"
    "border-top:1px solid #393148;padding:12px 2px}.job{flex:1;min-width:0}.state{color:#c9b1ff}"
    "progress{width:100%;height:8px;accent-color:#a77cff}"
    ".error{color:#ff9a9a;min-height:1.4em}.joberror{color:#ff9a9a}small{color:#aaa1b8}"
    "</style></head><body><header><h1>RD Downloader</h1>"
    "<div class=\"muted\">PS5 downloads · plain HTTP only in this MVP</div></header>"
    "<section class=\"card\"><label for=\"urls\"><strong>Add direct HTTP links</strong></label>"
    "<p class=\"muted\">Paste one link per line. HTTPS downloads are not enabled yet.</p>"
    "<textarea id=\"urls\" placeholder=\"http://example.com/file.zip\"></textarea>"
    "<p><button id=\"add\">Add to queue</button> <button id=\"start\">Start queued downloads</button></p>"
    "<div id=\"message\" class=\"error\" role=\"status\"></div>"
    "</section><section class=\"card\"><strong>Queue</strong><ul id=\"queue\"></ul>"
    "<div id=\"empty\" class=\"muted\">No queued links.</div></section>"
    "<small>Queue state is stored on the PS5. Closing this page does not remove items.</small>"
    "<script>"
    "const list=document.querySelector('#queue'),empty=document.querySelector('#empty'),msg=document.querySelector('#message');"
    "function size(n){n=Number(n)||0;if(n<1024)return n+' B';const u=['KB','MB','GB','TB'];let i=-1;"
    "do{n/=1024;i++}while(n>=1024&&i<u.length-1);return n.toFixed(1)+' '+u[i]}"
    "async function refresh(){try{const r=await fetch('/api/queue',{cache:'no-store'});const d=await r.json();"
    "if(!r.ok)throw Error(d.error||'Queue unavailable');list.replaceChildren();empty.hidden=d.items.length>0;"
    "for(const item of d.items){const li=document.createElement('li'),label=document.createElement('div'),"
    "state=document.createElement('span'),remove=document.createElement('button'),bar=document.createElement('progress'),"
    "bytes=document.createElement('small');label.className='job';"
    "bar.max=Number(item.expected_bytes)||1;if(Number(item.expected_bytes)>0)bar.value=Number(item.downloaded_bytes);"
    "else bar.removeAttribute('value');bytes.textContent=size(item.downloaded_bytes)+' / '+"
    "(Number(item.expected_bytes)>0?size(item.expected_bytes):'size unknown');label.textContent='Item '+item.id;"
    "label.append(document.createElement('br'),bar,document.createElement('br'),bytes);"
    "if(item.error){const e=document.createElement('small');e.className='joberror';e.textContent=item.error;"
    "label.append(document.createElement('br'),e)}state.className='state';state.textContent=item.state;"
    "remove.textContent='Remove';remove.setAttribute('aria-label','Remove item '+item.id);"
    "remove.onclick=()=>command('/api/queue/'+item.id,'DELETE');li.append(label,state,remove);list.append(li)}}"
    "catch(e){empty.hidden=true;msg.textContent=e.message}}"
    "async function command(path,method,body){try{const r=await fetch(path,{method,body,headers:{'X-RD-Command':'1',"
    "'Content-Type':'text/plain'},cache:'no-store'});const t=await r.text();if(!r.ok)throw Error(t||'Request failed');"
    "msg.textContent='';if(path==='/api/queue')document.querySelector('#urls').value='';await refresh()}catch(e){msg.textContent=e.message}}"
    "document.querySelector('#add').onclick=()=>{const b=document.querySelector('#urls').value.trim();"
    "if(b)command('/api/queue','POST',b)};"
    "document.querySelector('#start').onclick=()=>{if(document.querySelector('#urls').value.trim())"
    "{msg.textContent='Add the links to the queue first, then press Start.';return}"
    "command('/api/start','POST','')};refresh();setInterval(refresh,2500);"
    "</script></body></html>\n";

static int send_response(int fd, int status, const char *reason,
                         const char *content_type, const char *body,
                         size_t body_length);

static int send_health(int fd) {
    char body[256];
    size_t i;
    int active = 0, length;
    pthread_mutex_lock(&queue_mutex);
    for (i = 0; i < server_queue.count; ++i)
        if (server_queue.items[i].state == RD_JOB_DOWNLOADING) active = 1;
    pthread_mutex_unlock(&queue_mutex);
    length = snprintf(body, sizeof(body),
        "{\"status\":\"ok\",\"application\":\"rd-downloader\","
        "\"version\":\"0.2.0-mvp-dev\",\"transport\":\"http-only\","
        "\"downloader_active\":%s}\n", active ? "true" : "false");
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
    n = snprintf(body, RD_HTTP_QUEUE_RESPONSE, "{\"items\":[");
    if (n < 0 || (size_t)n >= RD_HTTP_QUEUE_RESPONSE) goto serialization_failed;
    used = (size_t)n;
    for (i = 0; i < server_queue.count; ++i) {
        const rd_queue_item *item = &server_queue.items[i];
        n = snprintf(body + used, RD_HTTP_QUEUE_RESPONSE - used,
            "%s{\"id\":%lu,\"state\":\"%s\",\"downloaded_bytes\":%llu,\"expected_bytes\":%llu,\"error\":\"%s\"}",
            i ? "," : "", item->id, rd_job_state_name(item->state),
            item->downloaded_bytes, item->expected_bytes, item->error);
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
} rd_progress_context;

static int record_progress(unsigned long long downloaded, unsigned long long expected,
                           void *opaque) {
    rd_progress_context *context = (rd_progress_context *)opaque;
    size_t i;
    int found = 0, persist;
    pthread_mutex_lock(&queue_mutex);
    for (i = 0; i < server_queue.count; ++i) {
        rd_queue_item *item = &server_queue.items[i];
        if (item->id != context->id) continue;
        if (item->state != RD_JOB_DOWNLOADING) break;
        item->downloaded_bytes = downloaded;
        item->expected_bytes = expected;
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

static void *download_worker(void *unused) {
    (void)unused;
    for (;;) {
        char url[RD_MAX_URL_LENGTH];
        char destination[512];
        unsigned long id = 0;
        size_t i;
        int result;
        rd_progress_context progress;
        char error[256];

        pthread_mutex_lock(&queue_mutex);
        while (!download_requested) pthread_cond_wait(&worker_condition, &queue_mutex);
        for (i = 0; i < server_queue.count; ++i) {
            rd_queue_item *item = &server_queue.items[i];
            if (item->state != RD_JOB_QUEUED) continue;
            if (rd_queue_update(&server_queue, item->id, RD_JOB_DOWNLOADING, 0, 0, "") != 0) {
                item->state = RD_JOB_FAILED;
                continue;
            }
            id = item->id;
            memcpy(url, item->url, sizeof(url));
#ifdef RD_HOST_BUILD
            snprintf(destination, sizeof(destination), "%s", RD_DOWNLOAD_DIRECTORY);
#else
            snprintf(destination, sizeof(destination), "%s", item->destination);
#endif
            break;
        }
        if (!id) {
            download_requested = 0;
            pthread_mutex_unlock(&queue_mutex);
            continue;
        }
        pthread_mutex_unlock(&queue_mutex);

        progress.id = id;
        progress.last_persisted = 0;
        result = rd_http_download(url, destination, id, record_progress,
                                  &progress, error, sizeof(error));

        pthread_mutex_lock(&queue_mutex);
        for (i = 0; i < server_queue.count; ++i) {
            rd_queue_item *item = &server_queue.items[i];
            if (item->id != id) continue;
            if (rd_queue_update(&server_queue, id,
                    result == 0 ? RD_JOB_COMPLETED : RD_JOB_FAILED,
                    item->downloaded_bytes, item->expected_bytes,
                    result == 0 ? "" : error) != 0) {
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
    struct timeval timeout = { .tv_sec = 5, .tv_usec = 0 };

    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    while (used < RD_HTTP_MAX_REQUEST) {
        ssize_t n = recv(fd, request + used, RD_HTTP_MAX_REQUEST - used, 0);
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
        } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/queue") == 0) {
            char urls[4][RD_MAX_URL_LENGTH];
            int count;
            size_t received;
            body_offset = (size_t)((end_headers + 4) - request);
            if (!command_header_ok(request) || body_length == 0 ||
                body_offset + body_length > RD_HTTP_MAX_REQUEST) {
                static const char error_body[] = "command header and URL body required\n";
                (void)send_response(fd, 400, "Bad Request", "text/plain; charset=utf-8",
                                    error_body, sizeof(error_body) - 1U);
                return;
            }
            received = used > body_offset ? used - body_offset : 0;
            if (received > body_length) received = body_length;
            used = body_offset + received;
            while (received < body_length) {
                ssize_t got = recv(fd, request + used, body_length - received, 0);
                if (got <= 0) return;
                used += (size_t)got;
                received += (size_t)got;
            }
            if (memchr(request + body_offset, '\0', body_length) != NULL) {
                static const char error_body[] = "URL body contains a NUL byte\n";
                (void)send_response(fd, 400, "Bad Request", "text/plain; charset=utf-8",
                                    error_body, sizeof(error_body) - 1U);
                return;
            }
            request[body_offset + body_length] = '\0';
            count = rd_parse_url_batch(request + body_offset, urls, 4);
            if (count <= 0) {
                static const char error_body[] = "invalid URL batch (maximum 4 URLs per request)\n";
                (void)send_response(fd, 400, "Bad Request", "text/plain; charset=utf-8",
                                    error_body, sizeof(error_body) - 1U);
            } else {
                int add_result;
                pthread_mutex_lock(&queue_mutex);
                add_result = rd_queue_add_batch(&server_queue,
                    (const char (*)[RD_MAX_URL_LENGTH])urls, (size_t)count);
                pthread_mutex_unlock(&queue_mutex);
                if (add_result < 0) {
                    static const char error_body[] = "queue persistence failed or queue is full\n";
                    (void)send_response(fd, 503, "Service Unavailable", "text/plain; charset=utf-8",
                                        error_body, sizeof(error_body) - 1U);
                } else {
                    (void)send_queue(fd);
                }
            }
        } else if (strcmp(method, "DELETE") == 0 && strncmp(path, "/api/queue/", 11) == 0) {
            char *end;
            unsigned long id;
            if (!command_header_ok(request)) {
                static const char error_body[] = "command header required\n";
                (void)send_response(fd, 403, "Forbidden", "text/plain; charset=utf-8",
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
                    int remove_result;
                    int active = 0;
                    size_t i;
                    pthread_mutex_lock(&queue_mutex);
                    for (i = 0; i < server_queue.count; ++i)
                        if (server_queue.items[i].id == id &&
                            server_queue.items[i].state == RD_JOB_DOWNLOADING) active = 1;
                    remove_result = active ? -3 : rd_queue_remove(&server_queue, id);
                    pthread_mutex_unlock(&queue_mutex);
                    if (remove_result == -3) {
                        static const char error_body[] = "active download cannot be removed yet\n";
                        (void)send_response(fd, 409, "Conflict", "text/plain; charset=utf-8",
                                            error_body, sizeof(error_body) - 1U);
                        return;
                    }
                    if (remove_result == -2) {
                        static const char error_body[] = "queue item not found\n";
                        (void)send_response(fd, 404, "Not Found", "text/plain; charset=utf-8",
                                            error_body, sizeof(error_body) - 1U);
                    } else if (remove_result != 0) {
                        static const char error_body[] = "queue persistence failed\n";
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

int rd_http_server_run(const char *bind_address, unsigned short port) {
    struct sockaddr_in address;
    int server_fd;
    int reuse = 1;

#ifdef RD_HOST_BUILD
    queue_ready = rd_queue_init(&server_queue, "/tmp/rd-downloader-host-queue.db") == 0;
    if (mkdir(RD_DOWNLOAD_DIRECTORY, 0700) != 0 && errno != EEXIST) queue_ready = 0;
#else
    queue_ready = rd_queue_init(&server_queue, "/data/rd-downloader-queue.db") == 0;
#endif

    {
        pthread_t worker;
        if (pthread_create(&worker, NULL, download_worker, NULL) == 0) {
            (void)pthread_detach(worker);
            worker_started = 1;
        }
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
        handle_client(client_fd);
        close(client_fd);
    }
}
