#include "rd/space_plan.h"
#include <limits.h>

static int add_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (UINT64_MAX - a < b) return -1;
    *out = a + b;
    return 0;
}

int rd_plan_space(const rd_space_request *r, uint64_t available, rd_space_plan *out) {
    uint64_t required = 0;
    if (!r || !out) return -1;
    out->needs_runtime_guard = false;
    if (add_u64(required, r->download_bytes, &required) != 0) return -2;
    if (r->will_extract && r->expanded_size_known) {
        /* The compressed archive remains present until extraction succeeds. */
        if (add_u64(required, r->expanded_bytes, &required) != 0) return -2;
    } else if (r->will_extract) {
        /* Compressed size is not a safe proxy for expanded size. */
        out->needs_runtime_guard = true;
    }
    if (add_u64(required, r->safety_reserve_bytes, &required) != 0) return -2;
    out->required_bytes = required;
    out->available_bytes = available;
    out->can_start = required <= available;
    if (r->will_extract && !r->expanded_size_known) out->needs_runtime_guard = true;
    return 0;
}
