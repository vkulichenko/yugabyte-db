# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#
# The following only applies to changes made to this file as part of YugaByte development.
#
# Portions Copyright (c) YugaByte, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
# in compliance with the License.  You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software distributed under the License
# is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
# or implied.  See the License for the specific language governing permissions and limitations
# under the License.
#

# Require cmake that includes FindICU module (https://cmake.org/cmake/help/v3.7/release/3.7.html)
cmake_minimum_required(VERSION 3.7.0)

project(YugabyteDB)

if("$CACHE{YB_PCH_ON}" STREQUAL "")
  if("$ENV{YB_USE_PCH}" STREQUAL "1")
    set(YB_USE_PCH ON)
  else()
    set(YB_USE_PCH OFF)
  endif()
  set(YB_PCH_ON ${YB_USE_PCH} CACHE BOOL "Whether to use precompiled headers")
endif()

message("-- CMAKE_SYSTEM_INFO_FILE: ${CMAKE_SYSTEM_INFO_FILE}")
message("-- CMAKE_SYSTEM_NAME:      ${CMAKE_SYSTEM_NAME}")
message("-- CMAKE_SYSTEM_PROCESSOR: ${CMAKE_SYSTEM_PROCESSOR}")
message("-- CMAKE_SYSTEM:           ${CMAKE_SYSTEM}")
message("-- CMAKE_VERSION:          ${CMAKE_VERSION}")
message("-- PRECOMPILED HEADERS:    ${YB_PCH_ON}")

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake_modules")
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake_standard_modules")
include(YugabyteFunctions)
yb_initialize_constants()
include(YugabyteTesting)

ADD_CXX_FLAGS("-Werror")
ADD_CXX_FLAGS("-fno-strict-aliasing -Wall")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  # Certain platforms such as ARM do not use signed chars by default
  # which causes issues with certain bounds checks.
  ADD_CXX_FLAGS("-fsigned-char")
  # Turn off fp-contract on aarch64 to avoid multiply-add operation result difference.
  ADD_CXX_FLAGS("-ffp-contract=off")
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
  ADD_CXX_FLAGS("-msse4.2")
else()
  message(FATAL_ERROR "Unsupported CPU architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
  # Optimize for Graviton on Linux/aarch64 (not mac/arm64)
  ADD_CXX_FLAGS("-march=armv8.2-a+fp16+rcpc+dotprod+crypto")
  ADD_CXX_FLAGS("-mtune=neoverse-n1")
  ADD_CXX_FLAGS("-mno-outline-atomics")
endif()

if (NOT "$ENV{YB_TARGET_ARCH}" STREQUAL "" AND
    NOT "$ENV{YB_TARGET_ARCH}" STREQUAL "${CMAKE_SYSTEM_PROCESSOR}")
  message(FATAL_ERROR "YB_TARGET_ARCH is set to $ENV{YB_TARGET_ARCH} but CMAKE_SYSTEM_PROCESSOR "
                      "is ${CMAKE_SYSTEM_PROCESSOR}.")
endif()
set(ENV{YB_TARGET_ARCH} "${CMAKE_SYSTEM_PROCESSOR}")

ADD_CXX_FLAGS("-Winvalid-pch")
ADD_CXX_FLAGS("-pthread -DBOOST_BIND_NO_PLACEHOLDERS")
ADD_LINKER_FLAGS("-pthread")
if (NOT APPLE)
  ADD_CXX_FLAGS("-DBOOST_UUID_RANDOM_PROVIDER_FORCE_POSIX")
endif()
ADD_CXX_FLAGS("-DROCKSDB_PLATFORM_POSIX")
ADD_CXX_FLAGS("-DBOOST_ERROR_CODE_HEADER_ONLY")

set(YB_NUM_TESTS "0" CACHE INTERNAL "Number of tests" FORCE)
set(YB_NUM_INCLUDED_TESTS "0" CACHE INTERNAL "Number of included tests" FORCE)
set(YB_NUM_EXECUTABLES "0" CACHE INTERNAL "Number of executables" FORCE)
set(YB_NUM_INCLUDED_EXECUTABLES "0" CACHE INTERNAL "Number of included executables" FORCE)
set(YB_ALL_DEPS "" CACHE INTERNAL "All dependencies" FORCE)

# This is used to let the add_executable wrapper know if we're adding a test.
set(YB_ADDING_TEST_EXECUTABLE "FALSE" CACHE INTERNAL "" FORCE)

if(NOT "$ENV{YB_DO_NOT_BUILD_TESTS}" STREQUAL "")
  if("$ENV{YB_DO_NOT_BUILD_TESTS}" STREQUAL "1")
    message("YB_DO_NOT_BUILD_TESTS is set to 1, will not build tests")
    set(BUILD_TESTS OFF)
  elseif("$ENV{YB_DO_NOT_BUILD_TESTS}" STREQUAL "0")
    message("YB_DO_NOT_BUILD_TESTS is set to 0, will build tests")
    set(BUILD_TESTS ON)
  else()
    message(FATAL_ERROR
            "Invalid value of the YB_DO_NOT_BUILD_TESTS environment variable, expected 0 or 1 but "
            "got '$ENV{YB_DO_NOT_BUILD_TESTS}'.")
  endif()
  set(BUILD_TESTS ${BUILD_TESTS} CACHE BOOL "Whether to build tests")
elseif("$CACHE{BUILD_TESTS}" STREQUAL "")
  set(BUILD_TESTS ON)
  message("Will build tests by default")
else()
  message("BUILD_TESTS from cache: ${BUILD_TESTS} (set YB_DO_NOT_BUILD_TESTS to 0 or 1 to change)")
endif()

parse_build_root_basename()
if("${YB_BUILD_TYPE}" STREQUAL "")
  message(FATAL_ERROR "YB_BUILD_TYPE still not set after parse_build_root_basename")
endif()

message("YB_BUILD_TYPE: ${YB_BUILD_TYPE}")
message("CMAKE_MAKE_PROGRAM: ${CMAKE_MAKE_PROGRAM}")

# CMAKE_LINK_DEPENDS_NO_SHARED prevents prevent re-linking dependents of a shared library when it
# is re-linked. Enabling the optimized behavior by default, and allowing to customize it with the
# YB_CMAKE_LINK_DEPENDS_NO_SHARED environment variable.
if (NOT "$ENV{YB_CMAKE_LINK_DEPENDS_NO_SHARED}" STREQUAL "")
  message(
    "Setting CMAKE_LINK_DEPENDS_NO_SHARED to '$ENV{YB_CMAKE_LINK_DEPENDS_NO_SHARED}' "
    "based on the YB_CMAKE_LINK_DEPENDS_NO_SHARED environment variable.")
  set(CMAKE_LINK_DEPENDS_NO_SHARED $ENV{YB_CMAKE_LINK_DEPENDS_NO_SHARED})
else()
  set(CMAKE_LINK_DEPENDS_NO_SHARED 1)
endif()

set(YB_FILTERING_TARGETS FALSE)
if (NOT "${YB_TEST_FILTER_RE}" STREQUAL "" OR NOT "${YB_EXECUTABLE_FILTER_RE}" STREQUAL "")
  set(YB_FILTERING_TARGETS TRUE)
endif()

set(YB_SRC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
message("YB_SRC_ROOT: ${YB_SRC_ROOT}")

set(YB_THIRDPARTY_DIR "$ENV{YB_THIRDPARTY_DIR}")
if("${YB_THIRDPARTY_DIR}" STREQUAL "")
  if(EXISTS "${CMAKE_CURRENT_BINARY_DIR}/thirdparty_path.txt")
    file(STRINGS "${CMAKE_CURRENT_BINARY_DIR}/thirdparty_path.txt" YB_THIRDPARTY_DIR)
  else()
    set(YB_THIRDPARTY_DIR "${YB_SRC_ROOT}/thirdparty")
  endif()
  set(ENV{YB_THIRDPARTY_DIR} "${YB_THIRDPARTY_DIR}")
endif()
message("YB_THIRDPARTY_DIR: ${YB_THIRDPARTY_DIR}")

set(YB_BUILD_ROOT "${CMAKE_CURRENT_BINARY_DIR}")
set(ENV{YB_BUILD_ROOT} "${YB_BUILD_ROOT}")
get_filename_component(YB_BUILD_ROOT_PARENT "${YB_BUILD_ROOT}" DIRECTORY)
message("YB_BUILD_ROOT: ${YB_BUILD_ROOT}")

message("YB_TARGET_ARCH: $ENV{YB_TARGET_ARCH}")

DETECT_NUMBER_OF_PROCESSORS()

# Detect the shared library suffix on this platform
set(YB_STATIC_LIBRARY_SUFFIX ".a")
if(APPLE)
  set(YB_SHARED_LIBRARY_SUFFIX ".dylib")
else()
  set(YB_SHARED_LIBRARY_SUFFIX ".so")
endif()
message("Using shared library suffix '${YB_SHARED_LIBRARY_SUFFIX}'.")

message("CMAKE_C_COMPILER=${CMAKE_C_COMPILER}")
CHECK_YB_COMPILER_PATH(${CMAKE_C_COMPILER})

message("CMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
CHECK_YB_COMPILER_PATH(${CMAKE_CXX_COMPILER})

# Provide a 'latest' symlink to this build directory if the "blessed" multi-build layout is
# detected:
#
# build/
# build/<first build directory>
# build/<second build directory>
# ...
set(LATEST_BUILD_SYMLINK_PATH "${YB_BUILD_ROOT_PARENT}/latest")
if (NOT "$ENV{YB_DISABLE_LATEST_SYMLINK}" STREQUAL "1")
  message("LATEST SYMLINK PATH: ${LATEST_BUILD_SYMLINK_PATH}")
  if ("${CMAKE_CURRENT_BINARY_DIR}" STREQUAL "${LATEST_BUILD_SYMLINK_PATH}")
    message(FATAL_ERROR "Should not run cmake inside the build/latest symlink. "
            "First change directories into the destination of the symlink.")
  endif()

  # This option is needed in addition to -sf, while linking, to force the link to occur on
  # directories. Unfortunately, Linux & MacOS differ on the option name.
  if (NOT APPLE)
    set(MORE_ARGS "-T")
  else()
    set(MORE_ARGS "-h")
  endif()
  add_custom_target(latest_symlink ALL
    "${BUILD_SUPPORT_DIR}/create_latest_symlink.sh"
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${LATEST_BUILD_SYMLINK_PATH}"
    COMMENT "Recreating the 'latest' symlink at '${LATEST_BUILD_SYMLINK_PATH}'")
endif()

add_custom_target(dummy_target ALL
  COMMAND cat /dev/null
  COMMENT "Dummy target for dependency resolution testing")

include(CMakeParseArguments)

# Allow "make install" to not depend on all targets.
#
# Must be declared in the top-level CMakeLists.txt.
set(CMAKE_SKIP_INSTALL_ALL_DEPENDENCY true)

# -------------------------------------------------------------------------------------------------
# Decide whether we will be rebuilding third-party dependencies.
# -------------------------------------------------------------------------------------------------

set(REBUILD_THIRDPARTY TRUE)
# Also allow specifying -DNO_REBUILD_THIRDPARTY, because CLion does not always pass user-specified
# environment variables correctly.
if ((NOT "$ENV{NO_REBUILD_THIRDPARTY}" STREQUAL "") OR
    ("${YB_NO_REBUILD_THIRDPARTY}" STREQUAL "1") OR
    (NOT "${YB_THIRDPARTY_DIR}" STREQUAL "${YB_SRC_ROOT}/thirdparty"))
  message("Decided that we should not rebuild third-party dependencies. Criteria: "
          "NO_REBUILD_THIRDPARTY env var: '$ENV{NO_REBUILD_THIRDPARTY}', "
          "YB_NO_REBUILD_THIRDPARTY CMake var: '${YB_NO_REBUILD_THIRDPARTY}', "
          "YB_THIRDPARTY_DIR: ${YB_THIRDPARTY_DIR}.")
  set(REBUILD_THIRDPARTY FALSE)
endif()

# -------------------------------------------------------------------------------------------------

include(CompilerInfo)

# This helps find the right third-party build directory.
if ("${YB_BUILD_TYPE}" MATCHES "^(asan|tsan)$")
  set(THIRDPARTY_INSTRUMENTATION_TYPE "${YB_BUILD_TYPE}")
elseif (IS_CLANG)
  set(THIRDPARTY_INSTRUMENTATION_TYPE "uninstrumented")
elseif (IS_GCC)
  set(THIRDPARTY_INSTRUMENTATION_TYPE "uninstrumented")
else()
  message(FATAL_ERROR "Unknown compiler family: '${COMPILER_FAMILY}'.")
endif()

message("THIRDPARTY_INSTRUMENTATION_TYPE=${THIRDPARTY_INSTRUMENTATION_TYPE}")

# Make sure third-party dependency is up-to-date.
# TODO: do not invoke this step as part of the build. Always invoke it separately.
if (REBUILD_THIRDPARTY)
  set(BUILD_THIRDPARTY_ARGS --build-type ${THIRDPARTY_INSTRUMENTATION_TYPE})
  message("Invoking build_thirdparty.sh with these arguments: ${BUILD_THIRDPARTY_ARGS}")
  execute_process(
    COMMAND ${YB_SRC_ROOT}/build-support/invoke_thirdparty_build.sh ${BUILD_THIRDPARTY_ARGS}
    RESULT_VARIABLE THIRDPARTY_SCRIPT_RESULT)
  if (NOT (${THIRDPARTY_SCRIPT_RESULT} EQUAL 0))
    message(FATAL_ERROR "Thirdparty was built unsuccessfully, terminating.")
  endif()
else()
  message("Skipping the third-party build (reasons logged earlier).")
endif()

if(NOT INCLUDE_COMPILER_INFO_EARLY)
  message("Including the CompilerInfo module after we have considered building third-party "
          "dependencies.")
  include(CompilerInfo)
endif()

# Generate a compile_commands.json "compilation database" file.
# See http://clang.llvm.org/docs/JSONCompilationDatabase.html
if (CMAKE_EXPORT_COMPILE_COMMANDS)
  message("CMAKE_EXPORT_COMPILE_COMMANDS is already enabled, will export compile_commands.json.")
elseif ("$ENV{YB_EXPORT_COMPILE_COMMANDS}" STREQUAL "1")
  message("The YB_EXPORT_COMPILE_COMMANDS environment variable is set to 1, will export "
          "compile_commands.json.")
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
elseif ("$ENV{YB_RUN_AFFECTED_TESTS_ONLY}" STREQUAL "1")
  message("YB_RUN_AFFECTED_TESTS_ONLY is set to 1, will export compile_commands.json.")
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
else()
  message("CMake will not export the compilation database")
endif()

# -------------------------------------------------------------------------------------------------
# Build type (debug, release, fastdebug, etc.)
# -------------------------------------------------------------------------------------------------

# If no build type is specified, default to debug builds
if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif(NOT CMAKE_BUILD_TYPE)

string (TOUPPER ${CMAKE_BUILD_TYPE} CMAKE_BUILD_TYPE)

# Alias RELEASE as RELWITHDEBINFO and MINSIZEREL. These are common CMake
# release type names and this provides compatibility with the CLion IDE.
if ("${CMAKE_BUILD_TYPE}" STREQUAL "RELWITHDEBINFO" OR "${CMAKE_BUILD_TYPE}" STREQUAL "MINSIZEREL")
  set(CMAKE_BUILD_TYPE RELEASE)
endif()

VALIDATE_COMPILER_TYPE()
DETECT_BREW()
enable_lto_if_needed()

message("Using COMPILER_FAMILY=${COMPILER_FAMILY}")

if (NOT APPLE AND "${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "x86_64")
  # To enable 16-byte atomics support we should specify appropriate architecture.
  ADD_CXX_FLAGS("-march=ivybridge")
  ADD_CXX_FLAGS("-mcx16")
endif()

# Include compiler type and version in the compiler command line so that binaries built by different
# versions of the compiler will have different keys in ccache.
ADD_CXX_FLAGS("-DYB_COMPILER_TYPE=$ENV{YB_COMPILER_TYPE}")
ADD_CXX_FLAGS("-DYB_COMPILER_VERSION=${COMPILER_VERSION}")
ADD_CXX_FLAGS("-DROCKSDB_LIB_IO_POSIX")
ADD_CXX_FLAGS("-DSNAPPY")
ADD_CXX_FLAGS("-DLZ4")
ADD_CXX_FLAGS("-DZLIB")

############################################################
# Compiler flags
############################################################

if(NOT APPLE AND "${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "x86_64")
  # The following flags are required to not assume the presence of too many CPU features so that the
  # code built can run on many platforms. For example, support building on c4.xlarge in AWS (dev
  # servers) and running on c3.xlarge (flash-based cluster machines). There are a couple more flags
  # (-mno-abm and -mno-movbe) that are not recognized by some clang versions we are using, so they
  # are being added in the gcc-specific section below.  We have also found that these flags don't
  # work on Mac OS X, so we're not using them there.
  ADD_CXX_FLAGS("-mno-avx -mno-bmi -mno-bmi2 -mno-fma")
endif()

# We want access to the PRI* print format macros.
ADD_CXX_FLAGS("-D__STDC_FORMAT_MACROS")

# Do not warn about uses of deprecated declarations. RocksDB has a few instances of those.
ADD_CXX_FLAGS("-Wno-deprecated-declarations")

ADD_CXX_FLAGS("-DGFLAGS=gflags")

# Don't allow virtual classes with non-virtual destructors.
ADD_CXX_FLAGS("-Wnon-virtual-dtor")

# Flags common to gcc and clang.
ADD_CXX_FLAGS("-Werror=enum-compare")
ADD_CXX_FLAGS("-Werror=reorder")
ADD_CXX_FLAGS("-Werror=switch")
ADD_CXX_FLAGS("-Werror=return-type")
ADD_CXX_FLAGS("-Werror=non-virtual-dtor")

if(IS_CLANG)
  ADD_CXX_FLAGS("-Werror=string-plus-int")
  ADD_CXX_FLAGS("-Werror=return-stack-address")
  ADD_CXX_FLAGS("-Werror=implicit-fallthrough")
  if("${COMPILER_VERSION}" VERSION_GREATER_EQUAL "11.0.0")
    # Clang versions earlier than 11 cannot correctly handle unique_lock with manual
    # locking/unlocking.
    ADD_CXX_FLAGS("-D_LIBCPP_ENABLE_THREAD_SAFETY_ANNOTATIONS")
    ADD_CXX_FLAGS("-Wthread-safety-analysis")
  endif()
  if("$ENV{YB_ENABLE_STATIC_ANALYZER}" STREQUAL "1")
    if(APPLE)
      message("YB_ENABLE_STATIC_ANALYZER is set to 1, but we are not running the static analyzer"
              " on macOS yet")
    else()
      message("YB_ENABLE_STATIC_ANALYZER is set to 1, enabling Clang static analyzer")
      include(yb_clang_analyzer)
      message("clang_analyzer_flags=${clang_analyzer_flags}")
      ADD_CXX_FLAGS("${clang_analyzer_flags}")
    endif()
  else()
    message("YB_ENABLE_STATIC_ANALYZER is not set to 1, not enabling Clang static analyzer")
  endif()

  ADD_CXX_FLAGS("-Wshorten-64-to-32")
endif()

if(USING_LINUXBREW AND IS_CLANG)
  # We are only using Clang with Linuxbrew. We never use Linuxbrew GCC 5 anymore, it is too old.
  ADD_LINKER_FLAGS("-Wl,--dynamic-linker=${LINUXBREW_LIB_DIR}/ld.so")
  ADD_LINKER_FLAGS("--gcc-toolchain=${LINUXBREW_DIR}")
endif()

if(IS_GCC)
  # For now code relies on fact that all libraries will be linked to binary (for using FLAGS_*)
  # This flag is enabled implicitly on centos but not on ubuntu
  # TODO: Subtitute it with '-as-needed' instead to avoid linking with unused library (issue #1495)
  # for reducing pocess start time
  ADD_LINKER_FLAGS("-Wl,-no-as-needed")
  if ("${COMPILER_VERSION}" MATCHES "^11[.].*$")
    # To silence these spurious warnings:
    # https://gist.githubusercontent.com/mbautin/60c7fb897a92b998a111ff38429a1158/raw
    # https://gist.githubusercontent.com/mbautin/56b95b7a6816ccdf77eaa60f53a7d3ef/raw
    ADD_CXX_FLAGS("-Wno-array-bounds")
    ADD_CXX_FLAGS("-Wno-stringop-overflow")
    ADD_CXX_FLAGS("-Wno-stringop-overread")
  endif()
endif()

if(USING_LINUXBREW)
  # This is needed for finding correct versions of Flex, Bison, and other tools.
  set(CMAKE_PREFIX_PATH "${LINUXBREW_DIR}" ${CMAKE_PREFIX_PATH})
endif()

# For both RocksDB and YugaByte code, we need to set the OS flag on Mac. Some code in RocksDB also
# distinguishes further into BSD vs everything else, but we probably do not care further.
if(APPLE)
  ADD_CXX_FLAGS("-DOS_MACOSX")
elseif(NOT "${YB_BUILD_TYPE}" MATCHES "^(asan|tsan)$")
  message("Specifying linker flags to not allow any undefined symbols")
  ADD_LINKER_FLAGS("-Wl,--no-undefined -Wl,--no-allow-shlib-undefined")
else()
  message("Not specifying linker flags to not allow any undefined symbols: this is ASAN/TSAN")
endif()

# compiler flags for different build types (run 'cmake -DCMAKE_BUILD_TYPE=<type> .')
# For all builds:
# For CMAKE_BUILD_TYPE=Debug
#   -ggdb: Enable gdb debugging
# For CMAKE_BUILD_TYPE=FastDebug
#   Same as DEBUG, except with some optimizations on.
# For CMAKE_BUILD_TYPE=Release
#   -O3: Enable all compiler optimizations
#   -g: Enable symbols for profiler tools (TODO: remove for shipping)
#   -DNDEBUG: Turn off dchecks/asserts/debug only code.
#   -fno-omit-frame-pointer
#       use frame pointers to allow simple stack frame walking for backtraces.
#       This has a small perf hit but worth it for the ability to profile in production
# For profile guided optimization (PGO) builds, in addition to the flags for release builds:
#   1. Build first with CMAKE_BUILD_TYPE_PROFILE_GEN:
#     -fprofile-generate: Indicates compiler should insert profile guided optimization events
#   2. Run the benchmarks (generates *.gcda profiling data).
#   3. Build again with CMAKE_BUILD_TYPE_PROFILE_BUILD
#     -fprofile-use: Compiler will use the profile outputs for optimizations
set(CXX_FLAGS_DEBUG "-ggdb")
set(CXX_FLAGS_FASTDEBUG "-ggdb -O1 -fno-omit-frame-pointer -DFASTDEBUG")
set(CXX_FLAGS_RELEASE "-O3 -g -DNDEBUG -fno-omit-frame-pointer")

# Nullify CMake's predefined flags since we are handling different build types on our own.
# Without this change CMake will add flags from these variables and
# optimization level will be changed because in case of multiple -O flags gcc uses the last one.
set(CMAKE_CXX_FLAGS_DEBUG "")
set(CMAKE_CXX_FLAGS_RELEASE "")

set(CXX_FLAGS_PROFILE_GEN "${CXX_FLAGS_RELEASE} -fprofile-generate")
set(CXX_FLAGS_PROFILE_BUILD "${CXX_FLAGS_RELEASE} -fprofile-use")

set(CLANG_GCC_TOOLCHAIN "")
if(IS_CLANG AND NOT APPLE AND USING_LINUXBREW)
  set(CLANG_GCC_TOOLCHAIN "${LINUXBREW_DIR}")
endif()

# Set compile flags based on the build type.
message("Configured for ${CMAKE_BUILD_TYPE} build "
        "(set with cmake -DCMAKE_BUILD_TYPE={release,debug,...})")
if ("${CMAKE_BUILD_TYPE}" STREQUAL "DEBUG")
  ADD_CXX_FLAGS("${CXX_FLAGS_DEBUG}")
elseif ("${CMAKE_BUILD_TYPE}" STREQUAL "FASTDEBUG")
  ADD_CXX_FLAGS("${CXX_FLAGS_FASTDEBUG}")
  # We specify RocksDB debug level that corresponds to the -O1 optimization level, the same as
  # the rest of YB code in the "fastdebug" mode (used for ASAN/TSAN).
elseif ("${CMAKE_BUILD_TYPE}" STREQUAL "RELEASE")
  ADD_CXX_FLAGS("${CXX_FLAGS_RELEASE}")
elseif ("${CMAKE_BUILD_TYPE}" STREQUAL "PROFILE_GEN")
  ADD_CXX_FLAGS("${CXX_FLAGS_PROFILE_GEN}")
elseif ("${CMAKE_BUILD_TYPE}" STREQUAL "PROFILE_BUILD")
  ADD_CXX_FLAGS("${CXX_FLAGS_PROFILE_BUILD}")
else()
  message(FATAL_ERROR "Unknown build type: ${CMAKE_BUILD_TYPE}")
endif ()

if (IS_CLANG)
  # Using Clang with ccache causes a bunch of spurious warnings that are
  # purportedly fixed in the next version of ccache. See the following for details:
  #
  #   http://petereisentraut.blogspot.com/2011/05/ccache-and-clang.html
  #   http://petereisentraut.blogspot.com/2011/09/ccache-and-clang-part-2.html

  # Clang generates ambiguous member template warnings when calling the ev++ api.
  ADD_CXX_FLAGS("-Wno-ambiguous-member-template")

  # Emit warnings on unannotated fallthrough in switch statements. This applies to both YB and
  # RocksDB parts of the code.
  ADD_CXX_FLAGS("-Wimplicit-fallthrough")

  # Silence the "unused argument" warning.
  ADD_CXX_FLAGS("-Qunused-arguments")

  # Only hardcode -fcolor-diagnostics if stderr is opened on a terminal. Otherwise
  # the color codes show up as noisy artifacts.
  #
  # This test is imperfect because 'cmake' and 'make' can be run independently
  # (with different terminal options), and we're testing during the former.
  execute_process(COMMAND test -t 2 RESULT_VARIABLE YB_IS_TTY)
  if ((${YB_IS_TTY} EQUAL 0) AND (NOT ("$ENV{TERM}" STREQUAL "dumb")))
    message("Running in a controlling terminal")
    ADD_CXX_FLAGS("-fcolor-diagnostics")
  else()
    message("Running without a controlling terminal or in a dumb terminal")
  endif()

  if ("${COMPILER_VERSION}" VERSION_GREATER_EQUAL "12.0.0" AND APPLE)
    ADD_CXX_FLAGS("-Wno-c++17-compat-mangling")
  endif()
elseif(IS_GCC)
  if ("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "x86_64")
    ADD_CXX_FLAGS("-mno-abm -mno-movbe")
  endif()
else()
  message(FATAL_ERROR "Unknown compiler family: ${COMPILER_FAMILY}")
endif()

set(YB_PREFIX_COMMON "${YB_THIRDPARTY_DIR}/installed/common")

if(NOT APPLE AND IS_CLANG)
  YB_SETUP_CLANG()
  if ("${YB_BUILD_TYPE}" MATCHES "^(asan|tsan)$")
    YB_SETUP_SANITIZER()
  endif()
endif()

if (USING_LINUXBREW)
  include_directories(SYSTEM "${LINUXBREW_DIR}/include")
endif()

if (NOT IS_CLANG)
  ADD_LINKER_FLAGS("-latomic")
endif()

# Flag to enable clang undefined behavior sanitizer
# We explicitly don't enable all of the sanitizer flags:
# - disable 'vptr' because it currently crashes somewhere in boost::intrusive::list code
# - disable 'alignment' because unaligned access is really OK on Nehalem and we do it
#   all over the place.
if ("${YB_BUILD_TYPE}" STREQUAL "asan")
  if(NOT (IS_CLANG OR
          IS_GCC AND "${COMPILER_VERSION}" VERSION_GREATER "4.9"))
    message(SEND_ERROR "Cannot use UBSAN without clang or gcc >= 4.9")
  endif()
  set(CXX_NO_SANITIZE_FLAG "alignment")
  if(NOT APPLE)
    set(CXX_NO_SANITIZE_FLAG "${CXX_NO_SANITIZE_FLAG},vptr")
    ADD_CXX_FLAGS("-fsanitize=undefined")
  endif()
  ADD_CXX_FLAGS("-fno-sanitize-recover=all -fno-sanitize=${CXX_NO_SANITIZE_FLAG}")
  ADD_CXX_FLAGS("-fsanitize-recover=float-cast-overflow")
endif ()

if ("${YB_BUILD_TYPE}" MATCHES "^(asan|tsan)$")
  # GCC 4.8 and 4.9 (latest as of this writing) don't allow you to specify a
  # sanitizer blacklist.
  if(IS_CLANG)
    # Require clang 3.4 or newer; clang 3.3 has issues with TSAN and pthread
    # symbol interception.
    if("${COMPILER_VERSION}" VERSION_LESS "3.4")
      message(SEND_ERROR "Must use clang 3.4 or newer to run a sanitizer build."
        " Try using clang from thirdparty/")
    endif()
    ADD_CXX_FLAGS("-fsanitize-blacklist=${BUILD_SUPPORT_DIR}/sanitize-blacklist.txt")
  else()
    message(WARNING "GCC does not support specifying a sanitizer blacklist. Known sanitizer "
                    "check failures will not be suppressed.")
  endif()
endif()

set(BUILD_SHARED_LIBS ON)
if ("${YB_BUILD_TYPE}" STREQUAL "prof_gen")
  ADD_CXX_FLAGS("-fprofile-instr-generate -DYB_PROFGEN")
endif ()

if ("${YB_BUILD_TYPE}" STREQUAL "prof_use")
  if (NOT YB_PGO_DATA_PATH)
    message (SEND_ERROR "Pgo data path is not set.")
  endif()
  ADD_CXX_FLAGS("-fprofile-instr-use=${YB_PGO_DATA_PATH}")
  # Even with the fresh profile data we might get warnings like
  # warning: Function control flow change detected (hash mismatch)
  #    [-Wbackend-plugin]
  # Silencing it for now.
  ADD_CXX_FLAGS("-Wno-backend-plugin")
  ADD_CXX_FLAGS("-Wno-profile-instr-unprofiled")
  ADD_CXX_FLAGS("-Wno-profile-instr-out-of-date")
endif ()

# Position independent code is only necessary when producing shared objects.
ADD_CXX_FLAGS(-fPIC)

# where to put generated archives (.a files)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib")
file(MAKE_DIRECTORY "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}")

# where to put generated libraries (.so files)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib")
file(MAKE_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")

# where to put generated binaries
set(EXECUTABLE_OUTPUT_PATH "${YB_BUILD_ROOT}/bin")
file(MAKE_DIRECTORY "${EXECUTABLE_OUTPUT_PATH}")

# Generated sources always have higher priority than even the "ent" directory include files.
include_directories(${CMAKE_CURRENT_BINARY_DIR}/src)
include_directories(ent/src)
include_directories(src)

enable_testing()

if (USING_LINUXBREW)
  ADD_GLOBAL_RPATH_ENTRY_AND_LIB_DIR("${YB_BUILD_ROOT}/postgres/lib")
  ADD_GLOBAL_RPATH_ENTRY_AND_LIB_DIR("${LINUXBREW_LIB_DIR}")
endif()

############################################################
# Dependencies
############################################################
function(ADD_THIRDPARTY_LIB LIB_NAME)
  set(options)
  set(one_value_args SHARED_LIB STATIC_LIB)
  set(multi_value_args DEPS)
  cmake_parse_arguments(ARG "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})
  if(ARG_UNPARSED_ARGUMENTS)
    message(SEND_ERROR "Error: unrecognized arguments: ${ARG_UNPARSED_ARGUMENTS}")
  endif()

  if(NOT ARG_SHARED_LIB)
    if(NOT ARG_STATIC_LIB)
      message(FATAL_ERROR "No static or shared library provided for ${LIB_NAME}")
    endif()
    add_library(${LIB_NAME} STATIC IMPORTED)
    set_target_properties(${LIB_NAME}
      PROPERTIES IMPORTED_LOCATION "${ARG_STATIC_LIB}")
    message("Added static library dependency ${LIB_NAME}: ${ARG_STATIC_LIB}")
  else()
    add_library(${LIB_NAME} SHARED IMPORTED)
    set_target_properties(${LIB_NAME}
      PROPERTIES IMPORTED_LOCATION "${ARG_SHARED_LIB}")
    message("Added shared library dependency ${LIB_NAME}: ${ARG_SHARED_LIB}")
  endif()

  if(ARG_DEPS)
    set_target_properties(${LIB_NAME}
      PROPERTIES IMPORTED_LINK_INTERFACE_LIBRARIES "${ARG_DEPS}")
  endif()

  # Set up an "exported variant" for this thirdparty library (see "Visibility"
  # above). It's the same as the real target, just with an "_exported" suffix.
  # We prefer the static archive if it exists (as it's akin to an "internal"
  # library), but we'll settle for the shared object if we must.
  #
  # A shared object exported variant will force any "leaf" library that
  # transitively depends on it to also depend on it at runtime; this is
  # desirable for some libraries (e.g. cyrus_sasl).
  set(LIB_NAME_EXPORTED ${LIB_NAME}_exported)
  if(ARG_STATIC_LIB)
    add_library(${LIB_NAME_EXPORTED} STATIC IMPORTED)
    set_target_properties(${LIB_NAME_EXPORTED}
      PROPERTIES IMPORTED_LOCATION "${ARG_STATIC_LIB}")
  else()
    add_library(${LIB_NAME_EXPORTED} SHARED IMPORTED)
    set_target_properties(${LIB_NAME_EXPORTED}
      PROPERTIES IMPORTED_LOCATION "${ARG_SHARED_LIB}")
  endif()
  if(ARG_DEPS)
    set_target_properties(${LIB_NAME_EXPORTED}
      PROPERTIES IMPORTED_LINK_INTERFACE_LIBRARIES "${ARG_DEPS}")
  endif()
endfunction()

# Look in thirdparty prefix paths before anywhere else for system dependencies.
set(CMAKE_PREFIX_PATH ${YB_PREFIX_COMMON} ${CMAKE_PREFIX_PATH})
ADD_LINKER_FLAGS("-L${YB_PREFIX_COMMON}/lib")

set(YB_THIRDPARTY_INSTALLED_DEPS_DIR
    "${YB_THIRDPARTY_DIR}/installed/${THIRDPARTY_INSTRUMENTATION_TYPE}")
set(CMAKE_PREFIX_PATH ${YB_THIRDPARTY_INSTALLED_DEPS_DIR} ${CMAKE_PREFIX_PATH})
ADD_LINKER_FLAGS("-L${YB_THIRDPARTY_INSTALLED_DEPS_DIR}/lib")

set(YB_THIRDPARTY_INSTALLED_DIR ${YB_PREFIX_COMMON})

# See thirdparty/yb_build_thirdparty_main.py for an explanation of differences between installed and
# installed-deps.
ADD_GLOBAL_RPATH_ENTRY("${YB_THIRDPARTY_INSTALLED_DIR}/lib")
ADD_GLOBAL_RPATH_ENTRY("${YB_THIRDPARTY_INSTALLED_DEPS_DIR}/lib")

# If we are using a non-default gcc or clang compiler, we need to add its library directory to
# rpath.
if(IS_GCC AND NOT "$ENV{YB_GCC_PREFIX}" STREQUAL "")
  # TODO: this works for the gcc 6.2.0 build on Linux, might be different on other platforms.
  ADD_GLOBAL_RPATH_ENTRY("$ENV{YB_GCC_PREFIX}/lib64")
endif()
if(IS_CLANG AND NOT "$ENV{YB_CLANG_PREFIX}" STREQUAL "")
  # TODO: this works for the Linux binary clang 3.9 package, might be different on Mac OS X.
  ADD_GLOBAL_RPATH_ENTRY("$ENV{YB_CLANG_PREFIX}/lib")
endif()

# -------------------------------------------------------------------------------------------------

include(YugabyteFindThirdParty)

############################################################
# Linker setup
############################################################
set(YB_MIN_TEST_LIBS yb_test_main yb_test_util ${YB_BASE_LIBS})
set(YB_TEST_LINK_LIBS ${YB_MIN_TEST_LIBS})

############################################################
# Subdirectories
############################################################

# TODO(dmitry): Add YB_CMAKE_CXX_EXTRA_FLAGS to CMAKE_CXX_FLAGS when all source code will be
#               compatible with with these flags #9279
set(YB_CMAKE_CXX_EXTRA_FLAGS "-Wextra -Wno-unused-parameter")

# For any C code, use the same flags as for C++.
set(CMAKE_C_FLAGS "${CMAKE_CXX_FLAGS}")

# C++-only flags.
ADD_CXX_FLAGS("-Werror=missing-field-initializers")

if ((IS_GCC) AND
    ("${CMAKE_CXX_COMPILER_VERSION}" VERSION_GREATER_EQUAL "7.0"))
  # Used with GCC 7.
  ADD_CXX_FLAGS("-faligned-new")
endif()

# -Wsign-compare is enabled for C++ only for GCC with just -Wall, and enabled for both C and C++
# for GCC and Clang with -Wextra. Remove when -Wextra is enabled for all source code (#9279).
if (IS_CLANG)
  ADD_CXX_FLAGS("-Wsign-compare")
endif()

string (REPLACE "-Werror=non-virtual-dtor" "" CMAKE_C_FLAGS ${CMAKE_C_FLAGS})
string (REPLACE "-Werror=reorder" "" CMAKE_C_FLAGS ${CMAKE_C_FLAGS})
string (REPLACE "-Wnon-virtual-dtor" "" CMAKE_C_FLAGS ${CMAKE_C_FLAGS})
string (REPLACE "-Woverloaded-virtual" "" CMAKE_C_FLAGS ${CMAKE_C_FLAGS})


message("CMAKE_C_FLAGS ${CMAKE_C_FLAGS}")

set(CMAKE_CXX_STANDARD 20)

if (IS_GCC AND
    "${COMPILER_VERSION}" VERSION_GREATER_EQUAL "8.0.0")
  # Remove after switching to C++17
  ADD_CXX_FLAGS("-Wno-aligned-new")
endif()

set(CMAKE_CXX_STANDARD_REQUIRED ON)

message("Linker flags for executables: ${CMAKE_EXE_LINKER_FLAGS}")

# Define full paths to libpq and libyb_pgbackend shared libraries. These libraries are built as part
# of building PostgreSQL. We need to declare these as by-products of running the PostgreSQL build
# command, as well as wrap them into CMake targets so that other libraries and executables can
# depend on them.
set(LIBPQ_SHARED_LIB
    "${YB_BUILD_ROOT}/postgres/lib/libpq${YB_SHARED_LIBRARY_SUFFIX}")
set(YB_PGBACKEND_SHARED_LIB
    "${YB_BUILD_ROOT}/postgres/lib/libyb_pgbackend${YB_SHARED_LIBRARY_SUFFIX}")

# -------------------------------------------------------------------------------------------------
# Subdirectories
# -------------------------------------------------------------------------------------------------

# PostgreSQL source code is not in src/yb so that we can grep non-PostgreSQL YB code easily.
add_subdirectory(src/postgres)

include_directories(${YB_BUILD_ROOT}/postgres/include)
add_postgres_shared_library(pq "${LIBPQ_SHARED_LIB}")
add_postgres_shared_library(yb_pgbackend "${YB_PGBACKEND_SHARED_LIB}")

add_subdirectory(src/yb/rocksdb)
add_subdirectory(src/yb/gutil)
add_subdirectory(src/yb/util)
add_subdirectory(src/yb/common)
add_subdirectory(src/yb/encryption)
add_subdirectory(src/yb/gen_yrpc)
add_subdirectory(src/yb/fs)
add_subdirectory(src/yb/server)
add_subdirectory(src/yb/tablet)
add_subdirectory(src/yb/bfql)
add_subdirectory(src/yb/bfpg)
add_subdirectory(src/yb/rpc)
add_subdirectory(src/yb/tserver)
add_subdirectory(src/yb/consensus)
add_subdirectory(src/yb/master)
add_subdirectory(src/yb/client)
add_subdirectory(src/yb/integration-tests)
add_subdirectory(src/yb/tools)
add_subdirectory(src/yb/rocksutil)
add_subdirectory(src/yb/docdb)
add_subdirectory(src/yb/yql)
add_subdirectory(src/yb/cdc)

include("${YB_SRC_ROOT}/ent/CMakeLists.txt")

# ------------------------------------------------------------------------------------------------
# Utilities for collecting the dependency graph
# ------------------------------------------------------------------------------------------------

# We could not accumulate multiple lines in YB_ALL_DEPS due to a CMake bug with cache variables,
# but we can write multiple lines in the output file.
string(REPLACE "\\n" "\n" YB_ALL_DEPS_FINAL "${YB_ALL_DEPS}")
# This file wlll contains dependencies in the following form, separated by newlines:
# target: dep1;dep2;..;depN
# The same target can appear on the left hand side of multiple such lines.
file(WRITE "${YB_BUILD_ROOT}/yb_cmake_deps.txt" "${YB_ALL_DEPS_FINAL}")

math(EXPR YB_NUM_EXCLUDED_TESTS "${YB_NUM_TESTS} - ${YB_NUM_INCLUDED_TESTS}")
message("Total tests: ${YB_NUM_TESTS}, "
        "included: ${YB_NUM_INCLUDED_TESTS}, "
        "excluded: ${YB_NUM_EXCLUDED_TESTS}")
if(NOT ${BUILD_TESTS} AND NOT ${YB_NUM_INCLUDED_TESTS} STREQUAL "1")
  message(
    FATAL_ERROR
    "BUILD_TESTS is not set, but we still added ${YB_NUM_INCLUDED_TESTS} tests"
    " (one test would be OK because we always add create_initial_sys_catalog_snapshot).")
endif()

math(EXPR YB_NUM_EXCLUDED_EXECUTABLES "${YB_NUM_EXECUTABLES} - ${YB_NUM_INCLUDED_EXECUTABLES}")
message("Total non-test executables: ${YB_NUM_EXECUTABLES}, "
        "included: ${YB_NUM_INCLUDED_EXECUTABLES}, "
        "excluded: ${YB_NUM_EXCLUDED_EXECUTABLES}")
