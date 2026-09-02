cmake_minimum_required(VERSION 3.16)
project(edgevision_sqlite_exercise LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
find_package(SQLite3 REQUIRED)
get_filename_component(EDGEVISION_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_library(exercise_cjson STATIC "${EDGEVISION_ROOT}/third_party/cjson/cJSON.c")
target_include_directories(exercise_cjson SYSTEM PUBLIC "${EDGEVISION_ROOT}/third_party/cjson")
target_link_libraries(exercise_cjson PUBLIC m)
add_executable(sqlite_measurement_demo sqlite_measurement_demo.c
    "${EDGEVISION_ROOT}/domain/measurement.c"
    "${EDGEVISION_ROOT}/domain/measurement_json.c")
target_include_directories(sqlite_measurement_demo PRIVATE "${EDGEVISION_ROOT}/domain")
target_link_libraries(sqlite_measurement_demo PRIVATE SQLite::SQLite3 exercise_cjson)
target_compile_options(sqlite_measurement_demo PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(sqlite_outbox_demo sqlite_outbox_demo.c
    "${EDGEVISION_ROOT}/domain/measurement.c"
    "${EDGEVISION_ROOT}/domain/measurement_json.c")
target_include_directories(sqlite_outbox_demo PRIVATE "${EDGEVISION_ROOT}/domain")
target_link_libraries(sqlite_outbox_demo PRIVATE SQLite::SQLite3 exercise_cjson)
target_compile_options(sqlite_outbox_demo PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(sqlite_outbox_delivery_demo sqlite_outbox_delivery_demo.c
    "${EDGEVISION_ROOT}/domain/measurement.c"
    "${EDGEVISION_ROOT}/domain/measurement_json.c")
target_include_directories(sqlite_outbox_delivery_demo PRIVATE "${EDGEVISION_ROOT}/domain")
target_link_libraries(sqlite_outbox_delivery_demo PRIVATE SQLite::SQLite3 exercise_cjson)
target_compile_options(sqlite_outbox_delivery_demo PRIVATE -Wall -Wextra -Wpedantic -Werror)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MOSQUITTO REQUIRED IMPORTED_TARGET libmosquitto)
add_library(exercise_mqtt STATIC "${EDGEVISION_ROOT}/modules/mqtt/mqtt_publisher.c")
target_include_directories(exercise_mqtt PUBLIC "${EDGEVISION_ROOT}/modules/mqtt" "${EDGEVISION_ROOT}/domain")
target_compile_definitions(exercise_mqtt PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(exercise_mqtt PUBLIC SQLite::SQLite3 Threads::Threads PkgConfig::MOSQUITTO exercise_cjson)
add_executable(sqlite_outbox_mqtt_demo sqlite_outbox_mqtt_demo.c
    "${EDGEVISION_ROOT}/domain/measurement.c"
    "${EDGEVISION_ROOT}/domain/measurement_json.c")
target_include_directories(sqlite_outbox_mqtt_demo PRIVATE "${EDGEVISION_ROOT}/domain")
target_link_libraries(sqlite_outbox_mqtt_demo PRIVATE SQLite::SQLite3 exercise_mqtt exercise_cjson)
target_compile_options(sqlite_outbox_mqtt_demo PRIVATE -Wall -Wextra -Wpedantic -Werror)
