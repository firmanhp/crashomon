cmake_minimum_required(VERSION 3.21)
project(crashomon VERSION 0.1.0 LANGUAGES C CXX)

# ── Language standards ─────────────────────────────────────────────────────────
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Default build flags ────────────────────────────────────────────────────────
# Single build type: optimized with debug info.
# No separate Debug/Release — sanitizers are opt-in toggles below.
add_compile_options(-O3 -g)

# ── Sanitizer options (opt-in) ─────────────────────────────────────────────────
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TSAN  "Enable ThreadSanitizer"            OFF)
option(ENABLE_ASAN  "Enable AddressSanitizer"           OFF)

if(ENABLE_UBSAN AND ENABLE_TSAN)
  message(FATAL_ERROR "ENABLE_UBSAN and ENABLE_TSAN cannot be used together.")
endif()
if(ENABLE_ASAN AND ENABLE_TSAN)
  message(FATAL_ERROR "ENABLE_ASAN and ENABLE_TSAN cannot be used together.")
endif()

if(ENABLE_UBSAN)
  add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=undefined)
endif()
if(ENABLE_TSAN)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()
if(ENABLE_ASAN)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()

# ── clang-tidy (opt-in) ────────────────────────────────────────────────────────
option(ENABLE_CLANG_TIDY "Enable clang-tidy static analysis" OFF)
if(ENABLE_CLANG_TIDY)
  find_program(CLANG_TIDY_EXE NAMES "clang-tidy" REQUIRED)
  set(CMAKE_C_CLANG_TIDY   "${CLANG_TIDY_EXE}")
  set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
endif()

# ── Compiler warnings — applied to OUR targets only via interface library ──────
# Third-party deps (sentry-native, GoogleTest) are fetched separately and must
# NOT inherit -Werror. We use target_link_libraries(our_target PRIVATE crashomon_warnings)
# on each of our own targets.
add_library(crashomon_warnings INTERFACE)
target_compile_options(crashomon_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Werror
  -Wshadow -Wconversion -Wsign-conversion
  -Wnull-dereference -Wdouble-promotion
  -Wformat=2 -Wformat-security
  -fstack-protector-strong
)

# ── Dependencies ───────────────────────────────────────────────────────────────
include(FetchContent)

# sentry-native with Crashpad backend, no upload transport
FetchContent_Declare(sentry
  GIT_REPOSITORY https://github.com/getsentry/sentry-native.git
  GIT_TAG        0.7.17
)
set(SENTRY_BACKEND       "crashpad" CACHE STRING "" FORCE)
set(SENTRY_TRANSPORT     "none"     CACHE STRING "" FORCE)
set(SENTRY_BUILD_SHARED_LIBS OFF    CACHE BOOL   "" FORCE)
set(SENTRY_PIC           ON         CACHE BOOL   "" FORCE)
FetchContent_MakeAvailable(sentry)

# GoogleTest for unit tests
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0
)
# Prevent GoogleTest from overriding our compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ── Subdirectories ─────────────────────────────────────────────────────────────
add_subdirectory(lib)
add_subdirectory(daemon)
add_subdirectory(tools/analyze)
add_subdirectory(examples)

enable_testing()
add_subdirectory(test)

# ── Install ────────────────────────────────────────────────────────────────────
install(TARGETS crashomon_client DESTINATION lib)
install(FILES lib/crashomon.h DESTINATION include/crashomon)
install(TARGETS crashomon_watcherd crashomon_analyze DESTINATION bin)
install(DIRECTORY systemd/ DESTINATION lib/systemd/system)
