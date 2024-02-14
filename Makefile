NCRON_C_SRCS = $(sort nk/io.c nk/pspawn.c)
NCRON_CXX_SRCS = $(sort ncron.cpp sched.cpp crontab.cpp)
NCRON_OBJS = $(NCRON_C_SRCS:.c=.o) $(NCRON_CXX_SRCS:.cpp=.o)
NCRON_DEP = $(NCRON_C_SRCS:.c=.d) $(NCRON_CXX_SRCS:.cpp=.d)
INCL = -I.

CFLAGS = -MMD -Os -flto -s -std=gnu99 -pedantic -Wall -Wextra -Wimplicit-fallthrough=0 -Wformat=2 -Wformat-nonliteral -Wformat-security -Wshadow -Wpointer-arith -Wmissing-prototypes -Wcast-qual -Wsign-conversion -Wstrict-overflow=5
CXXFLAGS = -MMD -Os -flto -s -std=gnu++20 -fno-rtti -fno-exceptions -pedantic -Wall -Wextra -Wimplicit-fallthrough=0 -Wformat=2 -Wformat-nonliteral -Wformat-security -Wshadow -Wpointer-arith -Wsign-conversion -Wstrict-overflow=5 -Wold-style-cast
#CFLAGS = -MMD -Og -g -fsanitize=address -fsanitize=undefined -flto -std=gnu99 -pedantic -Wall -Wextra -Wimplicit-fallthrough=0 -Wformat=2 -Wformat-nonliteral -Wformat-security -Wshadow -Wpointer-arith -Wmissing-prototypes -Wcast-qual -Wsign-conversion -Wstrict-overflow=5
#CXXFLAGS = -MMD -Og -g -fsanitize=address -fsanitize=undefined -flto -std=gnu++20 -fno-rtti -fno-exceptions -pedantic -Wall -Wextra -Wimplicit-fallthrough=0 -Wformat=2 -Wformat-nonliteral -Wformat-security -Wshadow -Wpointer-arith -Wsign-conversion -Wstrict-overflow=5 -Wold-style-cast
CPPFLAGS += $(INCL)

all: ragel ncron

ncron: $(NCRON_OBJS)
	$(CXX) $(CXXFLAGS) $(INCL) -o $@ $^

-include $(NCRON_DEP)

clean:
	rm -f $(NCRON_OBJS) $(NCRON_DEP) ncron

cleanragel:
	rm -f crontab.cpp

crontab.cpp: crontab.rl
	ragel -F0 -o crontab.cpp crontab.rl

ragel: crontab.cpp

.PHONY: all clean cleanragel

