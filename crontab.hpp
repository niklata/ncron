// Copyright 2003-2014 Nicholas J. Kain <njkain at gmail dot com>
// SPDX-License-Identifier: MIT
#ifndef NCRON_CONFIG_H_
#define NCRON_CONFIG_H_
#include <memory>
#include "sched.hpp"
void parse_config(const std::string &path, const std::string &execfile,
                  std::vector<StackItem> &stack,
                  std::vector<StackItem> &deadstack);
#endif
