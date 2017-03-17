/* rlimit.cpp - sets rlimits for ncron jobs
 *
 * (c) 2003-2016 Nicholas J. Kain <njkain at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>
#include <errno.h>
#include <fmt/format.h>
#include "rlimit.hpp"

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
    default: throw std::logic_error("unknown rlimit");
    }
}

int rlimits::do_limit(int resource, const rlimit &rlim, uid_t uid, gid_t gid,
                      const std::string &cmd)
{
    auto r = setrlimit(resource, &rlim);
    if (r < 0) {
        switch (errno) {
        case EFAULT:
            fmt::print(stderr, "setrlimit({}) given bad value for job: uid={} gid={} command='{}'\n",
                       resource_to_str(resource), uid, gid, cmd);
            break;
        case EINVAL:
            fmt::print(stderr, "setrlimit({}) given invalid RLIMIT for job: uid={} gid={} command='{}'\n",
                       resource_to_str(resource), uid, gid, cmd);
            break;
        case EPERM:
            fmt::print(stderr, "setrlimit({}) denied permission to set limit for job: uid={} gid={} command='{}'\n",
                       resource_to_str(resource), uid, gid, cmd);
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

