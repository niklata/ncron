/* crontab.rl - configure file parser for ncron
 *
 * Copyright 2003-2016 Nicholas J. Kain <njkain at gmail dot com>
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

#include <algorithm>
#include <utility>
#include <unordered_map>
#include <optional>
#include <cstdio>
#include <fmt/format.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <assert.h>
#include <pwd.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <limits.h>
#include <nk/string_replace_all.hpp>
#include <nk/from_string.hpp>
#include <nk/scopeguard.hpp>
extern "C" {
#include "nk/privilege.h"
}
#include "ncron.hpp"
#include "sched.hpp"
#include "crontab.hpp"

/* BSD uses OFILE rather than NOFILE... */
#ifndef RLIMIT_NOFILE
#  define RLIMIT_NOFILE RLIMIT_OFILE
#endif

#define MAX_LINE 2048

static int cfg_reload;    /* 0 on first call, 1 on subsequent calls */
extern int gflags_debug;

static void get_history(std::unique_ptr<cronentry_t> &item);

struct ParseCfgState
{
    ParseCfgState(const std::string &ef, std::vector<StackItem> &stk,
                  std::vector<StackItem> &dstk) :
        stack(stk), deadstack(dstk), ce(nullptr), execfile(ef),
        jobid_st(nullptr), time_st(nullptr), intv_st(nullptr),
        intv2_st(nullptr), strv_st(nullptr), v_strlen(0), linenum(0), v_int(0),
        v_int2(0), cs(0), cmdret(0), intv2_exist(false), runat(false),
        parse_error(false)
    {
        memset(v_str, 0, sizeof v_str);
    }
    char v_str[1024];

    std::vector<StackItem> &stack;
    std::vector<StackItem> &deadstack;
    std::unique_ptr<cronentry_t> ce;

    const std::string execfile;

    const char *jobid_st;
    const char *time_st;
    const char *intv_st;
    const char *intv2_st;
    const char *strv_st;

    size_t v_strlen;
    size_t linenum;

    unsigned int v_time;

    int v_int;
    int v_int2;

    int cs;
    int cmdret;

    bool intv2_exist;
    bool runat;

    bool parse_error;

    void create_ce()
    {
        assert(!ce);
        ce = std::make_unique<cronentry_t>();
        cmdret = 0;
        runat = false;
    }

    inline void debug_print_ce() const
    {
        if (!gflags_debug)
            return;
        fmt::print(stderr, "-=- finish_ce -=-\n");
        fmt::print(stderr, "id: {}\n", ce->id);
        fmt::print(stderr, "command: {}\n", ce->command);
        fmt::print(stderr, "args: {}\n", ce->args);
        fmt::print(stderr, "chroot: {}\n", ce->chroot);
        fmt::print(stderr, "path: {}\n", ce->path);
        fmt::print(stderr, "numruns: {}\n", ce->numruns);
        fmt::print(stderr, "maxruns: {}\n", ce->maxruns);
        fmt::print(stderr, "journal: {}\n", ce->journal);
        fmt::print(stderr, "user: {}\n", ce->user);
        fmt::print(stderr, "group: {}\n", ce->group);
        for (const auto &i: ce->month)
            fmt::print(stderr, "month: [{},{}]\n", i.first, i.second);
        for (const auto &i: ce->day)
            fmt::print(stderr, "day: [{},{}]\n", i.first, i.second);
        for (const auto &i: ce->weekday)
            fmt::print(stderr, "weekday: [{},{}]\n", i.first, i.second);
        for (const auto &i: ce->hour)
            fmt::print(stderr, "hour: [{},{}]\n", i.first, i.second);
        for (const auto &i: ce->minute)
            fmt::print(stderr, "minute: [{},{}]\n", i.first, i.second);
        fmt::print(stderr, "interval: {}\n", ce->interval);
        fmt::print(stderr, "exectime: {}\n", ce->exectime);
        fmt::print(stderr, "lasttime: {}\n", ce->lasttime);
    }

    inline void debug_print_ce_history() const
    {
        if (!gflags_debug)
            return;
        fmt::print(stderr, "[{}]->numruns = {}\n", ce->id, ce->numruns);
        fmt::print(stderr, "[{}]->exectime = {}\n", ce->id, ce->exectime);
        fmt::print(stderr, "[{}]->lasttime = {}\n", ce->id, ce->lasttime);
    }

    void finish_ce()
    {
        if (!ce)
            return;
        debug_print_ce();

        if (ce->id <= 0
            || (ce->interval <= 0 && ce->exectime <= 0)
            || ce->command.empty() || cmdret < 1) {
            if (gflags_debug)
                fmt::print(stderr, "===> IGNORE\n");
            ce.reset();
            return;
        }
        if (gflags_debug)
            fmt::print(stderr, "===> ADD\n");

        /* we have a job to insert */
        if (runat) { /* runat task */
            auto forced_exectime = ce->exectime;
            get_history(ce);
            ce->exectime = forced_exectime;
            debug_print_ce_history();

            /* insert iif we haven't exceeded maxruns */
            if (!ce->numruns)
                stack.emplace_back(std::move(ce));
            else
                deadstack.emplace_back(std::move(ce));
        } else { /* interval task */
            get_history(ce);
            debug_print_ce_history();
            set_initial_exectime(*ce);

            /* insert iif numruns < maxruns and no constr error */
            if ((ce->maxruns == 0 || ce->numruns < ce->maxruns)
                && ce->exectime != 0)
                stack.emplace_back(std::move(ce));
            else
                deadstack.emplace_back(std::move(ce));
        }
        ce.reset();
    }

    void setgroupv()
    {
        if (nk_gidbyname(v_str, &ce->group)) {
            fmt::print(stderr, "{}: nonexistent group specified at line {}\n",
                       __func__, linenum);
            std::exit(EXIT_FAILURE);
        }
    }

    void setuserv()
    {
        if (nk_uidgidbyname(v_str, &ce->user, &ce->group)) {
            fmt::print(stderr, "{}: nonexistent user specified at line {}\n",
                       __func__, linenum);
            std::exit(EXIT_FAILURE);
        }
    }

    void setlim(int type)
    {
        struct rlimit rli;
        rli.rlim_cur = v_int == 0 ? RLIM_INFINITY : v_int;
        rli.rlim_max = v_int2 == 0 ? RLIM_INFINITY : v_int2;

        try {
            ce->limits.add(type, rli);
        } catch (const std::logic_error&) {
            fmt::print(stderr, "{}: Bad RLIMIT_type specified.\n", __func__);
            std::exit(EXIT_FAILURE);
        }
    }

};

struct item_history {
    item_history() {}
    void set_exectime(time_t v) { exectime_ = v; }
    void set_lasttime(time_t v) { lasttime_ = v; }
    void set_numruns(unsigned int v) { numruns_ = v; }
    auto exectime() const { return exectime_; }
    auto lasttime() const { return lasttime_; }
    auto numruns() const { return numruns_; }
private:
    std::optional<time_t> exectime_;
    std::optional<time_t> lasttime_;
    std::optional<unsigned int> numruns_;
};

struct hstm {
    hstm() : st(nullptr), cs(0), id(0), parse_error(false) {}
    const char *st;
    int cs;
    unsigned int id;
    item_history h;
    bool parse_error;
};

%%{
    machine history_m;
    access hst.;

    action St { hst.st = p; }
    action LastTimeEn {
        if (auto t = nk::from_string<time_t>(hst.st, p - hst.st)) hst.h.set_lasttime(*t); else {
            hst.parse_error = true;
            fbreak;
        }
    }
    action NumRunsEn {
        if (auto t = nk::from_string<unsigned>(hst.st, p - hst.st)) hst.h.set_numruns(*t); else {
            hst.parse_error = true;
            fbreak;
        }
    }
    action ExecTimeEn {
        if (auto t = nk::from_string<time_t>(hst.st, p - hst.st)) hst.h.set_exectime(*t); else {
            hst.parse_error = true;
            fbreak;
        }
    }
    action IdEn {
        if (auto t = nk::from_string<unsigned>(hst.st, p - hst.st)) hst.id = *t; else {
            hst.parse_error = true;
            fbreak;
        }
    }

    lasttime = '|' digit+ > St % LastTimeEn;
    numruns = ':' digit+ > St % NumRunsEn;
    exectime = '=' digit+ > St % ExecTimeEn;
    id = digit+ > St % IdEn;
    main := id (numruns | exectime | lasttime)+;
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

static std::unordered_map<unsigned int, item_history> history_map;

static void parse_history(const std::string &path)
{
    char buf[MAX_LINE];
    auto f = fopen(path.c_str(), "r");
    if (!f) {
        fmt::print(stderr, "{}: failed to open history file \"{}\" for read: {}\n",
                   __func__, path, strerror(errno));
        return;
    }
    SCOPE_EXIT{ fclose(f); };
    size_t linenum = 0;
    while (!feof(f)) {
        if (!fgets(buf, sizeof buf, f)) {
            if (!feof(f))
                fmt::print(stderr, "{}: io error fetching line of '{}'\n", __func__, path);
            break;
        }
        auto llen = strlen(buf);
        if (llen == 0)
            continue;
        if (buf[llen-1] == '\n')
            buf[--llen] = 0;
        ++linenum;
        hstm h;
        const auto r = do_parse_history(h, buf, llen);
        if (r < 0) {
            if (r == -2)
                fmt::print(stderr, "{}: Incomplete configuration at line {}; ignoring\n",
                           __func__, linenum);
            else
                fmt::print(stderr, "{}: Malformed configuration at line {}; ignoring.\n",
                           __func__, linenum);
            continue;
        }
        history_map.emplace(std::make_pair(h.id, h.h));
    }
}

static void get_history(std::unique_ptr<cronentry_t> &item)
{
    assert(item);

    auto i = history_map.find(item->id);
    if (i == history_map.end())
        return;
    if (const auto exectm = i->second.exectime())
        item->exectime = *exectm > 0 ? *exectm : 0;
    if (const auto lasttm = i->second.lasttime())
        item->lasttime = *lasttm > 0 ? *lasttm : 0;
    if (const auto t = i->second.numruns())
        item->numruns = *t;
}

static void addcstlist(ParseCfgState &ncs, cronentry_t::cst_list &list,
                       int wildcard, int min, int max)
{
    int low = ncs.v_int;
    int high = wildcard;
    if (ncs.intv2_exist)
        high = ncs.v_int2;

    if (low > max || low < min)
        low = wildcard;
    if (high > max || high < min)
        high = wildcard;

    /* we don't allow meaningless 'rules' */
    if (low == wildcard && high == wildcard)
        return;

    if (low > high) {
        /* discontinuous range, split into two continuous rules... */
        list.emplace_back(std::make_pair(low, max));
        list.emplace_back(std::make_pair(min, high));
    } else {
        /* handle continuous ranges normally */
        list.emplace_back(std::make_pair(low, high));
    }
}

struct pckm {
    pckm() : st(nullptr), cs(0) {}
    char *st;
    int cs;
};

%%{
    machine parse_cmd_key_m;
    access pckm.;

    action St { pckm.st = p; }
    action CmdEn {
        ncs.ce->command = std::string(pckm.st, p - pckm.st);
        string_replace_all(ncs.ce->command, "\\ ", 2, " ");
        string_replace_all(ncs.ce->command, "\\\\", 2, "\\");
    }
    action ArgEn { ncs.ce->args = std::string(pckm.st, p - pckm.st); }

    sptab = [ \t];
    cmdstr = ([^\0 \t] | '\\\\' | '\\ ')+;
    cmd = sptab* (cmdstr > St % CmdEn);
    args = sptab+ ([^\0])* > St % ArgEn;
    main := cmd args?;
}%%

%% write data;

// cmdret = 0: Not parsed a command key yet.
// cmdret = 1: Success.  Got a command key.
// cmdret = -1: Error: malformed command key.
// cmdret = -2: Error: incomplete command key.
// cmdret = -3: Error: duplicate command key.
static void parse_command_key(ParseCfgState &ncs)
{
    char *p = ncs.v_str;
    const char *pe = ncs.v_str + ncs.v_strlen;
    const char *eof = pe;

    struct pckm pckm;

    if (ncs.cmdret != 0) {
        ncs.cmdret = -3;
        fmt::print(stderr, "Duplicate 'command' value at line {}\n",
                   ncs.linenum);
        std::exit(EXIT_FAILURE);
    }

    %% write init;
    %% write exec;

    if (pckm.cs == parse_cmd_key_m_error) {
        ncs.cmdret = -1;
        fmt::print(stderr, "Malformed 'command' value at line {}\n",
                   ncs.linenum);
        std::exit(EXIT_FAILURE);
    } else if (pckm.cs >= parse_cmd_key_m_first_final)
        ncs.cmdret = 1;
    else {
        ncs.cmdret = -2;
        fmt::print(stderr, "Incomplete 'command' value at line {}\n",
                   ncs.linenum);
        std::exit(EXIT_FAILURE);
    }
}

%%{
    machine ncrontab;
    access ncs.;

    spc = [ \t];
    eqsep = spc* '=' spc*;

    action TUnitSt { ncs.time_st = p; ncs.v_time = 0; }
    action TSecEn  {
        if (auto t = nk::from_string<unsigned>(ncs.time_st, p - ncs.time_st - 1)) ncs.v_time += *t; else {
            ncs.parse_error = true;
            fbreak;
        }
    }
    action TMinEn  {
        if (auto t = nk::from_string<unsigned>(ncs.time_st, p - ncs.time_st - 1)) ncs.v_time += 60 * *t; else {
            ncs.parse_error = true;
            fbreak;
        }
    }
    action THrEn   {
        if (auto t = nk::from_string<unsigned>(ncs.time_st, p - ncs.time_st - 1)) ncs.v_time += 3600 * *t; else {
            ncs.parse_error = true;
            fbreak;
        }
    }
    action TDayEn  {
        if (auto t = nk::from_string<unsigned>(ncs.time_st, p - ncs.time_st - 1)) ncs.v_time += 86400 * *t; else {
            ncs.parse_error = true;
            fbreak;
        }
    }
    action TWeekEn {
        if (auto t = nk::from_string<unsigned>(ncs.time_st, p - ncs.time_st - 1)) ncs.v_time += 604800 * *t; else {
            ncs.parse_error = true;
            fbreak;
        }
    }

    action IntValSt {
        ncs.intv_st = p;
        ncs.v_int = ncs.v_int2 = 0;
        ncs.intv2_exist = false;
    }
    action IntValEn {
        if (auto t = nk::from_string<int>(ncs.intv_st, p - ncs.intv_st)) ncs.v_int = *t; else {
            ncs.parse_error = true;
            fbreak;
        }
    }
    action IntVal2St { ncs.intv2_st = p; }
    action IntVal2En {
        if (auto t = nk::from_string<int>(ncs.intv2_st, p - ncs.intv2_st)) ncs.v_int2 = *t; else {
            ncs.parse_error = true;
            fbreak;
        }
        ncs.intv2_exist = true;
    }

    action StrValSt { ncs.strv_st = p; ncs.v_strlen = 0; }
    action StrValEn {
        ncs.v_strlen = p - ncs.strv_st;
        if (ncs.v_strlen <= INT_MAX) {
            ssize_t snl = snprintf(ncs.v_str, sizeof ncs.v_str,
                                   "%.*s", (int)ncs.v_strlen, ncs.strv_st);
            if (snl < 0 || (size_t)snl >= sizeof ncs.v_str) {
                fmt::print(stderr, "error parsing line {} in crontab; too long?\n",
                           ncs.linenum);
                std::exit(EXIT_FAILURE);
            }
        }
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

    action JournalEn { ncs.ce->journal = true; }
    journal = 'journal'i % JournalEn;

    action RunAtEn {
        ncs.runat = true;
        ncs.ce->exectime = ncs.v_int;
        ncs.ce->maxruns = 1;
        ncs.ce->journal = true;
    }
    action MaxRunsEn {
        if (!ncs.runat)
            ncs.ce->maxruns = ncs.v_int;
    }

    runat = 'runat'i eqsep intval % RunAtEn;
    maxruns = 'maxruns'i eqsep intval % MaxRunsEn;

    action LimAsEn { ncs.setlim(RLIMIT_AS); }
    action LimMemlockEn { ncs.setlim(RLIMIT_MEMLOCK); }
    action LimNofileEn { ncs.setlim(RLIMIT_NOFILE); }
    action LimNprocEn { ncs.setlim(RLIMIT_NPROC); }
    action LimRssEn { ncs.setlim(RLIMIT_RSS); }
    action LimCoreEn { ncs.setlim(RLIMIT_CORE); }
    action LimStackEn { ncs.setlim(RLIMIT_STACK); }
    action LimDataEn { ncs.setlim(RLIMIT_DATA); }
    action LimFsizeEn { ncs.setlim(RLIMIT_FSIZE); }
    action LimCpuEn { ncs.setlim(RLIMIT_CPU); }
    action LimMsgQueueEn { ncs.setlim(RLIMIT_MSGQUEUE); }
    action LimNiceEn { ncs.setlim(RLIMIT_NICE); }
    action LimRtTimeEn { ncs.setlim(RLIMIT_RTTIME); }
    action LimRtPrioEn { ncs.setlim(RLIMIT_RTPRIO); }
    action LimSigPendingEn { ncs.setlim(RLIMIT_SIGPENDING); }

    lim_as = 'l_as'i eqsep intrangeval % LimAsEn;
    lim_memlock = 'l_memlock'i eqsep intrangeval % LimMemlockEn;
    lim_nofile = 'l_nofile'i eqsep intrangeval % LimNofileEn;
    lim_nproc = 'l_nproc'i eqsep intrangeval % LimNprocEn;
    lim_rss = 'l_rss'i eqsep intrangeval % LimRssEn;
    lim_core = 'l_core'i eqsep intrangeval % LimCoreEn;
    lim_stack = 'l_stack'i eqsep intrangeval % LimStackEn;
    lim_data = 'l_data'i eqsep intrangeval % LimDataEn;
    lim_fsize = 'l_fsize'i eqsep intrangeval % LimFsizeEn;
    lim_cpu = 'l_cpu'i eqsep intrangeval % LimCpuEn;
    lim_msgqueue = 'l_msgqueue'i eqsep intrangeval % LimMsgQueueEn;
    lim_nice = 'l_nice'i eqsep intrangeval % LimNiceEn;
    lim_rttime = 'l_rttime'i eqsep intrangeval % LimRtTimeEn;
    lim_rtprio = 'l_rtprio'i eqsep intrangeval % LimRtPrioEn;
    lim_sigpending = 'l_sigpending'i eqsep intrangeval % LimSigPendingEn;

    action IntervalEn { ncs.ce->interval = ncs.v_time; }

    interval = 'interval'i eqsep timeval % IntervalEn;

    action MonthEn { addcstlist(ncs, ncs.ce->month, 0, 1, 12); }
    action DayEn { addcstlist(ncs, ncs.ce->day, 0, 1, 31); }
    action WeekdayEn { addcstlist(ncs, ncs.ce->weekday, 0, 1, 7); }
    action HourEn { addcstlist(ncs, ncs.ce->hour, 24, 0, 23); }
    action MinuteEn { addcstlist(ncs, ncs.ce->minute, 60, 0, 59); }

    month = 'month'i eqsep intrangeval % MonthEn;
    day = 'day'i eqsep intrangeval % DayEn;
    weekday = 'weekday'i eqsep intrangeval % WeekdayEn;
    hour = 'hour'i eqsep intrangeval % HourEn;
    minute = 'minute'i eqsep intrangeval % MinuteEn;

    action GroupEn { ncs.setgroupv(); }
    action UserEn { ncs.setuserv(); }
    action ChrootEn {
        ncs.ce->chroot = std::string(ncs.v_str, ncs.v_strlen);
    }
    action CommandEn { parse_command_key(ncs); }
    action PathEn {
        ncs.ce->path = std::string(ncs.v_str, ncs.v_strlen);
    }

    group = 'group'i eqsep stringval % GroupEn;
    user = 'user'i eqsep stringval % UserEn;
    chroot = 'chroot'i eqsep stringval % ChrootEn;
    command = 'command'i eqsep stringval % CommandEn;
    path = 'path'i eqsep stringval % PathEn;

    cmds = command | chroot | path | user | group | minute | hour | weekday | day |
           month | interval | lim_cpu | lim_fsize | lim_data | lim_stack |
           lim_core | lim_rss | lim_nproc | lim_nofile | lim_memlock | lim_as |
           lim_msgqueue | lim_nice | lim_rttime | lim_rtprio | lim_sigpending |
           maxruns | runat | journal;

    action JobIdSt { ncs.jobid_st = p; }
    action JobIdEn {
        if (auto t = nk::from_string<unsigned>(ncs.jobid_st, p - ncs.jobid_st)) ncs.ce->id = *t; else {
            ncs.parse_error = true;
            fbreak;
        }
    }
    action CreateCe { ncs.finish_ce(); ncs.create_ce(); }

    jobid = ('!' > CreateCe) (digit+ > JobIdSt) % JobIdEn;

    main := jobid | cmds;
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

void parse_config(const std::string &path, const std::string &execfile,
                  std::vector<StackItem> &stk,
                  std::vector<StackItem> &deadstk)
{
    struct ParseCfgState ncs(execfile, stk, deadstk);
    parse_history(ncs.execfile);

    char buf[MAX_LINE];
    auto f = fopen(path.c_str(), "r");
    if (!f) {
        fmt::print(stderr, "{}: failed to open file: '{}': {}\n",
                   __func__, path, strerror(errno));
        std::exit(EXIT_FAILURE);
    }
    SCOPE_EXIT{ fclose(f); };
    while (!feof(f)) {
        if (!fgets(buf, sizeof buf, f)) {
            if (!feof(f))
                fmt::print(stderr, "{}: io error fetching line of '{}'\n", __func__, path);
            break;
        }
        auto llen = strlen(buf);
        if (llen == 0)
            continue;
        if (buf[llen-1] == '\n')
            buf[--llen] = 0;
        ++ncs.linenum;
        if (do_parse_config(ncs, buf, llen) < 0) {
            fmt::print(stderr, "{}: do_parse_config({}) failed at line {}\n",
                       __func__, path, ncs.linenum);
            std::exit(EXIT_FAILURE);
        }
    }
    std::make_heap(stk.begin(), stk.end(), GtCronEntry);
    history_map.clear();
    cfg_reload = 1;
}

