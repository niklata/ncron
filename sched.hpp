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

#include <vector>
#include <memory>
#include <sys/time.h>
#include <sys/resource.h>
#include <boost/optional.hpp>
#include <boost/utility.hpp>

struct ipair_t
{
    int l;
    int h;
};

struct ipair_node_t
{
    ipair_t node;
    ipair_node_t *next;
};

class rlimits : boost::noncopyable
{
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
    boost::optional<rlimit> sigpending;
};

struct cronentry_t
{
    unsigned int id;
    uid_t user;
    gid_t group;
    time_t exectime;        /* time at which we will execute in the future */
    time_t lasttime;        /* time that the job last ran */
    unsigned int interval;  /* min interval between executions in seconds */
    unsigned int numruns;   /* number of times a job has run */
    unsigned int maxruns;   /* max # of times a job will run, 0 = nolim */
    int journal;
    char *command;
    char *args;
    char *chroot;
    ipair_node_t *month;     /* 1-12, l=0  is wildcard, h=l is no range */
    ipair_node_t *day;       /* 1-31, l=0  is wildcard, h=l is no range */
    ipair_node_t *weekday;   /* 1-7,  l=0  is wildcard, h=l is no range */
    ipair_node_t *hour;      /* 0-23, l=24 is wildcard, h=l is no range */
    ipair_node_t *minute;    /* 0-59, l=60 is wildcard, h=l is no range */
    rlimits *limits;

    inline bool operator<(const cronentry_t &o) {
        return exectime < o.exectime;
    }
};

static inline bool GtCronEntry(const std::unique_ptr<cronentry_t> &a,
                               const std::unique_ptr<cronentry_t> &b) {
    return a->exectime > b->exectime;
}

void set_initial_exectime(cronentry_t *entry);
time_t get_next_time(const cronentry_t &entry);
void save_stack(const char *file,
                const std::vector<std::unique_ptr<cronentry_t>> &stack,
                const std::vector<std::unique_ptr<cronentry_t>> &deadstack);
void free_ipair_node_list (ipair_node_t *list);
void free_cronentry (cronentry_t **p);

#endif
