# Changelog

## Beta — 1.1.2-beta.8

- Recreate a missing download destination immediately before each transfer, validate it as a directory, and restore mode 0755.
- Log successful destination recreation; report setup failures with the filesystem errno.

## Beta — 1.1.2-beta.7

- Keep `.elf` in the process title so Payload Manager recognizes RD Downloader as a payload and displays its payload icon.

## Beta — 1.1.2-beta.6

- Set the running process title to `rd-downloader` at startup so Payload Manager's Active Processes view shows the project name regardless of launch method.
- Continue publishing the version badge through the Payload Manager catalog metadata.

## Beta — 1.1.2-beta.5

- Replace the PS5 `access(W_OK)` destination check with an actual temporary-file write probe, avoiding false permission failures when real and effective UIDs differ.
- Record real/effective UID details at startup and on write-probe failures.

## Beta — 1.1.2-beta.4

- Log backend errors and key lifecycle events to `/data/homebrew/downloads/rd-downloader.log`; URLs and archive passwords are excluded.
- Record queue persistence, API error responses, request read/write failures, download errors, extraction/helper errors and startup failures.
- Include filesystem mode, owner IDs and errno details for download destination failures.
- Make `make deploy` return after elfldr fetches the ELF and disable the stdio pipe that kept `socat` waiting.
- Publish a stable Payload Manager catalog ID and explicit version/update date for the version badge and update checks.

## Beta — 1.1.2-beta.3

- Write timestamped service and download diagnostics to `/data/homebrew/downloads/rd-downloader.log`.
- Record download failures with item ID, filename, destination, and filesystem error details; URLs and archive passwords are never logged.
- Include destination mode and owner IDs in write-check failures to diagnose the reported PS5 permission issue.

## Beta — 1.1.2-beta.2

- Create `/data/homebrew/downloads` with mode 755; report queue readiness separately from download-folder readiness.
- Save new downloads in `/data/homebrew/downloads`; extract archives and copy `.pkg` files into `/data/homebrew`.
- Preserve archive-provided folder structure during extraction.
- Copy extracted `.pkg` files to the destination root without removing their extracted originals or overwriting existing root files.
- Clarify that an already-running WFM helper in Payload Manager can be reused; update dashboard retry guidance.
- Keep the downloaded archive and extracted members after successful extraction. The helper cleans only its own incomplete temporary outputs.
- Add an optional setting to extract compressed files found inside downloaded archives. Nested archives are moved to `/data/homebrew/downloads`; extracted content stays in `/data/homebrew`.
- Persist related nested archive names in queue format RDQ7 and offer explicit archive cleanup on queue removal. The cleanup never removes files published under `/data/homebrew`.
- Retry transient network and HTTP 408/425/429/5xx download failures up to five times with exponential delays of 5, 10, 20, 40 and 80 seconds.
- Handle HTTP clients concurrently with a bounded eight-client limit and a 15-second request read deadline.
- Preserve the original downloaded file's ownership marker until queue removal so explicit deletion can verify ownership.

## v1.0.0 — first major MVP release

This release checkpoint covers the user-tested PS5 downloader flow.

- Direct HTTP/HTTPS downloads to `/data/homebrew` with streaming writes.
- Persistent queue and mobile-first dashboard with pause, resume, retry, confirmed stop/removal, progress, speed and ETA.
- Optional WFM 7-Zip helper integration for archive extraction and extraction progress.
- User confirmed an HTTPS RAR downloaded and extracted correctly on the PS5.
- Archive passwords are entered visibly in the review dialog and remembered in the browser; the backend keeps them only in memory.

Known limits: one sequential download worker; interrupted downloads restart from the beginning on retry; no Real-Debrid integration, range resume, segmented downloads, destination selector or storage recovery workflow. Multipart and password-protected archive behavior, payload restart recovery and large downloads have not been validated on-console. See [`IMPLEMENTATION.md`](IMPLEMENTATION.md) for current details.
