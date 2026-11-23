#include "rd/range_plan.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void test_empty_file(void)
{
    rd_byte_range ranges[8];
    assert(rd_plan_ranges(0, 4, 8, ranges, 8) == 0);
}

static void test_single_range(void)
{
    rd_byte_range ranges[8];
    int n = rd_plan_ranges(1024, 4, 2048, ranges, 8);
    assert(n == 1);
    assert(ranges[0].start == 0);
    assert(ranges[0].end == 1023);
}

static void test_even_partition(void)
{
    rd_byte_range ranges[4];
    int n = rd_plan_ranges(64, 4, 16, ranges, 4);
    assert(n == 4);
    for (int i = 0; i < n; i++) {
        assert(ranges[i].end - ranges[i].start + 1 == 16);
        if (i > 0) assert(ranges[i].start == ranges[i - 1].end + 1);
    }
    assert(ranges[0].start == 0 && ranges[3].end == 63);
}

static void test_remainder_is_covered(void)
{
    rd_byte_range ranges[4];
    int n = rd_plan_ranges(101, 4, 1, ranges, 4);
    uint64_t next = 0;
    assert(n == 4);
    for (int i = 0; i < n; i++) {
        assert(ranges[i].start == next);
        assert(ranges[i].end >= ranges[i].start);
        next = ranges[i].end + 1;
    }
    assert(next == 101);
}

static void test_capacity_and_arguments(void)
{
    rd_byte_range ranges[2];
    assert(rd_plan_ranges(64, 4, 1, ranges, 2) == -2);
    assert(rd_plan_ranges(64, 0, 1, ranges, 2) == -1);
    assert(rd_plan_ranges(64, 2, 0, ranges, 2) == -1);
    assert(rd_plan_ranges(64, 2, 1, NULL, 2) == -1);
}

static void test_large_values(void)
{
    rd_byte_range ranges[8];
    const uint64_t total = UINT64_MAX - 17;
    int n = rd_plan_ranges(total, 8, 1, ranges, 8);
    uint64_t next = 0;
    assert(n == 8);
    for (int i = 0; i < n; i++) {
        assert(ranges[i].start == next);
        next = ranges[i].end + 1;
    }
    assert(ranges[7].end == total - 1);
}

int main(void)
{
    test_empty_file();
    test_single_range();
    test_even_partition();
    test_remainder_is_covered();
    test_capacity_and_arguments();
    test_large_values();
    puts("range planner: all tests passed");
    return 0;
}
