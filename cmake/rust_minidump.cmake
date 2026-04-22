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

# +crt-static: link glibc, libm, and libgcc statically — fully self-contained,
# no glibc version floor on the target.
ExternalProject_Add(rust_minidump_build
  GIT_REPOSITORY https://github.com/rust-minidump/rust-minidump.git
  GIT_TAG        v0.26.1
  GIT_SHALLOW    TRUE
  CONFIGURE_COMMAND ""
  BUILD_COMMAND
    ${CMAKE_COMMAND} -E env
      "RUSTFLAGS=-C target-feature=+crt-static"
    ${CARGO} build --release --bin minidump-stackwalk
    --target-dir ${CMAKE_BINARY_DIR}/rust_minidump_target
  BUILD_IN_SOURCE   TRUE
  INSTALL_COMMAND   ""
  BUILD_BYPRODUCTS
    ${CMAKE_BINARY_DIR}/rust_minidump_target/release/minidump-stackwalk
  BUILD_ALWAYS      FALSE
)

set(MINIDUMP_STACKWALK_EXECUTABLE
  "${CMAKE_BINARY_DIR}/rust_minidump_target/release/minidump-stackwalk"
  CACHE FILEPATH "Path to the rust-minidump minidump-stackwalk binary" FORCE)
