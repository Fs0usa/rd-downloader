# Updated implementation plan

## Current code status (2026-11-23)

The current build contains a mobile dashboard, atomic persistent queue, and a single sequential download worker. The PS5 target uses `libSceHttp`/`libSceSsl` with an embedded CA bundle, certificate verification, bounded streaming, manual redirects and HTTPS downgrade refusal. The user confirmed the sample HTTPS download completed on the PS5. Files are written to `/data/homebrew`, preflight known sizes with a 16 MiB reserve, and publish only after `fsync` and successful completion.

The dashboard now asks for confirmation before adding links, shows URL-derived filenames and the destination, and provides pause/resume, retry and remove controls. Pause is cooperative at HTTP read callback boundaries. Retry starts from byte zero and removes only the corresponding regular `.part` file. Current/average speed and ETA are calculated from transfer callbacks. Queue format RDQ5 persists extraction choice, filename and destination. An optional extraction password stays in process memory and is neither serialized nor returned by the API.

Archive extraction is still not implemented: WFM helper has not been built or integrated. When extraction is selected for a downloaded archive, its queue state becomes `ready_to_extract` and the UI reports that helper integration is pending. No extraction is performed. Names currently come from the URL path; `Content-Disposition` and final redirect names are not yet read. Range resume, segmented transfers, Real-Debrid, destination selection and storage wait/recovery remain open.

Current verification: `make host-test`, `make host-server`, and the PS5 cross-build pass. Host network transfer and new command behavior remain to be validated, including on actual PS5 hardware.

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
