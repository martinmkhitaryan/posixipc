cmake_minimum_required(VERSION 3.20)

option(BUILD_TESTING "Build tests_c" OFF)
option(POSIXIPC_DEVELOPER_MODE "Strict warnings and -Werror" OFF)
set(POSIXIPC_SANITIZER "OFF" CACHE STRING "Sanitizer: OFF | address | thread | undefined")
set(POSIXIPC_CACHELINE_BYTES "64" CACHE STRING "Slot stride in bytes")

project(posixipc LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_compile_definitions(_GNU_SOURCE)

find_package(Python 3.12 REQUIRED COMPONENTS Interpreter Development.Module)
find_package(Threads REQUIRED)

include(cmake/FeatureChecks.cmake)
posixipc_run_feature_checks()

if(POSIXIPC_SANITIZER AND NOT POSIXIPC_SANITIZER STREQUAL "OFF")
    add_compile_options(-fsanitize=${POSIXIPC_SANITIZER} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${POSIXIPC_SANITIZER})
endif()

function(posixipc_apply_warnings tgt)
    if(POSIXIPC_DEVELOPER_MODE)
        target_compile_options(${tgt} PRIVATE
            -Wall -Wextra
            -Wstrict-prototypes -Wmissing-prototypes
            -Wshadow -Werror)
        get_target_property(_kind ${tgt} TYPE)
        if(_kind STREQUAL "OBJECT_LIBRARY")
            target_compile_options(${tgt} PRIVATE -Wpedantic)
        endif()
    endif()
endfunction()

add_subdirectory(src/core)
add_subdirectory(src/bindings)

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/include/posixipc.h"
        DESTINATION posixipc)

if(BUILD_TESTING)
    include(CTest)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests_c/CMakeLists.txt")
        add_subdirectory(tests_c)
    endif()
endif()
