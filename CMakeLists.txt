cmake_minimum_required(VERSION 3.16)
project(edgevision_gateway VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
option(EDGEVISION_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

function(edgevision_enable_warnings target)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic
        $<$<BOOL:${EDGEVISION_WARNINGS_AS_ERRORS}>:-Werror>)
endfunction()

add_library(edgevision_log STATIC
    modules/log/async_logger.c
    modules/blocking_queue/blocking_queue.c)
target_include_directories(edgevision_log PUBLIC modules/log modules/blocking_queue)
target_compile_definitions(edgevision_log PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(edgevision_log PUBLIC Threads::Threads ZLIB::ZLIB)
edgevision_enable_warnings(edgevision_log)

add_library(edgevision_runtime STATIC modules/runtime/graceful_shutdown.c)
target_include_directories(edgevision_runtime PUBLIC modules/runtime)
target_compile_definitions(edgevision_runtime PRIVATE _POSIX_C_SOURCE=200809L)
edgevision_enable_warnings(edgevision_runtime)

add_executable(gateway apps/gateway/main.c)
target_compile_definitions(gateway PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(gateway PRIVATE edgevision_log edgevision_runtime)
edgevision_enable_warnings(gateway)

include(CTest)
if(BUILD_TESTING)
    add_executable(async_logger_test tests/async_logger_test.c)
    target_link_libraries(async_logger_test PRIVATE edgevision_log)
    edgevision_enable_warnings(async_logger_test)
    add_test(NAME async_logger COMMAND async_logger_test)
    add_test(NAME graceful_shutdown_sigterm
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_graceful_shutdown.sh
                $<TARGET_FILE:gateway> SIGTERM)
    add_test(NAME graceful_shutdown_sigint
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_graceful_shutdown.sh
                $<TARGET_FILE:gateway> SIGINT)
endif()
