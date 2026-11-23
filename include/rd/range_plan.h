#ifndef RD_RANGE_PLAN_H
#define RD_RANGE_PLAN_H

#include <stddef.h>
#include <stdint.h>

typedef struct rd_byte_range {
    uint64_t start; /* inclusive */
    uint64_t end;   /* inclusive */
} rd_byte_range;

/*
 * Plans non-overlapping inclusive byte ranges.
 * Returns the number of ranges, 0 for an empty file, or a negative error:
 * -1 invalid arguments; -2 output capacity is insufficient.
 *
 * The planner caps the number of ranges at max_connections and avoids
 * creating ranges smaller than min_segment_bytes unless the file itself
 * is smaller than that threshold.
 */
int rd_plan_ranges(uint64_t total_bytes,
                   unsigned int max_connections,
                   uint64_t min_segment_bytes,
                   rd_byte_range *out,
                   size_t out_capacity);

#endif
