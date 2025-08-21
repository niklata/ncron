# ncron
Copyright 2003-2025 Nicholas J. Kain.
See LICENSE for licensing information.

## Introduction

ncron runs programs at user specified times.  It works somewhat
differently than traditional cron, which wakes up at regular intervals
to see if it should run jobs at that current moment.

ncron instead has a run interval specified for every job that tells it
the minimum time to wait between invocations of a given program.  Then
constraints can be specified for jobs that restrict when a program can
be run.  For example, a job might be restricted to only run between
1AM and 5AM.

The most recent time that a program has been run is persistently
stored to disk, so system downtimes cause minimal disruptions to run
schedules.  ncron also has the ability to simply run a given job one
time and never again.

A nice advantage of this scheme is that ncron can precalculate when
the next job will run and sleep until that time.  It does not wake
at regular intervals just to check to see if it should run jobs.
This approach allows for better power efficiency from fewer context
switches.

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
# install -o root -g root -m 755 ncron /usr/local/bin/ncron

# mkdir -m 755 -p /usr/local/share/man/man1
# mkdir -m 755 -p /usr/local/share/man/man5
# install -m 644 ncron.1 /usr/local/share/man/man1/
# install -m 644 crontab.5 /usr/local/share/man/man5/
# exit
```

Read the manpages for information on crontab format and paths.

I provide a sample init file for the OpenRC init system in
'init/openrc/ncron'.

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

The exectimes file must be readable and writable by ncron; create
it with touch and set proper ownership and permissions on it.  exectimes
stores the history of previously run jobs (denoted by the `!NUMBER`
markers in the crontab).

Read the man pages for more info!  But what I've written here should be
enough to get you started.

## Upgrading

The most recent (Feb2024) ncron makes some changes to the crontab format:

- The dummy '!0' job is no longer required to terminate the crontab.
- Single line comments starting with '#' or ';' are now allowed.
- The syntax for specifying constraints uses '-' as a pair delimiter
  rather than ','.
- The semantics and syntax for specifying time constraints has changed.
  'hour=' and 'minute=' are replaced with 'time=', which allows for
  continuous blocks of hh:mm time to be specified as a constraint.
- Constraint ranges must be low-high.  If the old high-low behavior
  is needed, write two constraints in the form of low-high instead.
  This restriction may be relaxed in the future.

## Notes

Legacy cron woke up at regular intervals to run jobs because
precalculating a run time subject to constraints is an instance of
solving the Boolean satisfiability problem, which is NP-complete in
the general case.  It may seem like solving this problem ahead of time
rather than by statistical sampling (as legacy cron essentially does)
would be computationally costly.  But ncron is in practice very fast,
as it can simply handle calendar day constraints by 'crossing out'
days from a year.  Constraint times for a given job are not allowed to
vary dependently on other constraints (for example, constraint times
can't vary by the day of the week).  Given these chosen limitations,
ncron is able to calculate the nearest valid runtime very quickly.

## Downloads

* [GitLab](https://gitlab.com/niklata/ncron)
* [Codeberg](https://codeberg.org/niklata/ncron)
* [BitBucket](https://bitbucket.com/niklata/ncron)
* [GitHub](https://github.com/niklata/ncron)

