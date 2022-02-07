// Copyright 2003-2016 Nicholas J. Kain <njkain at gmail dot com>
// SPDX-License-Identifier: MIT
#ifndef NCRON_RLIMIT_H_
#define NCRON_RLIMIT_H_
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

