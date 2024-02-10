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
#include <algorithm>
#include "ncron.hpp"
#include "sched.hpp"

extern char **environ;

// If the job isn't able to be run in the next five years,
// it probably won't run in the uptime of the machine.
#define MAX_YEARS 5

bool Job::in_month(int v) const
{
    assert(v > 0 && v < 13);
    return cst_mon_[v - 1];
}

bool Job::in_mday(int v) const
{
    assert(v > 0 && v < 32);
    return cst_mday_[v - 1];
}

bool Job::in_wday(int v) const
{
    assert(v > 0 && v < 8);
    return cst_wday_[v - 1];
}

bool Job::in_hhmm(int h, int m) const
{
    assert(h >= 0 && h < 24);
    assert(m >= 0 && h < 60);
    return cst_hhmm_[h * 60 + m];
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

    [[nodiscard]] bool day_ok(int i) const { return filter[i] == 7; }

    [[nodiscard]] bool build(Job const *entry, int year)
    {
        memset(filter, 0, sizeof filter);

        struct tm t = {};
        t.tm_mday = 1;
        t.tm_year = year;
        t.tm_isdst = -1;
        start_ts = mktime(&t);
        if (start_ts == -1) return false;

        size_t fi = 0;
        for (size_t month = 1; month <= 12; ++month) {
            bool include_month = entry->in_month(month);
            for (int j = 0, jend = days_in_month(month, year); j < jend; ++j, ++fi) {
                if (include_month) filter[fi] |= 1;
            }
        }
        fi = 0;
        for (size_t month = 1; month <= 12; ++month) {
            for (int day = 1, dayend = days_in_month(month, year); day <= dayend; ++day, ++fi) {
                if (entry->in_mday(day)) {
                    filter[fi] |= 2;
                }
            }
        }
        auto sdow = t.tm_wday + 1; // starting wday of year
        auto weekday = sdow; // day of the week we're checking
        auto sday = 0; // starting day of year
        for (;;) {
            if (entry->in_wday(weekday)) {
                for (size_t i = static_cast<size_t>(sday); i < sizeof filter; i += 7) filter[i] |= 4;
            }
            weekday = weekday % 7 + 1;
            if (weekday == sdow) break;
            ++sday;
            assert(sday < 7);
        }
        // At least one day should be allowed, otherwise
        // the job will never run.
        for (size_t i = 0; i < sizeof filter; ++i) {
            if (filter[i] == 7) return true;
        }
        return false;
    }
};

/* stime is the time we're constraining
 * returns a time value that has been appropriately constrained */
time_t Job::constrain_time(time_t stime) const
{
    struct tm *rtime;
    time_t t;

    rtime = localtime(&stime);

    int cyear = rtime->tm_year;
    int syear = cyear;
    day_sieve ds;
    if (!ds.build(this, rtime->tm_year)) return 0;

    for (;;) {
        if (cyear - syear >= MAX_YEARS)
            return 0;
        t = mktime(rtime);
        rtime = localtime(&t);
        if (rtime->tm_year != cyear) {
            if (!ds.build(this, rtime->tm_year)) return 0;
            cyear = rtime->tm_year;
        }

        if (!ds.day_ok(rtime->tm_yday)) {
            // Day isn't allowed.  Advance to the start of
            // the next allowed day.
            rtime->tm_min = 0;
            rtime->tm_hour = 0;
            rtime->tm_mday++;
            auto ndays = is_leap_year(rtime->tm_year) ? 365 : 364;
            for (int i = rtime->tm_yday + 1; i < ndays; ++i) {
                if (ds.day_ok(i))
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
            if (in_hhmm(rtime->tm_hour, rtime->tm_min))
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
void Job::set_initial_exectime()
{
    struct timespec ts;
    clock_or_die(&ts);
    time_t ttm = constrain_time(ts.tv_sec);
    time_t ttd = ttm - lasttime_;
    if (ttd < interval_) {
        ttm += interval_ - ttd;
        ttm = constrain_time(ttm);
    }
    exectime_ = ttm;
}

/* stupidly advances to next time of execution; performs constraint.  */
void Job::set_next_time()
{
    struct timespec ts;
    clock_or_die(&ts);
    auto etime = constrain_time(ts.tv_sec + interval_);
    exectime_ = etime > ts.tv_sec ? etime : 0;
}

void Job::exec(const struct timespec &ts)
{
    pid_t pid;
    if (int ret = nk_pspawn(&pid, command_, nullptr, nullptr, args_, environ)) {
        log_line("posix_spawn failed for '%s': %s", command_, strerror(ret));
        return;
    }
    ++numruns_;
    lasttime_ = ts.tv_sec;
    set_next_time();
}

