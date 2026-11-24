# Changelog

## Unreleased — 1.1.0 development

- Create `/data/homebrew/downloads` with mode 755; report queue readiness separately from download-folder readiness.
- Save new downloads in `/data/homebrew/downloads`; extract archives and copy `.pkg` files into `/data/homebrew`.
- Preserve archive-provided folder structure during extraction.
- Copy extracted `.pkg` files to the destination root without removing their extracted originals or overwriting existing root files.
- Clarify that an already-running WFM helper in Payload Manager can be reused; update dashboard retry guidance.
- Keep the downloaded archive and extracted members after successful extraction. The helper cleans only its own incomplete temporary outputs.

## v1.0.0 — first major MVP release

This release checkpoint covers the user-tested PS5 downloader flow.

- Direct HTTP/HTTPS downloads to `/data/homebrew` with streaming writes.
- Persistent queue and mobile-first dashboard with pause, resume, retry, confirmed stop/removal, progress, speed and ETA.
- Optional WFM 7-Zip helper integration for archive extraction and extraction progress.
- User confirmed an HTTPS RAR downloaded and extracted correctly on the PS5.
- Archive passwords are entered visibly in the review dialog and remembered in the browser; the backend keeps them only in memory.

Known limits: one sequential download worker; interrupted downloads restart from the beginning on retry; no Real-Debrid integration, range resume, segmented downloads, destination selector or storage recovery workflow. Multipart and password-protected archive behavior, payload restart recovery and large downloads have not been validated on-console. See [`IMPLEMENTATION.md`](IMPLEMENTATION.md) for current details.
