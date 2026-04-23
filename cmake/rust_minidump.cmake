# cmake/rust_minidump.cmake — build rust-minidump's minidump-stackwalk via Cargo
#
# Produces a single binary: minidump-stackwalk
# Requires: cargo (rustup) on PATH or in $HOME/.cargo/bin
#
# Exported variable:
#   MINIDUMP_STACKWALK_EXECUTABLE — path to the built binary

find_program(CARGO cargo
  HINTS "$ENV{HOME}/.cargo/bin"
  DOC "Cargo package manager (install via rustup)")

if(NOT CARGO)
  message(FATAL_ERROR
    "cargo not found. Install Rust via rustup: https://rustup.rs/\n"
    "Then re-run cmake.")
endif()

include(ExternalProject)

# Determine the Rust host triple so we can use CARGO_TARGET_<TRIPLE>_RUSTFLAGS
# instead of RUSTFLAGS.  CARGO_TARGET_… (Cargo ≥1.64) applies +crt-static only
# to the final binary, not to proc-macro crates, avoiding:
#   "cannot produce proc-macro … as the target does not support these crate types"
# which RUSTFLAGS=-C target-feature=+crt-static triggers on *-linux-gnu hosts.
get_filename_component(_CARGO_DIR "${CARGO}" DIRECTORY)
find_program(_RUSTC rustc HINTS "${_CARGO_DIR}" "$ENV{HOME}/.cargo/bin" NO_DEFAULT_PATH)
execute_process(
  COMMAND "${_RUSTC}" --print host
  OUTPUT_VARIABLE _RUST_HOST_TRIPLE
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
string(TOUPPER  "${_RUST_HOST_TRIPLE}" _RUST_TRIPLE_UPPER)
string(REPLACE  "-" "_" _RUST_TRIPLE_UPPER "${_RUST_TRIPLE_UPPER}")
set(_RUSTFLAGS_VAR "CARGO_TARGET_${_RUST_TRIPLE_UPPER}_RUSTFLAGS")

# +crt-static: link glibc/libm/libgcc statically — fully self-contained,
# no glibc version floor on the target device.
ExternalProject_Add(rust_minidump_build
  GIT_REPOSITORY https://github.com/rust-minidump/rust-minidump.git
  GIT_TAG        v0.26.1
  GIT_SHALLOW    TRUE
  CONFIGURE_COMMAND ""
  BUILD_COMMAND
    ${CMAKE_COMMAND} -E env
      "${_RUSTFLAGS_VAR}=-C target-feature=+crt-static"
    ${CARGO} build --release --bin minidump-stackwalk
    --target-dir ${CMAKE_BINARY_DIR}/rust_minidump_target
  BUILD_IN_SOURCE   TRUE
  INSTALL_COMMAND   ""
  BUILD_BYPRODUCTS
    ${CMAKE_BINARY_DIR}/rust_minidump_target/release/minidump-stackwalk
  BUILD_ALWAYS      FALSE
)

unset(_CARGO_DIR)
unset(_RUSTC)
unset(_RUST_HOST_TRIPLE)
unset(_RUST_TRIPLE_UPPER)
unset(_RUSTFLAGS_VAR)

set(MINIDUMP_STACKWALK_EXECUTABLE
  "${CMAKE_BINARY_DIR}/rust_minidump_target/release/minidump-stackwalk"
  CACHE FILEPATH "Path to the rust-minidump minidump-stackwalk binary" FORCE)
