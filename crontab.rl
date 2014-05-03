/* crontab.rl - configure file parser for ncron
 *
 * (c) 2003-2014 Nicholas J. Kain <njkain at gmail dot com>
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
#include <boost/algorithm/string/replace.hpp>
#include <fstream>
#include <unistd.h>
#include <stdio.h>
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
extern "C" {
#include "nk/log.h"
#include "nk/privilege.h"
}
#include "make_unique.hpp"

#include "ncron.hpp"
#include "sched.hpp"
#include "crontab.hpp"

/* BSD uses OFILE rather than NOFILE... */
#ifndef RLIMIT_NOFILE
#  define RLIMIT_NOFILE RLIMIT_OFILE
#endif

static int cfg_reload;    /* 0 on first call, 1 on subsequent calls */

struct ParseCfgState
{
    ParseCfgState(const std::string &ef, std::vector<StackItem> &stk,
                  std::vector<StackItem> &dstk) :
        stack(stk), deadstack(dstk), ce(nullptr), execfile(ef),
        jobid_st(nullptr), time_st(nullptr), intv_st(nullptr),
        intv2_st(nullptr), strv_st(nullptr), v_strlen(0), linenum(0), v_int(0),
        v_int2(0), cs(0), cmdret(0), intv2_exist(false), runat(false)
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
};

struct item_history {
    item_history() {}
    item_history(boost::optional<time_t> e, boost::optional<time_t> l,
                 boost::optional<unsigned int> n) :
        exectime(e), lasttime(l), numruns(n) {}
    boost::optional<time_t> exectime;
    boost::optional<time_t> lasttime;
    boost::optional<unsigned int> numruns;
};

struct hstm {
    hstm() : st(nullptr), cs(0), id(0) {}
    const char *st;
    int cs;
    unsigned int id;
    item_history h;
};

%%{
    machine history_m;
    access hst.;

    action St { hst.st = p; }
    action LastTimeEn { hst.h.lasttime = atoi(hst.st); }
    action NumRunsEn { hst.h.numruns = atoi(hst.st); }
    action ExecTimeEn { hst.h.exectime = atoi(hst.st); }
    action IdEn { hst.id = atoi(hst.st); }

    lasttime = '|' digit+ > St % LastTimeEn;
    numruns = ':' digit+ > St % NumRunsEn;
    exectime = '=' digit+ > St % ExecTimeEn;
    id = digit+ > St % IdEn;
    main := id (numruns | exectime | lasttime)+;
}%%

%% write data;

static int do_parse_history(hstm &hst, const std::string &l)
{
    const char *p = l.c_str();
    const char *pe = p + l.size();
    const char *eof = pe;

    %% write init;
    %% write exec;

    if (hst.cs >= history_m_first_final)
        return 1;
    if (hst.cs == history_m_error)
        return -1;
    return -2;
}

static std::unordered_map<unsigned int, item_history> history_map;

static void parse_history(const std::string &path)
{
    std::string l;
    std::ifstream f(path, std::ifstream::in);
    if (f.fail() || f.bad() || f.eof()) {
        log_error("failed to open history file \"%s\" for read", path.c_str());
        return;
    }
    size_t linenum = 0;
    while (1) {
        std::getline(f, l);
        ++linenum;
        if (f.eof())
            break;
        else if (f.bad() || f.fail()) {
            log_error("%s: io error fetching line of '%s'", path.c_str());
            return;
        }
        if (l.empty())
            continue;
        hstm h;
        auto r = do_parse_history(h, l);
        if (r < 0) {
            if (r == -2)
                log_error("%s: Incomplete configuration at line %zu; ignoring",
                          __func__, linenum);
            else
                log_error("%s: Malformed configuration at line %zu; ignoring.",
                          __func__, linenum);
            continue;
        }
        history_map.emplace(std::make_pair(
            h.id, item_history(h.h.exectime, h.h.lasttime, h.h.numruns)));
    }
}

static void get_history(std::unique_ptr<cronentry_t> &item)
{
    assert(item);

    auto i = history_map.find(item->id);
    if (i == history_map.end())
        return;
    if (i->second.exectime) {
        auto exectm = *i->second.exectime;
        item->exectime = exectm > 0 ? exectm : 0;
    }
    if (i->second.lasttime) {
        auto lasttm = *i->second.lasttime;
        item->lasttime = lasttm > 0 ? lasttm : 0;
    }
    if (i->second.numruns) {
        item->numruns = *i->second.numruns;
    }
}

static void setlim(ParseCfgState &ncs, int type)
{
    struct rlimit rli;
    rli.rlim_cur = ncs.v_int == 0 ? RLIM_INFINITY : ncs.v_int;
    rli.rlim_max = ncs.v_int2 == 0 ? RLIM_INFINITY : ncs.v_int2;

    if (!ncs.ce->limits)
        ncs.ce->limits = nk::make_unique<rlimits>();

    switch (type) {
    case RLIMIT_CPU: ncs.ce->limits->cpu = rli; break;
    case RLIMIT_FSIZE: ncs.ce->limits->fsize = rli; break;
    case RLIMIT_DATA: ncs.ce->limits->data = rli; break;
    case RLIMIT_STACK: ncs.ce->limits->stack = rli; break;
    case RLIMIT_CORE: ncs.ce->limits->core = rli; break;
    case RLIMIT_RSS: ncs.ce->limits->rss = rli; break;
    case RLIMIT_NPROC: ncs.ce->limits->nproc = rli; break;
    case RLIMIT_NOFILE: ncs.ce->limits->nofile = rli; break;
    case RLIMIT_MEMLOCK: ncs.ce->limits->memlock = rli; break;
#ifndef BSD
    case RLIMIT_AS: ncs.ce->limits->as = rli; break;
    case RLIMIT_MSGQUEUE: ncs.ce->limits->msgqueue = rli; break;
    case RLIMIT_NICE: ncs.ce->limits->nice = rli; break;
    case RLIMIT_RTTIME: ncs.ce->limits->rttime = rli; break;
    case RLIMIT_RTPRIO: ncs.ce->limits->rtprio = rli; break;
    case RLIMIT_SIGPENDING: ncs.ce->limits->sigpending = rli; break;
#endif /* BSD */
    default: suicide("%s: Bad RLIMIT_type specified.", __func__);
    }
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

static void setgroupv(ParseCfgState &ncs)
{
    if (nk_gidbyname(ncs.v_str, &ncs.ce->group))
        suicide("%s: nonexistent group specified at line %zu", ncs.linenum);
}

static void setuserv(ParseCfgState &ncs)
{
    if (nk_uidgidbyname(ncs.v_str, &ncs.ce->user, &ncs.ce->group))
        suicide("%s: nonexistent user specified at line %zu", ncs.linenum);
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
        boost::algorithm::replace_all(ncs.ce->command, "\\\\", "\\");
        boost::algorithm::replace_all(ncs.ce->command, "\\ ", " ");
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
        suicide("Duplicate 'command' value at line %zu", ncs.linenum);
    }

    %% write init;
    %% write exec;

    if (pckm.cs == parse_cmd_key_m_error) {
        ncs.cmdret = -1;
        suicide("Malformed 'command' value at line %zu", ncs.linenum);
    } else if (pckm.cs >= parse_cmd_key_m_first_final)
        ncs.cmdret = 1;
    else {
        ncs.cmdret = -2;
        suicide("Incomplete 'command' value at line %zu", ncs.linenum);
    }
}

static void create_ce(ParseCfgState &ncs)
{
    assert(!ncs.ce);
    ncs.ce = nk::make_unique<cronentry_t>();
    ncs.cmdret = 0;
    ncs.runat = false;
}

static inline void debug_print_ce(const ParseCfgState &ncs)
{
    if (!gflags_debug)
        return;
    log_debug("-=- finish_ce -=-");
    log_debug("id: %u", ncs.ce->id);
    log_debug("command: %s", ncs.ce->command.c_str());
    log_debug("args: %s", ncs.ce->args.c_str());
    log_debug("chroot: %s", ncs.ce->chroot.c_str());
    log_debug("numruns: %u", ncs.ce->numruns);
    log_debug("maxruns: %u", ncs.ce->maxruns);
    log_debug("journal: %d", ncs.ce->journal);
    log_debug("user: %u", ncs.ce->user);
    log_debug("group: %u", ncs.ce->group);
    for (const auto &i: ncs.ce->month)
        log_debug("month: [%d,%d]", i.first, i.second);
    for (const auto &i: ncs.ce->day)
        log_debug("day: [%d,%d]", i.first, i.second);
    for (const auto &i: ncs.ce->weekday)
        log_debug("weekday: [%d,%d]", i.first, i.second);
    for (const auto &i: ncs.ce->hour)
        log_debug("hour: [%d,%d]", i.first, i.second);
    for (const auto &i: ncs.ce->minute)
        log_debug("minute: [%d,%d]", i.first, i.second);
    log_debug("interval: %u", ncs.ce->interval);
    log_debug("exectime: %lu", ncs.ce->exectime);
    log_debug("lasttime: %lu", ncs.ce->lasttime);
}

static inline void debug_print_ce_history(const ParseCfgState &ncs)
{
    if (!gflags_debug)
        return;
    log_debug("[%u]->numruns = %u", ncs.ce->id, ncs.ce->numruns);
    log_debug("[%u]->exectime = %u", ncs.ce->id, ncs.ce->exectime);
    log_debug("[%u]->lasttime = %u", ncs.ce->id, ncs.ce->lasttime);
}

static void finish_ce(ParseCfgState &ncs)
{
    if (!ncs.ce)
        return;
    debug_print_ce(ncs);

    if (ncs.ce->id <= 0
        || (ncs.ce->interval <= 0 && ncs.ce->exectime <= 0)
        || ncs.ce->command.empty() || ncs.cmdret < 1) {
        log_debug("===> IGNORE");
        ncs.ce.reset();
        return;
    }
    log_debug("===> ADD");

    /* we have a job to insert */
    if (ncs.runat) { /* runat task */
        auto forced_exectime = ncs.ce->exectime;
        get_history(ncs.ce);
        ncs.ce->exectime = forced_exectime;
        debug_print_ce_history(ncs);

        /* insert iif we haven't exceeded maxruns */
        if (!ncs.ce->numruns)
            ncs.stack.emplace_back(std::move(ncs.ce));
        else
            ncs.deadstack.emplace_back(std::move(ncs.ce));
    } else { /* interval task */
        get_history(ncs.ce);
        debug_print_ce_history(ncs);
        set_initial_exectime(*ncs.ce);

        /* insert iif numruns < maxruns and no constr error */
        if ((ncs.ce->maxruns == 0 || ncs.ce->numruns < ncs.ce->maxruns)
            && ncs.ce->exectime != 0)
            ncs.stack.emplace_back(std::move(ncs.ce));
        else
            ncs.deadstack.emplace_back(std::move(ncs.ce));
    }
    ncs.ce.reset();
}

%%{
    machine ncrontab;
    access ncs.;

    spc = [ \t];
    eqsep = spc* '=' spc*;

    action TUnitSt { ncs.time_st = p; ncs.v_time = 0; }
    action TSecEn  { ncs.v_time +=          atoi(ncs.time_st); }
    action TMinEn  { ncs.v_time += 60     * atoi(ncs.time_st); }
    action THrEn   { ncs.v_time += 3600   * atoi(ncs.time_st); }
    action TDayEn  { ncs.v_time += 86400  * atoi(ncs.time_st); }
    action TWeekEn { ncs.v_time += 604800 * atoi(ncs.time_st); }

    action IntValSt {
        ncs.intv_st = p;
        ncs.v_int = ncs.v_int2 = 0;
        ncs.intv2_exist = false;
    }
    action IntValEn { ncs.v_int = atoi(ncs.intv_st); }
    action IntVal2St { ncs.intv2_st = p; }
    action IntVal2En {
        ncs.v_int2 = atoi(ncs.intv2_st);
        ncs.intv2_exist = true;
    }

    action StrValSt { ncs.strv_st = p; ncs.v_strlen = 0; }
    action StrValEn {
        ncs.v_strlen = p - ncs.strv_st;
        if (ncs.v_strlen <= INT_MAX) {
            ssize_t snl = snprintf(ncs.v_str, sizeof ncs.v_str,
                                   "%.*s", (int)ncs.v_strlen, ncs.strv_st);
            if (snl < 0 || (size_t)snl >= sizeof ncs.v_str)
                suicide("error parsing line %u in crontab; too long?",
                        ncs.linenum);
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

    action LimAsEn { setlim(ncs, RLIMIT_AS); }
    action LimMemlockEn { setlim(ncs, RLIMIT_MEMLOCK); }
    action LimNofileEn { setlim(ncs, RLIMIT_NOFILE); }
    action LimNprocEn { setlim(ncs, RLIMIT_NPROC); }
    action LimRssEn { setlim(ncs, RLIMIT_RSS); }
    action LimCoreEn { setlim(ncs, RLIMIT_CORE); }
    action LimStackEn { setlim(ncs, RLIMIT_STACK); }
    action LimDataEn { setlim(ncs, RLIMIT_DATA); }
    action LimFsizeEn { setlim(ncs, RLIMIT_FSIZE); }
    action LimCpuEn { setlim(ncs, RLIMIT_CPU); }
    action LimMsgQueueEn { setlim(ncs, RLIMIT_MSGQUEUE); }
    action LimNiceEn { setlim(ncs, RLIMIT_NICE); }
    action LimRtTimeEn { setlim(ncs, RLIMIT_RTTIME); }
    action LimRtPrioEn { setlim(ncs, RLIMIT_RTPRIO); }
    action LimSigPendingEn { setlim(ncs, RLIMIT_SIGPENDING); }

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

    action GroupEn { setgroupv(ncs); }
    action UserEn { setuserv(ncs); }
    action ChrootEn {
        ncs.ce->chroot = std::string(ncs.v_str, ncs.v_strlen);
    }
    action CommandEn { parse_command_key(ncs); }

    group = 'group'i eqsep stringval % GroupEn;
    user = 'user'i eqsep stringval % UserEn;
    chroot = 'chroot'i eqsep stringval % ChrootEn;
    command = 'command'i eqsep stringval % CommandEn;

    cmds = command | chroot | user | group | minute | hour | weekday | day |
           month | interval | lim_cpu | lim_fsize | lim_data | lim_stack |
           lim_core | lim_rss | lim_nproc | lim_nofile | lim_memlock | lim_as |
           lim_msgqueue | lim_nice | lim_rttime | lim_rtprio | lim_sigpending |
           maxruns | runat | journal;

    action JobIdSt { ncs.jobid_st = p; }
    action JobIdEn { ncs.ce->id = atoi(ncs.jobid_st); }
    action CreateCe { finish_ce(ncs); create_ce(ncs); }

    jobid = ('!' > CreateCe) (digit+ > JobIdSt) % JobIdEn;

    main := jobid | cmds;
}%%

%% write data;

static int do_parse_config(ParseCfgState &ncs, const std::string &l)
{
    const char *p = l.c_str();
    const char *pe = p + l.size();
    const char *eof = pe;

    %% write init;
    %% write exec;

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

    std::string l;
    std::ifstream f(path, std::ifstream::in);
    if (f.fail() || f.bad() || f.eof())
        suicide("%s: failed to open file: '%s'", path.c_str());
    while (1) {
        std::getline(f, l);
        ++ncs.linenum;
        if (f.eof())
            break;
        else if (f.bad() || f.fail())
            suicide("%s: io error fetching line of '%s'", path.c_str());
        if (l.empty())
            continue;
        auto r = do_parse_config(ncs, l);
        if (r < 0)
            suicide("%s: do_parse_config(%s) failed at line %u",
                    __func__, path.c_str(), ncs.linenum);
    }
    f.close();

    std::make_heap(stk.begin(), stk.end(), GtCronEntry);

    history_map.clear();
    cfg_reload = 1;
}

