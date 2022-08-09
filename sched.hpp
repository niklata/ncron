// Copyright 2003-2012 Nicholas J. Kain <njkain at gmail dot com>
// SPDX-License-Identifier: MIT
#ifndef NCRON_SCHED_HPP_
#define NCRON_SCHED_HPP_
#include <string>
#include <vector>
#include <memory>
#include <sys/time.h>
#include <sys/resource.h>

struct cronentry_t
{
    cronentry_t() : id(0), exectime(0), lasttime(0), interval(0),
                    numruns(0), maxruns(0), journal(false) {}
    typedef std::vector<std::pair<int,int>> cst_list;
    unsigned int id;
    time_t exectime;        /* time at which we will execute in the future */
    time_t lasttime;        /* time that the job last ran */
    unsigned int interval;  /* min interval between executions in seconds */
    unsigned int numruns;   /* number of times a job has run */
    unsigned int maxruns;   /* max # of times a job will run, 0 = nolim */
    bool journal;
    std::string command;
    std::string args;
    std::string path;

    cst_list month;       /* 1-12, l=0  is wildcard, h=l is no range */
    cst_list day;         /* 1-31, l=0  is wildcard, h=l is no range */
    cst_list weekday;     /* 1-7,  l=0  is wildcard, h=l is no range */
    cst_list hour;        /* 0-23, l=24 is wildcard, h=l is no range */
    cst_list minute;      /* 0-59, l=60 is wildcard, h=l is no range */

    inline bool operator<(const cronentry_t &o) const {
        return exectime < o.exectime;
    }
    void exec(const struct timespec &ts);
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
