// Copyright 2004-2018 Nicholas J. Kain <njkain at gmail dot com>
// SPDX-License-Identifier: MIT
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "nk/signals.h"
#include "nk/log.h"

void hook_signal(int signum, void (*fn)(int), int flags) {
  struct sigaction new_action;

  new_action.sa_handler = fn;
  sigemptyset(&new_action.sa_mask);
  new_action.sa_flags = flags;

  if (sigaction(signum, &new_action, (struct sigaction *)0))
    suicide("%s: sigaction(%d, ...) failed: %s", __func__, signum,
            strerror(errno));
}

void disable_signal(int signum) {
  struct sigaction new_action;

  new_action.sa_handler = SIG_IGN;
  sigemptyset(&new_action.sa_mask);
  new_action.sa_flags = 0;

  if (sigaction(signum, &new_action, (struct sigaction *)0))
    suicide("%s: sigaction(%d, ...) failed: %s", __func__, signum,
            strerror(errno));
}
