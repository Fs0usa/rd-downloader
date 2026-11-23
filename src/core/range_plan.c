#include "rd/range_plan.h"

int
rd_plan_ranges(uint64_t total_bytes,
              unsigned int max_connections,
              uint64_t min_segment_bytes,
              rd_byte_range *out,
              size_t out_capacity)
{
    uint64_t desired;
    uint64_t count;
    uint64_t base;
    uint64_t remainder;
    uint64_t cursor = 0;
    uint64_t i;

    if (total_bytes == 0) {
        return 0;
    }
    if (max_connections == 0 || min_segment_bytes == 0 || out == NULL) {
        return -1;
    }

    /* ceil(total / min_segment), written without total + divisor overflow. */
    desired = total_bytes / min_segment_bytes;
    if (total_bytes % min_segment_bytes != 0) {
        desired++;
    }
    if (desired == 0) {
        desired = 1;
    }

    count = desired < (uint64_t)max_connections
          ? desired : (uint64_t)max_connections;
    if (count > (uint64_t)out_capacity) {
        return -2;
    }

    base = total_bytes / count;
    remainder = total_bytes % count;

    for (i = 0; i < count; i++) {
        uint64_t length = base + (i < remainder ? 1U : 0U);
        out[i].start = cursor;
        out[i].end = cursor + length - 1;
        cursor += length;
    }

    return (int)count;
}
