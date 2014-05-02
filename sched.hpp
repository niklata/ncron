#ifndef NCRON_SCHED_HPP_
#define NCRON_SCHED_HPP_
/* sched.hpp - ncron job scheduling
 *
 * (c) 2003-2012 Nicholas J. Kain <njkain at gmail dot com>
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

#include <string>
#include <vector>
#include <memory>
#include <sys/time.h>
#include <sys/resource.h>
#include <boost/optional.hpp>
#include <boost/utility.hpp>
#include "rlimit.hpp"

struct cronentry_t
{
    cronentry_t() : id(0), user(0), group(0), exectime(0), lasttime(0),
                    interval(0), numruns(0), maxruns(0), journal(false) {}
    typedef std::vector<std::pair<int,int>> cst_list;
    unsigned int id;
    uid_t user;
    gid_t group;
    time_t exectime;        /* time at which we will execute in the future */
    time_t lasttime;        /* time that the job last ran */
    unsigned int interval;  /* min interval between executions in seconds */
    unsigned int numruns;   /* number of times a job has run */
    unsigned int maxruns;   /* max # of times a job will run, 0 = nolim */
    bool journal;
    std::string command;
    std::string args;
    std::string chroot;

    cst_list month;       /* 1-12, l=0  is wildcard, h=l is no range */
    cst_list day;         /* 1-31, l=0  is wildcard, h=l is no range */
    cst_list weekday;     /* 1-7,  l=0  is wildcard, h=l is no range */
    cst_list hour;        /* 0-23, l=24 is wildcard, h=l is no range */
    cst_list minute;      /* 0-59, l=60 is wildcard, h=l is no range */
    std::unique_ptr<rlimits> limits;

    inline bool operator<(const cronentry_t &o) const {
        return exectime < o.exectime;
    }
    void exec_and_fork(const struct timespec &ts);
private:
    void set_next_time();
};

struct StackItem {
    StackItem(std::unique_ptr<cronentry_t> &&v) : ce(std::move(v)) {
        exectime = ce ? ce->exectime : 0;
    }
    time_t exectime;
    std::unique_ptr<cronentry_t> ce;
};

static inline bool GtCronEntry(const StackItem &a,
                               const StackItem &b) {
    return a.exectime > b.exectime;
}

void set_initial_exectime(cronentry_t &entry);
time_t get_next_time(const cronentry_t &entry);
void save_stack(const std::string &file,
                const std::vector<StackItem> &stack,
                const std::vector<StackItem> &deadstack);

#endif
