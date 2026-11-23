#ifndef RD_ARCHIVE_H
#define RD_ARCHIVE_H
#include <stdbool.h>
#include <stddef.h>

typedef enum rd_archive_kind {
    RD_ARCHIVE_NONE = 0,
    RD_ARCHIVE_RAR_SINGLE,
    RD_ARCHIVE_RAR_MULTIPART_PART,
    RD_ARCHIVE_RAR_LEGACY_VOLUME,
    RD_ARCHIVE_SPLIT_001,
    RD_ARCHIVE_7Z,
    RD_ARCHIVE_ZIP,
    RD_ARCHIVE_OTHER
} rd_archive_kind;

typedef struct rd_archive_name {
    rd_archive_kind kind;
    char first_volume[512];
    unsigned int part_number;
} rd_archive_name;

/* Classifies by filename only; actual format must be verified by the extractor. */
int rd_classify_archive_name(const char *name, rd_archive_name *out);
#endif
