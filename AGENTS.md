# AGENTS.md

## Build

- **CLI is OFF by default.** Must pass `-DMOO_BUILD_APPS=ON` to build `cli/`.
- Full debug build: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build --parallel`
- Quick shortcut: `npm run build` (adds `-DMOO_BUILD_APPS=ON -DX_BUILD_EXAMPLES=ON -DX_DEBUG_LEVEL=1`)
- Libraries are **shared** by default. Use `-DX_BUILD_STATIC=ON` for static.
- Transitive deps (libcurl, llhttp, nghttp2, cJSON, usrsctp, QuickJS-ng, GoogleTest, Google Benchmark) are auto-fetched via CMake `FetchContent` — no manual install needed for a first build.
- macOS: Homebrew OpenSSL and mbedTLS are keg-only. The test scripts auto-detour via `brew --prefix`, but manual cmake calls need `-DOPENSSL_ROOT_DIR=$(brew --prefix openssl)` or similar.

## Test

- All tests: `ctest --test-dir build --output-on-failure --parallel 4`
- Single test binary: `./build/libx/x/base/xbase_test --gtest_filter="HeapTest.*"`
- **Affected-modules only** (much faster): `./scripts/test-mac.sh` or `./scripts/test-mac.sh -t mbedtls --all`
  - Diffs against `origin/main` by default; pass `--base-sha <SHA>` or `-b <ref>` to override.
  - Pass `--detect-only` to just print affected module names.
  - `xline` has no test binary and is skipped automatically by the scripts.
- **TLS backend matters.** Some tests (xhttp, xp2p, xfer) behave differently under openssl vs mbedtls. CI tests both. For local: `cmake -S . -B build-openssl -DX_TLS_BACKEND=openssl` and `build-mbedtls` respectively.
- CI runs with `--asan` enabled. To reproduce: `cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DMOO_ENABLE_ASAN=ON`.

## Formatting & Lint

- Run `clang-format` before committing. Config is `.clang-format` (LLVM-based, 100-col, 2-space, pointer-right `int *p`).
- **C code compiles with `-Wall -Wextra -Werror`** — any warning is a build failure. C++ tests use `-Wall -Wextra` without `-Werror`.
- There is no separate lint target; formatting is the only check beyond compiler warnings.

## Architecture

- `libx/x/` contains independently reusable C modules. `cli/` is the only consumer app.
- Dependency chain: `xagent → xhttp → xnet → xbase`; `xhttp` also uses `xbuf`. Changing `xbase` triggers retest of everything.
- `libx++/xpp/` is an optional C++14 RAII wrapper. It has its own test target `xpp_test`. When modifying libx++, the test scripts also build a C++11 strict-mode guard (`xpp_cxx11_guard`) to catch accidental C++14-only usage.

## Conventions (non-obvious)

- **Language**: C99 for libraries, C++ for `cli/` and tests.
- **Public API prefix**: `x` + PascalCase (`xHeapPush`). **Internal/static functions**: snake_case (`submit_timer`).
- **Callbacks** end with `Func` (`xEventFunc`). **Configs** end with `Conf` (`xTaskGroupConf`).
- **cli/ headers** use `.h` (not `.hpp`) with guards `MOO_CLI_<FILE>_H`.
- **Include order**: corresponding public header → stdlib → system → project-private (`*_private.h`). Public headers use `<x/base/xxx.h>` angle brackets; private use `"xxx.h"`.
- **Error handling**: return `xErrno` enum; create functions return NULL on failure; multi-step init uses `goto fail`.
- **Export macro**: `XCAPI(T)` on every public function. Type macros: `XDEF_STRUCT`, `XDEF_ENUM`, `XDEF_HANDLE`.
- **File headers**: copyright block + `<filename> - <brief description>`.
- Internal structs end with trailing `_`: `struct xEventSource_`.
- Sections inside `.c` files use `/* ── Section ── */` dividers.

## Commit & Branch

- Conventional Commits: `<type>(<scope>): <subject>`. Lowercase subject, no period.
- Scopes: `xbase`, `xbuf`, `xnet`, `xhttp`, `xlog`, `xcrypto`, `xp2p`, `xfer`, `xagent`, `cli` (or omit for cross-module).
- Branch naming: `<author>/<short-description>`, all lowercase with hyphens (e.g. `mivinci/add-sse-support`).
- CI enforces branch name prefix on PRs: must match `moo/`, `qclaw/`, `codebuddy/`, `workbuddy/`, `claude/`, `opencode/`, `renovate/`, or `copilot/`.
