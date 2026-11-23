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
                const char *number_start = digits;
                while (*digits >= '0' && *digits <= '9') {
                    if (value > 999U) return -1;
                    value = value * 10U + (unsigned int)(*digits - '0');
                    ++digits;
                }
                if (value > 0 && value <= 999U && strlen(digits) == 4U && ends_ci(digits, ".rar")) {
                    out->kind = RD_ARCHIVE_RAR_MULTIPART_PART;
                    out->part_number = value;
                    if (value != 1U) {
                        size_t prefix = (size_t)(part_marker - base);
                        size_t width = (size_t)(digits - number_start);
                        if (prefix + width + 5U >= sizeof(out->first_volume)) return -1;
                        memcpy(out->first_volume, base, prefix);
                        snprintf(out->first_volume + prefix,
                                 sizeof(out->first_volume) - prefix,
                                 ".part%0*u.rar", (int)width, 1U);
                    }
                    return 0;
                }
            }
        }
    }
    if (len >= 4 && ends_ci(base, ".rar")) {
        out->kind = RD_ARCHIVE_RAR_SINGLE;
        return 0;
    }
    if (len >= 4 && tolower((unsigned char)base[len - 4]) == '.' &&
        isdigit((unsigned char)base[len - 3]) &&
        isdigit((unsigned char)base[len - 2]) &&
        isdigit((unsigned char)base[len - 1])) {
        out->part_number = (unsigned int)(base[len - 3] - '0') * 100U +
                           (unsigned int)(base[len - 2] - '0') * 10U +
                           (unsigned int)(base[len - 1] - '0');
        out->kind = RD_ARCHIVE_SPLIT_001;
        if (out->part_number != 1U)
            snprintf(out->first_volume, sizeof(out->first_volume), "%.*s.001",
                     (int)(len - 4U), base);
        return 0;
    }
    if (len >= 4 && tolower((unsigned char)base[len - 4]) == '.' &&
        (base[len - 3] == 'r' || base[len - 3] == 'R') &&
        isdigit((unsigned char)base[len - 2]) &&
        isdigit((unsigned char)base[len - 1])) {
        unsigned int volume = (unsigned int)(base[len - 2] - '0') * 10U +
                              (unsigned int)(base[len - 1] - '0');
        if (len - 4U + 4U >= sizeof(out->first_volume)) return -1;
        memcpy(out->first_volume, base, len - 4U);
        memcpy(out->first_volume + len - 4U, ".rar", 5U);
        out->kind = RD_ARCHIVE_RAR_LEGACY_VOLUME;
        out->part_number = volume + 1U;
        return 0;
    }
    if (len >= 4 && ends_ci(base, ".001")) {
        out->kind = RD_ARCHIVE_SPLIT_001;
        out->part_number = 1;
        return 0;
    }
    if (ends_ci(base, ".7z")) out->kind = RD_ARCHIVE_7Z;
    else if (ends_ci(base, ".zip") || ends_ci(base, ".zipx")) out->kind = RD_ARCHIVE_ZIP;
    else if (ends_ci(base, ".tar") || ends_ci(base, ".gz") || ends_ci(base, ".xz") || ends_ci(base, ".zst")) out->kind = RD_ARCHIVE_OTHER;
    return 0;
}
