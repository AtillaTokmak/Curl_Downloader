# Release Notes - `curldl` v1.2.0 (Hardening, pass 2)

- **Atomic download:** curl writes to a `mkstemp` temp file (`.curldl-XXXXXX`) that is `rename()`d onto the target only on success. Symlinks planted at the target are never followed; failures/Ctrl+C leave no partial file and never truncate an existing file.
- **Signal handling:** SIGINT/SIGTERM/SIGHUP are forwarded to curl, the temp file is removed, exit code is `128+signal`.
- **`~/.curlrc` ignored:** `-q` is passed to every curl call.
- **Content-Disposition** is taken only from the final 2xx response, not from intermediate redirects.
- **Terminal escape injection:** all names/URLs printed to stderr are escaped; filenames with invalid UTF-8 or C1 control characters are rejected.
- **URL validation:** control characters/whitespace rejected; `user@host`, IPv6 `[::1]:port` supported; empty host rejected.
- **Limits:** probe output capped at 1 MiB, max 10 redirects, filenames over 255 bytes rejected.
- **Dotfiles:** leading dots stripped from automatically chosen names (`.bashrc` -> `bashrc`). `-o` with a directory path now errors.

Verified on Linux: `g++ -std=c++17 -Wall -Wextra -Werror` builds clean; `file://` URL rejected (exit 1); HTTPS download succeeds (exit 0) with no temp file left behind.

---

# Release Notes - `curldl` v1.1.0 (Security Hardening & Robustness Update)

## 🛡️ Overview

`curldl` v1.1.0 introduces critical security hardening, strict protocol enforcement, safer process execution, and robust error handling while maintaining its core design philosophy: a zero-dependency, single-file C++17 utility for Linux.

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

## 🚀 Building & Installation

```bash
# Build binary
make

# Install globally
sudo make install
```
