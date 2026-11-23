#define _POSIX_C_SOURCE 200809L
#include "rd/output.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

int rd_output_paths(rd_output *out, const char *directory, const char *url, unsigned long id) {
    char name[256];
    int final_length, partial_length, owner_length;
    if (!out || !directory || !url || !id) return -1;
    rd_download_filename(url, name, sizeof(name));
    final_length = snprintf(out->final, sizeof(out->final), "%s/%s", directory, name);
    if (final_length < 0 || (size_t)final_length >= sizeof(out->final)) return -1;
    partial_length = snprintf(out->partial, sizeof(out->partial), "%s.part", out->final);
    if (partial_length < 0 || (size_t)partial_length >= sizeof(out->partial)) return -1;
    owner_length = snprintf(out->owner, sizeof(out->owner), "%s/.rd-partials/%lu", directory, id);
    return owner_length < 0 || (size_t)owner_length >= sizeof(out->owner) ? -1 : 0;
}

/* The private link identifies this job's file even across restarts and collisions. */
int rd_output_create(const rd_output *out) {
    char parent[sizeof(out->owner)];
    struct stat st;
    int fd;
    strcpy(parent, out->owner);
    *strrchr(parent, '/') = 0;
    if (mkdir(parent, 0700) && errno != EEXIST) return -1;
    if (lstat(parent, &st) || !S_ISDIR(st.st_mode)) return -1;
    fd = open(out->owner, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd >= 0 && link(out->owner, out->partial) != 0) {
        int saved = errno;
        close(fd);
        unlink(out->owner);
        errno = saved;
        return -1;
    }
    return fd;
}

static int unlink_matching(const char *path, const struct stat *owner) {
    struct stat st;
    if (lstat(path, &st)) return errno == ENOENT ? 0 : -1;
    if (st.st_dev != owner->st_dev || st.st_ino != owner->st_ino) return 0;
    return unlink(path);
}

int rd_output_discard(const rd_output *out, int include_final) {
    struct stat owner;
    if (lstat(out->owner, &owner)) return errno == ENOENT ? 0 : -1;
    if (!S_ISREG(owner.st_mode) || unlink_matching(out->partial, &owner) ||
        (include_final && unlink_matching(out->final, &owner))) return -1;
    return unlink(out->owner);
}
