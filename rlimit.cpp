/* rlimit.cpp - sets rlimits for ncron jobs
 *
 * (c) 2003-2014 Nicholas J. Kain <njkain at gmail dot com>
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
extern "C" {
#include "nk/log.h"
}
#include "rlimit.hpp"

int rlimits::do_limit(int resource, const boost::optional<struct rlimit> &rlim,
                      const std::string &rstr, const pprargs &ppr)
{
    if (!rlim)
        return 0;
    auto r = setrlimit(resource, &*rlim);
    if (r < 0) {
        switch (errno) {
        case EFAULT:
            log_error("setrlimit(%s) given bad value for job: uid=%u gid=%u command='%s'",
                      rstr.c_str(), ppr.uid_, ppr.gid_, ppr.cmd_.c_str());
            break;
        case EINVAL:
            log_error("setrlimit(%s) given invalid RLIMIT for job: uid=%u gid=%u command='%s'",
                      rstr.c_str(), ppr.uid_, ppr.gid_, ppr.cmd_.c_str());
            break;
        case EPERM:
            log_error("setrlimit(%s) denied permission to set limit for job: uid=%u gid=%u command='%s'",
                      rstr.c_str(), ppr.uid_, ppr.gid_, ppr.cmd_.c_str());
        default:
            break;
        }
    }
    return r;
}

int rlimits::enforce(uid_t uid, gid_t gid, const std::string &command)
{
    auto ppr = pprargs(uid, gid, command);
    if (do_limit(RLIMIT_CPU, cpu, "cpu", ppr))
        return -1;
    if (do_limit(RLIMIT_FSIZE, fsize, "fsize", ppr))
        return -1;
    if (do_limit(RLIMIT_DATA, data, "data", ppr))
        return -1;
    if (do_limit(RLIMIT_STACK, stack, "stack", ppr))
        return -1;
    if (do_limit(RLIMIT_CORE, core, "core", ppr))
        return -1;
    if (do_limit(RLIMIT_RSS, rss, "rss", ppr))
        return -1;
    if (do_limit(RLIMIT_NPROC, nproc, "nproc", ppr))
        return -1;
    if (do_limit(RLIMIT_NOFILE, nofile, "nofile", ppr))
        return -1;
    if (do_limit(RLIMIT_MEMLOCK, memlock, "memlock", ppr))
        return -1;
    if (do_limit(RLIMIT_AS, as, "as", ppr))
        return -1;
    if (do_limit(RLIMIT_MSGQUEUE, msgqueue, "msgqueue", ppr))
        return -1;
    if (do_limit(RLIMIT_NICE, nice, "nice", ppr))
        return -1;
    if (do_limit(RLIMIT_RTTIME, rttime, "rttime", ppr))
        return -1;
    if (do_limit(RLIMIT_SIGPENDING, sigpending, "sigpending", ppr))
        return -1;
    return 0;
}

