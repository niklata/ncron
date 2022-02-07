// Copyright 2004-2014 Nicholas J. Kain <njkain at gmail dot com>
// SPDX-License-Identifier: MIT
#ifndef NCM_SIGNALS_H_
#define NCM_SIGNALS_H_

void hook_signal(int signum, void (*fn)(int), int flags);
void disable_signal(int signum);

#endif
