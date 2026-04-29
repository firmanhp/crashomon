# CLAUDE.md — Crashomon Project Context

## What this project is

Crashomon is a crash monitoring ecosystem for ELF binaries on embedded Linux. It provides Android tombstone-style crash reports, Breakpad minidumps, and a web UI for symbolicated offline analysis. Target: 4GB RAM, 2GB disk, 4 cores, systemd, Linux.

## Architecture summary

- **libcrashomon.so**: C++20 implementation with C-compatible public header (`extern "C"` linkage). Works via LD_PRELOAD (zero-code integration) or explicit linking. Constructor connects to watcherd's Unix socket, receives a shared Crashpad socket fd + handler PID via `SCM_RIGHTS`, then calls `CrashpadClient::SetHandlerSocket(shared_fd, handler_pid)`.
- **crashomon-watcherd**: systemd service. Hosts a single `ExceptionHandlerServer` (Crashpad) with `multiple_clients=true` on a dedicated thread. Clients share one socketpair endpoint via `SCM_RIGHTS` fd passing — no per-client threads, no subprocess spawning. Also uses inotify to watch for new minidumps, parses them via Breakpad's minidump-processor, prints Android-style tombstone to journald, and prunes old minidumps.
- **crashomon-analyze**: CLI tool. Symbolicates a minidump or pasted tombstone text using `minidump_stackwalk` + a symbol store. Stateless.
- **crashomon-syms**: Script. Ingests debug binaries into the Breakpad symbol store (`dump_syms` wrapper).
- **crashomon-web**: Flask web app. Manages a symbol store (indexed by build ID), stores crash history in SQLite, auto-symbolicates uploaded minidumps.

## Key design decisions

- **No full memory copy**: The watcherd's `ExceptionHandlerServer` uses ptrace to read only registers + stack snippets (128KB/thread) + /proc maps. Never copies heap.
- **Shared socket, not per-client threads**: A single `ExceptionHandlerServer` handles all crash requests on one thread. Clients share the socket endpoint via `SCM_RIGHTS` fd passing. No thread pool, no per-client threads, no subprocess spawning. This is the architecture Crashpad's design doc recommends for long-lived handlers.
- **LD_PRELOAD works via inheritance**: Set `LD_PRELOAD=/usr/lib/libcrashomon.so` in a systemd unit file. Child processes inherit the env var. All dynamic binaries in the process tree get crash monitoring without code changes. Note: spawning `crashpad_handler` per process would cause a fork bomb under LD_PRELOAD — the shared watcherd socket breaks the cycle.
- **Crash notification is signal-based**: In `multiple_clients` mode, Crashpad sends `kDumpDoneSignal` (SIGCONT) directly to the crashing thread via `tgkill()` after writing the minidump, not a socket message. Clients wait via `sigtimedwait` with a 5-second timeout.
- **Symbol store uses Breakpad layout**: `<root>/<module_name>/<build_id>/<module_name>.sym`. `minidump_stackwalk` auto-resolves the right version by build ID from the minidump. CI/CD uploads symbols after each build with `crashomon-syms add`.
- **Stack unwinding without frame pointers**: Works at `-O2`/`-O3`. Crashpad captures raw stack memory + registers. minidump_stackwalk uses DWARF CFI from `.sym` files (extracted from `.eh_frame` by `dump_syms`). On-target tombstone uses Breakpad's stack scanning (heuristic, approximate).

## Code standards

- **C**: C11, no GNU extensions (`-std=c11`)
- **C++**: C++20, no GNU extensions, no exceptions (`-std=c++20 -fno-exceptions`)
- **Compiler flags**: `-Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion -Wnull-dereference -Wformat=2 -fstack-protector-strong -O3 -g -fno-exceptions`
- **Style**: Google C++ Style Guide + Abseil Tips of the Week
- **Formatting**: clang-format Google style (see `.clang-format`)
- **Static analysis**: clang-tidy (see `.clang-tidy`). Enable with `-DENABLE_CLANG_TIDY=ON`
- **Python**: ruff, line length 100, Python 3.11+ (see `pyproject.toml`)
- **Tests**: GoogleTest for C/C++, pytest for Python
- **Sanitizers**: opt-in toggles `-DENABLE_UBSAN=ON`, `-DENABLE_TSAN=ON`, `-DENABLE_ASAN=ON`. No Debug/Release split — single build with `-O3 -g`.

## Key Abseil TotW practices to follow

- Prefer `std::string_view` over `const std::string&` for read-only string params
- Use `auto` only when type is obvious from context
- Prefer `enum class` over plain `enum`
- Return by value, rely on copy elision; avoid `std::move` on return statements
- Prefer `std::make_unique` over `new`
- Use `absl::Status` and `absl::StatusOr<T>` for error handling — no exceptions

## Build

```bash
cmake -B build && cmake --build build && ctest --test-dir build

# With benchmarks
cmake -B build -DENABLE_BENCHMARKS=ON && cmake --build build
```

## Directory layout

```
lib/          C++20 client library with C-compatible header (libcrashomon.so/.a)
cmake/        CMake helper modules (cmake/breakpad.cmake defines Breakpad targets)
daemon/       C++ watcher daemon (crashomon-watcherd)
tools/
  analyze/    Python CLI analysis tool (crashomon-analyze)
  syms/       Symbol ingestion script (crashomon-syms)
web/          Python Flask web UI (crashomon-web)
bench/        Microbenchmarks (Google Benchmark, opt-in via -DENABLE_BENCHMARKS=ON)
test/         C/C++ unit tests (GoogleTest) + fixtures + integration tests
examples/     Example crasher programs
systemd/      systemd unit files
```

## Change checklists

These are mandatory — run them whenever the described change is made.

### Touching Python imports or dependencies (`web/`, `tools/`)
1. If you add or remove a top-level import in any `web/` or `tools/` file, verify the package is declared in `pyproject.toml` `[project.dependencies]`.
2. After editing `pyproject.toml` dependencies, run `uv lock` to regenerate `uv.lock`. Both files must be committed together.
3. Run `uv run pytest` to confirm tests still pass.
4. If `_host_toolkit/` exists, re-stage the Python artifacts so the integration tests use fresh copies:
   ```bash
   cmake --build _host_toolkit --target host_toolkit_stage_analyze host_toolkit_stage_syms host_toolkit_stage_patchdmp host_toolkit_stage_pylib
   ```

### Touching the Dockerfile or docker-compose.yml
1. Run `docker build -f web/Dockerfile -t crashomon-web:test .` — the build must succeed.
2. Start the container and hit at least one route: `docker run --rm -e SECRET_KEY=test crashomon-web:test` and verify workers boot without errors in the log.
3. Confirm the app uses `/data/` for storage (env vars `CRASHOMON_SYMBOL_STORE` and `CRASHOMON_DB` are set in the Dockerfile).

### Touching `web/README.md`
- Verify every command block in the README actually works in the current repo state.
- The Docker section must stay in sync with `web/Dockerfile` and `web/docker-compose.yml`.

## What NOT to do

- Do not copy full process memory. Crashpad reads selectively via ptrace.
- Do not mix ASan and TSan (they conflict).
- Do not apply `-Werror` to third-party dependencies (Crashpad, GoogleTest, Breakpad, Abseil). Use `target_compile_options()` on our own targets only.
- Do not do any heap allocation or non-async-signal-safe operations in the client library's signal handler path.
- Do not add upload/network support — `ExceptionHandlerServer` is always configured with no upload thread.
- Do not use `try`/`catch`/`throw`. All C++ is compiled with `-fno-exceptions`. Use `absl::Status`/`absl::StatusOr` for internal error handling.
- Do not expose Abseil types in public or cross-library interfaces. Public APIs must be freestanding — consumers must not need to add Abseil as a dependency. `absl::Status`/`absl::StatusOr` are used only in `.cpp` files and internal (non-public) headers.
- Do not rely on system-installed versions of project dependencies. All C/C++ deps (Crashpad, GoogleTest, Abseil, Breakpad, Google Benchmark) are fetched via CMake FetchContent.
