cmake_minimum_required(VERSION 3.10)
project(Pietric)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Find LLVM (adjust if necessary for your installation)
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# Setup stb_image
include(FetchContent)

FetchContent_Declare(
  stb_image
  URL https://raw.githubusercontent.com/nothings/stb/master/stb_image.h
  DOWNLOAD_NO_EXTRACT TRUE
)

FetchContent_GetProperties(stb_image)
if(NOT stb_image_POPULATED)
  FetchContent_MakeAvailable(stb_image)
  # Add the directory where stb_image.h was downloaded to the include path.
  include_directories(${stb_image_SOURCE_DIR})
endif()

# Include our source directory
include_directories(${CMAKE_SOURCE_DIR}/include)

# List source files
set(SOURCES
    src/main.cpp
    src/Utils.cpp
    src/Parser.cpp
    src/Graph.cpp
    src/IRBuilder.cpp
    src/Runtime.cpp
    src/Backend.cpp
    src/ImageLoader.cpp
)

add_executable(Pietric ${SOURCES})

llvm_map_components_to_libnames(llvm_libs support core bitwriter targetparser)

target_link_libraries(Pietric ${llvm_libs})

# Pietric runs opt and llc from the LLVM it was built with (or else from the PATH).
target_compile_definitions(Pietric PRIVATE PIETRIC_LLVM_TOOLS_DIR="${LLVM_TOOLS_BINARY_DIR}")

# LLVM's static libraries are built with one section per function; let the linker drop the
# unused ones.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
  target_link_options(Pietric PRIVATE -Wl,--gc-sections)
endif()

# Code that uses LLVM's command-line library must match LLVM's RTTI setting.
if(NOT LLVM_ENABLE_RTTI AND NOT MSVC)
  target_compile_options(Pietric PRIVATE -fno-rtti)
endif()

# End-to-end tests (run with `ctest` from the build directory).
enable_testing()
add_test(NAME piet-programs
  COMMAND ${CMAKE_SOURCE_DIR}/tests/run_tests.sh
          $<TARGET_FILE:Pietric> ${CMAKE_C_COMPILER} ${CMAKE_SOURCE_DIR}/tests/cases)
