#define _POSIX_C_SOURCE 200809L
#include "rd/queue.h"
#include "rd/output.h"
#include "rd/log.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>

static int has_http_scheme(const char *s, size_t n);

static int save_queue(const rd_queue *queue) {
    char temporary[sizeof(queue->state_path) + 16U];
    FILE *file;
    size_t i, j;
    int fd, failed = 0;
    if (snprintf(temporary, sizeof(temporary), "%s.tmpXXXXXX", queue->state_path) >=
        (int)sizeof(temporary)) {
        rd_log_write("ERROR", "queue state path is too long");
        return -1;
    }
    fd = mkstemp(temporary);
    if (fd < 0) {
        rd_log_write("ERROR", "cannot create queue state temporary file path=%s errno=%d",
                     queue->state_path, errno);
        return -1;
    }
    file = fdopen(fd, "w");
    if (!file) { close(fd); unlink(temporary); return -1; }
    if (fprintf(file, "RDQ7\t%lu\n", queue->next_id) < 0) failed = 1;
    for (i = 0; !failed && i < queue->count; ++i) {
        const unsigned char *url = (const unsigned char *)queue->items[i].url;
        if (fprintf(file, "%lu\t%u\t%llu\t%llu\t", queue->items[i].id,
                    (unsigned int)queue->items[i].state,
                    queue->items[i].expected_bytes,
                    queue->items[i].downloaded_bytes) < 0) { failed = 1; break; }
        for (j = 0; queue->items[i].error[j]; ++j) {
            if (fprintf(file, "%02X", (unsigned int)(unsigned char)queue->items[i].error[j]) < 0) {
                failed = 1; break;
            }
        }
        if (failed || fputc('\t', file) == EOF) { failed = 1; break; }
        if (fprintf(file, "%d\t%d\t%s\t%s\t%d\t%u\t%llu\t%d\t",
                    queue->items[i].legacy_partial,
                    !!queue->items[i].extract_after_download,
                    queue->items[i].filename, queue->items[i].destination,
                    !!queue->items[i].extract_nested_archives,
                    queue->items[i].retry_attempts,
                    queue->items[i].retry_after_epoch,
                    !!queue->items[i].remove_downloads_on_cancel) < 0) { failed = 1; break; }
        for (j = 0; url[j]; ++j) {
            if (fprintf(file, "%02X", (unsigned int)url[j]) < 0) { failed = 1; break; }
        }
        if (!failed && fputc('\n', file) == EOF) failed = 1;
    }
    for (i = 0; !failed && i < queue->related_file_count; ++i) {
        if (fprintf(file, "F\t%lu\t%s\n", queue->related_files[i].item_id,
                    queue->related_files[i].filename) < 0) failed = 1;
    }
    fd = fileno(file);
    if (!failed && fflush(file) != 0) failed = 1;
    if (!failed && fsync(fd) != 0) failed = 1;
    if (fclose(file) != 0) failed = 1;
    if (!failed && rename(temporary, queue->state_path) != 0) failed = 1;
    if (failed) {
        int saved_errno = errno;
        unlink(temporary);
        rd_log_write("ERROR", "cannot persist queue state path=%s errno=%d",
                     queue->state_path, saved_errno);
    }
    return failed ? -1 : 0;
}

static int load_queue(rd_queue *queue) {
    char line[RD_MAX_URL_LENGTH * 2U + 2048U];
    char *header_end;
    unsigned long stored_next_id;
    int version, needs_save = 0;
    FILE *file = fopen(queue->state_path, "r");
    if (!file) {
        if (errno != ENOENT)
            rd_log_write("ERROR", "cannot read queue state path=%s errno=%d",
                         queue->state_path, errno);
        return errno == ENOENT ? 0 : -1;
    }
    if (!fgets(line, sizeof(line), file) ||
        (strncmp(line, "RDQ2\t", 5U) != 0 && strncmp(line, "RDQ3\t", 5U) != 0 &&
         strncmp(line, "RDQ4\t", 5U) != 0 && strncmp(line, "RDQ5\t", 5U) != 0 &&
         strncmp(line, "RDQ6\t", 5U) != 0 && strncmp(line, "RDQ7\t", 5U) != 0)) {
        fclose(file);
        rd_log_write("ERROR", "queue state header is invalid path=%s", queue->state_path);
        return -1;
    }
    version = line[3] - '0';
    if (version < 2 || version > 7) { fclose(file); return -1; }
    errno = 0;
    stored_next_id = strtoul(line + 5, &header_end, 10);
    if (errno || header_end == line + 5 || stored_next_id == 0 ||
        header_end[0] != '\n' || header_end[1] != '\0') {
        fclose(file);
        return -1;
    }
    queue->next_id = stored_next_id;
    needs_save = version < 7;
    while (fgets(line, sizeof(line), file)) {
        if (version >= 7 && line[0] == 'F' && line[1] == '\t') {
            char *id_end, *name_end;
            unsigned long id;
            size_t name_length, index;
            errno = 0;
            id = strtoul(line + 2, &id_end, 10);
            if (errno || id == 0 || id_end == line + 2 || *id_end != '\t' ||
                queue->related_file_count >= RD_MAX_RELATED_FILES) goto bad_file;
            name_end = strpbrk(id_end + 1, "\r\n");
            if (!name_end) goto bad_file;
            *name_end = '\0';
            name_length = strlen(id_end + 1);
            if (!name_length || name_length >= sizeof(queue->related_files[0].filename) ||
                strchr(id_end + 1, '/') || strchr(id_end + 1, '\\') ||
                strcmp(id_end + 1, ".") == 0 || strcmp(id_end + 1, "..") == 0) goto bad_file;
            for (index = 0; index < queue->count; ++index)
                if (queue->items[index].id == id) break;
            if (index == queue->count) goto bad_file;
            for (index = 0; index < name_length; ++index) {
                unsigned char c = (unsigned char)id_end[1 + index];
                if (!(isalnum(c) || c == '.' || c == '_' || c == '-')) goto bad_file;
            }
            queue->related_files[queue->related_file_count].item_id = id;
            snprintf(queue->related_files[queue->related_file_count].filename,
                     sizeof(queue->related_files[0].filename), "%s", id_end + 1);
            ++queue->related_file_count;
            continue;
        }
        char *tab = strchr(line, '\t');
        char *end;
        unsigned long id;
        unsigned long long expected = 0, downloaded = 0;
        unsigned int state = (unsigned int)RD_JOB_QUEUED;
        char *error_hex = NULL;
        int interrupted = 0, extraction_interrupted = 0;
        size_t n, i;
        rd_queue_item *item;
        if (!tab || !strchr(line, '\n') || queue->count >= RD_MAX_QUEUE_ITEMS) goto bad_file;
        *tab++ = '\0';
        errno = 0;
        id = strtoul(line, &end, 10);
        if (errno || *line == '\0' || id == 0 || id >= queue->next_id) goto bad_file;
        for (i = 0; i < queue->count; ++i)
            if (queue->items[i].id == id) goto bad_file;
        if (*end != '\0') goto bad_file;
        if (version >= 3) {
            char *state_end, *expected_end, *downloaded_end;
            errno = 0;
            state = (unsigned int)strtoul(tab, &state_end, 10);
            if (errno || state_end == tab || *state_end != '\t' || state >= 14U) goto bad_file;
            errno = 0;
            expected = strtoull(state_end + 1, &expected_end, 10);
            if (errno || expected_end == state_end + 1 || *expected_end != '\t') goto bad_file;
            errno = 0;
            downloaded = strtoull(expected_end + 1, &downloaded_end, 10);
            if (errno || downloaded_end == expected_end + 1 || *downloaded_end != '\t') goto bad_file;
            tab = downloaded_end;
            ++tab;
            if (downloaded > expected && expected != 0) goto bad_file;
            if (state == (unsigned int)RD_JOB_DOWNLOADING) {
                state = (unsigned int)RD_JOB_FAILED;
                needs_save = 1;
                interrupted = 1;
            } else if (state == (unsigned int)RD_JOB_EXTRACTING) {
                state = (unsigned int)RD_JOB_READY_TO_EXTRACT;
                needs_save = 1;
                extraction_interrupted = 1;
            }
        }
        item = &queue->items[queue->count];
        memset(item, 0, sizeof(*item));
        if (version >= 4) {
            char *error_end = strchr(tab, '\t');
            size_t error_length, error_index;
            if (!error_end) goto bad_file;
            *error_end = '\0';
            error_hex = tab;
            error_length = strlen(error_hex);
            if ((error_length & 1U) || error_length / 2U >= sizeof(item->error)) goto bad_file;
            for (error_index = 0; error_index < error_length; error_index += 2U) {
                char pair[3] = { error_hex[error_index], error_hex[error_index + 1U], '\0' };
                char *hex_end;
                unsigned long byte = strtoul(pair, &hex_end, 16);
                if (*hex_end || byte == 0) goto bad_file;
                item->error[error_index / 2U] =
                    byte < 0x20U || byte >= 0x7fU || byte == '"' || byte == '\\' ?
                    '_' : (char)byte;
            }
            tab = error_end + 1;
        }
        item->id = id;
        item->state = (rd_job_state)state;
        item->extract_after_download = 1;
        item->expected_bytes = expected;
        item->downloaded_bytes = downloaded;
        if (version >= 6) {
            if ((tab[0] != '0' && tab[0] != '1') || tab[1] != '\t') goto bad_file;
            item->legacy_partial = tab[0] == '1';
            tab += 2;
        } else {
            item->legacy_partial = interrupted || state == RD_JOB_FAILED || state == RD_JOB_PAUSED;
        }
        if (version >= 5) {
            char *extract_end;
            char *value_end;
            unsigned long extract = strtoul(tab, &extract_end, 10);
            if (extract_end == tab || *extract_end != '\t' || extract > 1U) goto bad_file;
            tab = extract_end + 1;
            value_end = strchr(tab, '\t');
            if (!value_end) goto bad_file;
            *value_end = '\0';
            if (strlen(tab) >= sizeof(item->filename)) goto bad_file;
            snprintf(item->filename, sizeof(item->filename), "%s", tab);
            tab = value_end + 1;
            value_end = strchr(tab, '\t');
            if (!value_end) goto bad_file;
            *value_end = '\0';
            if (strlen(tab) >= sizeof(item->destination)) goto bad_file;
            snprintf(item->destination, sizeof(item->destination), "%s", tab);
            tab = value_end + 1;
            item->extract_after_download = (int)extract;
            if (version >= 7) {
                char *nested_end, *attempts_end, *retry_end, *remove_end;
                unsigned long nested, attempts, remove_downloads;
                nested = strtoul(tab, &nested_end, 10);
                if (nested_end == tab || *nested_end != '\t' || nested > 1U) goto bad_file;
                attempts = strtoul(nested_end + 1, &attempts_end, 10);
                if (attempts_end == nested_end + 1 || *attempts_end != '\t' || attempts > 5U)
                    goto bad_file;
                item->retry_after_epoch = strtoull(attempts_end + 1, &retry_end, 10);
                if (retry_end == attempts_end + 1 || *retry_end != '\t') goto bad_file;
                remove_downloads = strtoul(retry_end + 1, &remove_end, 10);
                if (remove_end == retry_end + 1 || *remove_end != '\t' || remove_downloads > 1U)
                    goto bad_file;
                item->extract_nested_archives = (int)nested;
                item->retry_attempts = (unsigned int)attempts;
                item->remove_downloads_on_cancel = (int)remove_downloads;
                tab = remove_end + 1;
            }
        }
        if (interrupted || state == (unsigned int)RD_JOB_PAUSED) {
            item->state = RD_JOB_FAILED;
            needs_save = 1;
            snprintf(item->error, sizeof(item->error), "Interrupted by payload restart; partial output kept");
        }
        if (extraction_interrupted)
            snprintf(item->error, sizeof(item->error),
                     "Extraction interrupted by payload restart; checking helper task");
        if (version < 5)
            snprintf(item->destination, sizeof(item->destination), "%s", RD_DEFAULT_DOWNLOAD_DIRECTORY);
        n = strcspn(tab, "\r\n");
        if (n == 0 || (n & 1U) || n / 2U >= RD_MAX_URL_LENGTH) goto bad_file;
        for (i = 0; i < n; i += 2U) {
            char pair[3] = { tab[i], tab[i + 1U], '\0' };
            char *hex_end;
            unsigned long byte = strtoul(pair, &hex_end, 16);
            if (*hex_end || byte == 0) goto bad_file;
            item->url[i / 2U] = (char)byte;
        }
        if (!has_http_scheme(item->url, strlen(item->url))) goto bad_file;
        if (!item->filename[0]) rd_download_filename(item->url, item->filename, sizeof(item->filename));
        ++queue->count;
    }
    if (ferror(file)) goto bad_file;
    fclose(file);
    if (needs_save && save_queue(queue) != 0) return -1;
    return 0;
bad_file:
    fclose(file);
    memset(queue->items, 0, sizeof(queue->items));
    queue->count = 0;
    return -1;
}

int rd_queue_init(rd_queue *queue, const char *state_path) {
    int result;
    if (!queue || !state_path || !*state_path || strlen(state_path) >= sizeof(queue->state_path)) return -1;
    memset(queue, 0, sizeof(*queue));
    strcpy(queue->state_path, state_path);
    queue->next_id = 1;
    result = load_queue(queue);
    if (result != 0)
        rd_log_write("ERROR", "queue initialization failed path=%s errno=%d",
                     state_path, errno);
    return result;
}

int rd_queue_add_batch_preferences(rd_queue *queue,
                                   const char (*urls)[RD_MAX_URL_LENGTH], size_t count,
                                   int extract_after_download, int extract_nested_archives,
                                   const char *password) {
    return rd_queue_add_batch_named_preferences(queue, urls, NULL, count,
        extract_after_download, extract_nested_archives, password);
}

int rd_queue_add_batch_named_preferences(rd_queue *queue,
                                   const char (*urls)[RD_MAX_URL_LENGTH],
                                   const char (*filenames)[512], size_t count,
                                   int extract_after_download, int extract_nested_archives,
                                   const char *password) {
    rd_queue *candidate;
    size_t i;
    if (!queue || (!urls && count) || queue->count > RD_MAX_QUEUE_ITEMS ||
        count > RD_MAX_QUEUE_ITEMS - queue->count ||
        (count && (queue->next_id == 0 || count > ULONG_MAX - queue->next_id))) return -1;
    candidate = (rd_queue *)malloc(sizeof(*candidate));
    if (!candidate) return -1;
    *candidate = *queue;
    for (i = 0; i < count; ++i) {
        rd_queue_item *item = &candidate->items[candidate->count++];
        size_t url_length = strnlen(urls[i], RD_MAX_URL_LENGTH);
        if (url_length == 0 || url_length >= RD_MAX_URL_LENGTH ||
            !has_http_scheme(urls[i], url_length)) { free(candidate); return -1; }
        memset(item, 0, sizeof(*item));
        item->id = candidate->next_id++;
        item->state = RD_JOB_QUEUED;
        item->extract_after_download = !!extract_after_download;
        item->extract_nested_archives = !!extract_nested_archives;
        snprintf(item->destination, sizeof(item->destination), "%s", RD_DEFAULT_DOWNLOAD_DIRECTORY);
        memcpy(item->url, urls[i], url_length + 1U);
        if (filenames && filenames[i][0]) {
            size_t name_length = strnlen(filenames[i], sizeof(item->filename));
            size_t j;
            if (!name_length || name_length >= sizeof(item->filename) || name_length > 250U ||
                !strcmp(filenames[i], ".") || !strcmp(filenames[i], "..")) { free(candidate); return -1; }
            for (j = 0; j < name_length; ++j) {
                unsigned char c = (unsigned char)filenames[i][j];
                if (c < 0x20U || c == 0x7fU || c == '/' || c == '\\' || c == ':') {
                    free(candidate); return -1;
                }
            }
            memcpy(item->filename, filenames[i], name_length + 1U);
        } else rd_download_filename(item->url, item->filename, sizeof(item->filename));
        if (item->extract_after_download && password && *password)
            snprintf(item->extraction_password, sizeof(item->extraction_password), "%s", password);
    }
    if (save_queue(candidate) != 0) { free(candidate); return -1; }
    *queue = *candidate;
    free(candidate);
    return (int)count;
}

int rd_queue_add_batch_options(rd_queue *queue, const char (*urls)[RD_MAX_URL_LENGTH], size_t count,
                               int extract_after_download, const char *password) {
    return rd_queue_add_batch_preferences(queue, urls, count, extract_after_download, 0, password);
}

int rd_queue_add_batch(rd_queue *queue, const char (*urls)[RD_MAX_URL_LENGTH], size_t count) {
    return rd_queue_add_batch_options(queue, urls, count, 1, NULL);
}

int rd_queue_remove(rd_queue *queue, unsigned long id) {
    rd_queue *candidate;
    size_t i;
    if (!queue || id == 0) return -1;
    candidate = (rd_queue *)malloc(sizeof(*candidate));
    if (!candidate) return -1;
    *candidate = *queue;
    for (i = 0; i < candidate->count; ++i) {
        if (candidate->items[i].id == id) {
            memmove(&candidate->items[i], &candidate->items[i + 1U],
                    (candidate->count - i - 1U) * sizeof(candidate->items[0]));
            --candidate->count;
            memset(&candidate->items[candidate->count], 0, sizeof(candidate->items[0]));
            {
                size_t read_index, write_index = 0;
                for (read_index = 0; read_index < candidate->related_file_count; ++read_index)
                    if (candidate->related_files[read_index].item_id != id) {
                        if (write_index != read_index)
                            candidate->related_files[write_index] = candidate->related_files[read_index];
                        ++write_index;
                    }
                memset(candidate->related_files + write_index, 0,
                       (candidate->related_file_count - write_index) * sizeof(candidate->related_files[0]));
                candidate->related_file_count = write_index;
            }
            if (save_queue(candidate) != 0) { free(candidate); return -1; }
            *queue = *candidate;
            free(candidate);
            return 0;
        }
    }
    free(candidate);
    return -2;
}

int rd_queue_clear_completed(rd_queue *queue) {
    rd_queue *candidate;
    size_t read_index, write_index = 0;
    int removed = 0;
    if (!queue) return -1;
    candidate = (rd_queue *)malloc(sizeof(*candidate));
    if (!candidate) return -1;
    *candidate = *queue;
    for (read_index = 0; read_index < candidate->count; ++read_index) {
        if (candidate->items[read_index].state == RD_JOB_COMPLETED) {
            ++removed;
            continue;
        }
        if (write_index != read_index)
            candidate->items[write_index] = candidate->items[read_index];
        ++write_index;
    }
    if (!removed) {
        free(candidate);
        return 0;
    }
    memset(candidate->items + write_index, 0,
           (candidate->count - write_index) * sizeof(candidate->items[0]));
    candidate->count = write_index;
    {
        size_t read_index, related_write = 0;
        for (read_index = 0; read_index < candidate->related_file_count; ++read_index) {
            size_t item_index;
            for (item_index = 0; item_index < candidate->count; ++item_index)
                if (candidate->items[item_index].id == candidate->related_files[read_index].item_id) break;
            if (item_index < candidate->count) {
                if (related_write != read_index)
                    candidate->related_files[related_write] = candidate->related_files[read_index];
                ++related_write;
            }
        }
        memset(candidate->related_files + related_write, 0,
               (candidate->related_file_count - related_write) * sizeof(candidate->related_files[0]));
        candidate->related_file_count = related_write;
    }
    if (save_queue(candidate) != 0) {
        free(candidate);
        return -1;
    }
    *queue = *candidate;
    free(candidate);
    return removed;
}

int rd_queue_update(rd_queue *queue, unsigned long id, rd_job_state state,
                    unsigned long long downloaded, unsigned long long expected,
                    const char *error) {
    rd_queue *candidate;
    size_t i;
    if (!queue || id == 0 || (unsigned int)state >= 14U ||
        (expected && downloaded > expected)) return -1;
    candidate = (rd_queue *)malloc(sizeof(*candidate));
    if (!candidate) return -1;
    *candidate = *queue;
    for (i = 0; i < candidate->count; ++i) {
        if (candidate->items[i].id == id) {
            candidate->items[i].state = state;
            candidate->items[i].downloaded_bytes = downloaded;
            candidate->items[i].expected_bytes = expected;
            if (error) {
                size_t j;
                for (j = 0; j + 1U < sizeof(candidate->items[i].error) && error[j]; ++j) {
                    unsigned char c = (unsigned char)error[j];
                    candidate->items[i].error[j] =
                        (c < 0x20U || c >= 0x7fU || c == '"' || c == '\\') ? '_' : (char)c;
                }
                candidate->items[i].error[j] = '\0';
                memset(candidate->items[i].error + j + 1U, 0,
                       sizeof(candidate->items[i].error) - j - 1U);
            }
            if (save_queue(candidate) != 0) { free(candidate); return -1; }
            *queue = *candidate;
            free(candidate);
            return 0;
        }
    }
    free(candidate);
    return -2;
}

static int safe_related_filename(const char *filename) {
    size_t i, length;
    if (!filename || !(length = strlen(filename)) || length >= sizeof(((rd_related_file *)0)->filename) ||
        strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0) return 0;
    for (i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)filename[i];
        if (!(isalnum(c) || c == '.' || c == '_' || c == '-')) return 0;
    }
    return 1;
}

int rd_queue_has_related_file(const rd_queue *queue, unsigned long item_id,
                              const char *filename) {
    size_t i;
    if (!queue || !item_id || !filename) return 0;
    for (i = 0; i < queue->related_file_count; ++i)
        if (queue->related_files[i].item_id == item_id &&
            strcmp(queue->related_files[i].filename, filename) == 0) return 1;
    return 0;
}

int rd_queue_set_filename(rd_queue *queue, unsigned long item_id, const char *filename) {
    rd_queue *candidate;
    size_t i, length;
    if (!queue || !item_id || !filename || !(length = strlen(filename)) ||
        length > 250U || length >= sizeof(queue->items[0].filename) ||
        !strcmp(filename, ".") || !strcmp(filename, "..")) return -1;
    for (i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)filename[i];
        if (c < 0x20U || c == 0x7fU || c == '/' || c == '\\' || c == ':') return -1;
    }
    candidate = (rd_queue *)malloc(sizeof(*candidate));
    if (!candidate) return -1;
    *candidate = *queue;
    for (i = 0; i < candidate->count; ++i) {
        if (candidate->items[i].id != item_id) continue;
        memcpy(candidate->items[i].filename, filename, length + 1U);
        if (save_queue(candidate) != 0) { free(candidate); return -1; }
        *queue = *candidate;
        free(candidate);
        return 0;
    }
    free(candidate);
    return -1;
}

int rd_queue_record_related_file(rd_queue *queue, unsigned long item_id,
                                 const char *filename) {
    rd_queue *candidate;
    size_t i;
    int found = 0;
    if (!queue || !item_id || !safe_related_filename(filename)) return -1;
    if (rd_queue_has_related_file(queue, item_id, filename)) return 0;
    for (i = 0; i < queue->count; ++i)
        if (queue->items[i].id == item_id) { found = 1; break; }
    if (!found || queue->related_file_count >= RD_MAX_RELATED_FILES) return -1;
    candidate = (rd_queue *)malloc(sizeof(*candidate));
    if (!candidate) return -1;
    *candidate = *queue;
    candidate->related_files[candidate->related_file_count].item_id = item_id;
    snprintf(candidate->related_files[candidate->related_file_count].filename,
             sizeof(candidate->related_files[0].filename), "%s", filename);
    ++candidate->related_file_count;
    if (save_queue(candidate) != 0) { free(candidate); return -1; }
    *queue = *candidate;
    free(candidate);
    return 0;
}

int rd_queue_set_retry(rd_queue *queue, unsigned long item_id,
                       unsigned int attempts, unsigned long long retry_after_epoch,
                       rd_job_state state, const char *error) {
    rd_queue *candidate;
    size_t i;
    if (!queue || !item_id || attempts > 5U || (unsigned int)state >= 14U) return -1;
    candidate = (rd_queue *)malloc(sizeof(*candidate));
    if (!candidate) return -1;
    *candidate = *queue;
    for (i = 0; i < candidate->count; ++i) if (candidate->items[i].id == item_id) {
        rd_queue_item *item = &candidate->items[i];
        item->retry_attempts = attempts;
        item->retry_after_epoch = retry_after_epoch;
        item->state = state;
        if (state == RD_JOB_WAITING_FOR_NETWORK) {
            item->downloaded_bytes = 0;
            item->expected_bytes = 0;
            item->speed_bps = item->average_bps = item->eta_seconds = 0;
        }
        if (error) {
            size_t j;
            for (j = 0; j + 1U < sizeof(item->error) && error[j]; ++j) {
                unsigned char c = (unsigned char)error[j];
                item->error[j] = c < 0x20U || c >= 0x7fU || c == '"' || c == '\\' ? '_' : (char)c;
            }
            item->error[j] = '\0';
            memset(item->error + j + 1U, 0, sizeof(item->error) - j - 1U);
        }
        if (save_queue(candidate) != 0) { free(candidate); return -1; }
        *queue = *candidate;
        free(candidate);
        return 0;
    }
    free(candidate);
    return -2;
}

static int has_http_scheme(const char *s, size_t n) {
    size_t authority_start;
    size_t i;
    if (n >= 7) {
        static const char http[] = "http://";
        for (i = 0; i < 7; ++i)
            if (tolower((unsigned char)s[i]) != http[i]) break;
        if (i == 7) authority_start = 7;
        else authority_start = 0;
    } else authority_start = 0;
    if (authority_start == 0) {
        if (n < 8) return 0;
        static const char https[] = "https://";
        for (i = 0; i < 8; ++i)
            if (tolower((unsigned char)s[i]) != https[i]) break;
        if (i != 8) return 0;
        authority_start = 8;
    }
    if (authority_start >= n || s[authority_start] == '/' ||
        s[authority_start] == '?' || s[authority_start] == '#') return 0;
    for (i = authority_start; i < n; ++i) {
        if (s[i] == '/' || s[i] == '?' || s[i] == '#') return i > authority_start;
        if (isspace((unsigned char)s[i])) return 0;
    }
    return n > authority_start;
}

int rd_parse_url_batch(const char *text, char (*urls)[RD_MAX_URL_LENGTH], size_t capacity) {
    const char *p;
    size_t count = 0;
    if (!text || !urls || capacity == 0) return -1;
    p = text;
    while (*p) {
        const char *start;
        const char *end;
        size_t n;
        while (*p && (*p == '\r' || *p == '\n' || isspace((unsigned char)*p))) ++p;
        if (!*p) break;
        start = p;
        while (*p && *p != '\r' && *p != '\n') ++p;
        end = p;
        while (end > start && isspace((unsigned char)end[-1])) --end;
        n = (size_t)(end - start);
        if (n == 0) continue;
        if (n >= RD_MAX_URL_LENGTH || !has_http_scheme(start, n) || count >= capacity)
            return -2;
        {
            size_t i;
            for (i = 0; i < n; ++i)
                if (isspace((unsigned char)start[i])) return -2;
        }
        memcpy(urls[count], start, n);
        urls[count][n] = '\0';
        ++count;
    }
    return (int)count;
}

const char *rd_job_state_name(rd_job_state state) {
    static const char *names[] = {
        "queued", "probing", "waiting_for_space", "downloading", "verifying",
        "ready_to_extract", "extracting", "publishing", "waiting_for_network",
        "waiting_for_storage", "paused", "completed", "failed", "cancelled"
    };
    if ((unsigned int)state >= sizeof(names) / sizeof(names[0])) return "unknown";
    return names[state];
}
