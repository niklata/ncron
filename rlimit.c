/*
 * rlimit.c - sets rlimits for ncron jobs
 *
 * (C) 2003-2007 Nicholas J. Kain <njk@aerifal.cx>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>
#include <errno.h>

#include "sched.h"
#include "defines.h"
#include "log.h"

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
            log_line("Attempt to pass bad value to setrlimit, terminating job (uid: %i, gid: %i, command: %s).\n", uid, gid, command);
            break;
        case EINVAL:
            log_line("Attempt to set a limit that doesn't exist.  Strange rlimit semantics?  Not running job (uid: %i, gid: %i, command: %s).\n", uid, gid, command);
            break;
        case EPERM:
            log_line("Job (uid: %i, gid: %i, command: %s) tried to set limits outside of permitted bounds.  Denied.\n", uid, gid, command);
            break;
        default:
            break;
    }
    return -1;
}
