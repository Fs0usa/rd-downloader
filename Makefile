PS5_HOST ?= ps5
PS5_PORT ?= 9021
ifeq ($(origin PS5_PAYLOAD_SDK),undefined)
ifneq ($(wildcard $(CURDIR)/.deps/ps5-payload-sdk/toolchain/prospero.mk),)
PS5_PAYLOAD_SDK := $(CURDIR)/.deps/ps5-payload-sdk
else
PS5_PAYLOAD_SDK := /opt/ps5-payload-sdk
endif
endif
PS5_DEPLOY ?= $(PS5_PAYLOAD_SDK)/bin/prospero-deploy
HOST_CC ?= cc
CA_BUNDLE ?= /etc/ssl/certs/ca-certificates.crt
RD_VERSION ?= $(shell cat VERSION 2>/dev/null || echo 1.1.2-beta.9)
VERSION_FLAGS := -DRD_DOWNLOADER_VERSION='"$(RD_VERSION)"'

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
WEB_EMBED := src/core/index_html.inc
CFLAGS := -Wall -Wextra -Werror -O2 -pthread -Iinclude $(VERSION_FLAGS)
SOURCES := src/main.c src/core/range_plan.c src/core/space_plan.c src/core/archive.c src/core/queue.c src/core/output.c src/core/log.c
SOURCES += src/core/http_server.c
SOURCES += src/core/http_client.c src/core/ps5_http_client.c
SOURCES += src/core/wfm_client.c
PS5_HTTP_LIBS := -lSceHttp -lSceSsl

.PHONY: all clean host-test host-server host-run deploy
all: $(ELF)

$(WEB_EMBED): web/index.html scripts/embed_web.py
	python3 scripts/embed_web.py web/index.html $(WEB_EMBED)

src/core/ca_bundle_pem.inc: $(CA_BUNDLE) certs/real-debrid-geotrust-intermediate.pem scripts/embed-ca-bundle.py
	python3 scripts/embed-ca-bundle.py $(CA_BUNDLE) $@ --extra certs/real-debrid-geotrust-intermediate.pem

$(ELF): $(SOURCES) $(WEB_EMBED) src/core/ca_bundle_pem.inc VERSION include/rd/version.h include/rd/range_plan.h include/rd/space_plan.h include/rd/archive.h include/rd/queue.h include/rd/output.h include/rd/log.h include/rd/http_server.h include/rd/http_client.h include/rd/wfm_client.h
	$(CC) $(CFLAGS) -o $@ $(SOURCES) $(PS5_HTTP_LIBS)

host-test:
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -Iinclude \
		tests/test_range_plan.c src/core/range_plan.c -o build-range-tests
	./build-range-tests
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -Iinclude \
		tests/test_space_archive.c src/core/space_plan.c src/core/archive.c src/core/queue.c src/core/output.c src/core/log.c -o build-space-tests
	./build-space-tests
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -Iinclude \
		tests/test_queue.c src/core/queue.c src/core/output.c src/core/log.c -o build-queue-tests
	./build-queue-tests
	$(HOST_CC) -std=c11 -DRD_HOST_BUILD -Wall -Wextra -Werror -O2 -Iinclude \
		tests/test_http_client.c src/core/http_client.c src/core/output.c src/core/log.c -o build-http-tests
	./build-http-tests
	rm -f build-range-tests build-space-tests build-queue-tests build-http-tests

host-server: src/main.c src/core/http_server.c src/core/http_client.c src/core/queue.c src/core/output.c src/core/archive.c src/core/wfm_client.c src/core/log.c $(WEB_EMBED) VERSION include/rd/version.h include/rd/http_server.h include/rd/http_client.h include/rd/queue.h include/rd/output.h include/rd/archive.h include/rd/wfm_client.h include/rd/log.h
	$(HOST_CC) -std=c11 -D_DEFAULT_SOURCE -DRD_HOST_BUILD -Wall -Wextra -Werror -O2 -pthread -Iinclude $(VERSION_FLAGS) \
		src/main.c src/core/http_server.c src/core/http_client.c src/core/ps5_http_client.c src/core/wfm_client.c src/core/queue.c src/core/output.c src/core/archive.c src/core/log.c -o rd-downloader-host

host-run: host-server
	./rd-downloader-host

deploy: $(ELF)
	python3 scripts/deploy-named.py --host "$(PS5_HOST)" --port "$(PS5_PORT)" --deploy "$(PS5_DEPLOY)" $(ELF)

clean:
	rm -f $(ELF) rd-downloader-host build-range-tests build-space-tests build-queue-tests
