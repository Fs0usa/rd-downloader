# RD Downloader 1.0.0 implementation status

## Current code status (2026-11-23)

This is the first major release checkpoint. The user confirmed direct HTTPS downloading and RAR extraction on the PS5. See [`CHANGELOG.md`](CHANGELOG.md) for the v1.0.0 scope and remaining limitations.

The current build contains a mobile dashboard, atomic persistent queue, and a single sequential download worker. The PS5 target uses `libSceHttp`/`libSceSsl` with an embedded CA bundle, certificate verification, bounded streaming, manual redirects and HTTPS downgrade refusal. The user confirmed the sample HTTPS download completed on the PS5. Files are written to `/data/homebrew`, preflight known sizes with a 16 MiB reserve, and publish only after `fsync` and successful completion.

The dashboard source in `web/index.html` follows the supplied mobile interface guide and is embedded by `scripts/embed_web.py`. The backend reports destination, extraction choice, and extraction progress with queue state. The page supports multiline link review, visible optional password persistence in guarded browser `localStorage`, pause/resume, retry, confirmed stop/remove and clearing completed queue records. `POST /api/add` sends URL-encoded `urls`, `extract`, and `password` fields. The backend retains the password only in process memory; it is neither serialized nor returned by the API. `POST /api/clear` atomically removes completed queue records and preserves their files. Pause is cooperative at HTTP read callback boundaries. Retry starts from byte zero. Active removal requires confirmation, aborts the request, removes the downloader-owned partial file after the worker closes it, and removes the queue entry. Removing a completed queue entry preserves the final download. Current/average speed and ETA are calculated from transfer callbacks. Queue format RDQ6 persists extraction choice, filename, destination and whether a legacy partial file uses the old `rd-{id}-` path.

Archive extraction has a W7HP IPC client for WFM's separate helper ELF. The helper is pinned at upstream commit `7a8392ab4a743bcc941fc531c2f8c65f8cf57800`; `scripts/build-wfm-helper.sh` reproduces its PS5 build. On PS5, the client first checks for an existing helper with PING/PONG. If the socket is absent, it sends the ELF at `/data/wfm/wfm-7zip-helper.elf` to the local ELF loader on port 9021 and waits for PING/PONG. If a helper socket accepts a connection but fails PING, RD Downloader refuses to start a possible duplicate. It never kills a helper. The IPC client sends extraction/password requests, reports progress/current member, cancels on confirmed stop, and lists/attaches to helper tasks after payload restart. The queue waits for queued multipart volumes before extracting the first volume. Archive classification covers `.partNN.rar`, `.r00`, and `.001` sets. The user confirmed on-console that a direct RAR download completed and extracted correctly using the WFM helper.

Output filename display and disk paths use the same sanitized basename from the input URL path, without a job-ID prefix; `Content-Disposition` and final redirect names are not read. Range resume, segmented transfers, Real-Debrid, destination selection and storage wait/recovery remain open. Extraction pause/resume is not provided by upstream IPC: stopping an extraction is cooperative cancellation, and a retry starts the extraction pass again while keeping the downloaded archive.

Current verification: the user confirmed the PS5 download-to-RAR-extraction flow works. `make host-test`, `make host-server`, and the PS5 cross-build pass. The generated dashboard JavaScript passes `node --check`; host tests cover archive grouping, queue clearing without removing files, the AkiraBox-style encoded basename and owned partial cleanup. A local end-to-end check downloads a ZIP over HTTP and extracts it through the Linux WFM helper into the expected destination. Multipart archives, password-protected archives, restart recovery and large downloads still need separate on-console validation.

The SDK HTTP2 sample and another PS5 downloader's reported large-transfer experience informed the decision to keep the first worker on POSIX sockets. See the transport evaluation in [`RESEARCH.md`](RESEARCH.md).

## Phase 1 — SDK and payload lifecycle
- Pin a PS5 Payload SDK release compatible with the target firmware.
- Build and load a minimal ELF; verify lifecycle and filesystem APIs on-console.
- Verify `/data/homebrew` at runtime with `stat`, `access`, `statvfs`, and a write/fsync/rename probe. Do not assume it exists or is writable.

## Phase 2 — local service and queue
- Implement a bounded HTTP server and JSON REST API.
- Parse newline-separated URLs; accept only `http://` and `https://` direct links plus explicitly supported Real-Debrid hoster URLs.
- Store queue state atomically on the PS5. Browser polling is read-only with respect to transfer state.
- Queue operations: add, reorder, pause, resume, retry, cancel, remove, extract, set password, choose destination.

## Phase 3 — direct-link downloader
- HEAD / range probe; handle redirects, Content-Disposition, unknown content length and servers that ignore Range.
- Use chunked Range workers only when `206`, Content-Range and validators prove the server supports the requested ranges.
- Fall back to one sequential stream if range support is absent or inconsistent.
- Persist original URL, effective URL only as ephemeral metadata, filename, size, ETag/Last-Modified, destination, completed ranges and retry state.
- Add bounded retries, Retry-After handling, DNS/TCP/TLS/HTTP/disk error classification and resume reconciliation.

## Phase 4 — optional Real-Debrid resolver
- Manual backend-only token configuration.
- `GET /user` and `POST /unrestrict/link`.
- Direct URLs bypass Real-Debrid completely.
- Never expose token or resolved private URL in browser responses or logs.

## Phase 5 — extraction helper integration
- Build/pin `wfm-7zip-helper.elf` from its source and bundle notices/source.
- Implement W7HP Unix-socket client with exact 20-byte big-endian frame prefix and max 1 MiB payload.
- Discover helper using PING/PONG; do not assume it exists or start duplicate helpers.
- Send the first volume only for multipart sets; wait until all parts are downloaded.
- Attach/list tasks after client restart; do not restart helper while extracting.
- Surface password-required, wrong-password, missing-volume, corrupt archive, no-space and cancelled errors separately.

## Phase 6 — disk-space and recovery policy
- Use `statvfs` on the actual destination and staging filesystem before starting jobs.
- For a known archive expansion size, reserve compressed bytes + expanded bytes + configurable safety margin; account for cross-filesystem copies if staging and destination differ.
- Unknown expansion size: mark estimate as provisional, inspect archive metadata where helper supports it, then enforce per-file and write-time checks.
- If space is insufficient, transition job to `WAITING_FOR_SPACE`; do not fail or discard partial downloads.
- Recheck capacity periodically and on explicit Resume. User may free space with another file manager.
- If destination disappears, stop writes, preserve checkpoints and transition to `WAITING_FOR_STORAGE`.
- Do not silently move output to another mount.

## Phase 7 — dashboard
- Mobile-first UI: multi-line URL input, queue, job details, destination selector, extraction toggle, per-archive password prompt, storage free-space indicator, speed/progress/ETA, logs.
- Browser only sends commands and renders backend state. Closing it has no effect on work.

## Queue state machine
`QUEUED -> PROBING -> WAITING_FOR_SPACE -> DOWNLOADING -> VERIFYING -> READY_TO_EXTRACT -> EXTRACTING -> PUBLISHING -> COMPLETED`

Side states: `PAUSED`, `WAITING_FOR_NETWORK`, `WAITING_FOR_STORAGE`, `FAILED`, `CANCELLED`. Space exhaustion during extraction transitions to `WAITING_FOR_SPACE`, preserving helper task and partial outputs where supported. The UI must clearly distinguish download progress from extraction progress.

## Extraction pause/resume compatibility gap
The published WFM helper protocol exposes cancellation and task attachment, but no PAUSE/RESUME command. A client disconnect does not pause extraction. Therefore true in-flight extraction pause/resume is **not available from the unmodified helper**. The integration must either (a) extend our maintained helper fork with new PAUSE/RESUME message types and cooperative checkpoints in decoder/output callbacks, or (b) report an ENOSPC extraction as `WAITING_FOR_SPACE` and retry the archive after capacity is restored (which restarts that archive's extraction pass). Do not label option (b) as byte-level extraction resume. The intended implementation is (a), subject to source-level implementation and PS5 testing.
