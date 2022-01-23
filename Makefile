NCRON_C_SRCS = $(sort $(wildcard *.c) $(wildcard nk/*.c))
NCRON_CXX_SRCS = $(sort $(wildcard *.cpp) $(wildcard nk/*.cpp) crontab.cpp)
NCRON_OBJS = $(NCRON_C_SRCS:.c=.o) $(NCRON_CXX_SRCS:.cpp=.o)
NCRON_DEP = $(NCRON_C_SRCS:.c=.d) $(NCRON_CXX_SRCS:.cpp=.d)
INCL = -I.

CC ?= gcc
CCX ?= g++
CFLAGS = -MMD -O2 -s -std=gnu99 -fno-strict-overflow -pedantic -Wall -Wextra -Wimplicit-fallthrough=0 -Wformat=2 -Wformat-nonliteral -Wformat-security -Wshadow -Wpointer-arith -Wmissing-prototypes -Wcast-qual -Wsign-conversion
CXXFLAGS = -MMD -O2 -s -std=gnu++17 -fno-strict-overflow -fno-rtti -pedantic -Wall -Wextra -Wimplicit-fallthrough=0 -Wformat-security -Wpointer-arith

-include $(NCRON_DEP)

all: ragel ncron

clean:
	rm -f $(NCRON_OBJS) $(NCRON_DEP) ncron

cleanragel:
	rm -f crontab.cpp

crontab.cpp:
	ragel -G2 -o crontab.cpp crontab.rl

ragel: crontab.cpp

%.o: %.c
	$(CC) $(CFLAGS) $(INCL) -c -o $@ $^

%.o: %.cpp
	$(CCX) $(CXXFLAGS) $(INCL) -c -o $@ $^

ncron: $(NCRON_OBJS)
	$(CCX) $(CXXFLAGS) $(INCL) -o $@ $^

.PHONY: all clean cleanragel

