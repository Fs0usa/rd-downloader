#include "rd/archive.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static int ends_ci(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    size_t i;
    if (a < b) return 0;
    for (i = 0; i < b; ++i)
        if (tolower((unsigned char)s[a-b+i]) != tolower((unsigned char)suffix[i])) return 0;
    return 1;
}

int rd_classify_archive_name(const char *name, rd_archive_name *out) {
    const char *base;
    size_t len;
    if (!name || !out) return -1;
    memset(out, 0, sizeof(*out));
    base = strrchr(name, '/');
    base = base ? base + 1 : name;
    len = strlen(base);
    if (len == 0 || len >= sizeof(out->first_volume)) return -1;
    snprintf(out->first_volume, sizeof(out->first_volume), "%s", base);

    if (ends_ci(base, ".rar")) {
        const char *part_marker = NULL;
        const char *scan = base;
        while ((scan = strstr(scan, ".part")) != NULL) { part_marker = scan; ++scan; }
        if (part_marker) {
            const char *digits = part_marker + 5;
            unsigned int value = 0;
            if (*digits >= '0' && *digits <= '9') {
                while (*digits >= '0' && *digits <= '9') {
                    value = value * 10U + (unsigned int)(*digits - '0');
                    ++digits;
                }
                if (value > 0 && ends_ci(digits, ".rar")) {
                    out->kind = RD_ARCHIVE_RAR_MULTIPART_PART;
                    out->part_number = value;
                    return 0;
                }
            }
        }
    }
    if (len >= 4 && ends_ci(base, ".rar")) {
        const char *dot = strrchr(base, '.');
        if (dot && dot > base && dot[-1] >= '0' && dot[-1] <= '9') {
            out->kind = RD_ARCHIVE_RAR_LEGACY_VOLUME;
            return 0;
        }
        out->kind = RD_ARCHIVE_RAR_SINGLE;
        return 0;
    }
    if (len >= 4 && ends_ci(base, ".001")) {
        out->kind = RD_ARCHIVE_SPLIT_001;
        return 0;
    }
    if (ends_ci(base, ".7z")) out->kind = RD_ARCHIVE_7Z;
    else if (ends_ci(base, ".zip") || ends_ci(base, ".zipx")) out->kind = RD_ARCHIVE_ZIP;
    else if (ends_ci(base, ".tar") || ends_ci(base, ".gz") || ends_ci(base, ".xz") || ends_ci(base, ".zst")) out->kind = RD_ARCHIVE_OTHER;
    return 0;
}
