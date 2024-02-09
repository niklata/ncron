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
        memset(&cst_hhmm_, 1, sizeof cst_hhmm_);
        memset(&cst_mday_, 1, sizeof cst_mday_);
        memset(&cst_wday_, 1, sizeof cst_wday_);
        memset(&cst_mon_, 1, sizeof cst_mon_);
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
        if (command_) free(command_);
        if (args_) free(args_);
    }

    char *command_ = nullptr;
    char *args_ = nullptr;
    time_t exectime_ = 0;        /* time at which we will execute in the future */
    time_t lasttime_ = 0;        /* time that the job last ran */
    int id_ = -1;
    unsigned int interval_ = 0;  /* min interval between executions in seconds */
    unsigned int numruns_ = 0;   /* number of times a job has run */
    unsigned int maxruns_ = 0;   /* max # of times a job will run, 0 = nolim */
    bool journal_ = false;

    bool cst_hhmm_[1440]; // If corresponding bit is set, time is allowed.
    bool cst_mday_[31];
    bool cst_wday_[7];
    bool cst_mon_[12];

    void swap(Job &o) noexcept
    {
        using std::swap;
        swap(id_, o.id_);
        swap(exectime_, o.exectime_);
        swap(lasttime_, o.lasttime_);
        swap(interval_, o.interval_);
        swap(numruns_, o.numruns_);
        swap(maxruns_, o.maxruns_);
        swap(journal_, o.journal_);
        swap(command_, o.command_);
        swap(args_, o.args_);
        for (size_t i = 0; i < sizeof cst_hhmm_; ++i)
            swap(cst_hhmm_[i], o.cst_hhmm_[i]);
        for (size_t i = 0; i < sizeof cst_mday_; ++i)
            swap(cst_mday_[i], o.cst_mday_[i]);
        for (size_t i = 0; i < sizeof cst_wday_; ++i)
            swap(cst_wday_[i], o.cst_wday_[i]);
        for (size_t i = 0; i < sizeof cst_mon_; ++i)
            swap(cst_mon_[i], o.cst_mon_[i]);
    }
    void clear()
    {
        id_ = -1;
        exectime_ = 0;
        lasttime_ = 0;
        interval_ = 0;
        numruns_ = 0;
        maxruns_ = 0;
        journal_ = false;
        if (command_) {
            free(command_);
            command_ = nullptr;
        }
        if (args_) {
            free(args_);
            args_ = nullptr;
        }
        // Allowed by default.
        memset(&cst_hhmm_, 1, sizeof cst_hhmm_);
        memset(&cst_mday_, 1, sizeof cst_mday_);
        memset(&cst_wday_, 1, sizeof cst_wday_);
        memset(&cst_mon_, 1, sizeof cst_mon_);
    }

    bool operator<(const Job &o) const { return exectime_ < o.exectime_; }
    bool operator>(const Job &o) const { return exectime_ > o.exectime_; }
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
