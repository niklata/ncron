# ncron
Copyright 2003-2026 Nicholas J. Kain.
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
* For developers or on non-x86 targets: [Ragel](https://www.colm.net/open-source/ragel)

ncron should work on most POSIX platforms.  I use and test it on
Linux/glibc amd64 and aarch64.

## Installation

Compile and install ncron.
* If you are building on a non-x86 target such as ARM, the bundled
  Ragel-generated artifacts may not be correct.  In that case,
  ragel must be installed on your system and the prebuilt ragel
  files must be removed via: `make cleanragel`
* Build ncron: `make`
* Install the `ncron` executable in a normal place:
```
$ su
# install -o root -g root -m 755 ncron /usr/local/bin/ncron

# mkdir -m 755 -p /usr/local/share/man/man1
# mkdir -m 755 -p /usr/local/share/man/man5
# install -m 644 ncron.1 /usr/local/share/man/man1/
# install -m 644 crontab.5 /usr/local/share/man/man5/
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
# chmod 700 /var/lib/ncron
# touch /var/lib/ncron/crontab
# chmod 600 /var/lib/ncron/crontab
# touch /var/lib/ncron/history
# chmod 644 /var/lib/ncron/history

# ls -laF /var/lib/ncron
total 12
drwx------  2 root root   36 May 13  2017 ./
drwxr-xr-x  3 root root 4096 May 13  2017 ../
-rw-------  1 root root  252 May 13  2017 crontab
-rw-r--r--  1 root root  131 May 13  2017 history
```

A simple crontab file (remove the leading spaces if you copy/paste):
```
!1
command=/bin/echo Hello world!
interval=5m
```

The history file must be readable and writable by ncron; create
it with touch and set proper ownership and permissions on it.  history
stores the history of previously run jobs (denoted by the `!NUMBER`
markers in the crontab).

Read the man pages for more info!  But what I've written here should be
enough to get you started.

## Upgrading

If you are upgrading from a version before Jun2026, it is necessary to
convert from the execfile format to the history format.  There is a
simple GNU sed script to perform the necessary changes (removal of the
exectime field):

```
# sed -E 's/([0-9]+)=[0-9]+:([0-9]+)[|]([0-9]+)/\1=\2:\3/' /var/lib/ncron/exectimes > /var/lib/ncron/history
# chmod 644 /var/lib/ncron/history
# rm /var/lib/ncron/exectimes
```

The exectime field is removed because ncron now recalculates the
runtime for each job when it is started.  This change is to make it
easier to redefine the properties of an existing job without having to
also modify the history file.  The old design dates back to when
computers were potentially a lot slower and saving cycles was more
important than ergonomics.

The runat= command in crontab no longer exists.  Instead, it is
recommended to instead use maxruns=1 with a reasonable interval= for
constraint solving (equal to that of the unit of the smallest
constraint that is set) and appropriate constraint to define the time
to run the job.

runat= is removed because the functionality that it provides is
redundant and less well-tested than the alternative just described.

If you are upgrading from a version before Feb2024, it is necessary to
make some changes to your crontab file:

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
