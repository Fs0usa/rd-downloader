# Engineering research and audit

Audit date: 2026-11-23. The workspace contains the `rd-downloader-v2` ZIP contents, but not upstream checkouts. GitHub is not resolvable from this environment (`git ls-remote` fails with `Could not resolve host: github.com`). Web-accessible upstream documentation was consulted; source-level verification that requires cloning/building those repositories remains outstanding. No PS5 console or SDK is available here.

> Historical audit snapshot: the sections below record the initial bootstrap state. The implementation update at the end records what has since been built and tested in this workspace.

## Project inventory

The bootstrap contains five C implementation files, four headers, three small host test programs and a Makefile. The only application entry point is a `main()` that prints a smoke message. There is no HTTP server/client, TLS, persistent queue, worker, storage discovery, Real-Debrid client, archive IPC, UI, or ELF artifact. The C primitives are host-testable; their presence does not establish downloader functionality.

### Existing primitive behavior and gaps

- `rd_plan_ranges` builds inclusive, contiguous byte ranges. It avoids total-plus-divisor overflow in the range-count calculation. The public return type is `int`, while `max_connections` is unsigned; an extreme caller-supplied connection count can exceed `INT_MAX` before the cast. Production callers must clamp this input.
- `rd_parse_url_batch` trims whitespace and accepts only lowercase-insensitive HTTP(S) schemes with a nonempty authority substring. It is a lightweight syntax gate, not a complete URL parser; it does not fully validate host/port syntax, percent encoding, userinfo policy, or URL control characters.
- `rd_plan_space` adds archive size, known expanded size when extracting, and a safety reserve. It does not discover filesystems or estimate staging/cross-filesystem copy costs. Unknown expanded size is flagged for runtime checks.
- `rd_classify_archive_name` classifies some suffixes only. It recognizes `.partN.rar` and `.001`, but does not recognize legacy `.r00` volumes; its `.rar`-ending numeric-name branch is not equivalent to `.r00`. Classification is filename-only and not archive validation.
- `rd_job_state_name` supplies display strings for enum values, but there is no state machine or persistence layer.

## SDK and ELF build

The project Makefile expects `PS5_PAYLOAD_SDK/toolchain/prospero.mk`. The project-local installer has now installed SDK release `v0.50` at `.deps/ps5-payload-sdk`; its recorded archive SHA-256 is `2993a69e52fea04a32bf1753318fc60b3f918ab635bdf946870e7d1849557bb2`. Host tools present include `/usr/bin/gcc`, `/usr/bin/make`, `/usr/bin/clang-18`, `/usr/bin/lld-18`, and `/usr/bin/llvm-config-18`.

The `ps5-payload-dev/sdk` README describes a payload SDK intended for payload ELFs loaded through supported exploit-chain ELF loaders. It documents Debian prerequisites including Clang 18 and LLD 18, the SDK's `toolchain/prospero.mk`, and its hello-world sample/build/deploy flow. The README's sample uses `main()`. The sibling `websrv` repository's PS5 Makefile links PS5 system libraries and `libmicrohttpd`, `microdns`, and `libsmb2` through SDK-aware pkg-config; it is a possible server implementation reference, but its dependencies and whole server should not be imported without reviewing source, license, runtime behavior and API fit.

`PS5Dev/PS5SDK` is a separate work-in-progress SDK project. Its README describes a different CMake/Ninja toolchain and `payload_main(struct payload_args *)` entrypoint, and explicitly says it cannot yet build full proper applications. Do not mix this project's entrypoint/build assumptions with `ps5-payload-dev/sdk` without selecting and pinning a target SDK.

The current default Makefile has an explicit opt-in host test goal, which avoids loading the SDK Makefile for `make host-test`. With SDK v0.50, a forced target rebuild (`make -B` with `PS5_PAYLOAD_SDK`, `LLVM_CONFIG`, and LLVM 18 on `PATH`) succeeded and produced an x86-64 PIE ELF. It has not been loaded on a PS5, so runtime compatibility remains unverified.

## HTTP server reference

`ps5-payload-dev/websrv` documents a PS5 payload web server on port 8080, browser access from a phone/PC, and its `Makefile.ps5` links against `libmicrohttpd` plus PS5 system libraries. Its README describes filesystem browsing and homebrew launching. The project has a GPL-3.0 license. These facts make it a relevant server reference, not proof that it offers safe routes, CSRF protection, a suitable embedded API, or that it can be reused without a source and license review. Inspect handlers and lifecycle directly before adopting it.

## Payload loading, process replacement and development loop

The current `ps5-payload-dev/elfldr` README says each incoming payload runs in an individual process; sending a new ELF does not replace an older instance. Its source sets the process name from `argv[0]`. For the raw ELF protocol, `socksrv.c` starts with the fallback name `payload.elf`; the raw `prospero-deploy` path therefore does not preserve the local filename as the process name. A second server instance will run as a separate process and fail to bind the already-used port rather than replacing the first.

Payload Manager's README describes a Processes view with termination for eligible payloads. Its source filters user daemon processes to app ID zero and `.elf` names (excluding `mini-syscore.elf`), and its `process_kill()` sends SIGTERM, waits up to roughly 1.5 seconds, then escalates to SIGKILL. The SDK header declares `setproctitle()`, but SDK v0.50 does not export the symbol, so it cannot be used by this payload. The raw ELF sender path names the process `payload.elf`; verify it in Payload Manager's process list and confirm port 8090 is released. The practical development loop is: stop only the old RD Downloader payload process in Payload Manager, then send the newly built ELF. A console reboot is not part of the normal rebuild loop.

Current server state is in memory only, so stopping it with Payload Manager loses no download state because downloads have not been implemented. Before persistent jobs or transfers are introduced, add safe SIGTERM handling and checkpoint behavior; force-killing during active writes must not be the normal application shutdown path.

Community reports are mixed and firmware-specific. One Reddit thread describes using Payload Manager's “kill all payloads” operation, while another reports an older Payload Manager process and its port remaining after an attempted update. Treat forum posts as operational anecdotes; verify that the named process exits and port 8090 is released on the target before resending. See [the rest-mode discussion](https://www.reddit.com/r/PS5_Jailbreak/comments/1vdjuy5/rest_mode_on_1260_y2jb_kstuff_lite_does_killing/) and [the Payload Manager update thread](https://www.reddit.com/r/PS5_Jailbreak/comments/1u358rq/ps5payloadmanager_how_to_update_from_ver_011_to/).

## WFM 7-Zip Helper

The WFM helper README documents a separate long-running PS5 ELF, a Unix stream socket at `/system_tmp/wfm-7zip-helper.sock`, and a 20-byte big-endian `W7HP` frame prefix with a stated 1 MiB payload cap. It documents `PING/PONG`, `EXTRACT`, `CANCEL`, `LIST_TASKS`, `ATTACH_TASK`, and `ACK_TASK`, including task reattachment after a client disconnect. It says disconnect detaches a client while extraction continues; it does not pause extraction. The README describes RAR/RAR5 via UnRAR and other formats via 7-Zip handlers, archive path protections, progress, and cancellation. See the helper's [README](https://github.com/owendswang/wfm-7zip-helper/blob/main/CPP/7zip/Bundles/WfmHelper/README.md).

These are upstream documentation claims, not a local build or source audit. Before integration or redistribution, obtain a pinned checkout and inspect actual source, build scripts, all license notices, symlink/path behavior, protocol constants, crash recovery and PS5 compile constraints. The helper README reports mixed third-party licensing and an UnRAR-specific license; the exact distributable obligations remain unverified. Its current documented protocol has no pause/resume command, and it says tasks do not survive helper/console restart. Thus restart reconciliation and true extraction pause/resume are unresolved engineering requirements.

## Download-engine reference

DebridDownloader's README describes a Rust/Tauri desktop application with an engine actor, segmented `Range` transfers, a `.part` file, checkpoint persistence in `downloads.json`, retry/resume, refresh of expired provider links, and OS credential stores. The README identifies `src-tauri/src/engine/` as the engine. This architecture is useful as a study guide, but it is not PS5-compatible evidence: its source has not been cloned or inspected here, and its desktop dependencies/runtime cannot be presumed portable. Before adapting algorithms, review actual engine code and tests at a pinned commit and keep only protocol-independent behavior such as validation of range responses, atomic checkpoints, validators and bounded retries. See the [repository](https://github.com/CasaVargas/DebridDownloader).

## Real-Debrid API

The official documentation gives the REST base URL `https://api.real-debrid.com/rest/1.0/`, documents `GET /user` and authenticated `POST /unrestrict/link`, and states a 250 requests/minute limit with HTTP 429 for refused requests. `POST /unrestrict/link` accepts `link`, optional hoster-side `password`, and `remote`. It documents 401/403 errors. The exact response fields must be read from the current schema at implementation time; do not assume response properties from examples elsewhere. See the [official API documentation](https://api.real-debrid.com/).

## Dependency and platform compatibility

No dependency has been built against the project's target SDK. `libmicrohttpd` is used by upstream websrv according to its makefile, but is not listed in this project and no SDK package availability was checked locally. TLS, HTTP client, JSON parser, persistence format, compression, and archive IPC dependencies have not been selected or validated. Host tests only prove the four primitive translation units compile with GCC under the current host flags.

## Unresolved risks / evidence still needed

1. Record the installed SDK release/hash (v0.50 above) and confirm Debian 13 toolchain compatibility beyond this successful compile.
2. Build the minimal ELF using the SDK and load it on the user's PS5; host ELF linking is not console validation.
3. Clone and inspect `ps5-payload-dev/sdk`, `websrv`, `PS5Dev/PS5SDK`, WFM helper, and DebridDownloader source/builds at recorded commits.
4. Verify the target's supported HTTP/TLS APIs and certificate trust configuration; TLS verification must remain enabled.
5. Verify SDK availability/compatibility for a small HTTP client, JSON parser, atomically durable storage, and thread/synchronization primitives.
6. Verify destination mounts, filesystem semantics, free-space reporting, write access and atomic rename on actual target hardware.
7. Verify helper behavior and protocol against a pinned helper build, including password and multipart paths, output collision handling, and no-space recovery.
8. Decide licensing/redistribution obligations before bundling SDK-linked payloads or helper binaries.

## Transport evaluation update

The locally installed SDK v0.50 contains `samples/http2_get/main.c`, which manually declares a small set of `sceNet*`, `sceSsl*` and `sceHttp2*` functions, initializes them, and performs a streaming `sceHttp2ReadData()` loop. That sample request uses plain HTTP. The SDK target includes `libSceHttp2.so` and `libSceSsl.so` stubs, and the symbol list contains request/header/status/read operations, but the SDK sample does not establish a safe HTTPS certificate-verification configuration or validate large downloads.

As an additional implementation reference, the current [ItsBlurf/ps5-downloader PS5 build notes](https://github.com/ItsBlurf/ps5-downloader/blob/main/docs/PS5_BUILD.md) report that a native POSIX-socket HTTP path was tested on a real PS5, while its earlier `SceHttp2` HTTPS path failed with `0x8095f00c` and a large-transfer test was followed by the console becoming unreachable. These are that project's reported observations, not independently reproduced here. Given the user's safety requirement, RD Downloader does not use `SceHttp2` for its first transfer implementation.

The [owendswang/ps5-aria2 PS5 build guide](https://github.com/owendswang/ps5-aria2/blob/master/ps5/README.md) describes building aria2 with static OpenSSL, c-ares, SDK SQLite3 and libssh2. It says HTTPS certificate checks are disabled by default and must be explicitly enabled with a CA bundle. It is a possible future backend candidate, but it adds a large GPL-2.0-or-later dependency and has not been built or tested in this workspace.

### Current transport implementation

The project now has a bounded sequential HTTP/1.x socket downloader in `src/core/http_client.c`, a small URL/response parser, an explicit-start pthread worker, persistent queue states, and a mobile queue page. It supports plain HTTP only. It streams to a `.part` file, preserves partial data on errors/restart, checks known content length against available space plus 16 MiB, and atomically renames a complete file. HTTPS, range requests, automatic resume, retries, pause/cancel, `Content-Disposition`, and runtime PS5 validation remain open. Host parser and queue tests pass; the target ELF cross-build passes. The latest worker build has not yet been loaded on the console.

## HTTPS transport implementation update (2026-11-23)

The SDK v0.50 installed in this workspace provides import stubs for `libSceHttp.sprx` and `libSceSsl.sprx`. For a source-level PS5 reference, ShadowMountPlus 1.7's `src/sm_ampr_updater.c` declares and uses the native APIs in a blocking HTTP/1.1 path: `sceNetInit`, network pool creation, `sceSslInit`, `sceSslLoadCert`, `sceHttpInit`, `sceHttpCreateTemplate`, `sceHttpSetAutoRedirect`, timeout configuration, URL connection/request creation, `sceHttpSendRequest`, `sceHttpReadData`, and orderly teardown. Its code loads public CA certs before HTTPS requests and streams bounded blocks to a temporary file. This is an independently maintained PS5 payload implementation, not a Sony SDK sample. [Source file](https://github.com/drakmor/ShadowMountPlus/blob/1.7/src/sm_ampr_updater.c).

The project now compiles the PS5 transport in `src/core/ps5_http_client.c`, links `-lSceHttp -lSceSsl`, embeds the Debian `ca-certificates` PEM bundle, enables the server/hostname/known-CA/SNI TLS option mask (`0xA5`), and manually follows at most five redirects. HTTPS-to-HTTP redirects are refused. Host builds keep the existing POSIX HTTP implementation and do not exercise the PS5 TLS code. The CA bundle was generated from `/etc/ssl/certs/ca-certificates.crt` (148 PEM certificates in this build) by `scripts/embed-ca-bundle.py`; regenerate using the installed Debian `ca-certificates` package when roots need updating. Mozilla CA data is distributed under MPL-2.0; preserve this notice and source provenance.

The `0xA5` option interpretation is based on reverse-engineered PS4/HTTP behavior documentation (server validation, common-name validation, known CA, and SNI), not an official PS5 public header. The source reference's use of `sceSslLoadCert` is direct evidence for API sequencing, but this exact payload's TLS initialization, roots, redirects, ABI calls, and large transfer have only been cross-compiled here. They are **not yet verified on a PS5**. Host tests cannot validate PS5 TLS, and the current execution environment blocks local sockets, so end-to-end HTTP transfer simulation was not possible here. [HTTP option reference](https://yharnam-hunters.github.io/byrgenwerth-site/runtime-reference/).

The prior user report that the payload stopped when a download started also matched a 64 KiB default pthread stack in PS5-focused community code. The project moved large queue copies to the heap and now requests a 1 MiB stack for the persistent worker. This is a process-level worker setting; it makes no kernel or firmware changes. The corrected stack size and the HTTPS path both still require on-console validation.

### On-console HTTPS checkpoint

On 2026-11-23, the user confirmed the sample HTTPS RAR downloaded successfully on their PS5 with build `0.3.0-https-dev`. This validates one real HTTPS transfer and the TLS/redirect path for that URL on the user's console. It does not establish coverage for every CA chain, redirect host, interrupted transfer, or large file. The tested source is checkpointed in Git as `working-https-v0.3.0`.
