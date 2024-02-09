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

// XXX: The constraint lists should go away and simply be replaced
//      by LUTs; we can then set the appropriate bits in the LUT
//      at configuration time.  This will make everything faster
//      and probably will cost a similar amount of memory.

static bool constraint_lt(const std::pair<int, int> &a, const std::pair<int, int> &b)
{
    if (a.first < b.first) return true;
    if (a.first == b.first) {
        return a.second < b.second;
    } else return false;
}
// PRECONDITION: al <= bl; because the constraints are sorted
// So, we can merge (al,ah) and (bl, bh) iif
// ah >= bl => (al, ah < bh ? bh : ah)
static void merge_constraint_list(Job::cst_list &list)
{
    auto a = list.begin();
    for (;;) {
        if (list.size() < 2) return;
        auto b = a + 1;
        if (b == list.end()) return;
        assert(a->first <= b->first);
        if (a->second >= b->first) {
            a->second = a->second < b->second ? b->second : a->second;
            list.erase(b);
            continue;
        }
        ++a;
    }
}

void Job::merge_constraints()
{
    merge_constraint_list(month);
    merge_constraint_list(day);
    merge_constraint_list(weekday);
    merge_constraint_list(hour);
    merge_constraint_list(minute);
}

bool Job::add_constraint(cst_list &list, int low, int high, int wildcard, int min, int max)
{
    if (low > max || low < min)
        low = wildcard;
    if (high > max || high < min)
        high = wildcard;

    /* we don't allow meaningless 'rules' */
    if (low == wildcard && high == wildcard)
        return false;

    // Keep the constraint lists sorted as we go so that merging is easier.
    if (low > high) {
        /* discontinuous range, split into two continuous rules... */
        auto a = std::make_pair(low, max);
        auto b = std::make_pair(min, high);
        list.insert(std::upper_bound(list.begin(), list.end(), a, constraint_lt), a);
        list.insert(std::upper_bound(list.begin(), list.end(), b, constraint_lt), b);
    } else {
        /* handle continuous ranges normally */
        auto a = std::make_pair(low, high);
        list.insert(std::upper_bound(list.begin(), list.end(), a, constraint_lt), a);
    }
    return true;
}

/*
 * returns -1 if below range
 * returns 0 if within range
 * returns 1 if above range
 */
static inline int compare_range(const std::pair<int,int> &r,
                                int v, int wildcard)
{
    if (r.first == wildcard && r.second == wildcard)
        return 0;
    if (r.first == wildcard) {
        if (v < r.second)
            return -1;
        if (v > r.second)
            return 1;
        return 0;
    }
    if (r.second == wildcard) {
        if (v < r.first)
            return -1;
        if (v > r.first)
            return 1;
        return 0;
    }
    if (v < r.first)
        return -1;
    if (v > r.second)
        return 1;
    return 0;
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
    time_t end_ts; // exclusive; actual last ts value in year + 1
    // bit0 = month
    // bit1 = mday
    // bit2 = wday
    uint8_t filter[366];

    [[nodiscard]] bool day_ok(int i) const { return filter[i] == 7; }

    [[nodiscard]] bool build(const Job &entry, int year)
    {
        memset(filter, 0, sizeof filter);

        struct tm t = {};
        t.tm_mday = 1;
        t.tm_year = year;
        t.tm_isdst = -1;
        start_ts = mktime(&t);
        if (start_ts == -1) return false;

        if (entry.month.empty()) {
            for (size_t i = 0; i < sizeof filter; ++i) filter[i] |= 1;
        } else {
            size_t fi = 0;
            for (size_t month = 1; month <= 12; ++month) {
                for (const auto &c: entry.month) {
                    if (compare_range(c, month, 0) == 0) {
                        for (int j = 0, jend = days_in_month(month, year); j < jend; ++j, ++fi) {
                            filter[fi] |= 1;
                        }
                    }
                }
            }
        }
        if (entry.day.empty()) {
            for (size_t i = 0; i < sizeof filter; ++i) filter[i] |= 2;
        } else {
            size_t fi = 0;
            for (size_t month = 1; month <= 12; ++month) {
                for (int day = 1, dayend = days_in_month(month, year); day <= dayend; ++day, ++fi) {
                    for (const auto &c: entry.day) {
                        if (compare_range(c, day, 0) == 0) {
                            filter[fi] |= 2;
                        }
                    }
                }
            }
        }
        if (entry.weekday.empty()) {
            for (size_t i = 0; i < sizeof filter; ++i) filter[i] |= 4;
        } else {
            auto sdow = t.tm_wday + 1; // starting wday of year
            auto weekday = sdow; // day of the week we're checking
            auto sday = 0; // starting day of year
            for (;;) {
                for (const auto &c: entry.weekday) {
                    if (compare_range(c, weekday, 0) == 0) {
                        for (size_t i = static_cast<size_t>(sday); i < sizeof filter; i += 7) filter[i] |= 4;
                    }
                }
                weekday = weekday % 7 + 1;
                if (weekday == sdow) break;
                ++sday;
                assert(sday < 7);
            }
        }

        t.tm_year = year + 1;
        end_ts = mktime(&t);
        if (end_ts == -1) return false;
        return true;
    }
};

/* same as above, but no check on range validity */
static int compare_list_range(const Job::cst_list &list, int *unit,
                              int *nextunit, int wildcard)
{
    int t, smallunit = INT_MAX, dist = INT_MAX, tinyunit = INT_MAX;

    if (list.empty() || !unit || !nextunit)
        return 0;

    /* find the range least distant from our target value */
    for (const auto &i: list) {
        if (i.first < tinyunit) tinyunit = i.first;
        switch (compare_range(i, *unit, wildcard)) {
            case 1: /* range below our value, avoid if possible */
                t = *unit - i.second; /* t > 0 */
                if (dist > 0 && t < dist)
                    dist = t;
                smallunit = i.first;
                break;
            default: /* bingo, our value is within a range */
                return 0;
            case -1: /* range above our value, favor */
                t = *unit - i.first; /* implicitly, t < 0 */
                if (dist > 0 || t > dist)
                    dist = t;
                smallunit = i.first;
                break;
        }
    }

    if (dist > 0) {
        (*nextunit)++;
        *unit = tinyunit;
    }
    if (dist < 0)
        *unit = smallunit;
    if (dist == 0)
        return 0;
    return 1;
}

/* entry is obvious, stime is the time we're constraining
 * returns a time value that has been appropriately constrained */
static time_t constrain_time(const Job &entry, time_t stime)
{
    struct tm *rtime;
    time_t t;

    rtime = localtime(&stime);

    int cyear = rtime->tm_year;
    int syear = cyear;
    day_sieve ds;
    if (!ds.build(entry, rtime->tm_year)) return 0;

    for (;;) {
        if (cyear - syear >= MAX_YEARS)
            return 0;
        t = mktime(rtime);
        rtime = localtime(&t);
        if (rtime->tm_year != cyear) {
            if (!ds.build(entry, rtime->tm_year)) return 0;
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
        if (compare_list_range(entry.minute, &(rtime->tm_min),
                    &(rtime->tm_hour), 60))
            continue;
        if (compare_list_range(entry.hour, &(rtime->tm_hour),
                    &(rtime->tm_mday), 24))
            continue;
        return mktime(rtime);
    }
    /* Failed to find a suitable time. */
    return 0;
}

/* Used when jobs without exectimes are first loaded. */
void set_initial_exectime(Job &entry)
{
    struct timespec ts;
    clock_or_die(&ts);
    time_t ttm = constrain_time(entry, ts.tv_sec);
    time_t ttd = ttm - entry.lasttime;
    if (ttd < entry.interval) {
        ttm += entry.interval - ttd;
        ttm = constrain_time(entry, ttm);
    }
    entry.exectime = ttm;
}

/* stupidly advances to next time of execution; performs constraint.  */
void Job::set_next_time()
{
    struct timespec ts;
    clock_or_die(&ts);
    auto etime = constrain_time(*this, ts.tv_sec + interval);
    exectime = etime > ts.tv_sec ? etime : 0;
}

void Job::exec(const struct timespec &ts)
{
    pid_t pid;
    if (int ret = nk_pspawn(&pid, command, nullptr, nullptr, args, environ)) {
        log_line("posix_spawn failed for '%s': %s", command, strerror(ret));
        return;
    }
    ++numruns;
    lasttime = ts.tv_sec;
    set_next_time();
}

