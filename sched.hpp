// Copyright 2003-2024 Nicholas J. Kain <njkain at gmail dot com>
// SPDX-License-Identifier: MIT
#ifndef NCRON_SCHED_HPP_
#define NCRON_SCHED_HPP_
#include <stdint.h>
#include <assert.h>
#include <sys/time.h>

struct Job
{
    struct Job *next_;
    char *command_;
    char *args_;
    time_t exectime_;        /* time at which we will execute in the future */
    time_t lasttime_;        /* time that the job last ran */
    int id_;
    unsigned int interval_;  /* min interval between executions in seconds */
    unsigned int numruns_;   /* number of times a job has run */
    unsigned int maxruns_;   /* max # of times a job will run, 0 = nolim */
    bool journal_;
    bool runat_;

    bool cst_hhmm_[1440]; // If corresponding bit is set, time is allowed.
    bool cst_mday_[31];
    bool cst_wday_[7];
    bool cst_mon_[12];
};

void job_init(struct Job *);
void job_destroy(struct Job *);
void job_insert(struct Job **head, struct Job *elt);

void job_set_initial_exectime(struct Job *);
void job_exec(struct Job *, const struct timespec *ts);

void parse_config(char const *path, char const *execfile,
                  struct Job **stack, struct Job **deadstack);
#endif
