#ifndef RD_QUEUE_H
#define RD_QUEUE_H
#include <stddef.h>

#define RD_MAX_URL_LENGTH 4096U
#define RD_MAX_QUEUE_ITEMS 64U
#define RD_MAX_RELATED_FILES 256U
#define RD_DEFAULT_DOWNLOAD_DIRECTORY "/data/homebrew/downloads"
#define RD_DEFAULT_EXTRACTION_DIRECTORY "/data/homebrew"
#define RD_MAX_PASSWORD_LENGTH 256U

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
    unsigned long long speed_bps;
    unsigned long long average_bps;
    unsigned long long eta_seconds;
    unsigned long long extraction_done_bytes;
    unsigned long long extraction_total_bytes;
    unsigned long long extraction_speed_bps;
    unsigned long long extraction_average_bps;
    unsigned long long extraction_eta_seconds;
    char extraction_current_file[512];
    int legacy_partial;
    int extract_after_download;
    int extract_nested_archives;
    int remove_downloads_on_cancel;
    unsigned int retry_attempts;
    unsigned long long retry_after_epoch;
    /* Password is intentionally in-memory only and never serialized. */
    char extraction_password[RD_MAX_PASSWORD_LENGTH];
    char error[256];
} rd_queue_item;

typedef struct rd_related_file {
    unsigned long item_id;
    char filename[256];
} rd_related_file;

typedef struct rd_queue {
    rd_queue_item items[RD_MAX_QUEUE_ITEMS];
    size_t count;
    unsigned long next_id;
    char state_path[512];
    rd_related_file related_files[RD_MAX_RELATED_FILES];
    size_t related_file_count;
} rd_queue;

/* Parse newline-separated URLs. Commit queue items only when the return value is nonnegative. */
int rd_parse_url_batch(const char *text, char (*urls)[RD_MAX_URL_LENGTH],
                       size_t capacity);
const char *rd_job_state_name(rd_job_state state);

/* State is stored as a versioned, escaped text file and replaced atomically. */
int rd_queue_init(rd_queue *queue, const char *state_path);
int rd_queue_add_batch(rd_queue *queue, const char (*urls)[RD_MAX_URL_LENGTH], size_t count);
int rd_queue_add_batch_options(rd_queue *queue, const char (*urls)[RD_MAX_URL_LENGTH],
                               size_t count, int extract_after_download,
                               const char *password);
int rd_queue_add_batch_preferences(rd_queue *queue,
                                   const char (*urls)[RD_MAX_URL_LENGTH], size_t count,
                                   int extract_after_download, int extract_nested_archives,
                                   const char *password);
int rd_queue_add_batch_named_preferences(rd_queue *queue,
                                   const char (*urls)[RD_MAX_URL_LENGTH],
                                   const char (*filenames)[512], size_t count,
                                   int extract_after_download, int extract_nested_archives,
                                   const char *password);
int rd_queue_record_related_file(rd_queue *queue, unsigned long item_id,
                                 const char *filename);
int rd_queue_set_filename(rd_queue *queue, unsigned long item_id, const char *filename);
int rd_queue_has_related_file(const rd_queue *queue, unsigned long item_id,
                              const char *filename);
int rd_queue_set_retry(rd_queue *queue, unsigned long item_id,
                       unsigned int attempts, unsigned long long retry_after_epoch,
                       rd_job_state state, const char *error);
int rd_queue_remove(rd_queue *queue, unsigned long id);
int rd_queue_clear_completed(rd_queue *queue);
int rd_queue_update(rd_queue *queue, unsigned long id, rd_job_state state,
                    unsigned long long downloaded, unsigned long long expected,
                    const char *error);
#endif
