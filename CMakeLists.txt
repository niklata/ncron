project (ncron)

cmake_minimum_required (VERSION 2.6)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -s -std=gnu99 -lrt -pedantic -Wall -Wno-format-extra-args -Wno-format-zero-length -Wformat-nonliteral -Wformat-security -DHAVE_CLEARENV")

file(GLOB NCRON_SRCS "*.c")

add_executable(ncron ${NCRON_SRCS})
