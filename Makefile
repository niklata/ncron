NCRON_C_SRCS = xmalloc.c strconv.c nk/io.c nk/pspawn.c ncron.c sched.c crontab.c
NCRON_OBJS = $(NCRON_C_SRCS:.c=.o)
NCRON_DEP = $(NCRON_C_SRCS:.c=.d)
INCL = -iquote .

CFLAGS = -MMD -Os -flto -s -std=gnu99 -pedantic -Wall -Wextra -Wimplicit-fallthrough=0 -Wformat=2 -Wformat-nonliteral -Wformat-security -Wshadow -Wpointer-arith -Wmissing-prototypes -Wunused-const-variable=0 -Wcast-qual -Wsign-conversion -Wstrict-overflow=5
#CFLAGS = -MMD -Og -g -fsanitize=address -fsanitize=undefined -flto -std=gnu99 -pedantic -Wall -Wextra -Wimplicit-fallthrough=0 -Wformat=2 -Wformat-nonliteral -Wformat-security -Wshadow -Wpointer-arith -Wmissing-prototypes -Wcast-qual -Wsign-conversion -Wstrict-overflow=5
CPPFLAGS += $(INCL)

all: ragel ncron

ncron: $(NCRON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^
#ncron: $(NCRON_OBJS)
#	$(CC) $(CFLAGS) -o $@ $^

-include $(NCRON_DEP)

clean:
	rm -f $(NCRON_OBJS) $(NCRON_DEP) ncron

cleanragel:
	rm -f crontab.c

crontab.c: crontab.rl
	ragel -F0 -o crontab.c crontab.rl

ragel: crontab.c

.PHONY: all clean cleanragel
