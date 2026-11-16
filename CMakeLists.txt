cmake_minimum_required(VERSION 3.24)
project(multicurve_bootstrap VERSION 1.0.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Multicurve bootstrapping, pricing and yield-curve statistics engine.
#
#   cmake -S . -B build            # configure (downloads Eigen/GoogleTest if needed)
#   cmake --build build -j         # build library, apps and tests
#   ctest --test-dir build         # run the unit tests
#   ./build/mcb_report --help      # produce the dashboard data
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(MCB_BUILD_TESTS "Build the GoogleTest unit tests" ON)

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# --- Eigen (header-only linear algebra) ------------------------------------
# A system copy is used when present (e.g. `apt install libeigen3-dev`);
# otherwise a pinned release is downloaded at configure time.
find_package(Eigen3 3.4 QUIET NO_MODULE)
if(NOT TARGET Eigen3::Eigen)
  FetchContent_Declare(eigen
    URL https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.tar.gz
    URL_HASH SHA256=8586084f71f9bde545ee7fa6d00288b264a2b7ac3607b974e54d13e7162c1c72
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR do-not-run-eigens-own-cmake)
  FetchContent_MakeAvailable(eigen)
  add_library(Eigen3::Eigen INTERFACE IMPORTED)
  target_include_directories(Eigen3::Eigen SYSTEM INTERFACE "${eigen_SOURCE_DIR}")
endif()

# --- The library -------------------------------------------------------------
set(MCB_SOURCES
  src/core/date.cpp
  src/core/calendar.cpp
  src/core/daycount.cpp
  src/core/schedule.cpp
  src/io/csv.cpp
  src/io/json.cpp
  src/math/interpolation.cpp
  src/math/solver.cpp
  src/curves/yield_curve.cpp
  src/curves/rate_helpers.cpp
  src/curves/bootstrap.cpp
  src/curves/conventions.cpp
  src/curves/curve_engine.cpp
  src/pricing/swaps.cpp
  src/pricing/bonds.cpp
  src/pricing/risk.cpp
  src/stats/linear_model.cpp
  src/stats/var_model.cpp
  src/stats/term_panel.cpp
  src/stats/cg_bootstrap.cpp
  src/stats/eh_tests.cpp
  src/stats/forecast.cpp
  src/data/eur_reference.cpp
  src/data/usd_market.cpp
  src/data/gsw.cpp
  src/data/moex.cpp
  src/report/report.cpp
  src/report/report_usd.cpp
  src/report/report_eur.cpp
  src/report/report_stats.cpp
  src/report/report_moex.cpp
)
add_library(mcb STATIC ${MCB_SOURCES})
target_include_directories(mcb PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(mcb PUBLIC Eigen3::Eigen)
if(MSVC)
  target_compile_options(mcb PRIVATE /W4)
else()
  target_compile_options(mcb PRIVATE -Wall -Wextra -Wpedantic)
endif()

# --- Applications ------------------------------------------------------------
foreach(app mcb_report mcb_bench)
  add_executable(${app} apps/${app}.cpp)
  target_link_libraries(${app} PRIVATE mcb)
endforeach()

# --- Tests -------------------------------------------------------------------
if(MCB_BUILD_TESTS)
  enable_testing()
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(googletest)

  set(MCB_TEST_SOURCES
    tests/test_date_calendar.cpp
    tests/test_daycount_schedule.cpp
    tests/test_io.cpp
    tests/test_interpolation_solver.cpp
    tests/test_bootstrap.cpp
    tests/test_eur_multicurve.cpp
    tests/test_pricing_risk.cpp
    tests/test_curve_engine.cpp
    tests/test_stats.cpp
    tests/test_cg_bootstrap.cpp
    tests/test_market_data.cpp
  )
  add_executable(mcb_tests ${MCB_TEST_SOURCES})
  target_link_libraries(mcb_tests PRIVATE mcb GTest::gtest_main)
  target_compile_definitions(mcb_tests PRIVATE
    MCB_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
    MCB_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  include(GoogleTest)
  gtest_discover_tests(mcb_tests DISCOVERY_TIMEOUT 120)
endif()
