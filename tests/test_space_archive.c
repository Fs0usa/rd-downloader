#include "rd/space_plan.h"
#include "rd/archive.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_space(void) {
    rd_space_request r = { .download_bytes=100, .expanded_bytes=500, .safety_reserve_bytes=50, .will_extract=true, .expanded_size_known=true };
    rd_space_plan p;
    assert(rd_plan_space(&r, 700, &p) == 0);
    assert(p.required_bytes == 650 && p.can_start);
    assert(rd_plan_space(&r, 649, &p) == 0 && !p.can_start);
    r.expanded_size_known = false;
    assert(rd_plan_space(&r, 10000, &p) == 0 && p.needs_runtime_guard);
}
static void test_archive(void) {
    rd_archive_name a;
    assert(rd_classify_archive_name("/data/downloads/Game.part01.rar", &a) == 0);
    assert(a.kind == RD_ARCHIVE_RAR_MULTIPART_PART && a.part_number == 1);
    assert(rd_classify_archive_name("Game.part12.rar", &a) == 0);
    assert(a.kind == RD_ARCHIVE_RAR_MULTIPART_PART && a.part_number == 12);
    assert(rd_classify_archive_name("Game.001", &a) == 0 && a.kind == RD_ARCHIVE_SPLIT_001);
    assert(rd_classify_archive_name("Game.rar", &a) == 0 && a.kind == RD_ARCHIVE_RAR_SINGLE);
    assert(rd_classify_archive_name("Game.zip", &a) == 0 && a.kind == RD_ARCHIVE_ZIP);
}
int main(void) { test_space(); test_archive(); puts("space/archive tests: OK"); return 0; }
