#ifndef RD_QUEUE_H
#define RD_QUEUE_H
#include <stddef.h>

#define RD_MAX_URL_LENGTH 4096U
#define RD_MAX_QUEUE_ITEMS 64U
#define RD_DEFAULT_DESTINATION "/data/homebrew"

typedef enum rd_job_state {
    RD_JOB_QUEUED = 0,
    RD_JOB_PROBING,
    RD_JOB_WAITING_FOR_SPACE,
    RD_JOB_DOWNLOADING,
    RD_JOB_VERIFYING,
    RD_JOB_READY_TO_EXTRACT,
    RD_JOB_EXTRACTING,
    RD_JOB_PUBLISHING,
    RD_JOB_WAITING_FOR_NETWORK,
    RD_JOB_WAITING_FOR_STORAGE,
    RD_JOB_PAUSED,
    RD_JOB_COMPLETED,
    RD_JOB_FAILED,
    RD_JOB_CANCELLED
} rd_job_state;

typedef struct rd_queue_item {
    unsigned long id;
    char url[RD_MAX_URL_LENGTH];
    char filename[512];
    char destination[512];
    rd_job_state state;
    unsigned long long expected_bytes;
    unsigned long long downloaded_bytes;
    int extract_after_download;
    char error[256];
} rd_queue_item;

typedef struct rd_queue {
    rd_queue_item items[RD_MAX_QUEUE_ITEMS];
    size_t count;
    unsigned long next_id;
    char state_path[512];
} rd_queue;

/* Parse newline-separated URLs. Commit queue items only when the return value is nonnegative. */
int rd_parse_url_batch(const char *text, char (*urls)[RD_MAX_URL_LENGTH],
                       size_t capacity);
const char *rd_job_state_name(rd_job_state state);

/* State is stored as a versioned, escaped text file and replaced atomically. */
int rd_queue_init(rd_queue *queue, const char *state_path);
int rd_queue_add_batch(rd_queue *queue, const char (*urls)[RD_MAX_URL_LENGTH], size_t count);
int rd_queue_remove(rd_queue *queue, unsigned long id);
int rd_queue_update(rd_queue *queue, unsigned long id, rd_job_state state,
                    unsigned long long downloaded, unsigned long long expected,
                    const char *error);
#endif
