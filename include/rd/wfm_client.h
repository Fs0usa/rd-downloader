#ifndef RD_WFM_CLIENT_H
#define RD_WFM_CLIENT_H

#include <stddef.h>
#include <stdint.h>

typedef int (*rd_wfm_progress_fn)(uint64_t done, uint64_t total,
                                  const char *current_file, void *context);
typedef int (*rd_wfm_cancel_fn)(void *context);

/* Extract one archive through the separately running WFM helper daemon. */
int rd_wfm_extract(unsigned long queue_id, const char *archive_path,
                   const char *destination, const char *password,
                   rd_wfm_progress_fn progress, rd_wfm_cancel_fn cancel,
                   void *context,
                   char *error, size_t error_capacity);

#endif
