# RD Downloader for PS5

Native, single-user PS5 payload for queued direct-link downloads, optional Real-Debrid link resolution, and archive extraction. The PS5 owns the queue, credentials, network transfers, extraction orchestration and persistent state. The browser is only a remote control/dashboard.

## Long-term product goal

1. Paste one or many direct HTTP/HTTPS URLs (one per line) or submit Real-Debrid-supported hoster links for resolution.
2. The backend validates and persists each queue item.
3. Downloads run with bounded file concurrency and per-file range workers when the origin supports byte ranges; otherwise they fall back to sequential streaming.
4. Completed downloads are verified and inspected for archive type / multipart naming.
5. The user can extract manually or enable extract-after-download. Passwords are supplied per archive/task and remain backend-only.
6. Output is published to the selected destination, defaulting to `/data/homebrew` after runtime validation.

The workflow above describes the goal, not the current feature set.

## Current implementation status (2026-11-23)

**Working PS5 HTTPS download checkpoint (v0.3.0).** The user confirmed that the HTTPS sample download completed on their PS5 with this build. The earlier HTTP service and persisted queue were also confirmed on-console. Current code includes:

- PS5 Payload SDK build target;
- byte-range planner;
- conservative disk-space preflight planner;
- filename-based archive / multipart classifier;
- host tests for those primitives;
- a bounded HTTP service and mobile-first queue page;
- an atomic, versioned queue file that preserves job state and progress checkpoints across payload restarts;
- one automatically started, single-worker download path with redirects, streaming writes, size checks and atomic publication; PS5 builds use `libSceHttp`/`libSceSsl`, host builds retain the POSIX HTTP test transport;
- certificate validation using an embedded Debian Mozilla-derived CA bundle; HTTP-to-HTTPS redirects are followed and HTTPS-to-HTTP downgrade redirects are refused;
- UI progress, status and error reporting, with queue mutation protected from cross-origin browser requests by a required custom header.

The working tree adds a visible optional-password field, immediate start after confirmation, a second-confirmation stop/remove action, partial-file cleanup, URL-derived output names without the old `rd-{id}-` prefix, and speed/average/ETA display. These changes pass host tests and cross-compile, but are not yet validated on the console. Additional files run sequentially. Pause is cooperative at progress callback boundaries; retry starts at byte zero; payload restart does not resume a transfer. Removing a completed queue entry leaves its final file.

Still missing: WFM extraction IPC (the toggle saves the choice and archive jobs report `ready_to_extract`; nothing is extracted), range/parallel downloads, retry backoff, Real-Debrid, destination selection, USB discovery and storage wait/recovery. The password stays in process memory and is not currently consumed by an extractor. Output names use a sanitized URL basename; HTTP `Content-Disposition` is not read.

## Queue API

- `GET /api/queue` returns filenames, states, byte counts, speed/ETA and safe errors. It omits URLs and passwords.
- `POST /api/queue` accepts up to four newline-separated HTTP or HTTPS URLs and starts the worker immediately. The dashboard sends `X-RD-Command: 1`, `X-RD-Extract: 0|1`, and the optional `X-RD-Password` header.
- `POST /api/start` remains available to wake queued jobs. Include `X-RD-Command: 1`.
- `DELETE /api/queue/{id}` removes an item. On an active transfer it aborts the request, deletes the owned partial file after worker cleanup, and removes the queue entry. The dashboard requires a second confirmation. Removing a completed item keeps its file.

The custom header makes browser cross-origin requests require a CORS preflight, which this server does not allow. This is a CSRF mitigation for the same-origin dashboard, not user authentication; use the service only on a trusted local network. The PS5 stores queue data at `/data/rd-downloader-queue.db` with mode 0600 and atomic replacement; Linux host mode uses `/tmp/rd-downloader-host-queue.db`. The prior HTTPS checkpoint was confirmed on-console; the current queue migration and stop/remove changes still need console validation. Files are saved as the sanitized URL basename under `/data/homebrew`, with `.part` appended while incomplete.

The PS5 transport uses `libSceHttp` with explicit TLS verification options and an embedded CA bundle generated from Debian's `ca-certificates` package. The user confirmed a direct HTTPS sample completed on the PS5. The transport follows up to five redirects, refuses HTTPS-to-HTTP redirects, writes a bounded stream to a unique `.part` file under `/data/homebrew`, checks known `Content-Length` against free space plus a 16 MiB reserve, flushes the file, and renames it only after a complete response. It refuses to overwrite an existing result or stale `.part` file. Unknown-length transfers have no preflight estimate. If the payload restarts during a transfer, the job becomes failed and its partial file remains; automatic resume is not implemented. Host builds retain the POSIX HTTP path and do not exercise PS5 TLS.

### Small PS5 download smoke test

Use a tiny file served by your Debian PC on the same LAN before trying any large or personal URL. In one PC terminal:

```sh
mkdir -p /tmp/rd-mvp-fixture
dd if=/dev/urandom of=/tmp/rd-mvp-fixture/hello.bin bs=1024 count=16 status=none
python3 -m http.server 8091 --bind 0.0.0.0 --directory /tmp/rd-mvp-fixture
```

After deploying the new ELF, open its page, remove any old test queue entries, add `http://PC_LAN_IP:8091/hello.bin`, and confirm the download settings. The expected output is `/data/homebrew/hello.bin`. This test exercises plain HTTP only and writes a 16 KiB file.

Example from a computer on the LAN:

```sh
curl http://PS5_IP:8090/api/queue
curl -X POST -H 'X-RD-Command: 1' -H 'Content-Type: text/plain' \
  --data-binary 'https://example.com/a.zip
https://example.com/b.rar' http://PS5_IP:8090/api/queue
curl -X DELETE -H 'X-RD-Command: 1' http://PS5_IP:8090/api/queue/1
```

## Build

On Debian 13, install the latest official `ps5-payload-dev/sdk` release and build prerequisites with:

```sh
./scripts/install-ps5-sdk.sh
```

The script installs compiler and deployment packages (including `socat`) with `apt`, downloads the SDK into the ignored project-local `.deps/` directory, checks the SHA-256 published in GitHub release metadata, runs host tests, and builds the PS5 ELF. It does not install the SDK onto the PS5. To use a separately installed SDK, set `PS5_PAYLOAD_SDK` to its directory before running `make`.

Host tests:

```sh
make host-test
```

To run the HTTP service locally on the PC, open two terminals in this directory. In the first, run `make host-run`; in the second, open `http://127.0.0.1:8090/` or `http://127.0.0.1:8090/api/health`. Stop the server with Ctrl+C. The host mode binds only to loopback.

## Default destination and storage safety

The configured default output path is `/data/homebrew`. Destination selection and USB discovery are not implemented. The downloader checks that the path is available and checks known file sizes against free space; it does not silently select another destination.

Preflight must reserve space for downloaded archives + known expanded size + safety margin. If expanded size is unknown, preflight is explicitly provisional and extraction must perform per-file and write-time free-space checks. On ENOSPC or destination removal, pause safely, retain partial state, and allow resume after the user frees space or reconnects storage.

## Archive extraction integration

Use [`owendswang/wfm-7zip-helper`](https://github.com/owendswang/wfm-7zip-helper) as the extraction engine rather than implementing RAR/7z decoders. It is a separate PS5 ELF helper and communicates over `/system_tmp/wfm-7zip-helper.sock` using the documented `W7HP` framed Unix-socket protocol. The helper supports encrypted archives, multipart RAR, numbered split archives, path traversal protection, progress, cancellation, and extraction-space checks.

The RD Downloader must implement a small IPC client and task reconciler (`PING`, `EXTRACT`, `CANCEL`, `LIST_TASKS`, `ATTACH_TASK`, `ACK_TASK`). It must not restart the helper while an extraction is active. Bundle both ELFs and preserve all third-party license notices and corresponding source obligations. The helper's own documentation states that tasks survive client disconnects but not helper/console crashes; RD Downloader must persist its own intent and reconcile task state on restart.

## Queue and job semantics

- Multiple URLs are accepted in one paste operation; one URL per line.
- Queue items are independent, ordered, cancellable and persist across payload restarts.
- Suggested initial limits: 2 simultaneous files, 4 range workers per file, configurable 1–8 workers.
- Direct HTTP/HTTPS URLs are first-class. Real-Debrid is an optional resolver, not a mandatory provider.
- Preserve original URL and safe metadata. Do not persist authorization headers or passwords in logs.
- A multipart archive is extracted only after all required volumes are present. Submit the first volume only to the helper (`.part01.rar`, `.rar` for legacy RAR sets, or `.001`); the helper discovers subsequent volumes.
- Never delete source archives until extraction succeeds and the user-selected cleanup policy permits it.

See `docs/RESEARCH.md` and `docs/IMPLEMENTATION.md`.

## Staging layout

Default staging should be a hidden application-owned directory on the same filesystem as the destination, e.g. `/data/homebrew/.rd-downloader/staging`, while extracted/published files go to `/data/homebrew`. This enables same-filesystem rename where the SDK/filesystem supports it. The exact directory must be created and write-tested at startup. If the user selects a USB destination, staging should move to a hidden directory on that same USB filesystem to avoid a second full copy; never silently change the selected destination.

## Extraction pause/resume limitation

The upstream WFM helper currently documents cancellation and task reattachment, but not in-flight pause/resume. We must extend our maintained helper fork with cooperative PAUSE/RESUME commands to satisfy true extraction pause/resume. Until that extension is implemented, a no-space extraction can wait for the user and then retry the archive extraction pass from the beginning; that is not byte-level resume and must be presented honestly in the UI.
