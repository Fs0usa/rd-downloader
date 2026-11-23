#ifndef RD_OUTPUT_H
#define RD_OUTPUT_H
#include <stddef.h>

typedef struct rd_output {
    char final[768];
    char partial[784];
    char owner[600];
} rd_output;

void rd_download_filename(const char *url, char *name, size_t capacity);
int rd_output_paths(rd_output *out, const char *directory, const char *url, unsigned long id);
int rd_output_create(const rd_output *out);
int rd_output_discard(const rd_output *out, int include_final);
#endif
