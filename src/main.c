#include <stdio.h>
#include <string.h>
#include "rd/http_server.h"

#define RD_DOWNLOADER_VERSION "0.2.1-dev"

#ifndef RD_HOST_BUILD
typedef struct rd_notification_request {
    char reserved[45];
    char message[3075];
} rd_notification_request;

/* This PS5 kernel API is used by the installed SDK's samples/hello_world. */
int sceKernelSendNotificationRequest(int, rd_notification_request *, size_t, int);
#endif

/*
 * Start the local dashboard and bounded download worker.
 */
int
main(void)
{
#ifdef RD_HOST_BUILD
    printf("Starting local development server at http://127.0.0.1:%d\n",
           RD_HTTP_DEFAULT_PORT);
    return rd_http_server_run("127.0.0.1", RD_HTTP_DEFAULT_PORT);
#else
    rd_notification_request request;

    memset(&request, 0, sizeof(request));
    snprintf(request.message, sizeof(request.message),
             "RD Downloader %s: web service starting on port %d; HTTP-only downloads",
             RD_DOWNLOADER_VERSION, RD_HTTP_DEFAULT_PORT);

    (void)sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
    return rd_http_server_run("0.0.0.0", RD_HTTP_DEFAULT_PORT);
#endif
}
