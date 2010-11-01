/*
 * sched.h - task scheduling features for ncron include
 *
 * (C) 2003-2007 Nicholas J. Kain <njk@aerifal.cx>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
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

void force_to_constraint(cronentry_t *entry);
time_t get_first_time(cronentry_t *entry);
time_t get_next_time(cronentry_t *entry);
void stack_insert(cronentry_t *item, cronentry_t **stack);
void save_stack(char *file, cronentry_t *stack, cronentry_t *deadstack);
void free_ipair_node_list (ipair_node_t *list);
void free_stack(cronentry_t **stack);
void free_cronentry (cronentry_t **p);

