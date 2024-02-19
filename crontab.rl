// Copyright 2003-2024 Nicholas J. Kain <njkain at gmail dot com>
// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <memory>
extern "C" {
#include "nk/log.h"
#include "xmalloc.h"
#include "strconv.h"
}
#include "ncron.hpp"
#include "sched.hpp"

#define MAX_LINE 2048

#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif

extern int gflags_debug;
extern size_t g_njobs;
extern Job *g_jobs;

struct item_history {
    time_t exectime = 0;
    time_t lasttime = 0;
    unsigned int numruns = 0;
};

struct ParseCfgState
{
    ParseCfgState(Job **stk, Job **dstk)
    : stackl(stk), deadstackl(dstk)
    {
        memset(v_str, 0, sizeof v_str);
    }
    char v_str[MAX_LINE];

    Job **stackl = nullptr;
    Job **deadstackl = nullptr;

    Job *ce = nullptr;

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

    bool seen_cst_hhmm = false;
    bool seen_cst_wday = false;
    bool seen_cst_mday = false;
    bool seen_cst_mon = false;

    bool seen_job = false;

    void create_ce()
    {
        if (ce == g_jobs + g_njobs) {
            log_line("job count mismatch");
            exit(EXIT_FAILURE);
        }
        new(ce) Job;
        seen_job = true;
        have_command = false;
        seen_cst_hhmm = false;
        seen_cst_wday = false;
        seen_cst_mday = false;
        seen_cst_mon = false;
    }

    inline void debug_print_ce() const
    {
        if (!gflags_debug) return;
        log_line("id=%d:\tcommand: %s", ce->id_, ce->command_ ? ce->command_ : "");
        log_line("\targs: %s", ce->args_ ? ce->args_ : "");
        log_line("\tnumruns: %u\n\tmaxruns: %u", ce->numruns_, ce->maxruns_);
        log_line("\tjournal: %s", ce->journal_ ? "true" : "false");
        log_line("\trunat: %s", ce->runat_ ? "true" : "false");
        log_line("\tinterval: %u\n\texectime: %lu\n\tlasttime: %lu", ce->interval_, ce->exectime_, ce->lasttime_);
    }

    void finish_ce()
    {
        if (!seen_job) return;

        debug_print_ce();

        if (ce->id_ < 0
            || (ce->interval_ <= 0 && ce->exectime_ <= 0)
            || !ce->command_ || !have_command) {
            log_line("ERROR IN CRONTAB: invalid id, command, or interval for job %d", ce->id_);
            exit(EXIT_FAILURE);
        }

        // XXX: O(n^2) might be nice to avoid.
        for (auto i = g_jobs, iend = ce; i != iend; ++i) {
            if (i->id_ == ce->id_) {
                log_line("ERROR IN CRONTAB: duplicate entry for job %d", ce->id_);
                exit(EXIT_FAILURE);
            }
        }

        // Preserve this job and work on the next one.
        ++ce;
    }
};

struct hstm {
    const char *st = nullptr;
    int cs = 0;
    int id = -1;
    item_history h;
    bool parse_error = false;

    void print() const
    {
        if (!gflags_debug) return;
        log_line("id=%d:\tnumruns = %u\n\texectime = %lu\n\tlasttime = %lu",
                 id, h.numruns, h.exectime, h.lasttime);
    }
};

%%{
    machine history_m;
    access hst.;

    action St { hst.st = p; }
    action LastTimeEn {
        if (!strconv_to_i64(hst.st, p, &hst.h.lasttime)) {
            hst.parse_error = true;
            fbreak;
        }
    }
    action NumRunsEn {
        if (!strconv_to_u32(hst.st, p, &hst.h.numruns)) {
            hst.parse_error = true;
            fbreak;
        }
    }
    action ExecTimeEn {
        if (!strconv_to_i64(hst.st, p, &hst.h.exectime)) {
            hst.parse_error = true;
            fbreak;
        }
    }
    action IdEn {
        if (!strconv_to_i32(hst.st, p, &hst.id)) {
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
        log_line("Failed to open history file '%s' for read: %s", path, strerror(errno));
        return;
    }
    size_t linenum = 0;
    while (!feof(f)) {
        if (!fgets(buf, sizeof buf, f)) {
            if (!feof(f)) log_line("IO error reading history file '%s'", path);
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
            log_line("%s history entry at line %zu; ignoring",
                     r == -2 ? "Incomplete" : "Malformed", linenum);
            continue;
        }

        for (auto j = g_jobs, jend = g_jobs + g_njobs; j != jend; ++j) {
            if (j->id_ == hst.id) {
                hst.print();
                j->numruns_ = hst.h.numruns;
                j->lasttime_ = hst.h.lasttime;
                if (!j->runat_) {
                    j->exectime_ = hst.h.exectime;
                    j->set_initial_exectime();
                } else {
                    if (j->interval_ > 0) {
                        log_line("ERROR IN CRONTAB: interval is unused when runat is set: job %d", j->id_);
                        exit(EXIT_FAILURE);
                    }
                }
            }
        }
    }
    fclose(f);
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

%%{
    machine parse_cmd_key_m;
    access pckm.;

    action St { pckm.st = p; }
    action CmdEn {
        size_t l = p > pckm.st ? static_cast<size_t>(p - pckm.st) : 0;
        if (l) {
            auto ts = static_cast<char *>(xmalloc(l + 1));
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
            auto ts = static_cast<char *>(xmalloc(l + 1));
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

static void parse_time_unit(const ParseCfgState &ncs, const char *p, unsigned unit, unsigned *dest)
{
    unsigned t;
    if (!strconv_to_u32(ncs.time_st, p - 1, &t)) {
        log_line("Invalid time unit at line %zu", ncs.linenum);
        exit(EXIT_FAILURE);
    }
    *dest += unit * t;
}

static void parse_int_value(const char *p, const char *start, size_t linenum, int *dest)
{
    if (!strconv_to_i32(start, p, dest)) {
        log_line("Invalid integer value at line %zu", linenum);
        exit(EXIT_FAILURE);
    }
}

%%{
    machine ncrontab;
    access ncs.;

    spc = [ \t];
    eqsep = spc* '=' spc*;

    action TUnitSt { ncs.time_st = p; ncs.v_time = 0; }
    action TSecEn  { parse_time_unit(ncs, p, 1, &ncs.v_time); }
    action TMinEn  { parse_time_unit(ncs, p, 60, &ncs.v_time); }
    action THrEn   { parse_time_unit(ncs, p, 3600, &ncs.v_time); }
    action TDayEn  { parse_time_unit(ncs, p, 86400, &ncs.v_time); }
    action TWeekEn { parse_time_unit(ncs, p, 604800, &ncs.v_time); }

    action IntValSt {
        ncs.intv_st = p;
        ncs.v_int1 = ncs.v_int2 = 0;
        ncs.intv2_exist = false;
    }
    action IntValEn { parse_int_value(p, ncs.intv_st, ncs.linenum, &ncs.v_int1); }
    action IntVal2St { ncs.intv2_st = p; }
    action IntVal2En { parse_int_value(p, ncs.intv2_st, ncs.linenum, &ncs.v_int2); ncs.intv2_exist = true; }
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
                  ('-' (digit+ > IntVal2St % IntVal2En))?;
    stringval = ([^\0\n]+ > StrValSt % StrValEn);

    action JournalEn { ncs.ce->journal_ = true; }
    journal = 'journal'i % JournalEn;

    action RunAtEn {
        ncs.ce->runat_ = true;
        ncs.ce->exectime_ = ncs.v_int1;
        ncs.ce->maxruns_ = 1;
        ncs.ce->journal_ = true;
    }
    action MaxRunsEn {
        if (!ncs.ce->runat_)
            ncs.ce->maxruns_ = ncs.v_int1 > 0 ? static_cast<unsigned>(ncs.v_int1) : 0;
    }

    runat = 'runat'i eqsep intval % RunAtEn;
    maxruns = 'maxruns'i eqsep intval % MaxRunsEn;

    action IntervalEn { ncs.ce->interval_ = ncs.v_time; }

    interval = 'interval'i eqsep timeval % IntervalEn;

    xhour = digit | ('0' digit) | ('1' digit) | '20' | '21' | '22' | '23';
    xminute = ('0' | '1' | '2' | '3' | '4' | '5') digit;
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
    action JobIdEn { parse_int_value(p, ncs.jobid_st, ncs.linenum, &ncs.ce->id_); }
    action CreateCe { ncs.finish_ce(); ncs.create_ce(); }

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

    if (ncs.cs == ncrontab_error)
        return -1;
    if (ncs.cs >= ncrontab_first_final)
        return 1;
    return 0;
}

// Seeks back to start of file when done.
size_t count_config_jobs(FILE *f)
{
    size_t r = 0;
    int lc = '\n', llc = 0;
    while (!feof(f)) {
        int c = fgetc(f);
        if (!c) {
            if (!feof(f))
                log_line("IO error reading config file");
            break;
        }
        if ((c >= '0' && c <= '9') && lc == '!' && llc == '\n') ++r;
        llc = lc;
        lc = c;
    }
    rewind(f);
    return r;
}

void parse_config(char const *path, char const *execfile,
                  Job **stk, Job **deadstk)
{
    ParseCfgState ncs(stk, deadstk);

    char buf[MAX_LINE];
    auto f = fopen(path, "r");
    if (!f) {
        log_line("Failed to open config file '%s': %s", path, strerror(errno));
        exit(EXIT_FAILURE);
    }
    g_njobs = count_config_jobs(f);
    if (!g_njobs) {
        log_line("No jobs found in config file.  Exiting.");
        exit(EXIT_SUCCESS);
    }
    g_jobs = static_cast<Job *>(xmalloc(g_njobs * sizeof(Job)));
    ncs.ce = g_jobs;
    while (!feof(f)) {
        if (!fgets(buf, sizeof buf, f)) {
            if (!feof(f))
                log_line("IO error reading config file '%s'", path);
            break;
        }
        auto llen = strlen(buf);
        if (llen == 0)
            continue;
        if (buf[llen-1] == '\n')
            buf[--llen] = 0;
        ++ncs.linenum;
        if (do_parse_config(ncs, buf, llen) < 0) {
            log_line("Config file '%s' is malformed at line %zu", path, ncs.linenum);
            exit(EXIT_FAILURE);
        }
    }
    ncs.finish_ce();
    parse_history(execfile);

    for (auto j = g_jobs, jend = g_jobs + g_njobs; j != jend; ++j) {
        bool alive = !j->runat_?
                     ((j->maxruns_ == 0 || j->numruns_ < j->maxruns_) && j->exectime_ != 0)
                   : (j->numruns_ == 0);
        job_insert(alive ? stk : deadstk, j);
    }
    fclose(f);
}
