#define _POSIX_C_SOURCE 200809L
#include "rd/output.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

void rd_download_filename(const char *url, char *filename, size_t capacity) {
    const char *scheme;
    const char *path;
    const char *base;
    const char *end;
    size_t length, i, used = 0;
    const char *last_slash = NULL;
    if (!filename || !capacity) return;
    if (!url) {
        snprintf(filename, capacity, "download.bin");
        return;
    }
    scheme = strstr(url, "://");
    path = scheme ? scheme + 3 + strcspn(scheme + 3, "/?#") : NULL;
    if (path && *path != '/') path = NULL;
    base = path ? path + 1 : "";
    end = strpbrk(base, "?#");
    length = end ? (size_t)(end - base) : strlen(base);
    for (i = 0; i < length; ++i) if (base[i] == '/') last_slash = base + i;
    if (last_slash) { length -= (size_t)(last_slash + 1 - base); base = last_slash + 1; }
    for (i = 0; i < length && used < 250U && used + 1U < capacity; ++i) {
        unsigned char c = (unsigned char)base[i];
        if (c == '%' && i + 2U < length && isxdigit((unsigned char)base[i + 1U]) &&
            isxdigit((unsigned char)base[i + 2U])) {
            char hex[3] = { base[i + 1U], base[i + 2U], '\0' };
            c = (unsigned char)strtoul(hex, NULL, 16);
            i += 2U;
        }
        if (c < 0x80U && !(isalnum(c) || c == '.' || c == '_' || c == '-')) c = '_';
        filename[used++] = (char)c;
    }
    while (used && filename[used - 1U] == '.') --used;
    filename[used] = '\0';
    if (!used || (used == 1U && filename[0] == '.') ||
        (used == 2U && filename[0] == '.' && filename[1] == '.'))
        snprintf(filename, capacity, "download.bin");
}

int rd_filename_from_content_disposition(const char *header, char *filename, size_t capacity) {
    const char *p, *start = NULL, *end;
    size_t n, i;
    if (!header || !filename || !capacity) return -1;
    for (p = header; *p; ++p) {
        if ((p == header || p[-1] == ';' || p[-1] == ' ' || p[-1] == '\t') &&
            !strncasecmp(p, "filename=", 9U)) { start = p + 9; break; }
    }
    if (!start) return -1;
    if (*start == '"') { ++start; end = strchr(start, '"'); }
    else { end = strchr(start, ';'); if (!end) end = start + strlen(start); }
    if (!end || end <= start) return -1;
    n = (size_t)(end - start);
    if (n > 250U || n >= capacity || !strncmp(start, ".", n) || !strncmp(start, "..", n)) return -1;
    for (i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)start[i];
        if (c < 0x20U || c == 0x7fU || c == '/' || c == '\\' || c == ':') return -1;
    }
    memcpy(filename, start, n);
    filename[n] = '\0';
    return 0;
}

int rd_output_paths(rd_output *out, const char *directory, const char *filename, unsigned long id) {
    int final_length, partial_length, owner_length;
    size_t i, length;
    if (!out || !directory || !filename || !id) return -1;
    length = strlen(filename);
    if (!length || length > 250U || !strcmp(filename, ".") || !strcmp(filename, "..")) return -1;
    for (i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)filename[i];
        if (c < 0x20U || c == 0x7fU || c == '/' || c == '\\' || c == ':') return -1;
    }
    final_length = snprintf(out->final, sizeof(out->final), "%s/%s", directory, filename);
    if (final_length < 0 || (size_t)final_length >= sizeof(out->final)) return -1;
    partial_length = snprintf(out->partial, sizeof(out->partial), "%s.part", out->final);
    if (partial_length < 0 || (size_t)partial_length >= sizeof(out->partial)) return -1;
    owner_length = snprintf(out->owner, sizeof(out->owner), "%s/.rd-partials/%lu", directory, id);
    return owner_length < 0 || (size_t)owner_length >= sizeof(out->owner) ? -1 : 0;
}

/* The private marker records the partial file's device/inode for safe cleanup. */
int rd_output_create(const rd_output *out) {
    char parent[sizeof(out->owner)];
    struct stat created, current;
    char marker[96];
    int fd, owner_fd, marker_length, result = -1, saved_errno;
    size_t written = 0;
    strcpy(parent, out->owner);
    *strrchr(parent, '/') = 0;
    if (mkdir(parent, 0700) && errno != EEXIST) return -1;
    if (lstat(parent, &created) || !S_ISDIR(created.st_mode)) return -1;
    fd = open(out->partial, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    if (fstat(fd, &created) != 0) goto cleanup_partial;
    if (!S_ISREG(created.st_mode)) { errno = EINVAL; goto cleanup_partial; }
    owner_fd = open(out->owner, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (owner_fd < 0) goto cleanup_partial;
    marker_length = snprintf(marker, sizeof(marker), "%llu\t%llu\n",
                             (unsigned long long)created.st_dev,
                             (unsigned long long)created.st_ino);
    if (marker_length < 0 || (size_t)marker_length >= sizeof(marker)) {
        close(owner_fd);
        unlink(out->owner);
        errno = EOVERFLOW;
        goto cleanup_partial;
    }
    while (written < (size_t)marker_length) {
        ssize_t n = write(owner_fd, marker + written, (size_t)marker_length - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        written += (size_t)n;
    }
    if (written == (size_t)marker_length && fsync(owner_fd) == 0) {
        if (close(owner_fd) == 0) return fd;
    } else if (errno == 0) {
        errno = EIO;
    }
    saved_errno = errno;
    close(owner_fd);
    unlink(out->owner);
    errno = saved_errno;

cleanup_partial:
    saved_errno = errno;
    if (fstat(fd, &created) == 0 && lstat(out->partial, &current) == 0 &&
        current.st_dev == created.st_dev && current.st_ino == created.st_ino)
        (void)unlink(out->partial);
    close(fd);
    errno = saved_errno;
    return result;
}

static int unlink_matching(const char *path, unsigned long long device,
                           unsigned long long inode) {
    struct stat st;
    if (lstat(path, &st)) return errno == ENOENT ? 0 : -1;
    if ((unsigned long long)st.st_dev != device ||
        (unsigned long long)st.st_ino != inode) return 0;
    return unlink(path);
}

int rd_output_discard(const rd_output *out, int include_final) {
    struct stat owner_st;
    char marker[96], *end;
    unsigned long long device, inode;
    ssize_t length;
    int fd;
    if (lstat(out->owner, &owner_st)) {
        if (errno != ENOENT || !include_final) return errno == ENOENT ? 0 : -1;
        if (lstat(out->final, &owner_st) != 0) return errno == ENOENT ? 0 : -1;
        if (!S_ISREG(owner_st.st_mode)) return -1;
        return unlink(out->final);
    }
    if (!S_ISREG(owner_st.st_mode)) return -1;
    fd = open(out->owner, O_RDONLY);
    if (fd < 0) return -1;
    length = read(fd, marker, sizeof(marker) - 1U);
    close(fd);
    if (length <= 0) return -1;
    marker[length] = '\0';
    errno = 0;
    device = strtoull(marker, &end, 10);
    if (errno || end == marker || *end++ != '\t') return -1;
    errno = 0;
    inode = strtoull(end, &end, 10);
    if (errno || end[0] != '\n' || end[1] != '\0') return -1;
    if (unlink_matching(out->partial, device, inode) ||
        (include_final && unlink_matching(out->final, device, inode))) return -1;
    return unlink(out->owner);
}
