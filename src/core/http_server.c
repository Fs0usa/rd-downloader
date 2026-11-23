#include "rd/http_server.h"
#include "rd/queue.h"
#include "rd/http_client.h"
#include "rd/archive.h"
#include "rd/output.h"

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
#define RD_HTTP_QUEUE_RESPONSE (RD_MAX_QUEUE_ITEMS * 1280U + 32U)
#define RD_PROGRESS_SAVE_INTERVAL (16ULL * 1024ULL * 1024ULL)
#define RD_DOWNLOAD_WORKER_STACK (1024U * 1024U)
#define RD_PASSWORD_HEADER_MAX 256U

#ifdef RD_HOST_BUILD
#define RD_DOWNLOAD_DIRECTORY "/tmp/rd-downloader-host-downloads"
#else
#define RD_DOWNLOAD_DIRECTORY RD_DEFAULT_DESTINATION
#endif

static rd_queue server_queue;
static int queue_ready;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t worker_condition = PTHREAD_COND_INITIALIZER;
static int download_requested = 1;
static int worker_started;
static unsigned long active_download_id;

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
    ".actions{display:flex;gap:6px}.icon{padding:8px 11px;min-width:42px}.dest{padding:10px;border-radius:8px;"
    "background:#15121c;color:#d6c9f1}.modal{position:fixed;inset:0;background:#000a;display:grid;place-items:center;padding:16px}"
    ".dialog{width:min(100%,520px);background:#211d2d;border:1px solid #504663;border-radius:14px;padding:18px}"
    ".dialog #password{width:100%;padding:11px;background:#15121c;color:white;border:1px solid #504663;border-radius:8px}"
    ".row{display:flex;align-items:center;gap:9px;margin:16px 0}.hidden{display:none}.metrics{display:flex;gap:12px;flex-wrap:wrap}"
    ".backend{font-size:.9rem;margin-top:8px}.online{color:#8de0a4}.offline{color:#ff9a9a}"
    "</style></head><body><header><h1>RD Downloader</h1>"
    "<div class=\"muted\">PS5 downloads · direct HTTP and HTTPS</div>"
    "<div id=\"backend\" class=\"backend offline\" role=\"status\">Checking PS5 service…</div></header>"
    "<section class=\"card\"><label for=\"urls\"><strong>Add direct download links</strong></label>"
    "<p class=\"muted\">Paste one HTTP or HTTPS link per line.</p>"
    "<textarea id=\"urls\" placeholder=\"https://example.com/file.zip\"></textarea>"
    "<p class=\"dest\">Downloads will be saved to <strong>/data/homebrew</strong>.</p>"
    "<p><button id=\"add\">Review and add</button></p>"
    "<div id=\"message\" class=\"error\" role=\"status\"></div>"
    "</section><section class=\"card\"><strong>Queue</strong><ul id=\"queue\"></ul>"
    "<div id=\"empty\" class=\"muted\">No queued links.</div></section>"
    "<small>Queue state is stored on the PS5. Closing this page does not stop downloads.</small>"
    "<div id=\"modal\" class=\"modal hidden\" role=\"dialog\" aria-modal=\"true\" aria-labelledby=\"modalTitle\">"
    "<div class=\"dialog\"><h2 id=\"modalTitle\">Confirm queue settings</h2><p>Save location: <strong>/data/homebrew</strong></p>"
    "<p class=\"row\"><input id=\"extract\" type=\"checkbox\" checked><label for=\"extract\">Extract archives after download</label></p>"
    "<small>Archive extraction is not active in this build; archive jobs will wait for helper integration.</small>"
    "<label for=\"password\">Archive password <span class=\"muted\">(optional)</span></label>"
    "<input id=\"password\" type=\"text\" spellcheck=\"false\" autocapitalize=\"none\" maxlength=\"255\" autocomplete=\"off\" placeholder=\"Leave blank if not needed\">"
    "<p class=\"error\" id=\"modalMessage\" role=\"status\"></p><button id=\"confirm\">Start download</button> "
    "<button id=\"abort\" type=\"button\">Cancel</button></div></div>"
    "<script>"
    "const list=document.querySelector('#queue'),empty=document.querySelector('#empty'),msg=document.querySelector('#message');"
    "function size(n){n=Number(n)||0;if(n<1024)return n+' B';const u=['KB','MB','GB','TB'];let i=-1;"
    "do{n/=1024;i++}while(n>=1024&&i<u.length-1);return n.toFixed(1)+' '+u[i]}"
    "function offlineMessage(){return 'Cannot reach PS5 service. Check whether the payload stopped in Payload Manager, then reload.'}"
    "let confirmId=null;"
    "async function refresh(){try{const r=await fetch('/api/queue',{cache:'no-store'});"
    "if(!r.ok)throw Error('PS5 service returned HTTP '+r.status);const d=await r.json();"
    "const backend=document.querySelector('#backend');backend.className='backend online';"
    "backend.textContent='PS5 service online · HTTP and HTTPS downloader';"
    "list.replaceChildren();empty.hidden=d.items.length>0;"
    "for(const item of d.items){const li=document.createElement('li'),label=document.createElement('div'),"
    "state=document.createElement('span'),actions=document.createElement('div'),bar=document.createElement('progress'),"
    "bytes=document.createElement('small');label.className='job';"
    "bar.max=Number(item.expected_bytes)||1;if(Number(item.expected_bytes)>0)bar.value=Number(item.downloaded_bytes);"
    "else bar.removeAttribute('value');bytes.textContent=size(item.downloaded_bytes)+' / '+"
    "(Number(item.expected_bytes)>0?size(item.expected_bytes):'size unknown');label.textContent=item.filename||('download-'+item.id);"
    "label.append(document.createElement('br'),bar,document.createElement('br'),bytes);"
    "const metrics=document.createElement('small');metrics.className='metrics';metrics.textContent='Current '+size(item.speed_bps)+'/s · Average '+size(item.average_bps)+'/s · ETA '+(item.eta_seconds>0?item.eta_seconds+'s':'—');"
    "label.append(document.createElement('br'),metrics);"
    "if(item.error){const e=document.createElement('small');e.className='joberror';e.textContent=item.error;"
    "label.append(document.createElement('br'),e)}state.className='state';state.textContent=item.state;"
    "actions.className='actions';"
    "if(item.state==='downloading'||item.state==='queued'||item.state==='paused'){const toggle=document.createElement('button');toggle.className='icon';"
    "const pausing=item.state!=='paused';toggle.textContent=pausing?'Ⅱ':'▶';toggle.title=pausing?'Pause':'Resume';toggle.setAttribute('aria-label',toggle.title+' '+item.filename);"
    "toggle.onclick=()=>command('/api/queue/'+item.id+'/'+(pausing?'pause':'resume'),'POST','');actions.append(toggle)}"
    "if(item.state==='failed'){const retry=document.createElement('button');retry.className='icon';retry.textContent='↻';retry.title='Retry';retry.setAttribute('aria-label','Retry '+item.filename);retry.onclick=()=>command('/api/queue/'+item.id+'/retry','POST','');actions.append(retry)}"
    "const stopping=item.state==='downloading'||item.state==='paused';const actionLabel=stopping?'Stop download and delete its partial file':'Remove from queue';"
    "const remove=document.createElement('button');remove.className='icon';remove.textContent=confirmId===item.id?'Confirm?':'×';remove.title=actionLabel;remove.setAttribute('aria-label',actionLabel+' for '+item.filename);"
    "if(confirmId===item.id){remove.title='Confirm: '+actionLabel;remove.setAttribute('aria-label',remove.title)}"
    "remove.onclick=()=>{if(confirmId!==item.id||remove.textContent!=='Confirm?'){confirmId=item.id;remove.textContent='Confirm?';remove.title='Confirm: '+actionLabel;remove.setAttribute('aria-label',remove.title);refresh();return}confirmId=null;remove.disabled=true;command('/api/queue/'+item.id,'DELETE')};actions.append(remove);li.append(label,state,actions);list.append(li)}}"
    "catch(e){empty.hidden=true;const backend=document.querySelector('#backend');"
    "backend.className='backend offline';backend.textContent='PS5 service unreachable';"
    "msg.textContent=e instanceof TypeError?offlineMessage():e.message}}"
    "async function command(path,method,body){try{const r=await fetch(path,{method,body,headers:{'X-RD-Command':'1',"
    "'Content-Type':'text/plain'},cache:'no-store'});const t=await r.text();if(!r.ok)throw Error(t||'Request failed');"
    "msg.textContent='';if(path==='/api/queue')document.querySelector('#urls').value='';await refresh()}"
    "catch(e){msg.textContent=e instanceof TypeError?offlineMessage():e.message}}"
    "const modal=document.querySelector('#modal');document.querySelector('#add').onclick=()=>{if(document.querySelector('#urls').value.trim()){"
    "document.querySelector('#modalMessage').textContent='';modal.classList.remove('hidden')}};"
    "document.querySelector('#abort').onclick=()=>{modal.classList.add('hidden');document.querySelector('#password').value=''};"
    "document.querySelector('#confirm').onclick=async()=>{const b=document.querySelector('#urls').value.trim();if(!b)return;"
    "const headers={'X-RD-Command':'1','X-RD-Extract':document.querySelector('#extract').checked?'1':'0','Content-Type':'text/plain'};"
    "const pw=document.querySelector('#password').value;if(pw)headers['X-RD-Password']=pw;try{const r=await fetch('/api/queue',{method:'POST',body:b,headers,cache:'no-store'});"
    "const t=await r.text();if(!r.ok)throw Error(t||'Request failed');document.querySelector('#urls').value='';document.querySelector('#password').value='';"
    "modal.classList.add('hidden');msg.textContent='';await refresh()}catch(e){document.querySelector('#modalMessage').textContent=e.message}};"
    "refresh();setInterval(refresh,2500);"
    "</script></body></html>\n";

static int send_response(int fd, int status, const char *reason,
                         const char *content_type, const char *body,
                         size_t body_length);

static int send_health(int fd) {
    char body[256];
    size_t i;
    int active = 0, length, ready;
    pthread_mutex_lock(&queue_mutex);
    for (i = 0; i < server_queue.count; ++i)
        if (server_queue.items[i].state == RD_JOB_DOWNLOADING) active = 1;
    ready = worker_started;
    pthread_mutex_unlock(&queue_mutex);
    length = snprintf(body, sizeof(body),
        "{\"status\":\"ok\",\"application\":\"rd-downloader\","
        "\"version\":\"0.3.0-https-dev\",\"transport\":\"http-https\","
        "\"worker_ready\":%s,\"downloader_active\":%s}\n",
        ready ? "true" : "false", active ? "true" : "false");
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
            "%s{\"id\":%lu,\"filename\":\"%s\",\"state\":\"%s\",\"downloaded_bytes\":%llu,\"expected_bytes\":%llu,\"speed_bps\":%llu,\"average_bps\":%llu,\"eta_seconds\":%llu,\"error\":\"%s\"}",
            i ? "," : "", item->id, item->filename, rd_job_state_name(item->state),
            item->downloaded_bytes, item->expected_bytes, item->speed_bps,
            item->average_bps, item->eta_seconds, item->error);
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
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (unsigned long long)tv.tv_sec * 1000ULL + (unsigned long long)tv.tv_usec / 1000ULL;
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

static int finish_removal(rd_queue_item *item) {
    if (discard_output(item, item->state == RD_JOB_CANCELLED)) return -1;
    return rd_queue_remove(&server_queue, item->id);
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
            rd_http_begin(id);
            active_download_id = id;
            memcpy(url, item->url, sizeof(url));
            snprintf(destination, sizeof(destination), "%s", download_directory(item));
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
        progress.started_ms = monotonic_ms();
        progress.previous_ms = 0;
        progress.previous_bytes = 0;
        result = rd_http_download(url, destination, id, record_progress,
                                  &progress, error, sizeof(error));

        pthread_mutex_lock(&queue_mutex);
        active_download_id = 0;
        for (i = 0; i < server_queue.count; ++i) {
            rd_queue_item *item = &server_queue.items[i];
            if (item->id != id) continue;
            if (item->state == RD_JOB_CANCELLED) {
                if (finish_removal(item))
                    snprintf(item->error, sizeof(item->error), "Cleanup failed. Confirm removal to retry.");
                break;
            }
            rd_archive_name archive_name;
            int is_archive = rd_classify_archive_name(item->filename, &archive_name) == 0 &&
                             archive_name.kind != RD_ARCHIVE_NONE;
            rd_job_state final_state = result == 0 ?
                ((item->extract_after_download && is_archive) ? RD_JOB_READY_TO_EXTRACT : RD_JOB_COMPLETED) :
                RD_JOB_FAILED;
            const char *final_error = result != 0 ? error :
                ((item->extract_after_download && is_archive) ?
                 "Downloaded; extraction helper is not integrated yet" : "");
            if (result == 0 && discard_output(item, 0) != 0)
                final_error = "Downloaded. Could not remove temporary file reference.";
            if (rd_queue_update(&server_queue, id,
                    final_state,
                    item->downloaded_bytes, item->expected_bytes,
                    final_error) != 0) {
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
            char password[RD_PASSWORD_HEADER_MAX] = "";
            char extract_setting[8] = "1";
            int extract_enabled = 1;
            int count;
            size_t received;
            body_offset = (size_t)((end_headers + 4) - request);
            if (!command_header_ok(request) || body_length == 0 ||
                copy_header_value(request, "X-RD-Extract", extract_setting, sizeof(extract_setting)) < 0 ||
                copy_header_value(request, "X-RD-Password", password, sizeof(password)) < 0 ||
                (strcmp(extract_setting, "0") != 0 && strcmp(extract_setting, "1") != 0) ||
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
            if (!queue_ready || !worker_started) {
                static const char error_body[] = "download worker or queue unavailable\n";
                (void)send_response(fd, 503, "Service Unavailable", "text/plain", error_body, sizeof(error_body)-1);
                return;
            }
            count = rd_parse_url_batch(request + body_offset, urls, 4);
            extract_enabled = strcmp(extract_setting, "1") == 0;
            if (count <= 0) {
                static const char error_body[] = "invalid URL batch (maximum 4 URLs per request)\n";
                (void)send_response(fd, 400, "Bad Request", "text/plain; charset=utf-8",
                                    error_body, sizeof(error_body) - 1U);
            } else {
                int add_result;
                pthread_mutex_lock(&queue_mutex);
                add_result = rd_queue_add_batch_options(&server_queue,
                    (const char (*)[RD_MAX_URL_LENGTH])urls, (size_t)count,
                    extract_enabled, password);
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
            size_t i;
            int found = 0, action_result = -1;
            if (!command_header_ok(request) || !id || *end != '/' || !*action || body_length != 0) {
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
                        (item->state == RD_JOB_QUEUED || item->state == RD_JOB_DOWNLOADING)) {
                        action_result = rd_queue_update(&server_queue, id, RD_JOB_PAUSED,
                            item->downloaded_bytes, item->expected_bytes, "");
                    } else if (strcmp(action, "resume") == 0 && item->state == RD_JOB_PAUSED) {
                        rd_job_state next = active_download_id == id ? RD_JOB_DOWNLOADING : RD_JOB_QUEUED;
                        action_result = rd_queue_update(&server_queue, id, next,
                            item->downloaded_bytes, item->expected_bytes, "");
                        if (next == RD_JOB_QUEUED) download_requested = 1;
                        pthread_cond_broadcast(&worker_condition);
                    } else if (strcmp(action, "retry") == 0 && item->state == RD_JOB_FAILED && !active_download_id) {
                        if (discard_output(item, 0) == 0) {
                            item->legacy_partial = 0;
                            action_result = rd_queue_update(&server_queue, id, RD_JOB_QUEUED, 0, 0, "");
                            if (action_result == 0) {
                                item = &server_queue.items[i];
                                item->speed_bps = item->average_bps = item->eta_seconds = 0;
                                download_requested = 1;
                                pthread_cond_signal(&worker_condition);
                            }
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
                    int remove_result = -2;
                    size_t i;
                    pthread_mutex_lock(&queue_mutex);
                    for (i = 0; i < server_queue.count; ++i) {
                        rd_queue_item *item = &server_queue.items[i];
                        if (item->id != id) continue;
                        if (active_download_id == id) {
                            remove_result = rd_queue_update(&server_queue, id, RD_JOB_CANCELLED,
                                item->downloaded_bytes, item->expected_bytes, "Stopping download");
                            if (remove_result == 0) rd_http_cancel(id);
                            pthread_cond_broadcast(&worker_condition);
                        } else {
                            remove_result = finish_removal(item);
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

    if (queue_ready) {
        for (size_t i = server_queue.count; i > 0; --i) {
            rd_queue_item *item = &server_queue.items[i - 1];
            if (item->state == RD_JOB_CANCELLED && finish_removal(item))
                snprintf(item->error, sizeof(item->error), "Cleanup failed. Confirm removal to retry.");
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
        handle_client(client_fd);
        close(client_fd);
    }
}
