// Copyright 2003-2016 Nicholas J. Kain <njkain at gmail dot com>
// SPDX-License-Identifier: MIT
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>
#include <errno.h>
#include "rlimit.hpp"
extern "C" {
#include "nk/log.h"
}

static inline const char *resource_to_str(int resource)
{
    switch (resource) {
    case RLIMIT_CPU: return "cpu";
    case RLIMIT_FSIZE: return "fsize";
    case RLIMIT_DATA: return "data";
    case RLIMIT_STACK: return "stack";
    case RLIMIT_CORE: return "core";
    case RLIMIT_RSS: return "rss";
    case RLIMIT_NPROC: return "nproc";
    case RLIMIT_NOFILE: return "nofile";
    case RLIMIT_MEMLOCK: return "memlock";
#ifndef BSD
    case RLIMIT_AS: return "as";
    case RLIMIT_MSGQUEUE: return "msgqueue";
    case RLIMIT_NICE: return "nice";
    case RLIMIT_RTTIME: return "rttime";
    case RLIMIT_RTPRIO: return "rtprio";
    case RLIMIT_SIGPENDING: return "sigpending";
#endif /* BSD */
    default: suicide("unknown rlimit");
    }
}

int rlimits::do_limit(int resource, const rlimit &rlim, uid_t uid, gid_t gid,
                      const std::string &cmd)
{
    auto r = setrlimit(resource, &rlim);
    if (r < 0) {
        switch (errno) {
        case EFAULT:
            log_line("setrlimit(%s) given bad value for job: uid=%u gid=%u command='%s'",
                     resource_to_str(resource), uid, gid, cmd.c_str());
            break;
        case EINVAL:
            log_line("setrlimit(%s) given invalid RLIMIT for job: uid=%u gid=%u command='%s'",
                     resource_to_str(resource), uid, gid, cmd.c_str());
            break;
        case EPERM:
            log_line("setrlimit(%s) denied permission to set limit for job: uid=%u gid=%u command='%s'",
                     resource_to_str(resource), uid, gid, cmd.c_str());
        default:
            break;
        }
    }
    return r;
}

void rlimits::add(int resource, const rlimit &rli) {
    resource_to_str(resource);
    rlims_.emplace_back(resource, rli);
}

int rlimits::enforce(uid_t uid, gid_t gid, const std::string &command)
{
    for (const auto &i: rlims_) {
        auto r = do_limit(i.resource, i.limits, uid, gid, command);
        if (r < 0)
            return -1;
    }
    return 0;
}

