#ifndef NCRON_RLIMIT_H_
#define NCRON_RLIMIT_H_
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
#include <sys/types.h>
#include <boost/optional.hpp>
#include <boost/utility.hpp>

class rlimits : boost::noncopyable
{
    struct pprargs {
        pprargs(uid_t uid, gid_t gid, const std::string &cmd) :
            uid_(uid), gid_(gid), cmd_(cmd) {}
        uid_t uid_;
        gid_t gid_;
        const std::string &cmd_;
    };
    int do_limit(int resource, const boost::optional<struct rlimit> &rlim,
                 const std::string &rstr, const pprargs &ppr);
public:
    boost::optional<rlimit> cpu;
    boost::optional<rlimit> fsize;
    boost::optional<rlimit> data;
    boost::optional<rlimit> stack;
    boost::optional<rlimit> core;
    boost::optional<rlimit> rss;
    boost::optional<rlimit> nproc;
    boost::optional<rlimit> nofile;
    boost::optional<rlimit> memlock;
    boost::optional<rlimit> as;
    boost::optional<rlimit> msgqueue;
    boost::optional<rlimit> nice;
    boost::optional<rlimit> rttime;
    boost::optional<rlimit> rtprio;
    boost::optional<rlimit> sigpending;

    int enforce(uid_t uid, gid_t gid, const std::string &command);
};

#endif

