// Copyright 2003-2014 Nicholas J. Kain <njkain at gmail dot com>
// SPDX-License-Identifier: MIT
#ifndef NCRON_CONFIG_H_
#define NCRON_CONFIG_H_
#include <vector>
void parse_config(char const *path, char const *execfile,
                  std::vector<Job *> *stack,
                  std::vector<Job *> *deadstack);
#endif
