cmake_minimum_required(VERSION 3.16)
project(gus VERSION 0.0.1 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(SQLite3 REQUIRED)
if(NOT TARGET SQLite3::SQLite3)
    add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
endif()

add_executable(gus src/main.c src/event_store.c src/checkpoint_store.c src/object_store.c src/git_capture.c)
target_link_libraries(gus PRIVATE SQLite3::SQLite3)
target_compile_options(gus PRIVATE -Wall -Wextra -Wpedantic)

add_executable(event_store_api_test tests/event_store_api.c src/event_store.c)
target_include_directories(event_store_api_test PRIVATE src)
target_link_libraries(event_store_api_test PRIVATE SQLite3::SQLite3)
target_compile_options(event_store_api_test PRIVATE -Wall -Wextra -Wpedantic)

add_executable(object_store_api_test tests/object_store_api.c src/object_store.c src/event_store.c)
target_include_directories(object_store_api_test PRIVATE src)
target_link_libraries(object_store_api_test PRIVATE SQLite3::SQLite3)
target_compile_options(object_store_api_test PRIVATE -Wall -Wextra -Wpedantic)

add_executable(checkpoint_store_api_test tests/checkpoint_store_api.c src/checkpoint_store.c src/event_store.c)
target_include_directories(checkpoint_store_api_test PRIVATE src)
target_link_libraries(checkpoint_store_api_test PRIVATE SQLite3::SQLite3)
target_compile_options(checkpoint_store_api_test PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_test(NAME integration COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.sh $<TARGET_FILE:gus>)
add_test(NAME git_capture_integration COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/git_capture_integration.sh $<TARGET_FILE:gus>)
add_test(NAME event_store_api COMMAND event_store_api_test)
add_test(NAME object_store_api COMMAND object_store_api_test)
add_test(NAME checkpoint_store_api COMMAND checkpoint_store_api_test)
