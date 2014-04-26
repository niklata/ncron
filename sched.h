#ifndef NCRON_SCHED_H_
#define NCRON_SCHED_H_
/* sched.h - ncron job scheduling
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

#include <sys/types.h>

typedef struct
{
    int l;
    int h;
} ipair_t;

typedef struct
{
    ipair_t node;
    void *next;
} ipair_node_t;

typedef struct
{
    struct rlimit *cpu;
    struct rlimit *fsize;
    struct rlimit *data;
    struct rlimit *stack;
    struct rlimit *core;
    struct rlimit *rss;
    struct rlimit *nproc;
    struct rlimit *nofile;
    struct rlimit *memlock;
    struct rlimit *as;
} limit_t;

typedef struct
{
    int id;
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
    limit_t *limits;
    void *next;
} cronentry_t;

void set_initial_exectime(cronentry_t *entry);
time_t get_next_time(cronentry_t *entry);
void stack_insert(cronentry_t *item, cronentry_t **stack);
void save_stack(char *file, cronentry_t *stack, cronentry_t *deadstack);
void free_ipair_node_list (ipair_node_t *list);
void free_stack(cronentry_t **stack);
void free_cronentry (cronentry_t **p);

#endif
