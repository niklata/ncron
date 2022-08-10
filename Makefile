NCRON_C_SRCS = $(sort $(wildcard *.c) $(wildcard nk/*.c))
NCRON_CXX_SRCS = $(sort $(wildcard *.cpp) $(wildcard nk/*.cpp) crontab.cpp)
NCRON_OBJS = $(NCRON_C_SRCS:.c=.o) $(NCRON_CXX_SRCS:.cpp=.o)
NCRON_DEP = $(NCRON_C_SRCS:.c=.d) $(NCRON_CXX_SRCS:.cpp=.d)
INCL = -I.

CFLAGS = -MMD -O2 -s -std=gnu99 -pedantic -Wall -Wextra -Wimplicit-fallthrough=0 -Wformat=2 -Wformat-nonliteral -Wformat-security -Wshadow -Wpointer-arith -Wmissing-prototypes -Wcast-qual -Wsign-conversion -Wstrict-overflow=5
CXXFLAGS = -MMD -O2 -s -std=gnu++17 -fno-rtti -fno-exceptions -pedantic -Wall -Wextra -Wimplicit-fallthrough=0 -Wformat=2 -Wformat-nonliteral -Wformat-security -Wshadow -Wpointer-arith -Wsign-conversion -Wstrict-overflow=5
CPPFLAGS += $(INCL)

all: ragel ncron

ncron: $(NCRON_OBJS)
	$(CXX) $(CXXFLAGS) $(INCL) -o $@ $^

-include $(NCRON_DEP)

clean:
	rm -f $(NCRON_OBJS) $(NCRON_DEP) ncron

cleanragel:
	rm -f crontab.cpp

crontab.cpp:
	ragel -G2 -o crontab.cpp crontab.rl

ragel: crontab.cpp

.PHONY: all clean cleanragel

