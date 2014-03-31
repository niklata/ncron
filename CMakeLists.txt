project (ncron)

cmake_minimum_required (VERSION 2.6)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -s -std=gnu99 -lrt -flto -pedantic -Wall -Wextra -Wformat=2 -Wformat-nonliteral -Wformat-security -Wshadow -Wpointer-arith -Wmissing-prototypes")

if (WIN32)
  set(OSNAME "Win32")
else()
execute_process(
  COMMAND uname
  COMMAND tr "\n" " "
  COMMAND sed "s/ //"
  OUTPUT_VARIABLE OSNAME
  )
endif()

if (${OSNAME} STREQUAL "Linux")
  message("Detected that the current host is a Linux system.")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DLINUX")
endif()

include_directories("${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/ncmlib")
add_subdirectory(ncmlib)

set(RAGEL_PARSECFG ${CMAKE_CURRENT_BINARY_DIR}/config.c)

find_program(RAGEL ragel)
add_custom_command(
  OUTPUT ${RAGEL_PARSECFG}
  COMMAND ${RAGEL} -G2 -o ${RAGEL_PARSECFG} config.rl
  DEPENDS config.rl
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "Compiling Ragel state machine: config.rl"
  VERBATIM
  )

file(GLOB NCRON_SRCS "*.c")
add_executable(ncron ${RAGEL_PARSECFG} ${NCRON_SRCS})
target_link_libraries(ncron ncmlib)
