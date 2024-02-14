// Copyright 2003-2024 Nicholas J. Kain <njkain at gmail dot com>
// SPDX-License-Identifier: MIT
#ifndef NCRON_SCHED_HPP_
#define NCRON_SCHED_HPP_
#include <stdint.h>
#include <assert.h>
#include <sys/time.h>

struct Job
{
    Job();
    Job(Job &) = delete;
    Job(Job &&o) = delete;
    Job &operator=(Job &) = delete;
    Job &operator=(Job &&o) noexcept = delete;
    ~Job();

    Job *next_ = nullptr;
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

void job_insert(Job **head, Job *elt);

void parse_config(char const *path, char const *execfile,
                  Job **stack, Job **deadstack);
#endif
