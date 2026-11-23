#define _POSIX_C_SOURCE 200809L
#include "rd/queue.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
static void write_hex(FILE *file, const char *text) {
    size_t i;
    for (i = 0; text[i]; ++i) fprintf(file, "%02X", (unsigned char)text[i]);
    fputc('\n', file);
}
int main(void) {
    char urls[3][RD_MAX_URL_LENGTH];
    int n = rd_parse_url_batch("  https://cdn.example/file.part01.rar  \n\nhttp://cdn.example/file.part02.rar\r\n", urls, 3);
    assert(n == 2);
    assert(strcmp(urls[0], "https://cdn.example/file.part01.rar") == 0);
    assert(strcmp(urls[1], "http://cdn.example/file.part02.rar") == 0);
    assert(rd_parse_url_batch("ftp://example/file", urls, 3) == -2);
    assert(rd_parse_url_batch("https://", urls, 3) == -2);
    assert(rd_parse_url_batch("http:///empty-host", urls, 3) == -2);
    assert(rd_parse_url_batch("https://example/1\nhttps://example/2", urls, 1) == -2);
    assert(strcmp(rd_job_state_name(RD_JOB_WAITING_FOR_SPACE), "waiting_for_space") == 0);
    {
        char path[] = "/tmp/rd-queue-test-XXXXXX";
        int fd = mkstemp(path);
        rd_queue q, restored;
        assert(fd >= 0);
        close(fd);
        unlink(path);
        assert(rd_queue_init(&q, path) == 0);
        assert(rd_parse_url_batch("https://one.example/a\nhttps://two.example/b", urls, 3) == 2);
        assert(rd_queue_add_batch(&q, (const char (*)[RD_MAX_URL_LENGTH])urls, 2) == 2);
        assert(q.count == 2 && q.items[0].id == 1 && q.items[1].id == 2);
        assert(rd_queue_update(&q, 1, RD_JOB_DOWNLOADING, 5, 10, "") == 0);
        assert(rd_queue_init(&restored, path) == 0);
        assert(restored.count == 2 && strcmp(restored.items[1].url, urls[1]) == 0);
        assert(restored.items[0].state == RD_JOB_FAILED &&
               restored.items[0].downloaded_bytes == 5);
        assert(rd_queue_remove(&restored, 1) == 0);
        assert(rd_queue_init(&q, path) == 0 && q.count == 1 && q.items[0].id == 2);
        assert(rd_parse_url_batch("https://three.example/c", urls, 3) == 1);
        assert(rd_queue_add_batch(&q, (const char (*)[RD_MAX_URL_LENGTH])urls, 1) == 1);
        assert(q.items[1].id == 3);
        assert(rd_queue_update(&q, 3, RD_JOB_FAILED, 0, 0, "bad\"error\n") == 0);
        assert(rd_queue_init(&restored, path) == 0 && restored.count == 2 &&
               restored.items[1].id == 3);
        assert(restored.items[1].state == RD_JOB_FAILED &&
               strcmp(restored.items[1].error, "bad_error_") == 0);
        unlink(path);
        {
            char temp_path[sizeof(path) + 8];
            snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
            unlink(temp_path);
        }
    }
    {
        char directory[] = "/tmp/rd-queue-missing-XXXXXX";
        char path[256];
        rd_queue q;
        assert(mkdtemp(directory) != NULL);
        snprintf(path, sizeof(path), "%s/no-such-dir/queue.db", directory);
        assert(rd_queue_init(&q, path) == 0);
        assert(rd_parse_url_batch("http://example.com/file", urls, 3) == 1);
        assert(rd_queue_add_batch(&q, (const char (*)[RD_MAX_URL_LENGTH])urls, 1) == -1);
        assert(q.count == 0);
        rmdir(directory);
    }
    {
        char path[] = "/tmp/rd-queue-v2-XXXXXX";
        int fd = mkstemp(path);
        FILE *file;
        char header[32];
        rd_queue q;
        assert(fd >= 0);
        file = fdopen(fd, "w");
        assert(file != NULL);
        fputs("RDQ2\t2\n1\t", file);
        write_hex(file, "http://legacy.example/file.bin");
        assert(fclose(file) == 0);
        assert(rd_queue_init(&q, path) == 0);
        assert(q.count == 1 && strcmp(q.items[0].url, "http://legacy.example/file.bin") == 0);
        file = fopen(path, "r");
        assert(file != NULL && fgets(header, sizeof(header), file) != NULL);
        assert(strcmp(header, "RDQ4\t2\n") == 0);
        fclose(file);
        unlink(path);
    }
    puts("queue parser tests: OK");
    return 0;
}
