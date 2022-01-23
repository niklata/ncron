#ifndef NCRON_RLIMIT_H_
#define NCRON_RLIMIT_H_
/* rlimit.c - sets rlimits for ncron jobs
 *
 * Copyright 2003-2016 Nicholas J. Kain <njkain at gmail dot com>
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
#include <vector>
#include <string>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>

struct rlimits
{
    rlimits(const rlimits&) = delete;
    rlimits &operator=(const rlimits&) = delete;
    rlimits() {}
    inline bool exist() { return rlims_.size() > 0; }
    void add(int resource, const rlimit &rli);
    int enforce(uid_t uid, gid_t gid, const std::string &command);
private:
    struct rlim {
        rlimit limits;
        int resource;
        rlim(int res, const rlimit &rli) : limits(rli), resource(res) {}
    };

    int do_limit(int resource, const rlimit &rlim, uid_t uid, gid_t gid,
                 const std::string &cmd);

    std::vector<rlim> rlims_;
};

#endif

