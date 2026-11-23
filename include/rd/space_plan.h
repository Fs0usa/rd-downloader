#ifndef RD_SPACE_PLAN_H
#define RD_SPACE_PLAN_H
#include <stdbool.h>
#include <stdint.h>

typedef struct rd_space_request {
    uint64_t download_bytes;
    uint64_t expanded_bytes;
    uint64_t safety_reserve_bytes;
    bool will_extract;
    bool expanded_size_known;
} rd_space_request;

typedef struct rd_space_plan {
    uint64_t required_bytes;
    uint64_t available_bytes;
    bool can_start;
    bool needs_runtime_guard;
} rd_space_plan;

/* Conservative preflight. Unknown expansion size always requires runtime checks. */
int rd_plan_space(const rd_space_request *request,
                  uint64_t available_bytes,
                  rd_space_plan *out);
#endif
