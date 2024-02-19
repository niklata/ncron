// Copyright 2003-2024 Nicholas J. Kain <njkain at gmail dot com>
// SPDX-License-Identifier: MIT
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <limits.h>
#include <assert.h>
extern "C" {
#include "nk/log.h"
#include "nk/pspawn.h"
#include "nk/io.h"
}
#include "ncron.hpp"
#include "sched.hpp"

extern char **environ;

// If the job isn't able to be run in the next five years,
// it probably won't run in the uptime of the machine.
#define MAX_YEARS 5

void job_init(struct Job *self)
{
    self->next_ = NULL;
    self->command_ = NULL;
    self->args_ = NULL;
    self->exectime_ = 0;
    self->lasttime_ = 0;
    self->id_ = -1;
    self->interval_ = 0;
    self->numruns_ = 0;
    self->journal_ = false;
    self->runat_ = false;
    // Allowed by default.
    memset(&self->cst_hhmm_, 1, sizeof self->cst_hhmm_);
    memset(&self->cst_mday_, 1, sizeof self->cst_mday_);
    memset(&self->cst_wday_, 1, sizeof self->cst_wday_);
    memset(&self->cst_mon_, 1, sizeof self->cst_mon_);
}

void job_destroy(struct Job *self)
{
    if (self->command_) { free(self->command_); self->command_ = NULL; }
    if (self->args_) { free(self->args_); self->args_ = NULL; }
}

static bool job_in_month(const struct Job *self, int v)
{
    assert(v > 0 && v < 13);
    return self->cst_mon_[v - 1];
}

static bool job_in_mday(const struct Job *self, int v)
{
    assert(v > 0 && v < 32);
    return self->cst_mday_[v - 1];
}

static bool job_in_wday(const struct Job *self, int v)
{
    assert(v > 0 && v < 8);
    return self->cst_wday_[v - 1];
}

static bool job_in_hhmm(const struct Job *self, int h, int m)
{
    assert(h >= 0 && h < 24);
    assert(m >= 0 && h < 60);
    return self->cst_hhmm_[h * 60 + m];
}

static bool is_leap_year(int year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static int days_in_month(int month, int year)
{
    int ret = 31;
    switch (month) {
    case 2: /* we follow the gregorian calendar */
        if (is_leap_year(year)) ret = 29;
        else ret = 28;
        break;
    case 4: case 6: case 9: case 11: ret = 30; default: break;
    }
    return ret;
}

// So, the trick here is that we have inclusive ranges in the constraint
// lists.  So, if something is to be allowed, it must be included in all
// extant constraint lists.  Thus, we either have to invert the sense of
// the constraint ranges to be exclusive, or we must construct the filter
// by assigning a bit to each list, and a particular day is allowed iif
// all corresponding bits to each list are set.
struct day_sieve
{
    time_t start_ts; // inclusive
    // bit0 = month
    // bit1 = mday
    // bit2 = wday
    uint8_t filter[366];

};

static bool day_sieve_day_ok(struct day_sieve *self, int i) { return self->filter[i] == 7; }

static bool day_sieve_build(struct day_sieve *self, Job const *entry, int year)
{
    memset(self->filter, 0, sizeof self->filter);

    struct tm t = {};
    t.tm_mday = 1;
    t.tm_year = year;
    t.tm_isdst = -1;
    self->start_ts = mktime(&t);
    if (self->start_ts == -1) return false;

    size_t fi = 0;
    for (size_t month = 1; month <= 12; ++month) {
        bool include_month = job_in_month(entry, month);
        for (int j = 0, jend = days_in_month(month, year); j < jend; ++j, ++fi) {
            if (include_month) self->filter[fi] |= 1;
        }
    }
    fi = 0;
    for (size_t month = 1; month <= 12; ++month) {
        for (int day = 1, dayend = days_in_month(month, year); day <= dayend; ++day, ++fi) {
            if (job_in_mday(entry, day)) {
                self->filter[fi] |= 2;
            }
        }
    }
    int sdow = t.tm_wday + 1; // starting wday of year
    int weekday = sdow; // day of the week we're checking
    int sday = 0; // starting day of year
    for (;;) {
        if (job_in_wday(entry, weekday)) {
            for (size_t i = static_cast<size_t>(sday); i < sizeof self->filter; i += 7) self->filter[i] |= 4;
        }
        weekday = weekday % 7 + 1;
        if (weekday == sdow) break;
        ++sday;
        assert(sday < 7);
    }
    // At least one day should be allowed, otherwise
    // the job will never run.
    for (size_t i = 0; i < sizeof self->filter; ++i) {
        if (self->filter[i] == 7) return true;
    }
    return false;
}

/* stime is the time we're constraining
 * returns a time value that has been appropriately constrained */
static time_t job_constrain_time(struct Job *self, time_t stime)
{
    struct tm *rtime;
    time_t t;

    rtime = localtime(&stime);

    int cyear = rtime->tm_year;
    int syear = cyear;
    struct day_sieve ds;
    if (!day_sieve_build(&ds, self, rtime->tm_year)) return 0;

    for (;;) {
        if (cyear - syear >= MAX_YEARS)
            return 0;
        t = mktime(rtime);
        rtime = localtime(&t);
        if (rtime->tm_year != cyear) {
            if (!day_sieve_build(&ds, self, rtime->tm_year)) return 0;
            cyear = rtime->tm_year;
        }

        if (!day_sieve_day_ok(&ds, rtime->tm_yday)) {
            // Day isn't allowed.  Advance to the start of
            // the next allowed day.
            rtime->tm_min = 0;
            rtime->tm_hour = 0;
            rtime->tm_mday++;
            int ndays = is_leap_year(rtime->tm_year) ? 365 : 364;
            for (int i = rtime->tm_yday + 1; i < ndays; ++i) {
                if (day_sieve_day_ok(&ds, i))
                    goto day_ok;
                rtime->tm_mday++;
            }
            // If we get here, then we've exhausted the year.
            rtime->tm_mday = 1;
            rtime->tm_mon = 0;
            rtime->tm_year++;
            continue;
        }
    day_ok:
        for (;;) {
            if (job_in_hhmm(self, rtime->tm_hour, rtime->tm_min))
                return mktime(rtime);
            ++rtime->tm_min;
            if (rtime->tm_min == 60) {
                // Advance to next hour.
                rtime->tm_min = 0;
                ++rtime->tm_hour;
                if (rtime->tm_hour == 24) {
                    // Advance to next day.
                    rtime->tm_min = 0;
                    rtime->tm_hour = 0;
                    rtime->tm_mday++;
                    break;
                }
                // Necessary to deal with DST hour shifts.
                t = mktime(rtime);
                rtime = localtime(&t);
            }
        }
    }
    /* Failed to find a suitable time. */
    return 0;
}

/* Used when jobs without exectimes are first loaded. */
void job_set_initial_exectime(struct Job *self)
{
    struct timespec ts;
    clock_or_die(&ts);
    time_t ttm = job_constrain_time(self, ts.tv_sec);
    time_t ttd = ttm - self->lasttime_;
    if (ttd < self->interval_) {
        ttm += self->interval_ - ttd;
        ttm = job_constrain_time(self, ttm);
    }
    self->exectime_ = ttm;
}

// Advances to next time of execution; performs constraint
static void job_set_next_time(struct Job *self)
{
    struct timespec ts;
    clock_or_die(&ts);
    time_t etime = job_constrain_time(self, ts.tv_sec + self->interval_);
    self->exectime_ = etime > ts.tv_sec ? etime : 0;
}

void job_exec(struct Job *self, const struct timespec *ts)
{
    pid_t pid;
    if (int ret = nk_pspawn(&pid, self->command_, nullptr, nullptr, self->args_, environ)) {
        log_line("posix_spawn failed for '%s': %s", self->command_, strerror(ret));
        return;
    }
    ++self->numruns_;
    self->lasttime_ = ts->tv_sec;
    job_set_next_time(self);
}

void job_insert(struct Job **head, struct Job *elt)
{
    elt->next_ = nullptr;
    for (;;) {
        if (!*head) {
            *head = elt;
            return;
        }
        if (elt->exectime_ < (*head)->exectime_) {
            elt->next_ = *head;
            *head = elt;
            return;
        }
        head = &(*head)->next_;
    }
}
