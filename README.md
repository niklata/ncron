# ncron
Copyright 2003-2024 Nicholas J. Kain.
See LICENSE for licensing information.

## Introduction

ncron is a constraint-solving cron daemon that supports persistence of
job execution times and also subsumes most of the abilities of atd.

The advantage over conventional crons is that it can execute tasks
at arbitrary (limited by system API and QoI) time precision and also
makes it easy to restrict job execution times to certain arbitrary time
constraints and intervals.

It also does not force the system to periodically wake and poll for
pending jobs; ncron calculates the next time it will need to act and
has the OS wake it after that time has passed.  This method is better
for power efficiency.

Setting spawned task state (chroot, rlimits, uid, gid, etc) should be performed
via a wrapper script (either shell or
[execline](https://skarnet.org/software/execline/)) that perform chain loading
("Bernstein chaining") for the job. I suggest using the tools such as
s6-softlimit or s6-applyuidgid from the
[s6 suite](https://www.skarnet.org/software/s6/overview.html).

## Requirements

* Linux kernel
* GCC or Clang
* GNU Make
* For developers: [Ragel](https://www.colm.net/open-source/ragel)

ncron should work on most POSIX platforms, but the primary development
platform is x64 Linux with recent glibc.

## Installation

Compile and install ncron.
* Build ncron: `make`
* Install the `ncron` executable in a normal place:
```
$ su
# cp ncron /usr/local/bin
# chown root.root /usr/local/bin/ncron
# chmod 755 /usr/local/bin/ncron

# mkdir -m 755 -p /usr/local/share/man/man[15]
# cp ncron.1 /usr/local/share/man/man1
# cp crontab.5 /usr/local/share/man/man5
# exit
```

Read the manpages for information on crontab format and paths.
```
$ man 5 crontab
$ man 1 ncron
```

A system-wide ncron will have its crontab at `/var/lib/ncron/crontab`.
The directory will look much like:
```
# ls -laF /var/lib/ncron
total 12
drwx------  2 root root   36 May 13  2017 ./
drwxr-xr-x  3 root root 4096 May 13  2017 ../
-rw-------  1 root root  252 May 13  2017 crontab
-rw-r--r--  1 root root  131 May 13  2017 exectimes
```

A simple crontab file (remove the leading spaces if you copy/paste):
```
!1
command=/bin/echo Hello world!
interval=5m
```

The exectimes file will be created by ncron; it stores the history of
previously run jobs (denoted by the `!NUMBER` markers in the crontab).

Read the man pages for more info!  But what I've written here should be
enough to get you started.

## Downloads

* [GitLab](https://gitlab.com/niklata/ncron)
* [Codeberg](https://codeberg.org/niklata/ncron)
* [BitBucket](https://bitbucket.com/niklata/ncron)
* [GitHub](https://github.com/niklata/ncron)

