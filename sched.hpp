// Copyright 2003-2024 Nicholas J. Kain <njkain at gmail dot com>
// SPDX-License-Identifier: MIT
#ifndef NCRON_SCHED_HPP_
#define NCRON_SCHED_HPP_
#include <assert.h>
#include <vector>
#include <sys/time.h>

struct Job
{
    Job() {}
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
    ~Job()
    {
        if (command) free(command);
        if (args) free(args);
    }

    using cst_list = std::vector<std::pair<int,int>>;

    int id = -1;
    time_t exectime = 0;        /* time at which we will execute in the future */
    time_t lasttime = 0;        /* time that the job last ran */
    unsigned int interval = 0;  /* min interval between executions in seconds */
    unsigned int numruns = 0;   /* number of times a job has run */
    unsigned int maxruns = 0;   /* max # of times a job will run, 0 = nolim */
    bool journal = false;
    char *command = nullptr;
    char *args = nullptr;

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
        id = -1;
        exectime = 0;
        lasttime = 0;
        interval = 0;
        numruns = 0;
        maxruns = 0;
        journal = false;
        if (command) {
            free(command);
            command = nullptr;
        }
        if (args) {
            free(args);
            args = nullptr;
        }
        month.clear();
        day.clear();
        weekday.clear();
        hour.clear();
        minute.clear();
    }

    bool add_constraint(cst_list &list, int low, int high, int wildcard, int min, int max);

    bool operator<(const Job &o) const { return exectime < o.exectime; }
    bool operator>(const Job &o) const { return exectime > o.exectime; }
    void exec(const struct timespec &ts);
private:
    void set_next_time();
};

extern std::vector<Job> g_jobs;

static inline bool LtCronEntry(size_t a, size_t b)
{
    return g_jobs[a] < g_jobs[b];
}

void set_initial_exectime(Job &entry);

#endif
