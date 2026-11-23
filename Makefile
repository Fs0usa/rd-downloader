PS5_HOST ?= ps5
PS5_PORT ?= 9021
ifeq ($(origin PS5_PAYLOAD_SDK),undefined)
ifneq ($(wildcard $(CURDIR)/.deps/ps5-payload-sdk/toolchain/prospero.mk),)
PS5_PAYLOAD_SDK := $(CURDIR)/.deps/ps5-payload-sdk
else
PS5_PAYLOAD_SDK := /opt/ps5-payload-sdk
endif
endif
HOST_CC ?= cc

HOST_GOALS := host-test host-server host-run
ifeq ($(filter $(HOST_GOALS),$(MAKECMDGOALS)),)
ifneq ($(MAKECMDGOALS),clean)
ifeq ($(PS5_PAYLOAD_SDK),)
$(error PS5_PAYLOAD_SDK is undefined)
endif
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
endif
endif

ELF := rd-downloader.elf
CFLAGS := -Wall -Wextra -Werror -O2 -pthread -Iinclude
SOURCES := src/main.c src/core/range_plan.c src/core/space_plan.c src/core/archive.c src/core/queue.c
SOURCES += src/core/http_server.c
SOURCES += src/core/http_client.c src/core/ps5_http_client.c
PS5_HTTP_LIBS := -lSceHttp -lSceSsl

.PHONY: all clean host-test host-server host-run deploy
all: $(ELF)

$(ELF): $(SOURCES) src/core/ca_bundle_pem.inc include/rd/range_plan.h include/rd/space_plan.h include/rd/archive.h include/rd/queue.h include/rd/http_server.h include/rd/http_client.h
	$(CC) $(CFLAGS) -o $@ $(SOURCES) $(PS5_HTTP_LIBS)

host-test:
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -Iinclude \
		tests/test_range_plan.c src/core/range_plan.c -o build-range-tests
	./build-range-tests
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -Iinclude \
		tests/test_space_archive.c src/core/space_plan.c src/core/archive.c src/core/queue.c -o build-space-tests
	./build-space-tests
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -Iinclude \
		tests/test_queue.c src/core/queue.c -o build-queue-tests
	./build-queue-tests
	$(HOST_CC) -std=c11 -DRD_HOST_BUILD -Wall -Wextra -Werror -O2 -Iinclude \
		tests/test_http_client.c src/core/http_client.c -o build-http-tests
	./build-http-tests
	rm -f build-range-tests build-space-tests build-queue-tests build-http-tests

host-server: src/main.c src/core/http_server.c src/core/http_client.c src/core/queue.c src/core/archive.c include/rd/http_server.h include/rd/http_client.h include/rd/queue.h include/rd/archive.h
	$(HOST_CC) -std=c11 -D_DEFAULT_SOURCE -DRD_HOST_BUILD -Wall -Wextra -Werror -O2 -pthread -Iinclude \
		src/main.c src/core/http_server.c src/core/http_client.c src/core/ps5_http_client.c src/core/queue.c src/core/archive.c -o rd-downloader-host

host-run: host-server
	./rd-downloader-host

deploy: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $(ELF)

clean:
	rm -f $(ELF) rd-downloader-host build-range-tests build-space-tests build-queue-tests
