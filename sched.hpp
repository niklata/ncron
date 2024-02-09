// Copyright 2003-2024 Nicholas J. Kain <njkain at gmail dot com>
// SPDX-License-Identifier: MIT
#ifndef NCRON_SCHED_HPP_
#define NCRON_SCHED_HPP_
#include <stdint.h>
#include <assert.h>
#include <sys/time.h>
#include <vector>

struct Job
{
    Job()
    {
        // Allowed by default.
        memset(&cst_hhmm, 1, sizeof cst_hhmm);
        memset(&cst_mday, 1, sizeof cst_mday);
        memset(&cst_wday, 1, sizeof cst_wday);
        memset(&cst_mon, 1, sizeof cst_mon);
    }
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

    int id = -1;
    time_t exectime = 0;        /* time at which we will execute in the future */
    time_t lasttime = 0;        /* time that the job last ran */
    unsigned int interval = 0;  /* min interval between executions in seconds */
    unsigned int numruns = 0;   /* number of times a job has run */
    unsigned int maxruns = 0;   /* max # of times a job will run, 0 = nolim */
    bool journal = false;
    char *command = nullptr;
    char *args = nullptr;

    bool cst_hhmm[1440]; // If corresponding bit is set, time is allowed.
    bool cst_mday[31];
    bool cst_wday[7];
    bool cst_mon[12];

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
        for (size_t i = 0; i < sizeof cst_hhmm; ++i)
            swap(cst_hhmm[i], o.cst_hhmm[i]);
        for (size_t i = 0; i < sizeof cst_mday; ++i)
            swap(cst_mday[i], o.cst_mday[i]);
        for (size_t i = 0; i < sizeof cst_wday; ++i)
            swap(cst_wday[i], o.cst_wday[i]);
        for (size_t i = 0; i < sizeof cst_mon; ++i)
            swap(cst_mon[i], o.cst_mon[i]);
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
        // Allowed by default.
        memset(&cst_hhmm, 1, sizeof cst_hhmm);
        memset(&cst_mday, 1, sizeof cst_mday);
        memset(&cst_wday, 1, sizeof cst_wday);
        memset(&cst_mon, 1, sizeof cst_mon);
    }

    bool operator<(const Job &o) const { return exectime < o.exectime; }
    bool operator>(const Job &o) const { return exectime > o.exectime; }
    void set_initial_exectime();
    void exec(const struct timespec &ts);

    bool in_month(int v) const;
    bool in_mday(int v) const;
    bool in_wday(int v) const;
    bool in_hhmm(int h, int m) const;
private:
    void set_next_time();
    time_t constrain_time(time_t stime) const;
};

extern std::vector<Job> g_jobs;

static inline bool LtCronEntry(size_t a, size_t b)
{
    return g_jobs[a] < g_jobs[b];
}

#endif
