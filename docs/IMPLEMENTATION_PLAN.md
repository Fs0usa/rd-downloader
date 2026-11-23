# Implementation plan

Audit date: 2026-11-23. This plan is based on the files in this ZIP and the research limitations in [`RESEARCH.md`](RESEARCH.md).

## Audit result

The repository is an early host-testable bootstrap, not a partially working downloader. It has a simple range planner, URL batch parser, space arithmetic helper, archive filename classifier, enum labels, and a print-only `main()`. The three host test programs pass. No UI, web service, HTTP/TLS client, downloader, persistent queue, Real-Debrid integration, extraction integration or PS5 ELF exists.

No local Git history is available: the ZIP contains an empty `.git` directory, not a usable repository. The archive itself is present as `rd-downloader-v2.zip` in the parent project directory. Create a real repository and commit a reviewed baseline once the project owner confirms the preferred remote/identity; until then, preserve the original ZIP as a recovery point.

## Technical findings

- The default build includes `/opt/ps5-payload-sdk/toolchain/prospero.mk`; this SDK path is missing. Therefore no target compile, ELF, deployment or console load has been demonstrated.
- `make host-test` compiles and passes all three current test executables.
- The Makefile's target build is only the smoke `main.c` plus four primitive modules. Even after the SDK is installed, that output would not be a network service or downloader.
- The source's `.r00` support requirement is unmet by `rd_classify_archive_name`; the current numeric branch handles `.rar` filenames ending with a digit instead of legacy `.r00` volumes.
- The range planner has no HTTP-response verification, chunk scheduling, retry or persistence. Its caller must cap `max_connections` to an `int`-safe value.
- The URL parser does not perform full URL validation. Treat it as a preliminary filter only; use a vetted parser or stronger authority validation before network access.
- Current docs overstate what is verified about WFM helper internals. Research now separates upstream README claims from local source/build verification.

## Phase 1 — minimal payload and runtime proof (current gate)

1. Install a pinned release of `ps5-payload-dev/sdk` and required compiler tools on the Debian build host.
2. Record SDK version/tag, archive hash and compiler versions.
3. Compare this repository's target flags/link recipe and entrypoint with the SDK's `samples/hello_world` and `websrv` PS5 makefile.
4. Build a dependency-free smoke ELF; inspect ELF architecture and linked symbols.
5. Load it through the user's supported ELF loader on the actual console and capture a visible success signal.
6. Confirm no compatibility claim beyond the specific target firmware, loader and SDK build tested.

**Current status:** `scripts/install-ps5-sdk.sh` installed SDK v0.50 into `.deps/ps5-payload-sdk`; a forced target rebuild succeeded, and the smoke ELF was loaded on the user's PS5. The user confirmed seeing the RD Downloader notification. Phase 1's compile/load gate is complete. Filesystem and long-running lifecycle probes remain outstanding.

## Phase 2 — local HTTP service, queue, and minimal dashboard (in progress)

The dependency-free socket server serves `/`, `/api/health`, and the mobile queue UI. Linux mode binds to `127.0.0.1`; PS5 mode binds to `0.0.0.0:8090`. The user confirmed LAN access and an earlier queue file persisted across the prior build. The current queue file is versioned and migrated atomically from that earlier format. The PS5 state path is `/data/rd-downloader-queue.db`. The new UI adds links, displays state/progress/error, supports removal, and requires an explicit Start action.

The current build adds a single sequential download worker. PS5 builds now use `libSceHttp`/`libSceSsl`, an embedded CA bundle, certificate verification, bounded reads and manual redirect handling that refuses HTTPS-to-HTTP downgrade. Host builds retain the plain-HTTP socket transport. Both write uniquely named files under `/data/homebrew`, preflight known sizes with a 16 MiB reserve, and publish only after `fsync` and successful completion. Queue copies are heap allocated and the worker requests a 1 MiB stack after the user observed the previous payload stop at download start. Host tests and target cross-compilation pass; PS5 HTTPS, downloads, destination writability, thread behavior and state migration still need console validation. Retries, resume, range segments, pause/cancel and configurable destinations remain incomplete.

For iterative deployment, `elfldr` creates one process per incoming payload; it does not replace a previous copy. Raw ELF deployment is named `payload.elf`. Stop only the identifiable old RD Downloader process in Payload Manager before replacing it, then confirm port 8090 is released. Do not terminate an uncertain process just because it has the generic name. No reboot is needed for each build. During a transfer, termination preserves the `.part` file and startup marks that job failed; true resume is not yet implemented.

## Ordered milestones

### 1. SDK and payload lifecycle

- Resolve target SDK/toolchain, build and load smoke ELF.
- Verify filesystem APIs (`stat`, `statvfs` or verified equivalent), path access, write/fsync/rename and signal/lifecycle behavior on-console.
- Keep the SDK and ELF-loader constraints documented.

### 2. HTTP service and REST skeleton

- Serve static dashboard assets and expose health/configuration endpoints.
- Add request-size limits, strict JSON parsing, path validation, state-changing request protection, and bind-address policy.
- Verify mobile Safari access on the local network.

### 3. Direct sequential download (HTTP and HTTPS transports compile; runtime gate open)

- The PS5 target uses the installed SDK's `libSceHttp` and `libSceSsl`; CA roots are embedded from Debian's `ca-certificates` bundle.
- Explicitly enable server, hostname, CA, and SNI verification; do not disable certificate validation.
- Stream bounded buffers to a staging file, follow up to five redirects, reject HTTPS downgrade, sanitize filenames, validate status/length, and atomically finalize.
- Verify HTTPS request, current CA-chain behavior, redirects, unknown length, interrupted transfer, disk-full behavior and final hash on the actual PS5.
- Test plain HTTP/HTTPS, redirects, unknown length, interrupted transfer, disk-full, and final hash on host fixtures and then on PS5.

### 4. Persistent queue and resume

- Introduce bounded scheduler and durable atomic state store, with crash-consistent partial file/checkpoint updates.
- Persist original and effective URLs carefully; never persist secrets in logs or API state.
- Add pause/resume/cancel/retry, queue ordering and restart recovery. Validate server validators and range response semantics before segmented downloads; otherwise use sequential fallback.

### 5. Archive helper integration

- Clone and pin WFM helper, inspect actual source/build/licenses and build it for the same target SDK.
- Implement framed IPC with length caps and robust partial send/receive handling.
- Test extraction formats, passwords, multipart discovery, traversal/symlink rejection, collisions, cancellation, task attachment and no-space behavior.
- Do not promise pause/resume until a maintained helper implementation supports and verifies it.

### 6. Storage safety

- Probe configured destination and staging filesystem at runtime.
- Calculate required space without double-counting same-filesystem files; account for cross-filesystem copying.
- Add persistent `WAITING_FOR_SPACE` / `WAITING_FOR_STORAGE` states and resume checks; never auto-delete user files.

### 7. Optional Real-Debrid

- Add backend-only token storage, account check and link unrestriction after direct URLs work.
- Use official API schemas, request limits and error handling; never expose token/private resolved URL in logs or frontend responses.

### 8. Dashboard and end-to-end validation

- Implement queue/settings UI as a command client and state renderer.
- Validate browser closure, payload restart and helper restart separately on real hardware.
- Compare downloaded artifact hashes against trusted values where available.

## Immediate next action

Stop the old RD Downloader process in Payload Manager, deploy the current build, and first confirm `/api/health` reports version `0.3.0-https-dev` with `worker_ready:true`. Then try the user's small HTTPS RAR sample. Verify the payload remains reachable, the queue reaches `completed`, and the output exists under `/data/homebrew`. If TLS or redirects fail, use the exact queue error to adjust the HTTPS path before attempting larger files. Compare the resulting file hash with the provided sample when a trusted hash is available.

## Reproduction commands

From `rd-downloader-v2/`:

```sh
make host-test
make
```

With the local SDK installed, `make host-test` runs host checks and `make` uses `.deps/ps5-payload-sdk` automatically. For a forced target rebuild in this environment, use:

```sh
PS5_PAYLOAD_SDK="$PWD/.deps/ps5-payload-sdk" \
LLVM_CONFIG=/usr/bin/llvm-config-18 \
PATH="/usr/lib/llvm-18/bin:$PATH" \
make -B
```
