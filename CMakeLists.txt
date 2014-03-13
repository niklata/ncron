project (ncron)

cmake_minimum_required (VERSION 2.6)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -s -std=gnu99 -lrt -flto -pedantic -Wall -Wextra -Wformat=2 -Wformat-nonliteral -Wformat-security -Wshadow -Wpointer-arith -Wmissing-prototypes -DHAVE_CLEARENV")

include_directories(
    "${PROJECT_SOURCE_DIR}/ncmlib")
add_subdirectory(ncmlib)

file(GLOB NCRON_SRCS "*.c")
add_executable(ncron ${NCRON_SRCS})
target_link_libraries(ncron ncmlib)
