# Implementation plan

Audit date: 2026-11-23. This plan is based on the files in this ZIP and the research limitations in [`RESEARCH.md`](RESEARCH.md).

## Audit result

Historical baseline only: the repository began as an early host-testable bootstrap. Later changes added the downloader, queue, dashboard and target ELF. See [`IMPLEMENTATION.md`](IMPLEMENTATION.md) for current status; steps below are remaining work unless explicitly marked complete.

The original ZIP contained no usable Git history. The project is now under Git; recent working HTTPS checkpoints are recorded in commits `73f2d74` and `003c529` (`working-https-v0.3.0`). The original archive remains a recovery point.

## Technical findings

- The SDK is installed at `.deps/ps5-payload-sdk` (v0.50), target cross-compilation succeeds, and the user has loaded the HTTPS downloader and confirmed an HTTPS sample download on PS5.
- Host tests, host-server build, and the target build currently pass.
- The downloader uses one sequential transfer worker. Range resume, segmented downloads, and Real-Debrid are not implemented. WFM archive extraction has since been integrated; see the current status in [`IMPLEMENTATION.md`](IMPLEMENTATION.md).
- The original bootstrap's `.r00` classification gap has since been fixed; the archive classifier now recognizes `.r00` and `.partNN.rar` sets.
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

**Current status:** SDK v0.50 is installed at `.deps/ps5-payload-sdk`; the PS5 ELF has loaded and the user validated a direct HTTPS download. More filesystem, pause/retry, restart and large-transfer testing is still outstanding.

## Phase 2 — local HTTP service, queue, and minimal dashboard (in progress)

The dependency-free socket server serves `/`, `/api/health`, and the mobile queue UI. Linux mode binds to `127.0.0.1`; PS5 mode binds to `0.0.0.0:8090`. The user confirmed LAN access and an earlier queue file persisted across the prior build. The current queue file is versioned and migrated atomically from that earlier format. The PS5 state path is `/data/rd-downloader-queue.db`. The new UI adds links, displays state/progress/error, supports removal, and requires an explicit Start action.

The current build status and limitations are tracked in [`IMPLEMENTATION.md`](IMPLEMENTATION.md). The queue page has confirmation, immediate start, URL-derived filenames, cooperative pause/resume, retry, confirmed active-stop/removal, destination display and transfer-rate/ETA fields. RDQ6 records the legacy partial-file naming transition. WFM archive extraction now passes a host download-to-extract smoke check; PS5 helper launch and socket behavior need console validation. Range resume, Real-Debrid and configurable destinations remain incomplete.

For iterative deployment, `elfldr` creates one process per incoming payload; it does not replace a previous copy. Raw ELF deployment is named `payload.elf`. Stop only the identifiable old RD Downloader process in Payload Manager before replacing it, then confirm port 8090 is released. Do not terminate an uncertain process just because it has the generic name. No reboot is needed for each build. During a transfer, termination preserves the `.part` file and startup marks that job failed; retry starts over after safely removing that partial file. Byte-range resume is not implemented.

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

- **Implemented:** pinned and inspected WFM helper source/build/license files; `scripts/build-wfm-helper.sh` builds the PS5 helper from its pinned commit.
- **Implemented:** W7HP IPC, safe helper startup through the local ELF loader, PING/PONG checks, task listing/attachment, progress, cancellation and terminal acknowledgments.
- **Host verified:** a downloaded ZIP was extracted through the Linux helper into the host destination.
- **Open hardware gate:** PS5 socket visibility, helper launch, RAR/password/multipart extraction, path protections, collisions, cancellation, restart attachment and no-space behavior.
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

Copy the helper ELF to `/data/wfm/wfm-7zip-helper.elf` on the PS5, then deploy the rebuilt RD Downloader ELF. Validate helper autostart through the local loader, ZIP extraction progress/output, visible password flow, one RAR password case and one multipart set. If helper socket or loader behavior differs on-console, record the observed result and adjust from that evidence. Recheck pause/resume and confirmed stop on real transfers.

## Reproduction commands

From `rd-downloader-v2/`:

```sh
make host-test
make
```

With the local SDK installed, `make host-test` runs host checks and `make` uses `.deps/ps5-payload-sdk` automatically. Build the pinned WFM helper with `./scripts/build-wfm-helper.sh`. For a forced target rebuild in this environment, use:

```sh
PS5_PAYLOAD_SDK="$PWD/.deps/ps5-payload-sdk" \
LLVM_CONFIG=/usr/bin/llvm-config-18 \
PATH="/usr/lib/llvm-18/bin:$PATH" \
make -B
```
