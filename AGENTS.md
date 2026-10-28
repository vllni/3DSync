# Agent context for 3DSync

> **Keep this file current.** It is the shared context for every agent session in
> this repo. When a change makes something here wrong or incomplete — a module
> gains a capability, a convention changes, a build flag or file moves — update
> the affected section in the *same* commit as the code change. A stale note here
> costs a later session more time than writing it did.

## What this repo is

Nintendo 3DS/2DS homebrew app (`.cia` / `.3dsx`) that syncs files between the SD card and a remote: Google Drive, Dropbox, an SMB2/3 file server, FTP/FTPS or WebDAV. Written in C++11, built with devkitPro/devkitARM.

Companion static web configurator at `docs/` (GitHub Pages at `https://3dsync.vllni.dev/`) generates the INI config file via OAuth and a stepper UI.

---

## Repo layout

```
source/             C++ application source
  main.cpp          Entry point, INI parsing, provider construction, 3DS UI
  version.h         APP_VERSION — the only place the version is written down
  sync/
    syncengine.cpp/h  The decision table: what to upload, download, prompt
                    about or skip.  Free of 3DS APIs — see "Tests".
  modules/
    syncprovider.h  SyncProvider interface + RemoteFileInfo — the sync engine's
                    only view of a remote.  Add a backend by implementing it.
    googledrive.cpp/h  Google Drive: OAuth refresh, MD5 tags, flat legacy upload
    smbremote.cpp/h    SMB2/3 via libsmb2 (NAS, Windows share)
    ftpremote.cpp/h    FTP/FTPS via libcurl
    webdavremote.cpp/h WebDAV via libcurl (PROPFIND/MKCOL/PUT/MOVE)
    dropbox.cpp/h   Dropbox: content_hash tags, PKCE refresh-token auth
    remoteparse.cpp/h  Response parsing and URL building for the remotes,
                    kept free of libcurl/libsmb2 so it is directly testable
    manifest.cpp/h  Local JSON manifest tracking sync state
  utils/
    curl.cpp/h      libcurl wrapper (methods, upload/download streaming, FTP
                    options, wildcard listing, header capture)
    json.cpp/h      JSON member lookups, escaping, array splitting
    xmlutil.cpp/h   WebDAV multistatus reading
    urlutil.cpp/h   Percent-encoding
    pathutil.cpp/h  Remote path normalisation, in-flight transfer names
    timeutil.cpp/h  Server clock parsing (RFC 3339 and RFC 7231)
    fsutil.cpp/h    mkdirs, temp-file + atomic-replace helpers
    hash.cpp/h      MD5 and the Dropbox content hash
    debug.cpp/h     Runtime debug flag, debugf(), body dumps, secret redaction,
                    the captured session log
    console.h       CONSOLE_* colours, from <3ds.h> or defined for the host
  ui/               Both screens — see "On-device UI"
    frontend.cpp/h  3DS glue: framebuffers, stdout, input loop, sync thread,
                    prompts, WorkerSyncUi.  The only ui/ file with <3ds.h>.
    screens.cpp/h   Layouts, touch buttons (each mapped to a KEY_* bit), drawing
    logbuffer.cpp/h Scrollback: ANSI colours, word wrap, bounded history; and
                    LogScroll, the view position
    touchinput.cpp/h  Stylus -> key bits (tap on release, repeat, drag)
    canvas.cpp/h    Software RGB565 drawing, anti-aliased text, UTF-8
    font.h, fontdata.cpp  Glyph tables; fontdata.cpp is generated
  libs/inih/        INI parser (inih + INIReader)
tools/make_font.py  Generates source/ui/fontdata.cpp from Inter + JetBrains Mono
tests/              Host unit tests (see "Tests"), and preview.cpp
testinfra/          Compose stack of SMB, FTP and WebDAV servers to sync a
                    real 3DS against; its README has the settings per case
docs/               Configurator static site
  index.html        Three-step stepper UI (auth → paths → download)
  static/js/index.js  All UI logic (PKCE OAuth, path config, INI generation)
output/             Build artefacts (.cia, .3dsx, .zip) — do not edit
build/              Intermediate object files — do not edit
buildtools/         devkitPro build scaffolding (git submodule) — do not edit
.github/            CI workflows, CODEOWNERS, unistore.jq (Universal-Updater feed)
```

---

## Build

Requires devkitPro with 3DS support (`DEVKITPRO` environment variable set), plus **libsmb2**, which devkitPro does not package. The devcontainer image cross-compiles it into `$DEVKITPRO/portlibs/3ds` (see `docker/3dsync-devcontainer.Dockerfile`, pinned to a commit). To build it by hand:

```bash
cmake -S libsmb2 -B build -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake \
  -DCMAKE_INSTALL_PREFIX=$DEVKITPRO/portlibs/3ds -DBUILD_SHARED_LIBS=OFF \
  -DENABLE_EXAMPLES=OFF -DENABLE_LIBKRB5=OFF -DENABLE_GSSAPI=OFF -DENABLE_LIBDCERPC=OFF
cmake --build build && cmake --install build
```

Because the published CI image is only rebuilt on pushes to main, `.github/workflows/pr.yml` builds it locally when a PR touches `docker/*Dockerfile`.

```bash
make          # produces output/3ds-arm/3DSync.cia and .3dsx
```

The Makefile auto-discovers all `.cpp` files under `source/`. Adding a new `.cpp` file there is sufficient — no Makefile edits needed.

`make` needs the `buildtools` submodule checked out (`git submodule update --init`); without it the build stops at `buildtools/make_base: No such file or directory`. When a full build is not available, type-check a translation unit directly:

```bash
arm-none-eabi-g++ -fsyntax-only -std=gnu++11 -Wall \
  -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -D__3DS__ -DARM11 \
  -DINI_MAX_LINE=2048 \
  -I$DEVKITPRO/libctru/include -I$DEVKITPRO/portlibs/3ds/include \
  -I$DEVKITPRO/portlibs/armv6k/include source/main.cpp
```

`source/libs/inih/ini.c` and `ini.h` are byte-identical to [benhoyt/inih](https://github.com/benhoyt/inih) `577ae2d` (2026-01-30) and must stay that way, so the next resync is a two-file copy with no patch to re-apply. Compile-time configuration of them therefore belongs in `BUILD_FLAGS` in the Makefile: `INI_MAX_LINE=2048` is set there because upstream's 200 truncates a pasted `Token=`, which is an OAuth access token and can exceed a kilobyte. When bumping, re-check that `ini_parse()` and `ini_parse_string()` keep their signatures and that `INI_HANDLER_LINENO` still defaults to 0 — `INIReader::ValueHandler` is a 4-argument handler.

`source/libs/inih/INIReader/` is *not* upstream and cannot be resynced blindly: `GetValues()` is a local addition that `getConfiguredSyncPaths()` depends on, and upstream's `INIReader` exposes no equivalent.

inih is deliberately vendored rather than a submodule. Upstream's tree carries `examples/`, `tests/`, `fuzzing/` and `cpp/`, all of which the Makefile's `rwildcard` over `source/` would sweep into the build (several define `main`), and the forked `INIReader` could not be patched inside a pinned checkout.

### Version

`source/version.h` holds `APP_VERSION` and is the only place the version lives. The Makefile greps that one line for `VERSION_MAJOR/MINOR/MICRO` (the CIA and NRO metadata), `main.cpp` prints it on the lower screen where the commit hash used to be, and `Curl` sends it as the user agent. It replaced `-DVERSION_STRING` / `-DREVISION_STRING` from `git describe`, which made the displayed version a property of the checkout rather than of the source — a shallow clone had none, and `describe` did not necessarily name the release being built.

Keep the format `major.minor.micro`: the Makefile splits it on the dots and `makerom` wants three numbers, so a suffix like `-rc1` breaks the metadata.

**Releasing** is two steps in this order: bump `APP_VERSION`, commit; then tag that commit `v<version>`. The `Version Check` job in `release.yml` reads the header with the same `sed` expression and fails the whole release before anything is built if the tag and the header disagree (a leading `v` on the tag is optional). If it fires, bump the header and retag rather than deleting the release.

---

## Key C++ conventions

- **C++11**, no exceptions, no RTTI.
- Logic worth testing goes in a unit that does not include `<3ds.h>`, libcurl or libsmb2, and gets a test. The providers should stay thin: talk to the network, hand the response to a parser.
- All HTTP goes through the `Curl` wrapper (`source/utils/curl.h`). Never call libcurl directly.
- `Curl::perform()` returns 0 on network success. **HTTP status is not checked by `perform()`** — callers must call `getStatusCode()`. `CURLOPT_FAILONERROR` is intentionally disabled so error response bodies are captured.
- Every provider owns a `_performWithRetry()` that maps transport errors to *transient* (retry with back-off), *fatal* (`_fatalError`, stop the run) or *per-file* (log and skip). Never call `_curl.perform()` directly from a provider method — the retry helper is also where the server `Date` header is captured.
- libcurl options are sticky on a handle. Providers that issue more than one shape of request call `Curl::reset()` first (via their own `_prepare()`); otherwise `NOBODY`, `UPLOAD`, `WILDCARDMATCH` or a custom method leaks into the next call.
- A status a caller treats as a normal outcome must not be announced as an error. `Dropbox::_rpc()` / `_performWithRetry()` and `WebDavRemote::_performWithRetry()` take an `expectedStatus`: it is still returned and still explained through `debugf()`, but the red `Dropbox API error: HTTP nnn` line is suppressed. Suppression is never the whole story — the caller must then read `error_summary` and, when the expectation was wrong, print it via `_report409Reason()`.
- **Dropbox says far too much with 409.** "The folder is already there", "nothing there yet", "a file is in the way", "your Path= is malformed" and "no write permission" are all HTTP 409, separated only by `error_summary`. `dropboxPathState()` in `remoteparse.cpp` is the single place that reads them, returning a `DropboxPathState`; both `ensureRoot()` and `_createFolder()` switch on it, and `test_remoteparse.cpp` pins each real response body. Do not re-derive this inline — misreading it either fills the console with an error for the most ordinary thing a sync does, or syncs into a path that is not the configured one.
- `Dropbox::ensureRoot()` **probes with `get_metadata` before creating**, and only calls `create_folder_v2` when the probe says `path/not_found`. The folder exists on every run but the first, so this trades one write that 409s for one read that succeeds, at the cost of a second round trip on the first run; in exchange the steady state is read-only and "is this a folder" comes from a `.tag` field rather than the wording of an error. `_createFolder()` still tolerates `path/conflict/folder`, because a second console on the same account can win the race between the probe and the create.
- Fatal Drive errors (401 / unrecoverable 403) set `_fatalError = true` and cause all subsequent Drive calls to no-op. Check `hasFatalError()` in callers to break out of sync loops early.
- File downloads use a temp file (`localPath + ".3dstmp"`) + atomic rename, via `openTempFor()` / `replaceLocalFile()` in `utils/fsutil.h`. On FAT (3DS SD), `rename()` cannot overwrite an existing file — the existing file is first renamed to `.3dsbak`, then the temp is renamed in, then the backup is removed. Restore the backup on failure.
- Uploads are equally guarded: write to `<remote>.3dstmp`, then swap it into place (WebDAV `MOVE` with `Overwrite: T`, FTP `RNFR`/`RNTO` post-quote, SMB unlink-then-rename because libsmb2 sends `ReplaceIfExists=0`). Listings skip `.3dstmp` leftovers.
- `performSync()` in `main.cpp` is provider-neutral: it talks only to `SyncProvider`. It returns `bool` — `false` means cancellation was requested or a fatal error occurred, and the sync loop must stop. Its `uploadOnly` argument mirrors local → remote: never download, never prompt, leave remote-only files alone.
- Change detection compares the local mtime **and** size against the manifest, plus the remote's `tag`. `SyncProvider::localTag()` is the content-based fallback for stale mtimes and is only meaningful where the remote's tag is derivable from file contents (Drive's MD5); it returns "" elsewhere.
- Manifest keys are `SyncProvider::manifestPrefix() + "|" + localPath`. Changing a provider's prefix orphans every existing entry for that remote, which makes the next sync treat every file as unseen — so keep them stable.
- Conflict resolution in `performSync()` calls `SyncUi::resolveConflict()`, which on the 3DS is `frontend::WorkerSyncUi` and returns a `ConflictChoice`: `CONFLICT_KEEP_LOCAL` (A), `CONFLICT_KEEP_REMOTE` (B), `CONFLICT_SKIP` (X), `CONFLICT_CANCEL` (START). L toggles "for all" on the prompt, and the next answer is then reused for every later conflict of the run.
- The main menu (`frontend::waitForMenu()`) returns a `frontend::MenuChoice`: A runs a sync, Y runs the same sync in debug mode, START exits. `main()` calls `setDebugEnabled()` and `debugLogReset()` from that choice *before* `frontend::runWorker()`, because the providers read the flag when they construct their `Curl` handle. START also cancels a running sync, and the flag stays set only for that run.
- The manifest (`/3ds/3DSync/manifest.json`) is a hand-parsed JSON file. Use `Manifest::set/get/has/remove` — never write JSON by hand elsewhere.
- `svcSleepThread(nanoseconds)` is the 3DS sleep call (from `<3ds.h>`). Use it for rate-limit back-offs.
- `printf` output goes to the scrollable log on the top screen. Use `CONSOLE_RED` / `CONSOLE_RESET` (from `<3ds.h>`, or `utils/console.h`) for error messages; `LogBuffer` turns the SGR codes into colours.
- The log is **54 columns** (`LOG_COLUMNS`) and wraps at the last space, so a long line is never cut mid-word — but its continuation starts at column 0, which breaks up an indented hint and reads as a new message. So keep every line of console output **under 50 columns**: split a long message into several `printf`s, indenting the continuation lines like the first. Anything of unpredictable length (a path, a folder or host name) goes on a line of its own, indented, rather than inside a sentence it would push past the edge.
- **The summary must not claim success over a failure.** `SyncSummary` holds `failures` (transfers that failed, recorded by `recordUpload()`/`recordDownload()`) and `problems` (an entry not resolved or listed, a remote not reached, a run stopped by a fatal error). `summaryLines()` and `summaryResult()` in `syncengine.cpp` build the report and are tested; with either list non-empty the run ends with "Sync finished with errors." and a red result on the bottom screen. A new way for a file or an entry to be skipped must add to one of them.
- Keep sync output quiet: print per-entry headers, the experimental notice, errors, interactive prompts and the closing `--- Sync Summary ---`. Do **not** add a line per file examined or per file transferred; the summary already lists every changed file, and per-file chatter scrolls the interesting parts off-screen. Verbose output belongs behind `debugf()` — see below.
- **Every swallowed error gets a `debugf()`.** `utils/debug.h` is a runtime flag (Y on the main menu, not a build define) plus `debugf()`, `debugBody()`, `debugFileBody()` and `debugErrno()`. The rule when writing code that absorbs a failure — a 404 read as "empty folder", a per-file error that is skipped, an ignored return value, a `stat()` that is not checked — is to say so through `debugf()` in the same place. A normal run must print exactly what it printed before; a debug run must leave nothing unexplained.
  - `Curl::perform()` already dumps the status, URL, timing and the body of every failed or non-2xx request. Providers add only what it cannot know: which file, which remote path, what the status *meant* there. Do not dump the same body twice.
  - A body streamed to a temp file is not in `getResponse()` — a failed download writes the API error into the temp file, so call `debugFileBody()` before removing it.
  - `debugRedact()` masks bearer headers and the OAuth token/secret fields, and every dump goes through it. Debug output is what ends up in a bug report, and once opened the credentials are plaintext in memory: when adding a dump of anything new that could carry a secret, extend `debugRedact()` and its tests rather than filtering at the call site.
  - `debug.cpp` is free of `<3ds.h>`, libcurl and libsmb2, so the sync engine and the host tests can both log.
  - **The session log.** While debug mode is on, every byte written to stdout is also kept in `debug.cpp` (`debugLogAppend()`), and the end of `runSync()` offers to write it to `/3ds/3DSync/logs/debug-<stamp>.log`. The capture is fed by the stdout devoptab `frontend::init()` installs (`logWrite()` in `ui/frontend.cpp`), the same one that feeds the on-screen log — so a plain `printf` lands in both with no extra call, and nothing has to be routed through `debugf()` to be reportable. Nothing may call `consoleInit()` any more: it would put libctru's console devoptab back over it. Only the first 96 KB and the last 48 KB are kept, and what fell out in between is counted and stated in the file. `debugLogSave()` strips ANSI sequences and runs `debugRedact()` over the whole text — a token can straddle two console writes, so redacting per write would miss it.
- `Curl::setReadData(FILE *, size)` streams a request body from a file. Pass the real byte count: without it libcurl sends `Transfer-Encoding: chunked`, which the Dropbox content endpoints reject.

---

## Tests

```bash
make -C tests            # build and run
make -C tests F=engine   # only tests whose "suite.name" contains "engine"
```

They build for the **host**, not the 3DS, and run in the `Unit Tests` job of `pr.yml`. Nothing here needs hardware or an emulator, which is only possible because the logic under test is kept free of libctru, libcurl and libsmb2 — keep it that way when adding code:

- `sync/syncengine.cpp` talks to `SyncProvider` and `SyncUi` and nothing else. Cancellation and the conflict prompt arrive through `SyncUi` precisely so the decision table can be driven by a scripted stand-in; do not reach for `hidKeysDown()` or `aptMainLoop()` in there.
- `ui/canvas`, `ui/logbuffer`, `ui/screens` and `ui/touchinput` are host units too: `test_ui.cpp` covers the scrollback (colours, split escapes, wrap, trimming, the view holding still while scrolled back), the tap/repeat/drag rules, and that every screen's buttons are on screen, apart, and wide enough for their label. `make -C tests preview` renders every screen to `tests/build/preview/*.png` — look at those after any layout change, there is no other way to see one without a console.
- Response parsing lives in `modules/remoteparse.cpp`, so a provider's `list()` is a request plus a call into a testable function. New protocol parsing belongs there, not inline in the provider.
- `utils/console.h` supplies the `CONSOLE_*` macros off-console, so host-portable code must include it instead of `<3ds.h>`.
- `utils/debug.cpp` is in the host `UNITS` because the engine, the manifest and `fsutil` all log through it. `test_debug.cpp` covers `debugRedact()`, which is the one part of debug output that has to be right: everything else is only printed, but a redaction that misses puts a token on screen. The `debuglog` suite covers the same risk one step further out — the capture must stay empty outside debug mode, stay bounded on a long run, and reach the SD card with the colour codes gone and the tokens masked.
- The hash tests need mbedtls on the host (`libmbedtls-dev`). Without it the suite still builds and runs, and the runner prints what was left out — deliberately loud rather than a silent pass.

The framework is `tests/framework.h`, about eighty lines: the build image has no gtest and vendoring one for this is more dependency than it is worth. `TEST(suite, name)` registers a case; `CHECK`/`CHECK_EQ`/`CHECK_STR_EQ` record a failure and keep going, so one broken case reports every mismatch rather than only the first.

**Mocking.** `tests/mocks.h` has `MockRemote` (a `SyncProvider` holding its files in memory, with scripted failures and recorded calls), `MockUi` (scripted conflict answers and cancellation), and `TempDir` (a real scratch directory for the local side). Mocking at `SyncProvider` is deliberate: it is the one seam every backend shares, so those tests cover the engine for all five remotes at once. An HTTP-level mock could not — SMB does not speak HTTP. Per-remote behaviour is covered instead by feeding recorded server responses through `remoteparse`.

What is *not* covered, and would need hardware or a much larger seam: the request-building paths inside each provider (libcurl option wiring, libsmb2 calls), retry and back-off behaviour, and the OAuth flows. A mockable transport interface would reach the first of those; it was left out because it means rewriting request code in four providers that have not yet been validated against real servers.

---

## On-device UI (`source/ui/`)

Top screen: the log. Bottom screen: a header, a card saying what is happening, the actions open right now as touch buttons, and a scroll strip for the log. Everything is drawn in software into RGB565 canvases and copied to the framebuffers (`gfxInit(GSP_RGB565_OES, …)`); there is no citro2d/3d and no libctru console.

- **Two threads.** `frontend::runWorker()` runs the sync on a worker thread one priority below the main thread, which stays in `frame()`: `aptMainLoop`, HID, touch, scrolling, redraw, `gspWaitForVBlank`. That is what lets the log scroll and START answer while libcurl or libsmb2 is blocked in a request. Consequences:
  - Code reached from `runSync()` — the engine, every provider — must **never** call `hidScanInput()`/`hidKeysDown()`, `aptMainLoop()` or any `gfx*`. Only the UI thread may scan input. Cancellation comes from `SyncUi::cancelRequested()`, which is why `legacyUpload()` takes a `SyncUi &`.
  - A question for the user is a prompt: the worker sets the mode and sleeps on a `LightEvent` (`ask()`), and the UI thread answers it from a key. Add a new one as a `ScreenMode` + card + buttons in `screens.cpp`, a case in `handleRunKeys()`, and a worker-side function like `askSaveDebugLog()`.
  - Shared state is `g_shared` under `g_stateLock`; the log is under `g_logLock`. Do not `printf` while holding either — stdout takes `g_logLock`.
  - If the system closes the app mid-run, the cancel flag is set, an open prompt is answered "stop", and the worker is joined, so the manifest is still saved.
- **Touch buttons are keys.** Each `Button` carries a `KEY_*` bit (`ui::K_*`, `static_assert`ed equal in `frontend.cpp`). `TouchInput` turns the stylus into those bits — tap fires on release over the same button, the ▲/▼ arrows fire on contact and repeat — and `frame()` ORs them into `hidKeysDown()`, so one handler serves both. Keep that: a touch-only action would be unreachable for someone using the buttons, and the label must keep naming the key.
- **The log scrolls in every mode**: D-pad ↑/↓ a row (repeating via `hidKeysDownRepeat()`), ←/→ a page, Circle Pad, the strip. `frame()` consumes the D-pad, so no prompt may use it. `LogScroll` follows the end until the user scrolls up, then holds still, and follows again once back at the bottom. The scrollback keeps 3000 rows across runs.
- **Redraws are on change only**: the top when the log revision or the view moves, the bottom when `signature()` of the model differs. A new `BottomModel` field that affects drawing must go into `signature()`, or the screen will not update.
- **Fonts** are pre-rendered 4-bit glyph tables (ASCII, Latin-1, `– — ‘ ’ “ ” • …`). Anything else draws as `?`. To change sizes or coverage, edit `tools/make_font.py` and regenerate `fontdata.cpp` (Inter 4.1 and JetBrains Mono 2.304, both OFL; the script says where to get them). The log font must stay monospaced at `LOG_ADVANCE`; a test checks it.
- `gpsp-3d` (sibling checkout, GPL-2) was the visual reference. None of its code is copied — its `ui.c` and font script are someone else's and GPL, and this repo is MIT — so keep it that way.

---

## Remotes

Everything the engine needs from a backend is in `SyncProvider` (`source/modules/syncprovider.h`): `connect`, `ensureRoot`, `list`, `download`, `upload`, `hasFatalError`, and the optional `localTag` / `serverTime` / `legacyUpload` / `isExperimental`. A new protocol means one new module and three lines in `runSync()`; `performSync()` should not need to change.

`isExperimental()` **defaults to true**, so a newly added backend warns until someone deliberately marks it stable. `GoogleDrive` and `Dropbox` override it to false. When the flag is set, `syncProvider()` prints a notice on the 3DS naming the backend and the issue-tracker URL (`ISSUES_URL` in `main.cpp`) before the first transfer.

Flipping a backend to stable is four edits, and all four should move together: the `isExperimental()` override, the status table at the top of `README.md`, the section note under its INI example there, and the badge on `docs/index.html` (plus `docs/configurator.html` for a backend the configurator can write).

Constraints that shaped the current set — worth knowing before proposing a fourth:

- **No SFTP.** devkitPro's libcurl 8.4.0 is built without libssh2 (`libcurl_la-libssh2.o` is empty; `curl-config --protocols` lists no SCP/SFTP). FTPS is the encrypted option.
- **curl's SMB is useless for sync.** It is SMB1-only (`NT LM 0.12`) and cannot list a directory, hence libsmb2 instead.
- **libsmb2's sync API blocks on `poll()`**, which libctru provides (`libctru/include/poll.h`). Include `smb2/smb2.h` *before* `smb2/libsmb2.h` — the latter uses macros the former defines.
- **FTP `TLS=try` falls back further than libcurl does.** `CURLUSESSL_TRY` drops to plaintext only when `AUTH` itself is refused; stock vsftpd accepts `AUTH TLS` and then refuses an *anonymous* login inside it (530 → `CURLE_LOGIN_DENIED`). `FtpRemote::_performWithRetry()` then reconnects once without TLS and keeps `_tlsDropped` for the run. The decision is `ftpShouldRetryWithoutTls()` in `remoteparse.cpp`, and it must stay limited to anonymous logins (`anonymous`/`ftp`): retrying a named user in plaintext would send the real password unencrypted after the server refused it over TLS. It is called with the *effective* mode, which is `FTP_TLS_NONE` once dropped — that is what makes a second fallback impossible.
- **libcurl reports a refused FTP `CWD` as `CURLE_REMOTE_ACCESS_DENIED`** (9), never as `CURLE_REMOTE_FILE_NOT_FOUND` (78), which is only for a missing file. Listing a directory starts with a `CWD` into it, and FTP answers 550 alike for a folder that does not exist and one the account may not enter — so on every first sync, and for a `Path=` not created yet, "access denied" means "nothing there yet". `FtpRemote::_performWithRetry()` takes an `FtpRequest` and lets `ftpRefusalOutcome()` in `remoteparse.cpp` decide: a listing reads it as empty, an upload (whose `MKD` for `CURLOPT_FTP_CREATE_MISSING_DIRS` fails with the same code) fails that one file with a red explanation once per folder, and only a refused folder of a file just listed stops the run. FTP creates directories at upload only, so do not make a listing refusal fatal again.
- **WebDAV `MKCOL` on an existing collection is 405, and that is expected.** RFC 4918 requires it and Apache sends it; `ensureRoot()` and every `upload()` (for the file's parent) issue `MKCOL` per level, so it is the answer on every sync after the first. `_mkcol()` passes `WEBDAV_MKCOL_EXISTS_STATUS` as `expectedStatus` and reads the result through `webdavMkcolResult()` in `remoteparse.cpp`; any other refusal (409 for a missing parent, 403, 507) is still reported, and 401/403 are still fatal.
- **`curl_fileinfo::time` is documented "always zero"**, so FTP timestamps come from a per-file `MDTM`/`SIZE` request after the wildcard listing. That is one extra round trip per file; do not "optimise" it away by trusting the listing's time.
- **Only Drive and Dropbox offer a content hash** (MD5 and `content_hash`), so only they can implement `localTag()`. SMB, FTP and WebDAV tag with size + mtime (or an ETag), which makes a modification preserving both invisible to them.
- Credentials for SMB/FTP/WebDAV arrive in plaintext in `3DSync.ini`, as the INI is the only configuration channel, and are sealed on first start (see "Sealed credentials"). Do not log them.

---

## Sealed credentials (`utils/secrets`, `utils/configseal`)

`loadConfig()` in `main.cpp` runs before anything reads the INI: it finishes an interrupted seal, replaces each plain credential with `enc:v1:<base64 nonce|ciphertext|tag>`, then opens the sealed values back into the `INIReader` with `Set()` (a local addition, like `GetValues()`), so every other `reader.Get()` sees plaintext exactly as before.

- The key is HKDF-SHA256 over `CFGU_GenHashConsoleUnique(SEAL_SALT)` — derived by the system from the console's LocalFriendCodeSeed, which is not on the SD card. **`SEAL_SALT` must never change**: every sealed file in the wild would stop opening. Random nonces come from `sslcGenerateRandomData()`.
- AES-256-GCM with the field's `section=name` (lowercased, as `INIReader` keys it) as associated data, so a sealed value pasted into another field fails instead of being used there.
- Which fields are secret is an explicit list in `isSealableSecret()`. A new credential field is sealed only once it is added there, and its test.
- `sealIniText()` mirrors `ini_parse_stream()` (BOM, `;`/`#` comments, `=` or `:`, inline ` ;` comments, indented continuation lines) and replaces only the value span, leaving every other byte as it was. A secret inih would join from several lines (continued or repeated) is left plain; so is one whose sealed line would pass `INI_MAX_LINE`. If inih's parsing rules change on a resync, this must follow.
- Writing (`configseal.cpp`): sealed text to `.3dstmp`, rename to `.sealed` once complete, **overwrite the original in place** (so the plaintext's FAT clusters are reused rather than freed and left readable), then remove `.sealed`. `recoverConfigSeal()` applies a leftover `.sealed` and drops a leftover `.3dstmp`.
- A value that will not open (another console, a system transfer, tampering) is set to `""` with an explanation on screen — the remote then reads as unconfigured. Never pass the sealed string through as if it were the credential.
- `[Security] SealCredentials=false` turns sealing off for people who share one INI between consoles; sealed values are still opened.
- The threat model is the SD card, not the console: anything running on this 3DS can derive the key. Do not describe it as more than that.

---

## Dropbox module (`source/modules/dropbox.cpp`)

A full `SyncProvider`: `files/list_folder` (+ `list_folder/continue`), `files/download`, `files/upload`, and `content_hash` as the change tag.

Things here that are easy to get wrong:

- **`content_hash` is not an MD5.** It is SHA-256 over the concatenated SHA-256 digests of each 4 MiB block; `computeDropboxHash()` in `utils/fsutil.cpp` reproduces it, which is what lets `localTag()` catch a save whose mtime never moves. An empty file hashes to SHA-256 of no input at all — do not "fix" that by hashing a zero-length block.
- **Downloads address `rev:<rev>`, not the path**, so the bytes that arrive are the revision `list()` reported even if the file changes mid-sync. `RemoteFileInfo::id` therefore holds `"rev:…"`; `upload()` rebuilds the real path from `root + relPath` instead of reading it back.
- Uploads use `"mode":"overwrite"`. The API default `"add"` does **not** overwrite — it silently renames the upload to `name (1).ext`, which breaks sync entirely.
- **A single-request upload is capped at 150 MB.** Larger files need the `upload_session` endpoints, which nothing on an SD card should hit; the module refuses them with a clear message rather than failing obscurely.
- `_headerJsonEscape()` — `Dropbox-API-Arg` is JSON carried in an HTTP header and must be pure ASCII, so non-ASCII path bytes are emitted as `\uXXXX`. Save folders are named after game titles, so non-ASCII paths are normal, not an edge case. `_jsonEscape()` is the laxer version for request bodies.
- `_splitEntries()` walks the `entries` array counting braces **string-aware**: a file name may contain `{` or `}`.
- **Dropbox paths are case-insensitive.** `list()` matches the root with `strncasecmp` and keeps the server's spelling for the remainder. A folder stored under a different case than the local one would produce a `relPath` that does not match, so the two sides would be treated as separate files.
- `validateToken()` — `POST /2/check/user` (needs no scopes) before any transfer, so a dead token gives one clear message instead of one per file.
- `_performWithRetry()` / `_rpc()`: back-off on network errors, on 429 (honouring `Retry-After`) and on 5xx; 401/403 set `_fatalError`; 400/409 are per-call errors that log the response body and continue. Do not call `_curl.perform()` directly.

**Auth.** `RefreshToken=` + `AppKey=` is the supported setup: Dropbox access tokens expire after about four hours, so `connect()` exchanges the refresh token for one on every run. `AppSecret=` is only sent for a confidential (non-PKCE) app. A bare `Token=` is still accepted for configs written before this flow existed, and `validateToken()` says what to do when such a token has expired. The configurator uses PKCE with `token_access_type=offline`; both providers now return `?code=&state=`, and the stored `state` decides which one is answering.

---

## INI format (on SD card)

```ini
[GoogleDrive]
ClientId=...
ClientSecret=...
RefreshToken=...
FolderId=...          ; optional

[Dropbox]
AppKey=...            ; app key from the configurator
RefreshToken=...      ; PKCE refresh token; Token= (short-lived) still accepted
AppSecret=            ; only for a confidential (non-PKCE) app
Path=                 ; optional folder inside the account

[SMB]                 ; SMB2/3 file server
Server=192.168.1.10
Share=3ds
User=...
Password=...
Domain=               ; optional
Path=                 ; optional folder inside the share

[FTP]
Host=192.168.1.10
Port=                 ; optional, 21 / 990 for TLS=implicit
User=...
Password=...
Path=                 ; optional base directory
TLS=try               ; none | try | require | implicit
Mode=passive          ; passive | active

[WebDAV]
Url=https://host/remote.php/dav/files/user/
User=...
Password=...

[Paths]               ; bidirectional, recursive
RemoteName=/LocalPath

[ShallowPaths]        ; bidirectional, non-recursive
[UploadPaths]         ; upload-only, recursive
[UploadShallowPaths]  ; upload-only, non-recursive
```

---

## Configurator (docs/)

- Pure static site — no server. Credentials are entered by the user (their own Google Cloud project). **Secrets live in memory** (`secrets` / `secret()` in `index.js`). The one exception is the Google client secret, which is written to `localStorage` just before the redirect to Google because it must survive it; every page load moves `SECRET_KEYS` from `localStorage` into memory and deletes them, which also clears what older versions of the page left behind. A reload therefore forgets an OAuth login — that is intended. Do not persist a token or password to make a reload nicer.
- Step 1 is a row of tiles, one per backend; clicking a tile selects it (clicking it again clears the choice) and its settings appear below. `selectedProvider` is what decides which section is written. Dropbox and Drive log in through OAuth and come back on a redirect, so their choice is kept in `localStorage` (`provider`) and their tokens only in memory for the visit; SMB, FTP and WebDAV are plain forms whose values live only in memory (`remoteSection` in `index.js`) — **never put their passwords in `localStorage`**; there is no redirect to survive. One Continue button validates whatever is selected.
- **Test connection** only exists where a browser can reach the service: Dropbox and Drive (token refresh + "who am I", and the Drive folder ID), and WebDAV (`PROPFIND` from the page). SMB and FTP are not HTTP, so their settings point at a debug sync on the 3DS instead — do not add a button that cannot work. A WebDAV test that fails at the network level is reported as "could not check" rather than as wrong settings: browsers give the same error for an unreachable host and for a server that refuses cross-origin requests, and most WebDAV servers do the latter.
- Every typed value goes through `iniValueProblem()`: inih ends a value at a `;` that follows whitespace and trims both ends, so such a value is refused with a reason rather than silently cut on the 3DS. Optional keys are written only when filled — an empty `User=` overrides the FTP default of `anonymous`, which the app applies only when the key is missing. Pasted forms (`\\nas\share\dir`, `ftps://user@host:port/dir`, a URL without a scheme) are split into the fields on blur.
- A step's title reopens it only when it lies before the open step (`markReachableSteps()` in `index.js`, kept current by a `MutationObserver` on the steps' `active` class). MStepper's own `stepTitleNavigation` stays off: it would also jump forward, past the validation in Continue and Confirm, and step 3's preview is only built by Confirm.
- Step 3 previews the file with `getConfigString(true)`, which masks passwords, secrets and tokens; the download uses `getConfigString(false)`.
- OAuth flow: PKCE (S256), authorization code, refresh token stored in INI. Redirect URI is `https://3dsync.vllni.dev/`.
- All `target="_blank"` links must include `rel="noopener noreferrer"`.
- The favicon is `docs/favicon.svg` (the two sync arrows from `meta/icon_3ds.png`, without the lettering, which is unreadable at 16 px). `favicon.ico` (16/32/48) and `apple-touch-icon.png` (180, opaque — iOS fills transparency with black) are rendered from it; regenerate both when it changes. Every page's `<head>` links all three.
- INI generation is in `getConfigString()` in `index.js`. It reads `remoteSection` first, then the in-memory OAuth tokens and `localStorage` for the provider, client ID and folder ID.

## Pages and the Universal-Updater store

`.github/workflows/pages.yml` deploys the site (Pages source: **GitHub Actions**, not a branch) as `docs/` plus `unistore/`: a UniStore v3 feed, its `storeInfo`-only `3dsync-info.json`, a `tex3ds` icon sheet from `meta/icon_3ds.png` and a QR code of the store URL. Nothing generated is committed. It runs after every successful `Release`, on pushes touching `docs/`, the icon or the generator, and on release edits/deletions.

- The feed is `.github/unistore.jq` applied to the full release list, so it can be run locally against `gh api repos/vllni/3DSync/releases`. Only releases with both `3DSync-<tag>.cia` and `.3dsx` assets are listed — the asset names `release.yml` uploads are part of the feed's contract. The newest non-prerelease is the unprefixed entry; older ones appear as `[<tag>] …`.
- **The site's address lives in two places.** Pages is deployed by Actions, which ignores a `CNAME` file: the custom domain is the one set under the repository's Settings → Pages. `docs/CNAME` is read by `pages.yml` only, for the base URL written into the feed (`storeInfo.url`, `infoURL`, `sheetURL`) and the QR code. Change both together, and the hard-coded mentions with them (`git grep 3dsync.` — README, `docs/*.html`, the "no configuration" card in `ui/screens.cpp`). Universal-Updater refreshes a store from the `url` in the copy it already holds and follows redirects, so after a domain move the old one must keep answering with an HTTP redirect until subscribers have picked up a feed pointing at the new one.
- **Assets are cache-busted at deploy time.** Cloudflare sits in front of the domain and sends `static/` scripts and styles with a 4-hour `max-age`, against 10 minutes for the HTML, so a returning visitor would run a new page against an old `index.js`. `pages.yml` appends `?v=<commit>` to every `src`/`href="static/…"` in `_site/*.html`; the files in `docs/` stay unstamped so they still open locally. Reference new assets the same way (relative, `static/…`) and they are covered.
- **`revision` must strictly increase** or Universal-Updater never refetches the store. It cannot be derived from the releases (deleting or editing one would leave it flat or move it back), so the workflow reads it from the deployed feed: kept when nothing else changed, otherwise `max(old + 1, now)`. A failed fetch of the deployed feed other than 404 stops the run rather than guess.
- `.3dsx` paths are `%3DSX%/3DSync.3dsx` with no subfolder: Universal-Updater's "3DSX in folder" option inserts `3DSync/` itself, and a path written with one would be doubled.
- `title_ids` is `UNIQUE_ID` from the Makefile in decimal; change both together.
- A `release` event runs on the tag ref, which the `github-pages` environment refuses to deploy, so that trigger only re-dispatches the workflow on main.

---

## Licensing

3DSync's own code is MIT. **libsmb2 is LGPL-2.1** and is statically linked, so the released binaries are a combined work. The relinking requirement is met by keeping the source public and the libsmb2 commit pinned in the Dockerfile — do not vendor a modified copy without recording the changes, and keep the note in README.md accurate.

---

## Branch / PR conventions

- Feature branches: `feat/...`, bug fixes: `fix/...` or `fiX/...`
- PRs target `vllni/3DSync:main` (the fork's default branch), **not** `Kyraminol/3DSync:master` (the upstream, which still uses `master`).
- Always run `make` and `make -C tests` before committing C++ changes.
- Commit messages: short imperative subject, blank line, then bullet-point body describing *why*.
- `.github/CODEOWNERS` assigns `@vllni` as the default reviewer for the whole repo.
- Never reuse code from a closed PR whose author has withdrawn permission for it. Implementing the same fix independently is fine; copying the commits is not.
