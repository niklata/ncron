/* rlimit.c - sets rlimits for ncron jobs
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
#include "nk/log.h"

#include "rlimit.h"

static int do_limit(int resource, const struct rlimit *rlim)
{
    if (!rlim)
        return 0;
    return setrlimit(resource, rlim);
}


int enforce_limits(limit_t *limits, int uid, int gid, char *command)
{
    if (!limits)
        return 0;

    if (do_limit(RLIMIT_CPU, limits->cpu))
        goto rlimit_failed;
    if (do_limit(RLIMIT_FSIZE, limits->fsize))
        goto rlimit_failed;
    if (do_limit(RLIMIT_DATA, limits->data))
        goto rlimit_failed;
    if (do_limit(RLIMIT_STACK, limits->stack))
        goto rlimit_failed;
    if (do_limit(RLIMIT_CORE, limits->core))
        goto rlimit_failed;
    if (do_limit(RLIMIT_RSS, limits->rss))
        goto rlimit_failed;
    if (do_limit(RLIMIT_NPROC, limits->nproc))
        goto rlimit_failed;
#ifndef RLIMIT_NOFILE
    if (do_limit(RLIMIT_OFILE, limits->nofile))
        goto rlimit_failed;
#else
    if (do_limit(RLIMIT_NOFILE, limits->nofile))
        goto rlimit_failed;
#endif
    if (do_limit(RLIMIT_MEMLOCK, limits->memlock))
        goto rlimit_failed;
#ifndef BSD
    if (do_limit(RLIMIT_AS, limits->as))
        goto rlimit_failed;
#endif

    return 0;
rlimit_failed:
    switch (errno) {
    case EFAULT:
        log_error("setrlimit given bad value for job: uid=%u gid=%u command='%s'",
                  uid, gid, command);
        break;
    case EINVAL:
        log_error("setrlimit given invalid RLIMIT for job: uid=%u gid=%u command='%s'",
                  uid, gid, command);
        break;
    case EPERM:
        log_error("setrlimit denied permission to set limit for job: uid=%u gid=%u command='%s'",
                  uid, gid, command);
    default:
        break;
    }
    return -1;
}
