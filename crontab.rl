// Copyright 2003-2024 Nicholas J. Kain <njkain at gmail dot com>
// SPDX-License-Identifier: MIT
#include <algorithm>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <nk/from_string.hpp>
#include <nk/defer.hpp>
extern "C" {
#include "nk/log.h"
}
#include "ncron.hpp"
#include "sched.hpp"
#include "crontab.hpp"

#define MAX_LINE 2048

#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif

extern int gflags_debug;

std::vector<std::unique_ptr<Job>> g_jobs;

struct item_history {
    time_t exectime = 0;
    time_t lasttime = 0;
    unsigned int numruns = 0;
};

struct history_entry
{
    history_entry(int id_, item_history h_) : id(id_), history(h_) {}

    int id;
    item_history history;
};
static std::vector<history_entry> history_lut;

struct ParseCfgState
{
    ParseCfgState(std::vector<size_t> *stk, std::vector<size_t> *dstk)
    : stack(stk), deadstack(dstk)
    {
        memset(v_str, 0, sizeof v_str);
        ce = std::make_unique<Job>();
    }
    char v_str[MAX_LINE];

    std::vector<size_t> *stack;
    std::vector<size_t> *deadstack;

    std::unique_ptr<Job> ce;

    const char *jobid_st = nullptr;
    const char *time_st = nullptr;
    const char *intv_st = nullptr;
    const char *intv2_st = nullptr;
    const char *strv_st = nullptr;

    size_t v_strlen = 0;
    size_t linenum = 0;

    unsigned int v_time;

    int v_int1 = 0;
    int v_int2 = 0;
    int v_int3 = -1;
    int v_int4 = -1;

    int cs = 0;
    bool have_command = false;

    bool intv2_exist = false;
    bool runat = false;

    bool parse_error = false;

    bool seen_cst_hhmm = false;
    bool seen_cst_wday = false;
    bool seen_cst_mday = false;
    bool seen_cst_mon = false;

    void get_history()
    {
        for (const auto &i: history_lut) {
            if (i.id == ce->id_) {
                ce->exectime_ = i.history.exectime;
                ce->lasttime_ = i.history.lasttime;
                ce->numruns_ = i.history.numruns;
                return;
            }
        }
    }

    void create_ce()
    {
        ce = std::make_unique<Job>();
        have_command = false;
        runat = false;
        seen_cst_hhmm = false;
        seen_cst_wday = false;
        seen_cst_mday = false;
        seen_cst_mon = false;
    }

    inline void debug_print_ce() const
    {
        if (!gflags_debug)
            return;
        log_line("-=- finish_ce -=-");
        log_line("id: %d", ce->id_);
        log_line("command: %s", ce->command_ ? ce->command_ : "");
        log_line("args: %s", ce->args_ ? ce->args_ : "");
        log_line("numruns: %u", ce->numruns_);
        log_line("maxruns: %u", ce->maxruns_);
        log_line("journal: %s", ce->journal_ ? "true" : "false");
        log_line("runat: %s", runat ? "true" : "false");
        log_line("interval: %u", ce->interval_);
        log_line("exectime: %lu", ce->exectime_);
        log_line("lasttime: %lu", ce->lasttime_);
    }

    inline void debug_print_ce_history() const
    {
        if (!gflags_debug)
            return;
        log_line("[%d]->numruns = %u", ce->id_, ce->numruns_);
        log_line("[%d]->exectime = %lu", ce->id_, ce->exectime_);
        log_line("[%d]->lasttime = %lu", ce->id_, ce->lasttime_);
    }

    void finish_ce()
    {
        const auto append_stack = [this](bool is_alive) {
            assert(g_jobs.size() > 0);
            (is_alive ? stack : deadstack)->emplace_back(g_jobs.size() - 1);
        };

        defer [this]{ create_ce(); };
        debug_print_ce();

        if (ce->id_ < 0
            || (ce->interval_ <= 0 && ce->exectime_ <= 0)
            || !ce->command_ || !have_command) {
            if (gflags_debug)
                log_line("===> IGNORE");
            return;
        }
        // XXX: O(n^2) might be nice to avoid.
        for (auto &i: g_jobs) {
            if (i->id_ == ce->id_) {
                log_line("ERROR IN CRONTAB: ignoring duplicate entry for job %d", ce->id_);
                return;
            }
        }
        if (gflags_debug)
            log_line("===> ADD");

        /* we have a job to insert */
        if (!runat) {
            get_history();
            debug_print_ce_history();
            ce->set_initial_exectime();

            auto numruns = ce->numruns_;
            auto maxruns = ce->maxruns_;
            auto exectime = ce->exectime_;
            g_jobs.emplace_back(std::move(ce));

            /* insert iif numruns < maxruns and no constr error */
            append_stack((maxruns == 0 || numruns < maxruns) && exectime != 0);
        } else {
            if (ce->interval_ > 0) {
                log_line("ERROR IN CRONTAB: interval is unused when runat is set: job %d", ce->id_);
            }
            auto forced_exectime = ce->exectime_;
            get_history();
            ce->exectime_ = forced_exectime;
            debug_print_ce_history();

            auto numruns = ce->numruns_;
            g_jobs.emplace_back(std::move(ce));

            /* insert iif we haven't exceeded maxruns */
            append_stack(numruns == 0);
        }
    }
};

struct hstm {
    const char *st = nullptr;
    int cs = 0;
    int id = -1;
    item_history h;
    bool parse_error = false;
};

#define MARKED_HST() hst.st, (p > hst.st ? static_cast<size_t>(p - hst.st) : 0)

%%{
    machine history_m;
    access hst.;

    action St { hst.st = p; }
    action LastTimeEn {
        if (!nk::from_string<time_t>(MARKED_HST(), &hst.h.lasttime)) {
            hst.parse_error = true;
            fbreak;
        }
    }
    action NumRunsEn {
        if (!nk::from_string<unsigned>(MARKED_HST(), &hst.h.numruns)) {
            hst.parse_error = true;
            fbreak;
        }
    }
    action ExecTimeEn {
        if (!nk::from_string<time_t>(MARKED_HST(), &hst.h.exectime)) {
            hst.parse_error = true;
            fbreak;
        }
    }
    action IdEn {
        if (!nk::from_string<int>(MARKED_HST(), &hst.id)) {
            hst.parse_error = true;
            fbreak;
        }
    }

    lasttime = '|' digit+ > St % LastTimeEn;
    numruns = ':' digit+ > St % NumRunsEn;
    exectime = '=' digit+ > St % ExecTimeEn;
    id = digit+ > St % IdEn;
    main := id exectime numruns lasttime;
}%%

%% write data;

static int do_parse_history(hstm &hst, const char *p, size_t plen)
{
    const char *pe = p + plen;
    const char *eof = pe;

    %% write init;
    %% write exec;

    if (hst.parse_error) return -1;
    if (hst.cs >= history_m_first_final)
        return 1;
    if (hst.cs == history_m_error)
        return -1;
    return -2;
}

static void parse_history(char const *path)
{
    char buf[MAX_LINE];
    auto f = fopen(path, "r");
    if (!f) {
        log_line("%s: failed to open history file \"%s\" for read: %s",
                 __func__, path, strerror(errno));
        return;
    }
    defer [&f]{ fclose(f); };
    size_t linenum = 0;
    while (!feof(f)) {
        if (!fgets(buf, sizeof buf, f)) {
            if (!feof(f))
                log_line("%s: io error fetching line of '%s'", __func__, path);
            break;
        }
        auto llen = strlen(buf);
        if (llen == 0)
            continue;
        if (buf[llen-1] == '\n')
            buf[--llen] = 0;
        ++linenum;
        hstm hst;
        const auto r = do_parse_history(hst, buf, llen);
        if (r < 0) {
            log_line("%s: %s configuration at line %zu; ignoring",
                     __func__, r == -2 ? "Incomplete" : "Malformed", linenum);
            continue;
        }
        history_lut.emplace_back(hst.id, hst.h);
    }
}

static bool add_cst_mon(ParseCfgState &ncs)
{
    int min = ncs.v_int1;
    int max = ncs.intv2_exist ? ncs.v_int2 : -1;
    if (max < 0) max = min;
    assert(min > 0 && min <= 12);
    assert(max > 0 && max <= 12);
    if (max < min) return false;
    if (min <= 0 || min > 12) return false;
    if (max <= 0 || max > 12) return false;
    if (!ncs.seen_cst_mon) {
        memset(&ncs.ce->cst_mon_, 0, sizeof ncs.ce->cst_mon_);
        ncs.seen_cst_mon = true;
    }
    for (int i = min; i <= max; ++i)
        ncs.ce->cst_mon_[i - 1] = true;
    return true;
}

static bool add_cst_mday(ParseCfgState &ncs)
{
    int min = ncs.v_int1;
    int max = ncs.intv2_exist ? ncs.v_int2 : -1;
    if (max < 0) max = min;
    assert(min > 0 && min <= 31);
    assert(max > 0 && max <= 31);
    if (max < min) return false;
    if (min <= 0 || min > 31) return false;
    if (max <= 0 || max > 31) return false;
    if (!ncs.seen_cst_mday) {
        memset(&ncs.ce->cst_mday_, 0, sizeof ncs.ce->cst_mday_);
        ncs.seen_cst_mday = true;
    }
    for (int i = min; i <= max; ++i)
        ncs.ce->cst_mday_[i - 1] = true;
    return true;
}

static bool add_cst_wday(ParseCfgState &ncs)
{
    int min = ncs.v_int1;
    int max = ncs.intv2_exist ? ncs.v_int2 : -1;
    if (max < 0) max = min;
    assert(min > 0 && min <= 7);
    assert(max > 0 && max <= 7);
    if (max < min) return false;
    if (min <= 0 || min > 7) return false;
    if (max <= 0 || max > 7) return false;
    if (!ncs.seen_cst_wday) {
        memset(&ncs.ce->cst_wday_, 0, sizeof ncs.ce->cst_wday_);
        ncs.seen_cst_wday = true;
    }
    for (int i = min; i <= max; ++i)
        ncs.ce->cst_wday_[i - 1] = true;
    return true;
}

static bool add_cst_time(ParseCfgState &ncs)
{
    bool single_value = ncs.v_int3 == -1 && ncs.v_int4 == -1;
    // Enforce that range is low-high.
    if (!single_value) {
        if (ncs.v_int3 < ncs.v_int1) return false;
        if (ncs.v_int3 == ncs.v_int1) {
            if (ncs.v_int4 < ncs.v_int2) return false;
        }
    }
    if (!ncs.seen_cst_hhmm) {
        memset(&ncs.ce->cst_hhmm_, 0, sizeof ncs.ce->cst_hhmm_);
        ncs.seen_cst_hhmm = true;
    }
    int min = ncs.v_int1 * 60 + ncs.v_int2;
    int max = ncs.v_int3 * 60 + ncs.v_int4;
    assert(min >= 0 && min < 1440);
    assert(max >= 0 && max < 1440);
    for (int i = min; i <= max; ++i)
        ncs.ce->cst_hhmm_[i] = true;
    return true;
}

struct Pckm {
    Pckm() {}
    char *st = nullptr;
    int cs = 0;
};

#define MARKED_PCKM() pckm.st, (p > pckm.st ? static_cast<size_t>(p - pckm.st) : 0)

%%{
    machine parse_cmd_key_m;
    access pckm.;

    action St { pckm.st = p; }
    action CmdEn {
        size_t l = p > pckm.st ? static_cast<size_t>(p - pckm.st) : 0;
        if (l) {
            auto ts = static_cast<char *>(malloc(l + 1));
            bool prior_bs = false;
            auto d = ts;
            for (auto c = pckm.st; c < p; ++c) {
                if (!prior_bs) {
                    switch (*c) {
                    case 0: abort(); // should never happen by construction
                    case '\\': prior_bs = true; break;
                    default: *d++ = *c; break;
                    }
                } else {
                    if (!*c) abort(); // should never happen by construction
                    *d++ = *c;
                    prior_bs = false;
                }
            }
            if (prior_bs) *d++ = '\\';
            *d++ = 0;
            ncs.ce->command_ = ts;
        }
    }
    action ArgEn {
        size_t l = p > pckm.st ? static_cast<size_t>(p - pckm.st) : 0;
        if (l) {
            auto ts = static_cast<char *>(malloc(l + 1));
            memcpy(ts, pckm.st, l);
            ts[l] = 0;
            ncs.ce->args_ = ts;
        }
    }

    sptab = [ \t];
    cmdstr = ([^\0 \t] | '\\ ' | '\\\t')+;
    cmd = sptab* (cmdstr > St % CmdEn);
    args = sptab+ ([^\0])* > St % ArgEn;
    main := cmd args?;
}%%

%% write data;

static void parse_command_key(ParseCfgState &ncs)
{
    char *p = ncs.v_str;
    const char *pe = ncs.v_str + ncs.v_strlen;
    const char *eof = pe;

    Pckm pckm;

    if (ncs.have_command) {
        log_line("Duplicate 'command' value at line %zu", ncs.linenum);
        exit(EXIT_FAILURE);
    }

    %% write init;
    %% write exec;

    if (pckm.cs == parse_cmd_key_m_error) {
        log_line("Malformed 'command' value at line %zu", ncs.linenum);
        exit(EXIT_FAILURE);
    } else if (pckm.cs >= parse_cmd_key_m_first_final) {
        ncs.have_command = true;
    } else {
        log_line("Incomplete 'command' value at line %zu", ncs.linenum);
        exit(EXIT_FAILURE);
    }
}

#define MARKED_TIME() ncs.time_st, (p > (ncs.time_st + 1) ? static_cast<size_t>(p - ncs.time_st - 1) : 0)
#define MARKED_INTV1() ncs.intv_st, (p > ncs.intv_st ? static_cast<size_t>(p - ncs.intv_st) : 0)
#define MARKED_INTV2() ncs.intv2_st, (p > ncs.intv2_st ? static_cast<size_t>(p - ncs.intv2_st) : 0)
#define MARKED_JOBID() ncs.jobid_st, (p > ncs.jobid_st ? static_cast<size_t>(p - ncs.jobid_st) : 0)

%%{
    machine ncrontab;
    access ncs.;

    spc = [ \t];
    eqsep = spc* '=' spc*;

    action TUnitSt { ncs.time_st = p; ncs.v_time = 0; }
    action TSecEn  {
        unsigned t;
        if (!nk::from_string<unsigned>(MARKED_TIME(), &t)) {
            ncs.parse_error = true;
            fbreak;
        }
        ncs.v_time += t;
    }
    action TMinEn  {
        unsigned t;
        if (!nk::from_string<unsigned>(MARKED_TIME(), &t)) {
            ncs.parse_error = true;
            fbreak;
        }
        ncs.v_time += 60 * t;
    }
    action THrEn   {
        unsigned t;
        if (!nk::from_string<unsigned>(MARKED_TIME(), &t)) {
            ncs.parse_error = true;
            fbreak;
        }
        ncs.v_time += 3600 * t;
    }
    action TDayEn  {
        unsigned t;
        if (!nk::from_string<unsigned>(MARKED_TIME(), &t)) {
            ncs.parse_error = true;
            fbreak;
        }
        ncs.v_time += 86400 * t;
    }
    action TWeekEn {
        unsigned t;
        if (!nk::from_string<unsigned>(MARKED_TIME(), &t)) {
            ncs.parse_error = true;
            fbreak;
        }
        ncs.v_time += 604800 * t;
    }

    action IntValSt {
        ncs.intv_st = p;
        ncs.v_int1 = ncs.v_int2 = 0;
        ncs.intv2_exist = false;
    }
    action IntValEn {
        if (!nk::from_string<int>(MARKED_INTV1(), &ncs.v_int1)) {
            ncs.parse_error = true;
            fbreak;
        }
    }
    action IntVal2St { ncs.intv2_st = p; }
    action IntVal2En {
        if (!nk::from_string<int>(MARKED_INTV2(), &ncs.v_int2)) {
            ncs.parse_error = true;
            fbreak;
        }
        ncs.intv2_exist = true;
    }
    action IntValSwap {
        using std::swap;
        swap(ncs.v_int1, ncs.v_int3);
        swap(ncs.v_int2, ncs.v_int4);
    }
    action IntVal34Clear {
        ncs.v_int3 = -1;
        ncs.v_int4 = -1;
    }

    action StrValSt { ncs.strv_st = p; ncs.v_strlen = 0; }
    action StrValEn {
        ncs.v_strlen = p > ncs.strv_st ? static_cast<size_t>(p - ncs.strv_st) : 0;
        if (ncs.v_strlen >= sizeof ncs.v_str) {
            log_line("error parsing line %zu in crontab: too long", ncs.linenum);
            exit(EXIT_FAILURE);
        }
        memcpy(ncs.v_str, ncs.strv_st, ncs.v_strlen);
        ncs.v_str[ncs.v_strlen] = 0;
    }

    t_sec  = (digit+ > TUnitSt) 's' % TSecEn;
    t_min  = (digit+ > TUnitSt) 'm' % TMinEn;
    t_hr   = (digit+ > TUnitSt) 'h' % THrEn;
    t_day  = (digit+ > TUnitSt) 'd' % TDayEn;
    t_week = (digit+ > TUnitSt) 'w' % TWeekEn;
    t_any = (t_sec | t_min | t_hr | t_day | t_week);

    intval = (digit+ > IntValSt % IntValEn);
    timeval = t_any (spc* t_any)*;
    intrangeval = (digit+ > IntValSt % IntValEn)
                  (',' (digit+ > IntVal2St % IntVal2En))?;
    stringval = ([^\0\n]+ > StrValSt % StrValEn);

    action JournalEn { ncs.ce->journal_ = true; }
    journal = 'journal'i % JournalEn;

    action RunAtEn {
        ncs.runat = true;
        ncs.ce->exectime_ = ncs.v_int1;
        ncs.ce->maxruns_ = 1;
        ncs.ce->journal_ = true;
    }
    action MaxRunsEn {
        if (!ncs.runat)
            ncs.ce->maxruns_ = ncs.v_int1 > 0 ? static_cast<unsigned>(ncs.v_int1) : 0;
    }

    runat = 'runat'i eqsep intval % RunAtEn;
    maxruns = 'maxruns'i eqsep intval % MaxRunsEn;

    action IntervalEn { ncs.ce->interval_ = ncs.v_time; }

    interval = 'interval'i eqsep timeval % IntervalEn;

    xhour = digit | ('1' digit) | '20' | '21' | '22' | '23';
    xminute = ('1' | '2' | '3' | '4' | '5') digit;
    hhmm = xhour > IntValSt % IntValEn ':' xminute > IntVal2St % IntVal2En;
    hhmm_range = hhmm > IntVal34Clear (spc* '-' spc* hhmm)? > IntValSwap % IntValSwap;

    action MonthEn { add_cst_mon(ncs); }
    action DayEn { add_cst_mday(ncs); }
    action WeekdayEn { add_cst_wday(ncs); }
    action TimeEn { add_cst_time(ncs); }

    month = 'month'i eqsep intrangeval % MonthEn;
    day = 'day'i eqsep intrangeval % DayEn;
    weekday = 'weekday'i eqsep intrangeval % WeekdayEn;
    time = 'time'i eqsep hhmm_range % TimeEn;

    action CommandEn { parse_command_key(ncs); }

    command = 'command'i eqsep stringval % CommandEn;

    cmds = command | time | weekday | day |
           month | interval | maxruns | runat | journal;

    action JobIdSt { ncs.jobid_st = p; }
    action JobIdEn {
        if (!nk::from_string<int>(MARKED_JOBID(), &ncs.ce->id_)) {
            ncs.parse_error = true;
            fbreak;
        }
    }
    action CreateCe { ncs.finish_ce(); }

    jobid = ('!' > CreateCe) (digit+ > JobIdSt) % JobIdEn;
    comment = (';'|'#') any*;

    main := jobid | cmds | comment;
}%%

%% write data;

static int do_parse_config(ParseCfgState &ncs, const char *p, size_t plen)
{
    const char *pe = p + plen;
    const char *eof = pe;

    %% write init;
    %% write exec;

    if (ncs.parse_error) return -1;
    if (ncs.cs == ncrontab_error)
        return -1;
    if (ncs.cs >= ncrontab_first_final)
        return 1;
    return 0;
}

void parse_config(char const *path, char const *execfile,
                  std::vector<size_t> *stk,
                  std::vector<size_t> *deadstk)
{
    g_jobs.clear();
    ParseCfgState ncs(stk, deadstk);
    parse_history(execfile);

    char buf[MAX_LINE];
    auto f = fopen(path, "r");
    if (!f) {
        log_line("%s: failed to open file: '%s': %s", __func__, path, strerror(errno));
        exit(EXIT_FAILURE);
    }
    defer [&f]{ fclose(f); };
    while (!feof(f)) {
        if (!fgets(buf, sizeof buf, f)) {
            if (!feof(f))
                log_line("%s: io error fetching line of '%s'", __func__, path);
            break;
        }
        auto llen = strlen(buf);
        if (llen == 0)
            continue;
        if (buf[llen-1] == '\n')
            buf[--llen] = 0;
        ++ncs.linenum;
        if (do_parse_config(ncs, buf, llen) < 0) {
            log_line("%s: do_parse_config(%s) failed at line %zu", __func__, path, ncs.linenum);
            exit(EXIT_FAILURE);
        }
    }
    ncs.finish_ce();
    std::sort(stk->begin(), stk->end(), LtCronEntry);
    history_lut.clear();
}

