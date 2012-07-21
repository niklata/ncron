CC = gcc -s -std=gnu99 -pedantic -Wall -Wno-format-extra-args -Wno-format-zero-length -Wformat-nonliteral -Wformat-security -DHAVE_CLEARENV
objects = log.o strl.o sched.o config.o chroot.o rlimit.o exec.o ncron.o

ncron : $(objects)
	$(CC) $(CFLAGS) $(archflags) $(LDFLAGS) $(objects) -o ncron

%.o : %.c
	$(CC) $(CFLAGS) $(archflags) -c $<

tags:
	-ctags -f tags *.[ch]
	-cscope -b
install: ncron
	-install -m 755 -s ncron /usr/local/bin
	-install -m 644 ncron.1.gz /usr/local/man/man1
	-install -m 644 crontab.5.gz /usr/local/man/man5
clean :
	-rm -f *.o ncron
distclean:
	-rm -f *.o ncron tags cscope.out
