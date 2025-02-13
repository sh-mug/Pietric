cmake_minimum_required(VERSION 3.10)
project(Pietric)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
    src/ImageLoader.cpp
)

add_executable(Pietric ${SOURCES})

llvm_map_components_to_libnames(llvm_libs support core irreader native)

target_link_libraries(Pietric ${llvm_libs})
