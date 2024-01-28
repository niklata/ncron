// Copyright 2003-2024 Nicholas J. Kain <njkain at gmail dot com>
// SPDX-License-Identifier: MIT
#ifndef NCRON_SCHED_HPP_
#define NCRON_SCHED_HPP_
#include <assert.h>
#include <string>
#include <vector>
#include <sys/time.h>

struct Job
{
    Job() : id(0), exectime(0), lasttime(0), interval(0),
                    numruns(0), maxruns(0), journal(false) {}
    Job(Job &) = delete;
    Job(Job &&o) noexcept
    {
        swap(o);
        o.clear();
    }
    Job &operator=(Job &) = delete;
    Job &operator=(Job &&o) noexcept
    {
        swap(o);
        o.clear();
        return *this;
    }

    using cst_list = std::vector<std::pair<int,int>>;

    unsigned int id;
    time_t exectime;        /* time at which we will execute in the future */
    time_t lasttime;        /* time that the job last ran */
    unsigned int interval;  /* min interval between executions in seconds */
    unsigned int numruns;   /* number of times a job has run */
    unsigned int maxruns;   /* max # of times a job will run, 0 = nolim */
    bool journal;
    std::string command;
    std::string args;

    cst_list month;       /* 1-12, l=0  is wildcard, h=l is no range */
    cst_list day;         /* 1-31, l=0  is wildcard, h=l is no range */
    cst_list weekday;     /* 1-7,  l=0  is wildcard, h=l is no range */
    cst_list hour;        /* 0-23, l=24 is wildcard, h=l is no range */
    cst_list minute;      /* 0-59, l=60 is wildcard, h=l is no range */

    void swap(Job &o) noexcept
    {
        using std::swap;
        swap(id, o.id);
        swap(exectime, o.exectime);
        swap(lasttime, o.lasttime);
        swap(interval, o.interval);
        swap(numruns, o.numruns);
        swap(maxruns, o.maxruns);
        swap(journal, o.journal);
        swap(command, o.command);
        swap(args, o.args);
        swap(month, o.month);
        swap(day, o.day);
        swap(weekday, o.weekday);
        swap(hour, o.hour);
        swap(minute, o.minute);
    }
    void clear()
    {
        id = 0;
        exectime = 0;
        lasttime = 0;
        interval = 0;
        numruns = 0;
        maxruns = 0;
        journal = false;
        command.clear();
        args.clear();
        month.clear();
        day.clear();
        weekday.clear();
        hour.clear();
        minute.clear();
    }

    bool operator<(const Job &o) const { return exectime < o.exectime; }
    bool operator>(const Job &o) const { return exectime > o.exectime; }
    void exec(const struct timespec &ts);
private:
    void set_next_time();
};

extern std::vector<Job> g_jobs;

struct StackItem {
    StackItem(size_t j) : jidx(j) { assert(g_jobs.size() > j); }
    size_t jidx;
};

static inline bool LtCronEntry(const StackItem &a,
                               const StackItem &b)
{
    return g_jobs[a.jidx] < g_jobs[b.jidx];
}

void set_initial_exectime(Job &entry);

#endif
