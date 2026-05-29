# Agent context for 3DSync

## What this repo is

Nintendo 3DS/2DS homebrew app (`.cia` / `.3dsx`) that syncs files between the SD card and cloud storage (Dropbox or Google Drive). Written in C++11, built with devkitPro/devkitARM.

Companion static web configurator at `docs/` (GitHub Pages at `https://vllni.github.io/3DSync/`) generates the INI config file via OAuth and a stepper UI.

---

## Repo layout

```
source/             C++ application source
  main.cpp          Entry point, sync engine, INI parsing
  modules/
    dropbox.cpp/h   Dropbox upload (upload-only, legacy)
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
buildtools/         devkitPro build scaffolding — do not edit
```

---

## Build

Requires devkitPro with 3DS support (`DEVKITPRO` environment variable set).

```bash
make          # produces output/3ds-arm/3DSync.cia and .3dsx
```

The Makefile auto-discovers all `.cpp` files under `source/`. Adding a new `.cpp` file there is sufficient — no Makefile edits needed.

---

## Key C++ conventions

- **C++11**, no exceptions, no RTTI.
- All HTTP goes through the `Curl` wrapper (`source/utils/curl.h`). Never call libcurl directly.
- `Curl::perform()` returns 0 on network success. **HTTP status is not checked by `perform()`** — callers must call `getStatusCode()`. `CURLOPT_FAILONERROR` is intentionally disabled so error response bodies are captured.
- All Google Drive API calls go through `GoogleDrive::_performWithRetry()`, which handles 429 back-off, 401/403 fatal detection, and `_fatalError` propagation. Do not call `_curl.perform()` directly from Drive methods.
- Fatal Drive errors (401 / unrecoverable 403) set `_fatalError = true` and cause all subsequent Drive calls to no-op. Check `hasFatalError()` in callers to break out of sync loops early.
- File downloads use a temp file (`localPath + ".3dstmp"`) + atomic rename. On FAT (3DS SD), `rename()` cannot overwrite an existing file — the existing file is first renamed to `.3dsbak`, then the temp is renamed in, then the backup is removed. Restore the backup on failure.
- `performSync()` in `main.cpp` returns `bool` — `false` means cancellation was requested or a fatal error occurred, and the sync loop must stop.
- Conflict resolution in `performSync()` calls `waitForConflictKey()` which returns a `ConflictChoice` enum: `CONFLICT_KEEP_LOCAL` (A), `CONFLICT_KEEP_DRIVE` (B), `CONFLICT_SKIP` (X), `CONFLICT_CANCEL` (START).
- The manifest (`/3ds/3DSync/manifest.json`) is a hand-parsed JSON file. Use `Manifest::set/get/has/remove` — never write JSON by hand elsewhere.
- `svcSleepThread(nanoseconds)` is the 3DS sleep call (from `<3ds.h>`). Use it for rate-limit back-offs.
- `printf` output goes to the 3DS top-screen console. Use `CONSOLE_RED` / `CONSOLE_RESET` (from `<3ds.h>`) for error messages.

---

## INI format (on SD card)

```ini
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
