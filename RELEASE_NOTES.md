# Release Notes - `curldl` v1.1.0 (Security Hardening & Robustness Update)

## 🛡️ Overview

`curldl` v1.1.0 introduces critical security hardening, strict protocol enforcement, safer process execution, and a dedicated automated test suite while maintaining its core design philosophy: a zero-dependency, single-file C++17 utility for Linux.

---

## 🔒 Security Fixes & Improvements

### 1. Elimination of Shell Command Execution (`popen` Removal)
- **Previous behavior:** Header probing used `popen()` to execute curl via `/bin/sh` with string concatenation and shell escaping.
- **Fixed:** Replaced `popen()` with direct POSIX process creation (`pipe()` + `fork()` + `execvp()`). `curl` arguments are passed as discrete array elements without invoking `/bin/sh` or any shell interpreter, completely eliminating shell metacharacter and argument injection risks.

### 2. Strict Protocol Whitelist & SSRF Mitigation
- **Previous behavior:** Any URL scheme containing `://` was accepted, allowing potential arbitrary local file reads (`file:///etc/passwd`) or internal network pivoting (`gopher://`, `dict://`).
- **Fixed:** Added startup URL scheme validation that strictly allows only `http://` and `https://` (case-insensitive). All other schemes are rejected before executing any network or process commands.
- Enforced `--proto "=http,https"` and `--proto-redir "=http,https"` on all `curl` invocations, preventing malicious 3xx HTTP redirects to local files or non-HTTP protocols.

### 3. Protection Against Symlink Hijack & Directory Collisions
- **Previous behavior:** File existence check used `stat()`, which follows symlinks and failed to protect against dangling symlinks or directory targets.
- **Fixed:** Switched to `lstat()`. `curldl` now explicitly detects if the destination target is a symbolic link (`S_ISLNK`) or a directory (`S_ISDIR`), aborting immediately to prevent target hijacking or overwriting unintended files.

### 4. CLI Option / Argument Injection Prevention
- **Previous behavior:** Filenames beginning with `-` were preserved, potentially causing subsequent shell command expansions or curl option interpretations (e.g. `-rf`, `-v`).
- **Fixed:** The `sanitize()` function strips leading hyphens. In addition, the `-o` argument parser explicitly rejects option-like values starting with `-`.

### 5. RFC 5987 / RFC 6266 Header Parsing Hardening
- **Previous behavior:** `fromContentDisposition()` failed to parse language-tagged RFC 5987 parameters (e.g., `UTF-8'en'filename.ext`) and did not provide fallback when `filename*=` was malformed.
- **Fixed:** Full support for `filename*=<charset>'<lang>'<value>` with and without language tags, accurate percent-decoding, and clean fallback to standard `filename="..."` (including unescaping backslash-escaped quotes).

### 6. Signal Interruption Resilience (`EINTR`)
- **Previous behavior:** If a signal (e.g. `SIGWINCH`) interrupted `waitpid()`, the program could exit prematurely, leaving `curl` as a background orphan.
- **Fixed:** Wrapped all `waitpid()` and `read()` calls in retry loops that properly resume on `EINTR`.

### 7. Filesystem & Special Device Name Filters
- Added filtering for special device names (`CON`, `PRN`, `AUX`, `NUL`, `COM1-9`, `LPT1-9`) and dangerous characters (`:`, `*`, `?`, `"`, `<`, `>`, `|`) for cross-filesystem compatibility (e.g., mounted NTFS/FAT partitions on Linux).

---

## 🧪 Test Suite & Build System

- **Automated Test Suite:** Added [test_curldl.cpp](file:///C:/Users/Atill/Desktop/curl_downloader/test_curldl.cpp) with zero external testing dependencies. Tests URL scheme validation, path sanitization, RFC 5987 parsing, and collision defenses.
- **Makefile Update:** Added `test` and `test_runner` targets to [Makefile](file:///C:/Users/Atill/Desktop/curl_downloader/Makefile).

---

## 🚀 Building & Testing

```bash
# Build binary
make

# Run automated tests
make test

# Install globally
sudo make install
```
