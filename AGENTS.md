# Agent context for 3DSync

> **Keep this file current.** It is the shared context for every agent session in
> this repo. When a change makes something here wrong or incomplete — a module
> gains a capability, a convention changes, a build flag or file moves — update
> the affected section in the *same* commit as the code change. A stale note here
> costs a later session more time than writing it did.

## What this repo is

Nintendo 3DS/2DS homebrew app (`.cia` / `.3dsx`) that syncs files between the SD card and cloud storage (Dropbox or Google Drive). Written in C++11, built with devkitPro/devkitARM.

Companion static web configurator at `docs/` (GitHub Pages at `https://vllni.github.io/3DSync/`) generates the INI config file via OAuth and a stepper UI.

---

## Repo layout

```
source/             C++ application source
  main.cpp          Entry point, sync engine, INI parsing
  modules/
    dropbox.cpp/h   Dropbox upload (upload-only — see "Dropbox module")
    googledrive.cpp/h  Google Drive bidirectional sync + OAuth token refresh
    manifest.cpp/h  Local JSON manifest tracking sync state
  utils/
    curl.cpp/h      libcurl wrapper (GET/POST/PATCH, streaming download, header capture)
  libs/inih/        INI parser (inih + INIReader)
docs/               Configurator static site
  index.html        Three-step stepper UI (auth → paths → download)
  static/js/index.js  All UI logic (PKCE OAuth, path config, INI generation)
output/             Build artefacts (.cia, .3dsx, .zip) — do not edit
build/              Intermediate object files — do not edit
buildtools/         devkitPro build scaffolding (git submodule) — do not edit
.github/            CI workflows, CODEOWNERS
```

---

## Build

Requires devkitPro with 3DS support (`DEVKITPRO` environment variable set).

```bash
make          # produces output/3ds-arm/3DSync.cia and .3dsx
```

The Makefile auto-discovers all `.cpp` files under `source/`. Adding a new `.cpp` file there is sufficient — no Makefile edits needed.

`make` needs the `buildtools` submodule checked out (`git submodule update --init`); without it the build stops at `buildtools/make_base: No such file or directory`. When a full build is not available, type-check a translation unit directly:

```bash
arm-none-eabi-g++ -fsyntax-only -std=gnu++11 -Wall \
  -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -D__3DS__ -DARM11 \
  -DINI_MAX_LINE=1024 -DVERSION_STRING='"x"' -DREVISION_STRING='"y"' \
  -I$DEVKITPRO/libctru/include -I$DEVKITPRO/portlibs/3ds/include \
  -I$DEVKITPRO/portlibs/armv6k/include source/main.cpp
```

Compile-time configuration of vendored libraries belongs in `BUILD_FLAGS` in the Makefile, not in the library sources. `INI_MAX_LINE=1024` is set there (inih defaults to 200, which truncates long `Paths=` lines); leave `source/libs/inih/` pristine.

---

## Key C++ conventions

- **C++11**, no exceptions, no RTTI.
- All HTTP goes through the `Curl` wrapper (`source/utils/curl.h`). Never call libcurl directly.
- `Curl::perform()` returns 0 on network success. **HTTP status is not checked by `perform()`** — callers must call `getStatusCode()`. `CURLOPT_FAILONERROR` is intentionally disabled so error response bodies are captured.
- All Google Drive API calls go through `GoogleDrive::_performWithRetry()`, which handles 429 back-off, 401/403 fatal detection, and `_fatalError` propagation. Do not call `_curl.perform()` directly from Drive methods.
- Fatal Drive errors (401 / unrecoverable 403) set `_fatalError = true` and cause all subsequent Drive calls to no-op. Check `hasFatalError()` in callers to break out of sync loops early.
- File downloads use a temp file (`localPath + ".3dstmp"`) + atomic rename. On FAT (3DS SD), `rename()` cannot overwrite an existing file — the existing file is first renamed to `.3dsbak`, then the temp is renamed in, then the backup is removed. Restore the backup on failure.
- `performSync()` in `main.cpp` returns `bool` — `false` means cancellation was requested or a fatal error occurred, and the sync loop must stop.
- Conflict resolution in `performSync()` calls `waitForConflictKey()` which returns a `ConflictChoice` enum: `CONFLICT_KEEP_LOCAL` (A), `CONFLICT_KEEP_REMOTE` (B), `CONFLICT_SKIP` (X), `CONFLICT_CANCEL` (START).
- The manifest (`/3ds/3DSync/manifest.json`) is a hand-parsed JSON file. Use `Manifest::set/get/has/remove` — never write JSON by hand elsewhere.
- `svcSleepThread(nanoseconds)` is the 3DS sleep call (from `<3ds.h>`). Use it for rate-limit back-offs.
- `printf` output goes to the 3DS top-screen console. Use `CONSOLE_RED` / `CONSOLE_RESET` (from `<3ds.h>`) for error messages.
- The console is **50 columns** wide and wraps mid-word. Keep any line of output — key prompts especially — under that.
- Keep sync output quiet: print per-entry headers, errors, interactive prompts and the closing `--- Sync Summary ---`. Do **not** add a line per file examined or per file transferred; the summary already lists every changed file, and per-file chatter scrolls the interesting parts off-screen.
- `Curl::setReadData(FILE *, size)` streams a request body from a file. Pass the real byte count: without it libcurl sends `Transfer-Encoding: chunked`, which the Dropbox content endpoints reject.

---

## Dropbox module (`source/modules/dropbox.cpp`)

**Upload-only.** Google Drive is the reference implementation for bidirectional sync; Dropbox has no listing, no download and no manifest integration, so `SYNC_BOTH` entries are merely uploaded and its transfers never appear in `SyncSummary`. In `runSync()` the Dropbox pass also runs for *every* configured entry regardless of `entry.direction`.

What it does have, and must keep:

- `validateToken()` — `POST /2/check/user` (needs no scopes) before any transfer, because configurator tokens are short-lived and a stale one otherwise fails silently.
- `_performWithRetry()` — the Dropbox counterpart of `GoogleDrive::_performWithRetry()`: back-off on network errors, on 429 (honouring the `Retry-After` header) and on 5xx; 401/403 set `_fatalError`; 400/409 are per-file errors that log the response body and skip that file only. Do not call `_curl.perform()` directly from Dropbox methods.
- Uploads use `"mode":"overwrite"`. The API default `"add"` does **not** overwrite — it silently renames the upload to `name (1).ext`, which breaks sync entirely.
- `_headerJsonEscape()` — `Dropbox-API-Arg` is JSON carried in an HTTP header and must be pure ASCII, so non-ASCII path bytes are emitted as `\uXXXX`. Save folders are named after game titles, so non-ASCII paths are normal, not an edge case.
- `upload()` returns `false` only for user cancellation (START) or a fatal error — distinguished by `hasFatalError()` in the caller. Per-file failures log and continue.

To reach parity with Drive, a future change needs: `files/list_folder` (+ `list_folder/continue` pagination) and `files/download`; the Dropbox **content hash** (SHA-256 over the SHA-256 of each 4 MB block — *not* MD5, so `computeMd5Hex()` in `main.cpp` does not transfer) and `rev` in place of `driveMd5`/`driveId`; a provider-neutral interface so `performSync()` stops taking `GoogleDrive &`; and provider-scoped manifest keys, since the single `/3ds/3DSync/manifest.json` is keyed by local path only and both providers would fight over the same entries.

Config-side: the configurator issues an implicit-flow token (`response_type=token`), so Dropbox tokens expire after ~4 hours and there is no refresh token. PKCE plus `token_access_type=offline` would be needed for unattended sync.

---

## INI format (on SD card)

```ini
[Dropbox]
Token=...             ; short-lived access token from the configurator

[GoogleDrive]
ClientId=...
ClientSecret=...
RefreshToken=...
FolderId=...          ; optional

[Paths]               ; bidirectional, recursive
RemoteName=/LocalPath

[ShallowPaths]        ; bidirectional, non-recursive
[UploadPaths]         ; upload-only, recursive
[UploadShallowPaths]  ; upload-only, non-recursive
```

---

## Configurator (docs/)

- Pure static site — no server. Credentials are entered by the user (their own Google Cloud project). The client secret lives in `localStorage` only long enough to survive the OAuth redirect, and must be cleared from `localStorage` after `exchangeGoogleCode` succeeds.
- OAuth flow: PKCE (S256), authorization code, refresh token stored in INI. Redirect URI is `https://vllni.github.io/3DSync/`.
- All `target="_blank"` links must include `rel="noopener noreferrer"`.
- INI generation is in `getConfigString()` in `index.js`. It reads `localStorage` for provider, tokens, and folder ID.

---

## Branch / PR conventions

- Feature branches: `feat/...`, bug fixes: `fix/...` or `fiX/...`
- PRs target `vllni/3DSync:master` (the fork), **not** `Kyraminol/3DSync:master` (the upstream).
- Always run `make` and verify a clean build before committing C++ changes.
- Commit messages: short imperative subject, blank line, then bullet-point body describing *why*.
- `.github/CODEOWNERS` assigns `@vllni` as the default reviewer for the whole repo.
- Never reuse code from a closed PR whose author has withdrawn permission for it. Implementing the same fix independently is fine; copying the commits is not.
