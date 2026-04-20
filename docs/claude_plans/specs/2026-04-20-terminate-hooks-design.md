# Design: Terminate & Assert Hooks with Minidump Annotation Propagation

**Date:** 2026-04-20
**Status:** Approved

## Goal

Automatically capture C++ termination context (unhandled exceptions, `assert()` failures) as Crashpad annotations so that crash reports carry a human-readable abort message — matching Android tombstone behaviour.

## Approach B: `abort_message` + optional `terminate_type`

Two annotation keys are written before the process crashes:

| Key | Source | Example value |
|-----|--------|---------------|
| `abort_message` | always | `"invariant violated: count >= 0"` / `"assertion failed: 'x > 0' (main.cpp:42, foo())"` / `"terminate called without active exception"` |
| `terminate_type` | `std::terminate` path only | `"std::runtime_error"` |

`abort_message` is the canonical display field. `terminate_type` is a bonus tag for tooling / web UI filtering.

---

## Phase 1 — Library hooks (`lib/crashomon.cpp`)

### Hook A: `__assert_fail` weak-symbol override

```cpp
extern "C" [[noreturn]] void __assert_fail(
    const char* assertion, const char* file, unsigned int line, const char* func);
```

- GNU libc calls this for every `assert()` failure before calling `abort()`.
- Because `libcrashomon.so` is LD_PRELOAD'd, the dynamic linker resolves our symbol first.
- Formats `abort_message` as `"assertion failed: '<expr>' (file:line, func)"`.
- Calls `crashomon_set_abort_message()`, then `std::abort()`.
- Safe before `DoInit()` runs: `crashomon_set_abort_message` is a no-op when the annotations pointer is null.

### Hook B: `std::set_terminate` handler

Installed at the end of `DoInit()`, after the `SimpleStringDictionary` is registered.

Logic:
1. `std::current_exception()` — if active exception:
   - `catch (const std::exception& e)` → `abort_message = e.what()`, `terminate_type = demangled_name`
   - `catch (...)` → `abort_message = "terminate called with unknown exception"`, `terminate_type = demangled name via abi::__cxa_current_exception_type()`
2. No active exception → `abort_message = "terminate called without active exception"`
3. Calls `std::abort()` (Crashpad catches SIGABRT; annotations are written before the signal fires).

Requires `<cxxabi.h>` for `abi::__cxa_demangle` and `abi::__cxa_current_exception_type()`.

### Testable internal helpers

Two internal (anonymous-namespace) free functions with no Crashpad dependency, called by both hooks and directly by tests:

```cpp
// Writes abort_message (and optionally terminate_type) into annotations.
void WriteTerminateAnnotation(std::exception_ptr exc);

// Formats and writes abort_message for an assert() failure.
void WriteAssertAnnotation(const char* assertion, const char* file,
                            unsigned int line, const char* func);
```

Both call `crashomon_set_abort_message` / `crashomon_set_tag`. Tests wire up a `SimpleStringDictionary` using the same fixture pattern as `TagsTest`.

---

## Phase 2 — Daemon tombstone (`tombstone/`)

### `MinidumpInfo` new fields

```cpp
std::string abort_message;   // Crashpad "abort_message" annotation, may be empty
std::string terminate_type;  // Crashpad "terminate_type" annotation, may be empty
```

### `ExtractCrashpadAnnotations` in `minidump_reader.cpp`

Reads stream type `0x43500007` (CrashpadInfo) by iterating Breakpad's raw stream directory. Parses the fixed-layout `MinidumpCrashpadInfo` structure:

```
offset  0: version (u32) — validated == 1
offset  4: report_id UUID (16 bytes, skipped)
offset 20: client_id UUID (16 bytes, skipped)
offset 36: simple_annotations MINIDUMP_LOCATION_DESCRIPTOR
             DataSize (u32) + RVA (u32)
```

At the annotations RVA: `count` (u32) followed by `(key_rva, value_rva)` pairs, each an RVA to a `MINIDUMP_UTF8_STRING` (`length` u32 + UTF-8 bytes, null-terminated). Extracts `"abort_message"` and `"terminate_type"` keys.

Called from `ReadMinidump()` after module extraction.

### `tombstone_formatter.cpp` display

Emitted after the signal line, before probable cause. Combines fields:

```
Abort message: 'std::runtime_error: invariant violated'
```

Rule: if `terminate_type` non-empty → `terminate_type + ": " + abort_message`; else `abort_message` verbatim. Omit line entirely if both are empty.

---

## Phase 3 — Analyzer (`tools/analyze/`)

### `ParsedTombstone` new fields (`log_parser.py`)

```python
abort_message: str = ""
terminate_type: str = ""
```

### `read_minidump_annotations(dmp_path: str) -> dict[str, str]` (`symbolizer.py`)

Raw binary parse of stream type `0x43500007`, same approach as `read_minidump_process_info`. Returns all annotation key-value pairs; caller selects `"abort_message"` and `"terminate_type"`.

### `_apply_minidump_metadata` (`analyze.py`)

Calls `read_minidump_annotations` and populates `tombstone.abort_message` / `tombstone.terminate_type`.

### `format_symbolicated` (`symbolizer.py`)

Emits after the signal line, before `backtrace:`, using the same combination rule as Phase 2:

```
Abort message: 'std::runtime_error: invariant violated'
```

`format_raw_tombstone` delegates to `format_symbolicated` and gets the line for free.

---

## Phase 4 — Tests

### Phase 1: `lib/test/test_terminate_hooks.cpp` (new, GoogleTest)

Fixture: same `SimpleStringDictionary` wiring as `TagsTest`.

| Test | What it exercises |
|------|-------------------|
| `AssertAnnotation_FormatsExprFileLineFuncCorrectly` | `WriteAssertAnnotation` → `abort_message` format |
| `AssertAnnotation_NullAnnotationsIsNoOp` | null dict guard |
| `TerminateAnnotation_StdExceptionWritesWhatAndType` | `std::runtime_error`, checks both keys |
| `TerminateAnnotation_UnknownExceptionWritesTypeOnly` | `catch(...)` path, `terminate_type` set |
| `TerminateAnnotation_NoActiveExceptionWritesFallback` | null `exception_ptr` path |
| `TerminateAnnotation_NullAnnotationsIsNoOp` | null dict guard |

### Phase 2: tombstone tests

**`tombstone/test/test_minidump_reader.cpp`** (extend or new):
- Build a minimal synthetic minidump binary in-memory with a CrashpadInfo stream containing `abort_message = "test abort"` and `terminate_type = "std::logic_error"`.
- Write to `tmpfile`, call `ReadMinidump`, assert both fields populated.

**`tombstone/test/test_tombstone_formatter.cpp`** (extend):
- `FormatTombstone` with `abort_message` set → assert `"Abort message: 'test abort'"` present, positioned after signal line.
- `FormatTombstone` with both `abort_message` + `terminate_type` → assert `"Abort message: 'std::logic_error: test abort'"`.
- `FormatTombstone` with empty `abort_message` → assert no `"Abort message:"` line.

### Phase 3: `tools/analyze/test/test_annotations.py` (new, pytest)

- Reuse the same synthetic minidump fixture (or Python-generated equivalent).
- `read_minidump_annotations` returns expected dict.
- `format_symbolicated` with a `ParsedTombstone` that has `abort_message` set includes the `Abort message:` line after the signal line and before `backtrace:`.
- Empty `abort_message` → no `Abort message:` line.

---

## Invariants

- `abort_message` and `terminate_type` are never written if the `SimpleStringDictionary` pointer is null (no-op path in `crashomon_set_tag`).
- The `__assert_fail` override must not heap-allocate (it runs in a crash path); use stack buffers or `snprintf`.
- The terminate handler must call `std::abort()` at the end; it must not return or call the previous handler (which typically also calls `abort()`).
- `abi::__cxa_demangle` allocates — call `free()` on the result. If demangling fails (returns null), fall back to the raw mangled name.
