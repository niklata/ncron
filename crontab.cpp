#line 1 "crontab.rl"
// Copyright 2003-2016 Nicholas J. Kain <njkain at gmail dot com>
// SPDX-License-Identifier: MIT
#include <algorithm>
#include <utility>
#include <unordered_map>
#include <optional>
#include <cstdio>
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
#include "nk/privs.h"
#include "nk/log.h"
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
		log_line("-=- finish_ce -=-");
		log_line("id: %u", ce->id);
		log_line("command: %s", ce->command.c_str());
		log_line("args: %s", ce->args.c_str());
		log_line("chroot: %s", ce->chroot.c_str());
		log_line("path: %s", ce->path.c_str());
		log_line("numruns: %u", ce->numruns);
		log_line("maxruns: %u", ce->maxruns);
		log_line("journal: %s", ce->journal ? "true" : "false");
		log_line("user: %u", ce->user);
		log_line("group: %u", ce->group);
		for (const auto &i: ce->month)
		log_line("month: [%u,%u]", i.first, i.second);
		for (const auto &i: ce->day)
		log_line("day: [%u,%u]", i.first, i.second);
		for (const auto &i: ce->weekday)
		log_line("weekday: [%u,%u]", i.first, i.second);
		for (const auto &i: ce->hour)
		log_line("hour: [%u,%u]", i.first, i.second);
		for (const auto &i: ce->minute)
		log_line("minute: [%u,%u]", i.first, i.second);
		log_line("interval: %u", ce->interval);
		log_line("exectime: %lu", ce->exectime);
		log_line("lasttime: %lu", ce->lasttime);
	}
	
	inline void debug_print_ce_history() const
	{
		if (!gflags_debug)
			return;
		log_line("[%u]->numruns = %u", ce->id, ce->numruns);
		log_line("[%u]->exectime = %lu", ce->id, ce->exectime);
		log_line("[%u]->lasttime = %lu", ce->id, ce->lasttime);
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
				log_line("===> IGNORE");
			ce.reset();
			return;
		}
		if (gflags_debug)
			log_line("===> ADD");
		
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
			log_line("%s: nonexistent group specified at line %zu",
			__func__, linenum);
			std::exit(EXIT_FAILURE);
		}
	}
	
	void setuserv()
	{
		if (nk_uidgidbyname(v_str, &ce->user, &ce->group)) {
			log_line("%s: nonexistent user specified at line %zu",
			__func__, linenum);
			std::exit(EXIT_FAILURE);
		}
	}
	
	void setlim(int type)
	{
		struct rlimit rli;
		rli.rlim_cur = v_int <= 0 ? RLIM_INFINITY : static_cast<size_t>(v_int);
		rli.rlim_max = v_int2 <= 0 ? RLIM_INFINITY : static_cast<size_t>(v_int2);
		ce->limits.add(type, rli);
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

#define MARKED_HST() hst.st, (p > hst.st ? static_cast<size_t>(p - hst.st) : 0)


#line 264 "crontab.rl"



#line 236 "crontab.cpp"
static const int history_m_start = 1;
static const int history_m_first_final = 6;
static const int history_m_error = 0;

static const int history_m_en_main = 1;


#line 266 "crontab.rl"


static int do_parse_history(hstm &hst, const char *p, size_t plen)
{
	const char *pe = p + plen;
	const char *eof = pe;
	
	
#line 253 "crontab.cpp"
	{
		hst.cs = (int)history_m_start;
	}
	
#line 273 "crontab.rl"
	
	
#line 261 "crontab.cpp"
	{
		switch ( hst.cs ) {
			case 1:
			goto st_case_1;
			case 0:
			goto st_case_0;
			case 2:
			goto st_case_2;
			case 3:
			goto st_case_3;
			case 6:
			goto st_case_6;
			case 4:
			goto st_case_4;
			case 7:
			goto st_case_7;
			case 5:
			goto st_case_5;
			case 8:
			goto st_case_8;
		}
		_st1:
		if ( p == eof )
			goto _out1;
		p+= 1;
		st_case_1:
		if ( p == pe && p != eof )
			goto _out1;
		if ( p == eof ) {
			goto _st1;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr2;
			}
			goto _st0;
		}
		_st0:
		if ( p == eof )
			goto _out0;
		st_case_0:
		goto _out0;
		_ctr2:
		{
#line 233 "crontab.rl"
			hst.st = p; }
		
#line 308 "crontab.cpp"
		
		goto _st2;
		_st2:
		if ( p == eof )
			goto _out2;
		p+= 1;
		st_case_2:
		if ( p == pe && p != eof )
			goto _out2;
		if ( p == eof ) {
			goto _st2;}
		else {
			switch( ( (*( p))) ) {
				case 58: {
					goto _ctr4;
				}
				case 61: {
					goto _ctr5;
				}
				case 124: {
					goto _ctr6;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st2;
			}
			goto _st0;
		}
		_ctr4:
		{
#line 252 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_HST())) hst.id = *t; else {
				hst.parse_error = true;
				{p+= 1; hst.cs = 3; goto _out;}
			}
		}
		
#line 347 "crontab.cpp"
		
		goto _st3;
		_ctr15:
		{
#line 240 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_HST())) hst.h.set_numruns(*t); else {
				hst.parse_error = true;
				{p+= 1; hst.cs = 3; goto _out;}
			}
		}
		
#line 360 "crontab.cpp"
		
		goto _st3;
		_ctr20:
		{
#line 246 "crontab.rl"
			
			if (auto t = nk::from_string<time_t>(MARKED_HST())) hst.h.set_exectime(*t); else {
				hst.parse_error = true;
				{p+= 1; hst.cs = 3; goto _out;}
			}
		}
		
#line 373 "crontab.cpp"
		
		goto _st3;
		_ctr25:
		{
#line 234 "crontab.rl"
			
			if (auto t = nk::from_string<time_t>(MARKED_HST())) hst.h.set_lasttime(*t); else {
				hst.parse_error = true;
				{p+= 1; hst.cs = 3; goto _out;}
			}
		}
		
#line 386 "crontab.cpp"
		
		goto _st3;
		_st3:
		if ( p == eof )
			goto _out3;
		p+= 1;
		st_case_3:
		if ( p == pe && p != eof )
			goto _out3;
		if ( p == eof ) {
			goto _st3;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr8;
			}
			goto _st0;
		}
		_ctr8:
		{
#line 233 "crontab.rl"
			hst.st = p; }
		
#line 409 "crontab.cpp"
		
		goto _st6;
		_ctr13:
		{
#line 240 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_HST())) hst.h.set_numruns(*t); else {
				hst.parse_error = true;
				{p+= 1; hst.cs = 6; goto _out;}
			}
		}
		
#line 422 "crontab.cpp"
		
		goto _st6;
		_st6:
		if ( p == eof )
			goto _out6;
		p+= 1;
		st_case_6:
		if ( p == pe && p != eof )
			goto _out6;
		if ( p == eof ) {
			goto _ctr13;}
		else {
			switch( ( (*( p))) ) {
				case 58: {
					goto _ctr15;
				}
				case 61: {
					goto _ctr16;
				}
				case 124: {
					goto _ctr17;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st6;
			}
			goto _st0;
		}
		_ctr5:
		{
#line 252 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_HST())) hst.id = *t; else {
				hst.parse_error = true;
				{p+= 1; hst.cs = 4; goto _out;}
			}
		}
		
#line 461 "crontab.cpp"
		
		goto _st4;
		_ctr16:
		{
#line 240 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_HST())) hst.h.set_numruns(*t); else {
				hst.parse_error = true;
				{p+= 1; hst.cs = 4; goto _out;}
			}
		}
		
#line 474 "crontab.cpp"
		
		goto _st4;
		_ctr21:
		{
#line 246 "crontab.rl"
			
			if (auto t = nk::from_string<time_t>(MARKED_HST())) hst.h.set_exectime(*t); else {
				hst.parse_error = true;
				{p+= 1; hst.cs = 4; goto _out;}
			}
		}
		
#line 487 "crontab.cpp"
		
		goto _st4;
		_ctr26:
		{
#line 234 "crontab.rl"
			
			if (auto t = nk::from_string<time_t>(MARKED_HST())) hst.h.set_lasttime(*t); else {
				hst.parse_error = true;
				{p+= 1; hst.cs = 4; goto _out;}
			}
		}
		
#line 500 "crontab.cpp"
		
		goto _st4;
		_st4:
		if ( p == eof )
			goto _out4;
		p+= 1;
		st_case_4:
		if ( p == pe && p != eof )
			goto _out4;
		if ( p == eof ) {
			goto _st4;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr10;
			}
			goto _st0;
		}
		_ctr10:
		{
#line 233 "crontab.rl"
			hst.st = p; }
		
#line 523 "crontab.cpp"
		
		goto _st7;
		_ctr18:
		{
#line 246 "crontab.rl"
			
			if (auto t = nk::from_string<time_t>(MARKED_HST())) hst.h.set_exectime(*t); else {
				hst.parse_error = true;
				{p+= 1; hst.cs = 7; goto _out;}
			}
		}
		
#line 536 "crontab.cpp"
		
		goto _st7;
		_st7:
		if ( p == eof )
			goto _out7;
		p+= 1;
		st_case_7:
		if ( p == pe && p != eof )
			goto _out7;
		if ( p == eof ) {
			goto _ctr18;}
		else {
			switch( ( (*( p))) ) {
				case 58: {
					goto _ctr20;
				}
				case 61: {
					goto _ctr21;
				}
				case 124: {
					goto _ctr22;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st7;
			}
			goto _st0;
		}
		_ctr6:
		{
#line 252 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_HST())) hst.id = *t; else {
				hst.parse_error = true;
				{p+= 1; hst.cs = 5; goto _out;}
			}
		}
		
#line 575 "crontab.cpp"
		
		goto _st5;
		_ctr17:
		{
#line 240 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_HST())) hst.h.set_numruns(*t); else {
				hst.parse_error = true;
				{p+= 1; hst.cs = 5; goto _out;}
			}
		}
		
#line 588 "crontab.cpp"
		
		goto _st5;
		_ctr22:
		{
#line 246 "crontab.rl"
			
			if (auto t = nk::from_string<time_t>(MARKED_HST())) hst.h.set_exectime(*t); else {
				hst.parse_error = true;
				{p+= 1; hst.cs = 5; goto _out;}
			}
		}
		
#line 601 "crontab.cpp"
		
		goto _st5;
		_ctr27:
		{
#line 234 "crontab.rl"
			
			if (auto t = nk::from_string<time_t>(MARKED_HST())) hst.h.set_lasttime(*t); else {
				hst.parse_error = true;
				{p+= 1; hst.cs = 5; goto _out;}
			}
		}
		
#line 614 "crontab.cpp"
		
		goto _st5;
		_st5:
		if ( p == eof )
			goto _out5;
		p+= 1;
		st_case_5:
		if ( p == pe && p != eof )
			goto _out5;
		if ( p == eof ) {
			goto _st5;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr12;
			}
			goto _st0;
		}
		_ctr12:
		{
#line 233 "crontab.rl"
			hst.st = p; }
		
#line 637 "crontab.cpp"
		
		goto _st8;
		_ctr23:
		{
#line 234 "crontab.rl"
			
			if (auto t = nk::from_string<time_t>(MARKED_HST())) hst.h.set_lasttime(*t); else {
				hst.parse_error = true;
				{p+= 1; hst.cs = 8; goto _out;}
			}
		}
		
#line 650 "crontab.cpp"
		
		goto _st8;
		_st8:
		if ( p == eof )
			goto _out8;
		p+= 1;
		st_case_8:
		if ( p == pe && p != eof )
			goto _out8;
		if ( p == eof ) {
			goto _ctr23;}
		else {
			switch( ( (*( p))) ) {
				case 58: {
					goto _ctr25;
				}
				case 61: {
					goto _ctr26;
				}
				case 124: {
					goto _ctr27;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st8;
			}
			goto _st0;
		}
		_out1: hst.cs = 1; goto _out; 
		_out0: hst.cs = 0; goto _out; 
		_out2: hst.cs = 2; goto _out; 
		_out3: hst.cs = 3; goto _out; 
		_out6: hst.cs = 6; goto _out; 
		_out4: hst.cs = 4; goto _out; 
		_out7: hst.cs = 7; goto _out; 
		_out5: hst.cs = 5; goto _out; 
		_out8: hst.cs = 8; goto _out; 
		_out: {}
	}
	
#line 274 "crontab.rl"
	
	
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
		log_line("%s: failed to open history file \"%s\" for read: %s",
		__func__, path.c_str(), strerror(errno));
		return;
	}
	SCOPE_EXIT{ fclose(f); };
	size_t linenum = 0;
	while (!feof(f)) {
		if (!fgets(buf, sizeof buf, f)) {
			if (!feof(f))
				log_line("%s: io error fetching line of '%s'", __func__, path.c_str());
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
				log_line("%s: Incomplete configuration at line %zu; ignoring",
			__func__, linenum);
			else
				log_line("%s: Malformed configuration at line %zu; ignoring.",
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

#define MARKED_PCKM() pckm.st, (p > pckm.st ? static_cast<size_t>(p - pckm.st) : 0)


#line 391 "crontab.rl"



#line 797 "crontab.cpp"
static const int parse_cmd_key_m_start = 1;
static const int parse_cmd_key_m_first_final = 2;
static const int parse_cmd_key_m_error = 0;

static const int parse_cmd_key_m_en_main = 1;


#line 393 "crontab.rl"


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
		log_line("Duplicate 'command' value at line %zu", ncs.linenum);
		std::exit(EXIT_FAILURE);
	}
	
	
#line 828 "crontab.cpp"
	{
		pckm.cs = (int)parse_cmd_key_m_start;
	}
	
#line 414 "crontab.rl"
	
	
#line 836 "crontab.cpp"
	{
		switch ( pckm.cs ) {
			case 1:
			goto st_case_1;
			case 2:
			goto st_case_2;
			case 0:
			goto st_case_0;
			case 3:
			goto st_case_3;
			case 4:
			goto st_case_4;
			case 5:
			goto st_case_5;
			case 6:
			goto st_case_6;
			case 7:
			goto st_case_7;
			case 8:
			goto st_case_8;
		}
		_st1:
		if ( p == eof )
			goto _out1;
		p+= 1;
		st_case_1:
		if ( p == pe && p != eof )
			goto _out1;
		if ( p == eof ) {
			goto _st1;}
		else {
			switch( ( (*( p))) ) {
				case 0: {
					goto _st0;
				}
				case 9: {
					goto _st1;
				}
				case 32: {
					goto _st1;
				}
				case 92: {
					goto _ctr3;
				}
			}
			goto _ctr2;
		}
		_ctr2:
		{
#line 378 "crontab.rl"
			pckm.st = p; }
		
#line 889 "crontab.cpp"
		
		goto _st2;
		_ctr4:
		{
#line 379 "crontab.rl"
			
			ncs.ce->command = std::string(MARKED_PCKM());
			string_replace_all(ncs.ce->command, "\\ ", 2, " ");
			string_replace_all(ncs.ce->command, "\\\\", 2, "\\");
		}
		
#line 901 "crontab.cpp"
		
		goto _st2;
		_st2:
		if ( p == eof )
			goto _out2;
		p+= 1;
		st_case_2:
		if ( p == pe && p != eof )
			goto _out2;
		if ( p == eof ) {
			goto _ctr4;}
		else {
			switch( ( (*( p))) ) {
				case 0: {
					goto _st0;
				}
				case 9: {
					goto _ctr6;
				}
				case 32: {
					goto _ctr6;
				}
				case 92: {
					goto _st5;
				}
			}
			goto _st2;
		}
		_st0:
		if ( p == eof )
			goto _out0;
		st_case_0:
		goto _out0;
		_ctr10:
		{
#line 378 "crontab.rl"
			pckm.st = p; }
		
#line 940 "crontab.cpp"
		
		goto _st3;
		_ctr6:
		{
#line 379 "crontab.rl"
			
			ncs.ce->command = std::string(MARKED_PCKM());
			string_replace_all(ncs.ce->command, "\\ ", 2, " ");
			string_replace_all(ncs.ce->command, "\\\\", 2, "\\");
		}
		
#line 952 "crontab.cpp"
		
		goto _st3;
		_ctr8:
		{
#line 378 "crontab.rl"
			pckm.st = p; }
		
#line 960 "crontab.cpp"
		
		{
#line 384 "crontab.rl"
			ncs.ce->args = std::string(MARKED_PCKM()); }
		
#line 966 "crontab.cpp"
		
		goto _st3;
		_ctr17:
		{
#line 379 "crontab.rl"
			
			ncs.ce->command = std::string(MARKED_PCKM());
			string_replace_all(ncs.ce->command, "\\ ", 2, " ");
			string_replace_all(ncs.ce->command, "\\\\", 2, "\\");
		}
		
#line 978 "crontab.cpp"
		
		{
#line 378 "crontab.rl"
			pckm.st = p; }
		
#line 984 "crontab.cpp"
		
		goto _st3;
		_st3:
		if ( p == eof )
			goto _out3;
		p+= 1;
		st_case_3:
		if ( p == pe && p != eof )
			goto _out3;
		if ( p == eof ) {
			goto _ctr8;}
		else {
			switch( ( (*( p))) ) {
				case 0: {
					goto _st0;
				}
				case 9: {
					goto _ctr10;
				}
				case 32: {
					goto _ctr10;
				}
			}
			goto _ctr9;
		}
		_ctr9:
		{
#line 378 "crontab.rl"
			pckm.st = p; }
		
#line 1015 "crontab.cpp"
		
		goto _st4;
		_ctr11:
		{
#line 384 "crontab.rl"
			ncs.ce->args = std::string(MARKED_PCKM()); }
		
#line 1023 "crontab.cpp"
		
		goto _st4;
		_st4:
		if ( p == eof )
			goto _out4;
		p+= 1;
		st_case_4:
		if ( p == pe && p != eof )
			goto _out4;
		if ( p == eof ) {
			goto _ctr11;}
		else {
			if ( ( (*( p))) == 0 ) {
				goto _st0;
			}
			goto _st4;
		}
		_ctr3:
		{
#line 378 "crontab.rl"
			pckm.st = p; }
		
#line 1046 "crontab.cpp"
		
		goto _st5;
		_ctr13:
		{
#line 379 "crontab.rl"
			
			ncs.ce->command = std::string(MARKED_PCKM());
			string_replace_all(ncs.ce->command, "\\ ", 2, " ");
			string_replace_all(ncs.ce->command, "\\\\", 2, "\\");
		}
		
#line 1058 "crontab.cpp"
		
		goto _st5;
		_st5:
		if ( p == eof )
			goto _out5;
		p+= 1;
		st_case_5:
		if ( p == pe && p != eof )
			goto _out5;
		if ( p == eof ) {
			goto _ctr13;}
		else {
			switch( ( (*( p))) ) {
				case 0: {
					goto _st0;
				}
				case 9: {
					goto _ctr6;
				}
				case 32: {
					goto _ctr14;
				}
				case 92: {
					goto _st5;
				}
			}
			goto _st2;
		}
		_ctr14:
		{
#line 379 "crontab.rl"
			
			ncs.ce->command = std::string(MARKED_PCKM());
			string_replace_all(ncs.ce->command, "\\ ", 2, " ");
			string_replace_all(ncs.ce->command, "\\\\", 2, "\\");
		}
		
#line 1096 "crontab.cpp"
		
		goto _st6;
		_ctr15:
		{
#line 379 "crontab.rl"
			
			ncs.ce->command = std::string(MARKED_PCKM());
			string_replace_all(ncs.ce->command, "\\ ", 2, " ");
			string_replace_all(ncs.ce->command, "\\\\", 2, "\\");
		}
		
#line 1108 "crontab.cpp"
		
		{
#line 378 "crontab.rl"
			pckm.st = p; }
		
#line 1114 "crontab.cpp"
		
		{
#line 384 "crontab.rl"
			ncs.ce->args = std::string(MARKED_PCKM()); }
		
#line 1120 "crontab.cpp"
		
		goto _st6;
		_st6:
		if ( p == eof )
			goto _out6;
		p+= 1;
		st_case_6:
		if ( p == pe && p != eof )
			goto _out6;
		if ( p == eof ) {
			goto _ctr15;}
		else {
			switch( ( (*( p))) ) {
				case 0: {
					goto _st0;
				}
				case 9: {
					goto _ctr17;
				}
				case 32: {
					goto _ctr17;
				}
				case 92: {
					goto _ctr18;
				}
			}
			goto _ctr16;
		}
		_ctr16:
		{
#line 378 "crontab.rl"
			pckm.st = p; }
		
#line 1154 "crontab.cpp"
		
		goto _st7;
		_ctr19:
		{
#line 379 "crontab.rl"
			
			ncs.ce->command = std::string(MARKED_PCKM());
			string_replace_all(ncs.ce->command, "\\ ", 2, " ");
			string_replace_all(ncs.ce->command, "\\\\", 2, "\\");
		}
		
#line 1166 "crontab.cpp"
		
		{
#line 384 "crontab.rl"
			ncs.ce->args = std::string(MARKED_PCKM()); }
		
#line 1172 "crontab.cpp"
		
		goto _st7;
		_st7:
		if ( p == eof )
			goto _out7;
		p+= 1;
		st_case_7:
		if ( p == pe && p != eof )
			goto _out7;
		if ( p == eof ) {
			goto _ctr19;}
		else {
			switch( ( (*( p))) ) {
				case 0: {
					goto _st0;
				}
				case 9: {
					goto _ctr6;
				}
				case 32: {
					goto _ctr6;
				}
				case 92: {
					goto _st8;
				}
			}
			goto _st7;
		}
		_ctr18:
		{
#line 378 "crontab.rl"
			pckm.st = p; }
		
#line 1206 "crontab.cpp"
		
		goto _st8;
		_ctr22:
		{
#line 379 "crontab.rl"
			
			ncs.ce->command = std::string(MARKED_PCKM());
			string_replace_all(ncs.ce->command, "\\ ", 2, " ");
			string_replace_all(ncs.ce->command, "\\\\", 2, "\\");
		}
		
#line 1218 "crontab.cpp"
		
		{
#line 384 "crontab.rl"
			ncs.ce->args = std::string(MARKED_PCKM()); }
		
#line 1224 "crontab.cpp"
		
		goto _st8;
		_st8:
		if ( p == eof )
			goto _out8;
		p+= 1;
		st_case_8:
		if ( p == pe && p != eof )
			goto _out8;
		if ( p == eof ) {
			goto _ctr22;}
		else {
			switch( ( (*( p))) ) {
				case 0: {
					goto _st0;
				}
				case 9: {
					goto _ctr6;
				}
				case 32: {
					goto _ctr14;
				}
				case 92: {
					goto _st8;
				}
			}
			goto _st7;
		}
		_out1: pckm.cs = 1; goto _out; 
		_out2: pckm.cs = 2; goto _out; 
		_out0: pckm.cs = 0; goto _out; 
		_out3: pckm.cs = 3; goto _out; 
		_out4: pckm.cs = 4; goto _out; 
		_out5: pckm.cs = 5; goto _out; 
		_out6: pckm.cs = 6; goto _out; 
		_out7: pckm.cs = 7; goto _out; 
		_out8: pckm.cs = 8; goto _out; 
		_out: {}
	}
	
#line 415 "crontab.rl"
	
	
	if (pckm.cs == parse_cmd_key_m_error) {
		ncs.cmdret = -1;
		log_line("Malformed 'command' value at line %zu", ncs.linenum);
		std::exit(EXIT_FAILURE);
	} else if (pckm.cs >= parse_cmd_key_m_first_final)
	ncs.cmdret = 1;
	else {
		ncs.cmdret = -2;
		log_line("Incomplete 'command' value at line %zu", ncs.linenum);
		std::exit(EXIT_FAILURE);
	}
}

#define MARKED_TIME() ncs.time_st, (p > (ncs.time_st + 1) ? static_cast<size_t>(p - ncs.time_st - 1) : 0)
#define MARKED_INTV1() ncs.intv_st, (p > ncs.intv_st ? static_cast<size_t>(p - ncs.intv_st) : 0)
#define MARKED_INTV2() ncs.intv2_st, (p > ncs.intv2_st ? static_cast<size_t>(p - ncs.intv2_st) : 0)
#define MARKED_JOBID() ncs.jobid_st, (p > ncs.jobid_st ? static_cast<size_t>(p - ncs.jobid_st) : 0)


#line 619 "crontab.rl"



#line 1291 "crontab.cpp"
static const int ncrontab_start = 1;
static const int ncrontab_first_final = 198;
static const int ncrontab_error = 0;

static const int ncrontab_en_main = 1;


#line 621 "crontab.rl"


static int do_parse_config(ParseCfgState &ncs, const char *p, size_t plen)
{
	const char *pe = p + plen;
	const char *eof = pe;
	
	
#line 1308 "crontab.cpp"
	{
		ncs.cs = (int)ncrontab_start;
	}
	
#line 628 "crontab.rl"
	
	
#line 1316 "crontab.cpp"
	{
		switch ( ncs.cs ) {
			case 1:
			goto st_case_1;
			case 0:
			goto st_case_0;
			case 2:
			goto st_case_2;
			case 198:
			goto st_case_198;
			case 3:
			goto st_case_3;
			case 4:
			goto st_case_4;
			case 5:
			goto st_case_5;
			case 6:
			goto st_case_6;
			case 7:
			goto st_case_7;
			case 8:
			goto st_case_8;
			case 9:
			goto st_case_9;
			case 199:
			goto st_case_199;
			case 200:
			goto st_case_200;
			case 10:
			goto st_case_10;
			case 11:
			goto st_case_11;
			case 12:
			goto st_case_12;
			case 13:
			goto st_case_13;
			case 14:
			goto st_case_14;
			case 15:
			goto st_case_15;
			case 16:
			goto st_case_16;
			case 201:
			goto st_case_201;
			case 202:
			goto st_case_202;
			case 17:
			goto st_case_17;
			case 18:
			goto st_case_18;
			case 19:
			goto st_case_19;
			case 20:
			goto st_case_20;
			case 203:
			goto st_case_203;
			case 21:
			goto st_case_21;
			case 204:
			goto st_case_204;
			case 22:
			goto st_case_22;
			case 23:
			goto st_case_23;
			case 24:
			goto st_case_24;
			case 25:
			goto st_case_25;
			case 26:
			goto st_case_26;
			case 27:
			goto st_case_27;
			case 205:
			goto st_case_205;
			case 206:
			goto st_case_206;
			case 28:
			goto st_case_28;
			case 29:
			goto st_case_29;
			case 30:
			goto st_case_30;
			case 31:
			goto st_case_31;
			case 32:
			goto st_case_32;
			case 207:
			goto st_case_207;
			case 33:
			goto st_case_33;
			case 208:
			goto st_case_208;
			case 34:
			goto st_case_34;
			case 35:
			goto st_case_35;
			case 36:
			goto st_case_36;
			case 37:
			goto st_case_37;
			case 38:
			goto st_case_38;
			case 39:
			goto st_case_39;
			case 40:
			goto st_case_40;
			case 41:
			goto st_case_41;
			case 42:
			goto st_case_42;
			case 43:
			goto st_case_43;
			case 209:
			goto st_case_209;
			case 210:
			goto st_case_210;
			case 211:
			goto st_case_211;
			case 212:
			goto st_case_212;
			case 213:
			goto st_case_213;
			case 44:
			goto st_case_44;
			case 45:
			goto st_case_45;
			case 46:
			goto st_case_46;
			case 47:
			goto st_case_47;
			case 48:
			goto st_case_48;
			case 49:
			goto st_case_49;
			case 214:
			goto st_case_214;
			case 50:
			goto st_case_50;
			case 51:
			goto st_case_51;
			case 52:
			goto st_case_52;
			case 53:
			goto st_case_53;
			case 54:
			goto st_case_54;
			case 215:
			goto st_case_215;
			case 55:
			goto st_case_55;
			case 216:
			goto st_case_216;
			case 56:
			goto st_case_56;
			case 57:
			goto st_case_57;
			case 58:
			goto st_case_58;
			case 59:
			goto st_case_59;
			case 60:
			goto st_case_60;
			case 217:
			goto st_case_217;
			case 61:
			goto st_case_61;
			case 218:
			goto st_case_218;
			case 62:
			goto st_case_62;
			case 63:
			goto st_case_63;
			case 64:
			goto st_case_64;
			case 219:
			goto st_case_219;
			case 65:
			goto st_case_65;
			case 220:
			goto st_case_220;
			case 66:
			goto st_case_66;
			case 67:
			goto st_case_67;
			case 68:
			goto st_case_68;
			case 69:
			goto st_case_69;
			case 70:
			goto st_case_70;
			case 221:
			goto st_case_221;
			case 71:
			goto st_case_71;
			case 222:
			goto st_case_222;
			case 72:
			goto st_case_72;
			case 73:
			goto st_case_73;
			case 74:
			goto st_case_74;
			case 75:
			goto st_case_75;
			case 76:
			goto st_case_76;
			case 77:
			goto st_case_77;
			case 223:
			goto st_case_223;
			case 78:
			goto st_case_78;
			case 224:
			goto st_case_224;
			case 79:
			goto st_case_79;
			case 80:
			goto st_case_80;
			case 81:
			goto st_case_81;
			case 82:
			goto st_case_82;
			case 83:
			goto st_case_83;
			case 84:
			goto st_case_84;
			case 85:
			goto st_case_85;
			case 86:
			goto st_case_86;
			case 225:
			goto st_case_225;
			case 87:
			goto st_case_87;
			case 226:
			goto st_case_226;
			case 88:
			goto st_case_88;
			case 89:
			goto st_case_89;
			case 90:
			goto st_case_90;
			case 91:
			goto st_case_91;
			case 92:
			goto st_case_92;
			case 93:
			goto st_case_93;
			case 94:
			goto st_case_94;
			case 95:
			goto st_case_95;
			case 227:
			goto st_case_227;
			case 96:
			goto st_case_96;
			case 228:
			goto st_case_228;
			case 97:
			goto st_case_97;
			case 98:
			goto st_case_98;
			case 99:
			goto st_case_99;
			case 100:
			goto st_case_100;
			case 101:
			goto st_case_101;
			case 229:
			goto st_case_229;
			case 102:
			goto st_case_102;
			case 230:
			goto st_case_230;
			case 103:
			goto st_case_103;
			case 104:
			goto st_case_104;
			case 105:
			goto st_case_105;
			case 106:
			goto st_case_106;
			case 107:
			goto st_case_107;
			case 108:
			goto st_case_108;
			case 231:
			goto st_case_231;
			case 109:
			goto st_case_109;
			case 232:
			goto st_case_232;
			case 110:
			goto st_case_110;
			case 111:
			goto st_case_111;
			case 112:
			goto st_case_112;
			case 113:
			goto st_case_113;
			case 114:
			goto st_case_114;
			case 233:
			goto st_case_233;
			case 115:
			goto st_case_115;
			case 234:
			goto st_case_234;
			case 116:
			goto st_case_116;
			case 117:
			goto st_case_117;
			case 118:
			goto st_case_118;
			case 119:
			goto st_case_119;
			case 235:
			goto st_case_235;
			case 120:
			goto st_case_120;
			case 236:
			goto st_case_236;
			case 121:
			goto st_case_121;
			case 122:
			goto st_case_122;
			case 123:
			goto st_case_123;
			case 124:
			goto st_case_124;
			case 125:
			goto st_case_125;
			case 126:
			goto st_case_126;
			case 237:
			goto st_case_237;
			case 127:
			goto st_case_127;
			case 238:
			goto st_case_238;
			case 128:
			goto st_case_128;
			case 129:
			goto st_case_129;
			case 130:
			goto st_case_130;
			case 131:
			goto st_case_131;
			case 132:
			goto st_case_132;
			case 239:
			goto st_case_239;
			case 133:
			goto st_case_133;
			case 240:
			goto st_case_240;
			case 134:
			goto st_case_134;
			case 135:
			goto st_case_135;
			case 136:
			goto st_case_136;
			case 137:
			goto st_case_137;
			case 138:
			goto st_case_138;
			case 139:
			goto st_case_139;
			case 140:
			goto st_case_140;
			case 141:
			goto st_case_141;
			case 142:
			goto st_case_142;
			case 143:
			goto st_case_143;
			case 144:
			goto st_case_144;
			case 241:
			goto st_case_241;
			case 145:
			goto st_case_145;
			case 242:
			goto st_case_242;
			case 146:
			goto st_case_146;
			case 147:
			goto st_case_147;
			case 148:
			goto st_case_148;
			case 149:
			goto st_case_149;
			case 150:
			goto st_case_150;
			case 243:
			goto st_case_243;
			case 151:
			goto st_case_151;
			case 244:
			goto st_case_244;
			case 152:
			goto st_case_152;
			case 153:
			goto st_case_153;
			case 154:
			goto st_case_154;
			case 155:
			goto st_case_155;
			case 156:
			goto st_case_156;
			case 157:
			goto st_case_157;
			case 158:
			goto st_case_158;
			case 159:
			goto st_case_159;
			case 245:
			goto st_case_245;
			case 160:
			goto st_case_160;
			case 161:
			goto st_case_161;
			case 162:
			goto st_case_162;
			case 163:
			goto st_case_163;
			case 164:
			goto st_case_164;
			case 165:
			goto st_case_165;
			case 246:
			goto st_case_246;
			case 166:
			goto st_case_166;
			case 247:
			goto st_case_247;
			case 167:
			goto st_case_167;
			case 168:
			goto st_case_168;
			case 169:
			goto st_case_169;
			case 170:
			goto st_case_170;
			case 171:
			goto st_case_171;
			case 248:
			goto st_case_248;
			case 172:
			goto st_case_172;
			case 249:
			goto st_case_249;
			case 173:
			goto st_case_173;
			case 174:
			goto st_case_174;
			case 175:
			goto st_case_175;
			case 176:
			goto st_case_176;
			case 177:
			goto st_case_177;
			case 250:
			goto st_case_250;
			case 251:
			goto st_case_251;
			case 178:
			goto st_case_178;
			case 179:
			goto st_case_179;
			case 180:
			goto st_case_180;
			case 181:
			goto st_case_181;
			case 182:
			goto st_case_182;
			case 183:
			goto st_case_183;
			case 252:
			goto st_case_252;
			case 184:
			goto st_case_184;
			case 185:
			goto st_case_185;
			case 186:
			goto st_case_186;
			case 187:
			goto st_case_187;
			case 188:
			goto st_case_188;
			case 253:
			goto st_case_253;
			case 254:
			goto st_case_254;
			case 189:
			goto st_case_189;
			case 190:
			goto st_case_190;
			case 191:
			goto st_case_191;
			case 192:
			goto st_case_192;
			case 193:
			goto st_case_193;
			case 194:
			goto st_case_194;
			case 195:
			goto st_case_195;
			case 196:
			goto st_case_196;
			case 255:
			goto st_case_255;
			case 197:
			goto st_case_197;
			case 256:
			goto st_case_256;
		}
		_st1:
		if ( p == eof )
			goto _out1;
		p+= 1;
		st_case_1:
		if ( p == pe && p != eof )
			goto _out1;
		if ( p == eof ) {
			goto _st1;}
		else {
			switch( ( (*( p))) ) {
				case 33: {
					goto _ctr2;
				}
				case 67: {
					goto _st3;
				}
				case 68: {
					goto _st17;
				}
				case 71: {
					goto _st22;
				}
				case 72: {
					goto _st28;
				}
				case 73: {
					goto _st34;
				}
				case 74: {
					goto _st44;
				}
				case 76: {
					goto _st50;
				}
				case 77: {
					goto _st152;
				}
				case 80: {
					goto _st173;
				}
				case 82: {
					goto _st178;
				}
				case 85: {
					goto _st184;
				}
				case 87: {
					goto _st189;
				}
				case 99: {
					goto _st3;
				}
				case 100: {
					goto _st17;
				}
				case 103: {
					goto _st22;
				}
				case 104: {
					goto _st28;
				}
				case 105: {
					goto _st34;
				}
				case 106: {
					goto _st44;
				}
				case 108: {
					goto _st50;
				}
				case 109: {
					goto _st152;
				}
				case 112: {
					goto _st173;
				}
				case 114: {
					goto _st178;
				}
				case 117: {
					goto _st184;
				}
				case 119: {
					goto _st189;
				}
			}
			goto _st0;
		}
		_st0:
		if ( p == eof )
			goto _out0;
		st_case_0:
		goto _out0;
		_ctr2:
		{
#line 614 "crontab.rl"
			ncs.finish_ce(); ncs.create_ce(); }
		
#line 1933 "crontab.cpp"
		
		goto _st2;
		_st2:
		if ( p == eof )
			goto _out2;
		p+= 1;
		st_case_2:
		if ( p == pe && p != eof )
			goto _out2;
		if ( p == eof ) {
			goto _st2;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr16;
			}
			goto _st0;
		}
		_ctr16:
		{
#line 607 "crontab.rl"
			ncs.jobid_st = p; }
		
#line 1956 "crontab.cpp"
		
		goto _st198;
		_ctr259:
		{
#line 608 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_JOBID())) ncs.ce->id = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 198; goto _out;}
			}
		}
		
#line 1969 "crontab.cpp"
		
		goto _st198;
		_st198:
		if ( p == eof )
			goto _out198;
		p+= 1;
		st_case_198:
		if ( p == pe && p != eof )
			goto _out198;
		if ( p == eof ) {
			goto _ctr259;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st198;
			}
			goto _st0;
		}
		_st3:
		if ( p == eof )
			goto _out3;
		p+= 1;
		st_case_3:
		if ( p == pe && p != eof )
			goto _out3;
		if ( p == eof ) {
			goto _st3;}
		else {
			switch( ( (*( p))) ) {
				case 72: {
					goto _st4;
				}
				case 79: {
					goto _st10;
				}
				case 104: {
					goto _st4;
				}
				case 111: {
					goto _st10;
				}
			}
			goto _st0;
		}
		_st4:
		if ( p == eof )
			goto _out4;
		p+= 1;
		st_case_4:
		if ( p == pe && p != eof )
			goto _out4;
		if ( p == eof ) {
			goto _st4;}
		else {
			switch( ( (*( p))) ) {
				case 82: {
					goto _st5;
				}
				case 114: {
					goto _st5;
				}
			}
			goto _st0;
		}
		_st5:
		if ( p == eof )
			goto _out5;
		p+= 1;
		st_case_5:
		if ( p == pe && p != eof )
			goto _out5;
		if ( p == eof ) {
			goto _st5;}
		else {
			switch( ( (*( p))) ) {
				case 79: {
					goto _st6;
				}
				case 111: {
					goto _st6;
				}
			}
			goto _st0;
		}
		_st6:
		if ( p == eof )
			goto _out6;
		p+= 1;
		st_case_6:
		if ( p == pe && p != eof )
			goto _out6;
		if ( p == eof ) {
			goto _st6;}
		else {
			switch( ( (*( p))) ) {
				case 79: {
					goto _st7;
				}
				case 111: {
					goto _st7;
				}
			}
			goto _st0;
		}
		_st7:
		if ( p == eof )
			goto _out7;
		p+= 1;
		st_case_7:
		if ( p == pe && p != eof )
			goto _out7;
		if ( p == eof ) {
			goto _st7;}
		else {
			switch( ( (*( p))) ) {
				case 84: {
					goto _st8;
				}
				case 116: {
					goto _st8;
				}
			}
			goto _st0;
		}
		_st8:
		if ( p == eof )
			goto _out8;
		p+= 1;
		st_case_8:
		if ( p == pe && p != eof )
			goto _out8;
		if ( p == eof ) {
			goto _st8;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st8;
				}
				case 32: {
					goto _st8;
				}
				case 61: {
					goto _st9;
				}
			}
			goto _st0;
		}
		_st9:
		if ( p == eof )
			goto _out9;
		p+= 1;
		st_case_9:
		if ( p == pe && p != eof )
			goto _out9;
		if ( p == eof ) {
			goto _st9;}
		else {
			switch( ( (*( p))) ) {
				case 0: {
					goto _st0;
				}
				case 9: {
					goto _ctr25;
				}
				case 10: {
					goto _st0;
				}
				case 32: {
					goto _ctr25;
				}
			}
			goto _ctr24;
		}
		_ctr24:
		{
#line 494 "crontab.rl"
			ncs.strv_st = p; ncs.v_strlen = 0; }
		
#line 2147 "crontab.cpp"
		
		goto _st199;
		_ctr261:
		{
#line 495 "crontab.rl"
			
			ncs.v_strlen = p > ncs.strv_st ? static_cast<size_t>(p - ncs.strv_st) : 0;
			if (ncs.v_strlen <= INT_MAX) {
				ssize_t snl = snprintf(ncs.v_str, sizeof ncs.v_str,
				"%.*s", (int)ncs.v_strlen, ncs.strv_st);
				if (snl < 0 || (size_t)snl >= sizeof ncs.v_str) {
					log_line("error parsing line %zu in crontab; too long?", ncs.linenum);
					std::exit(EXIT_FAILURE);
				}
			}
		}
		
#line 2165 "crontab.cpp"
		
		{
#line 587 "crontab.rl"
			
			ncs.ce->chroot = std::string(ncs.v_str, ncs.v_strlen);
		}
		
#line 2173 "crontab.cpp"
		
		goto _st199;
		_st199:
		if ( p == eof )
			goto _out199;
		p+= 1;
		st_case_199:
		if ( p == pe && p != eof )
			goto _out199;
		if ( p == eof ) {
			goto _ctr261;}
		else {
			switch( ( (*( p))) ) {
				case 0: {
					goto _st0;
				}
				case 10: {
					goto _st0;
				}
			}
			goto _st199;
		}
		_ctr25:
		{
#line 494 "crontab.rl"
			ncs.strv_st = p; ncs.v_strlen = 0; }
		
#line 2201 "crontab.cpp"
		
		goto _st200;
		_ctr263:
		{
#line 495 "crontab.rl"
			
			ncs.v_strlen = p > ncs.strv_st ? static_cast<size_t>(p - ncs.strv_st) : 0;
			if (ncs.v_strlen <= INT_MAX) {
				ssize_t snl = snprintf(ncs.v_str, sizeof ncs.v_str,
				"%.*s", (int)ncs.v_strlen, ncs.strv_st);
				if (snl < 0 || (size_t)snl >= sizeof ncs.v_str) {
					log_line("error parsing line %zu in crontab; too long?", ncs.linenum);
					std::exit(EXIT_FAILURE);
				}
			}
		}
		
#line 2219 "crontab.cpp"
		
		{
#line 587 "crontab.rl"
			
			ncs.ce->chroot = std::string(ncs.v_str, ncs.v_strlen);
		}
		
#line 2227 "crontab.cpp"
		
		goto _st200;
		_st200:
		if ( p == eof )
			goto _out200;
		p+= 1;
		st_case_200:
		if ( p == pe && p != eof )
			goto _out200;
		if ( p == eof ) {
			goto _ctr263;}
		else {
			switch( ( (*( p))) ) {
				case 0: {
					goto _st0;
				}
				case 9: {
					goto _ctr25;
				}
				case 10: {
					goto _st0;
				}
				case 32: {
					goto _ctr25;
				}
			}
			goto _ctr24;
		}
		_st10:
		if ( p == eof )
			goto _out10;
		p+= 1;
		st_case_10:
		if ( p == pe && p != eof )
			goto _out10;
		if ( p == eof ) {
			goto _st10;}
		else {
			switch( ( (*( p))) ) {
				case 77: {
					goto _st11;
				}
				case 109: {
					goto _st11;
				}
			}
			goto _st0;
		}
		_st11:
		if ( p == eof )
			goto _out11;
		p+= 1;
		st_case_11:
		if ( p == pe && p != eof )
			goto _out11;
		if ( p == eof ) {
			goto _st11;}
		else {
			switch( ( (*( p))) ) {
				case 77: {
					goto _st12;
				}
				case 109: {
					goto _st12;
				}
			}
			goto _st0;
		}
		_st12:
		if ( p == eof )
			goto _out12;
		p+= 1;
		st_case_12:
		if ( p == pe && p != eof )
			goto _out12;
		if ( p == eof ) {
			goto _st12;}
		else {
			switch( ( (*( p))) ) {
				case 65: {
					goto _st13;
				}
				case 97: {
					goto _st13;
				}
			}
			goto _st0;
		}
		_st13:
		if ( p == eof )
			goto _out13;
		p+= 1;
		st_case_13:
		if ( p == pe && p != eof )
			goto _out13;
		if ( p == eof ) {
			goto _st13;}
		else {
			switch( ( (*( p))) ) {
				case 78: {
					goto _st14;
				}
				case 110: {
					goto _st14;
				}
			}
			goto _st0;
		}
		_st14:
		if ( p == eof )
			goto _out14;
		p+= 1;
		st_case_14:
		if ( p == pe && p != eof )
			goto _out14;
		if ( p == eof ) {
			goto _st14;}
		else {
			switch( ( (*( p))) ) {
				case 68: {
					goto _st15;
				}
				case 100: {
					goto _st15;
				}
			}
			goto _st0;
		}
		_st15:
		if ( p == eof )
			goto _out15;
		p+= 1;
		st_case_15:
		if ( p == pe && p != eof )
			goto _out15;
		if ( p == eof ) {
			goto _st15;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st15;
				}
				case 32: {
					goto _st15;
				}
				case 61: {
					goto _st16;
				}
			}
			goto _st0;
		}
		_st16:
		if ( p == eof )
			goto _out16;
		p+= 1;
		st_case_16:
		if ( p == pe && p != eof )
			goto _out16;
		if ( p == eof ) {
			goto _st16;}
		else {
			switch( ( (*( p))) ) {
				case 0: {
					goto _st0;
				}
				case 9: {
					goto _ctr33;
				}
				case 10: {
					goto _st0;
				}
				case 32: {
					goto _ctr33;
				}
			}
			goto _ctr32;
		}
		_ctr32:
		{
#line 494 "crontab.rl"
			ncs.strv_st = p; ncs.v_strlen = 0; }
		
#line 2410 "crontab.cpp"
		
		goto _st201;
		_ctr264:
		{
#line 495 "crontab.rl"
			
			ncs.v_strlen = p > ncs.strv_st ? static_cast<size_t>(p - ncs.strv_st) : 0;
			if (ncs.v_strlen <= INT_MAX) {
				ssize_t snl = snprintf(ncs.v_str, sizeof ncs.v_str,
				"%.*s", (int)ncs.v_strlen, ncs.strv_st);
				if (snl < 0 || (size_t)snl >= sizeof ncs.v_str) {
					log_line("error parsing line %zu in crontab; too long?", ncs.linenum);
					std::exit(EXIT_FAILURE);
				}
			}
		}
		
#line 2428 "crontab.cpp"
		
		{
#line 590 "crontab.rl"
			parse_command_key(ncs); }
		
#line 2434 "crontab.cpp"
		
		goto _st201;
		_st201:
		if ( p == eof )
			goto _out201;
		p+= 1;
		st_case_201:
		if ( p == pe && p != eof )
			goto _out201;
		if ( p == eof ) {
			goto _ctr264;}
		else {
			switch( ( (*( p))) ) {
				case 0: {
					goto _st0;
				}
				case 10: {
					goto _st0;
				}
			}
			goto _st201;
		}
		_ctr33:
		{
#line 494 "crontab.rl"
			ncs.strv_st = p; ncs.v_strlen = 0; }
		
#line 2462 "crontab.cpp"
		
		goto _st202;
		_ctr266:
		{
#line 495 "crontab.rl"
			
			ncs.v_strlen = p > ncs.strv_st ? static_cast<size_t>(p - ncs.strv_st) : 0;
			if (ncs.v_strlen <= INT_MAX) {
				ssize_t snl = snprintf(ncs.v_str, sizeof ncs.v_str,
				"%.*s", (int)ncs.v_strlen, ncs.strv_st);
				if (snl < 0 || (size_t)snl >= sizeof ncs.v_str) {
					log_line("error parsing line %zu in crontab; too long?", ncs.linenum);
					std::exit(EXIT_FAILURE);
				}
			}
		}
		
#line 2480 "crontab.cpp"
		
		{
#line 590 "crontab.rl"
			parse_command_key(ncs); }
		
#line 2486 "crontab.cpp"
		
		goto _st202;
		_st202:
		if ( p == eof )
			goto _out202;
		p+= 1;
		st_case_202:
		if ( p == pe && p != eof )
			goto _out202;
		if ( p == eof ) {
			goto _ctr266;}
		else {
			switch( ( (*( p))) ) {
				case 0: {
					goto _st0;
				}
				case 9: {
					goto _ctr33;
				}
				case 10: {
					goto _st0;
				}
				case 32: {
					goto _ctr33;
				}
			}
			goto _ctr32;
		}
		_st17:
		if ( p == eof )
			goto _out17;
		p+= 1;
		st_case_17:
		if ( p == pe && p != eof )
			goto _out17;
		if ( p == eof ) {
			goto _st17;}
		else {
			switch( ( (*( p))) ) {
				case 65: {
					goto _st18;
				}
				case 97: {
					goto _st18;
				}
			}
			goto _st0;
		}
		_st18:
		if ( p == eof )
			goto _out18;
		p+= 1;
		st_case_18:
		if ( p == pe && p != eof )
			goto _out18;
		if ( p == eof ) {
			goto _st18;}
		else {
			switch( ( (*( p))) ) {
				case 89: {
					goto _st19;
				}
				case 121: {
					goto _st19;
				}
			}
			goto _st0;
		}
		_st19:
		if ( p == eof )
			goto _out19;
		p+= 1;
		st_case_19:
		if ( p == pe && p != eof )
			goto _out19;
		if ( p == eof ) {
			goto _st19;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st19;
				}
				case 32: {
					goto _st19;
				}
				case 61: {
					goto _st20;
				}
			}
			goto _st0;
		}
		_st20:
		if ( p == eof )
			goto _out20;
		p+= 1;
		st_case_20:
		if ( p == pe && p != eof )
			goto _out20;
		if ( p == eof ) {
			goto _st20;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st20;
				}
				case 32: {
					goto _st20;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr37;
			}
			goto _st0;
		}
		_ctr37:
		{
#line 474 "crontab.rl"
			
			ncs.intv_st = p;
			ncs.v_int = ncs.v_int2 = 0;
			ncs.intv2_exist = false;
		}
		
#line 2610 "crontab.cpp"
		
		goto _st203;
		_ctr267:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 203; goto _out;}
			}
		}
		
#line 2623 "crontab.cpp"
		
		{
#line 574 "crontab.rl"
			addcstlist(ncs, ncs.ce->day, 0, 1, 31); }
		
#line 2629 "crontab.cpp"
		
		goto _st203;
		_st203:
		if ( p == eof )
			goto _out203;
		p+= 1;
		st_case_203:
		if ( p == pe && p != eof )
			goto _out203;
		if ( p == eof ) {
			goto _ctr267;}
		else {
			if ( ( (*( p))) == 44 ) {
				goto _ctr268;
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st203;
			}
			goto _st0;
		}
		_ctr268:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 21; goto _out;}
			}
		}
		
#line 2660 "crontab.cpp"
		
		goto _st21;
		_st21:
		if ( p == eof )
			goto _out21;
		p+= 1;
		st_case_21:
		if ( p == pe && p != eof )
			goto _out21;
		if ( p == eof ) {
			goto _st21;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr39;
			}
			goto _st0;
		}
		_ctr39:
		{
#line 485 "crontab.rl"
			ncs.intv2_st = p; }
		
#line 2683 "crontab.cpp"
		
		goto _st204;
		_ctr270:
		{
#line 486 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV2())) ncs.v_int2 = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 204; goto _out;}
			}
			ncs.intv2_exist = true;
		}
		
#line 2697 "crontab.cpp"
		
		{
#line 574 "crontab.rl"
			addcstlist(ncs, ncs.ce->day, 0, 1, 31); }
		
#line 2703 "crontab.cpp"
		
		goto _st204;
		_st204:
		if ( p == eof )
			goto _out204;
		p+= 1;
		st_case_204:
		if ( p == pe && p != eof )
			goto _out204;
		if ( p == eof ) {
			goto _ctr270;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st204;
			}
			goto _st0;
		}
		_st22:
		if ( p == eof )
			goto _out22;
		p+= 1;
		st_case_22:
		if ( p == pe && p != eof )
			goto _out22;
		if ( p == eof ) {
			goto _st22;}
		else {
			switch( ( (*( p))) ) {
				case 82: {
					goto _st23;
				}
				case 114: {
					goto _st23;
				}
			}
			goto _st0;
		}
		_st23:
		if ( p == eof )
			goto _out23;
		p+= 1;
		st_case_23:
		if ( p == pe && p != eof )
			goto _out23;
		if ( p == eof ) {
			goto _st23;}
		else {
			switch( ( (*( p))) ) {
				case 79: {
					goto _st24;
				}
				case 111: {
					goto _st24;
				}
			}
			goto _st0;
		}
		_st24:
		if ( p == eof )
			goto _out24;
		p+= 1;
		st_case_24:
		if ( p == pe && p != eof )
			goto _out24;
		if ( p == eof ) {
			goto _st24;}
		else {
			switch( ( (*( p))) ) {
				case 85: {
					goto _st25;
				}
				case 117: {
					goto _st25;
				}
			}
			goto _st0;
		}
		_st25:
		if ( p == eof )
			goto _out25;
		p+= 1;
		st_case_25:
		if ( p == pe && p != eof )
			goto _out25;
		if ( p == eof ) {
			goto _st25;}
		else {
			switch( ( (*( p))) ) {
				case 80: {
					goto _st26;
				}
				case 112: {
					goto _st26;
				}
			}
			goto _st0;
		}
		_st26:
		if ( p == eof )
			goto _out26;
		p+= 1;
		st_case_26:
		if ( p == pe && p != eof )
			goto _out26;
		if ( p == eof ) {
			goto _st26;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st26;
				}
				case 32: {
					goto _st26;
				}
				case 61: {
					goto _st27;
				}
			}
			goto _st0;
		}
		_st27:
		if ( p == eof )
			goto _out27;
		p+= 1;
		st_case_27:
		if ( p == pe && p != eof )
			goto _out27;
		if ( p == eof ) {
			goto _st27;}
		else {
			switch( ( (*( p))) ) {
				case 0: {
					goto _st0;
				}
				case 9: {
					goto _ctr46;
				}
				case 10: {
					goto _st0;
				}
				case 32: {
					goto _ctr46;
				}
			}
			goto _ctr45;
		}
		_ctr45:
		{
#line 494 "crontab.rl"
			ncs.strv_st = p; ncs.v_strlen = 0; }
		
#line 2855 "crontab.cpp"
		
		goto _st205;
		_ctr272:
		{
#line 495 "crontab.rl"
			
			ncs.v_strlen = p > ncs.strv_st ? static_cast<size_t>(p - ncs.strv_st) : 0;
			if (ncs.v_strlen <= INT_MAX) {
				ssize_t snl = snprintf(ncs.v_str, sizeof ncs.v_str,
				"%.*s", (int)ncs.v_strlen, ncs.strv_st);
				if (snl < 0 || (size_t)snl >= sizeof ncs.v_str) {
					log_line("error parsing line %zu in crontab; too long?", ncs.linenum);
					std::exit(EXIT_FAILURE);
				}
			}
		}
		
#line 2873 "crontab.cpp"
		
		{
#line 585 "crontab.rl"
			ncs.setgroupv(); }
		
#line 2879 "crontab.cpp"
		
		goto _st205;
		_st205:
		if ( p == eof )
			goto _out205;
		p+= 1;
		st_case_205:
		if ( p == pe && p != eof )
			goto _out205;
		if ( p == eof ) {
			goto _ctr272;}
		else {
			switch( ( (*( p))) ) {
				case 0: {
					goto _st0;
				}
				case 10: {
					goto _st0;
				}
			}
			goto _st205;
		}
		_ctr46:
		{
#line 494 "crontab.rl"
			ncs.strv_st = p; ncs.v_strlen = 0; }
		
#line 2907 "crontab.cpp"
		
		goto _st206;
		_ctr274:
		{
#line 495 "crontab.rl"
			
			ncs.v_strlen = p > ncs.strv_st ? static_cast<size_t>(p - ncs.strv_st) : 0;
			if (ncs.v_strlen <= INT_MAX) {
				ssize_t snl = snprintf(ncs.v_str, sizeof ncs.v_str,
				"%.*s", (int)ncs.v_strlen, ncs.strv_st);
				if (snl < 0 || (size_t)snl >= sizeof ncs.v_str) {
					log_line("error parsing line %zu in crontab; too long?", ncs.linenum);
					std::exit(EXIT_FAILURE);
				}
			}
		}
		
#line 2925 "crontab.cpp"
		
		{
#line 585 "crontab.rl"
			ncs.setgroupv(); }
		
#line 2931 "crontab.cpp"
		
		goto _st206;
		_st206:
		if ( p == eof )
			goto _out206;
		p+= 1;
		st_case_206:
		if ( p == pe && p != eof )
			goto _out206;
		if ( p == eof ) {
			goto _ctr274;}
		else {
			switch( ( (*( p))) ) {
				case 0: {
					goto _st0;
				}
				case 9: {
					goto _ctr46;
				}
				case 10: {
					goto _st0;
				}
				case 32: {
					goto _ctr46;
				}
			}
			goto _ctr45;
		}
		_st28:
		if ( p == eof )
			goto _out28;
		p+= 1;
		st_case_28:
		if ( p == pe && p != eof )
			goto _out28;
		if ( p == eof ) {
			goto _st28;}
		else {
			switch( ( (*( p))) ) {
				case 79: {
					goto _st29;
				}
				case 111: {
					goto _st29;
				}
			}
			goto _st0;
		}
		_st29:
		if ( p == eof )
			goto _out29;
		p+= 1;
		st_case_29:
		if ( p == pe && p != eof )
			goto _out29;
		if ( p == eof ) {
			goto _st29;}
		else {
			switch( ( (*( p))) ) {
				case 85: {
					goto _st30;
				}
				case 117: {
					goto _st30;
				}
			}
			goto _st0;
		}
		_st30:
		if ( p == eof )
			goto _out30;
		p+= 1;
		st_case_30:
		if ( p == pe && p != eof )
			goto _out30;
		if ( p == eof ) {
			goto _st30;}
		else {
			switch( ( (*( p))) ) {
				case 82: {
					goto _st31;
				}
				case 114: {
					goto _st31;
				}
			}
			goto _st0;
		}
		_st31:
		if ( p == eof )
			goto _out31;
		p+= 1;
		st_case_31:
		if ( p == pe && p != eof )
			goto _out31;
		if ( p == eof ) {
			goto _st31;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st31;
				}
				case 32: {
					goto _st31;
				}
				case 61: {
					goto _st32;
				}
			}
			goto _st0;
		}
		_st32:
		if ( p == eof )
			goto _out32;
		p+= 1;
		st_case_32:
		if ( p == pe && p != eof )
			goto _out32;
		if ( p == eof ) {
			goto _st32;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st32;
				}
				case 32: {
					goto _st32;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr51;
			}
			goto _st0;
		}
		_ctr51:
		{
#line 474 "crontab.rl"
			
			ncs.intv_st = p;
			ncs.v_int = ncs.v_int2 = 0;
			ncs.intv2_exist = false;
		}
		
#line 3075 "crontab.cpp"
		
		goto _st207;
		_ctr275:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 207; goto _out;}
			}
		}
		
#line 3088 "crontab.cpp"
		
		{
#line 576 "crontab.rl"
			addcstlist(ncs, ncs.ce->hour, 24, 0, 23); }
		
#line 3094 "crontab.cpp"
		
		goto _st207;
		_st207:
		if ( p == eof )
			goto _out207;
		p+= 1;
		st_case_207:
		if ( p == pe && p != eof )
			goto _out207;
		if ( p == eof ) {
			goto _ctr275;}
		else {
			if ( ( (*( p))) == 44 ) {
				goto _ctr276;
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st207;
			}
			goto _st0;
		}
		_ctr276:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 33; goto _out;}
			}
		}
		
#line 3125 "crontab.cpp"
		
		goto _st33;
		_st33:
		if ( p == eof )
			goto _out33;
		p+= 1;
		st_case_33:
		if ( p == pe && p != eof )
			goto _out33;
		if ( p == eof ) {
			goto _st33;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr53;
			}
			goto _st0;
		}
		_ctr53:
		{
#line 485 "crontab.rl"
			ncs.intv2_st = p; }
		
#line 3148 "crontab.cpp"
		
		goto _st208;
		_ctr278:
		{
#line 486 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV2())) ncs.v_int2 = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 208; goto _out;}
			}
			ncs.intv2_exist = true;
		}
		
#line 3162 "crontab.cpp"
		
		{
#line 576 "crontab.rl"
			addcstlist(ncs, ncs.ce->hour, 24, 0, 23); }
		
#line 3168 "crontab.cpp"
		
		goto _st208;
		_st208:
		if ( p == eof )
			goto _out208;
		p+= 1;
		st_case_208:
		if ( p == pe && p != eof )
			goto _out208;
		if ( p == eof ) {
			goto _ctr278;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st208;
			}
			goto _st0;
		}
		_st34:
		if ( p == eof )
			goto _out34;
		p+= 1;
		st_case_34:
		if ( p == pe && p != eof )
			goto _out34;
		if ( p == eof ) {
			goto _st34;}
		else {
			switch( ( (*( p))) ) {
				case 78: {
					goto _st35;
				}
				case 110: {
					goto _st35;
				}
			}
			goto _st0;
		}
		_st35:
		if ( p == eof )
			goto _out35;
		p+= 1;
		st_case_35:
		if ( p == pe && p != eof )
			goto _out35;
		if ( p == eof ) {
			goto _st35;}
		else {
			switch( ( (*( p))) ) {
				case 84: {
					goto _st36;
				}
				case 116: {
					goto _st36;
				}
			}
			goto _st0;
		}
		_st36:
		if ( p == eof )
			goto _out36;
		p+= 1;
		st_case_36:
		if ( p == pe && p != eof )
			goto _out36;
		if ( p == eof ) {
			goto _st36;}
		else {
			switch( ( (*( p))) ) {
				case 69: {
					goto _st37;
				}
				case 101: {
					goto _st37;
				}
			}
			goto _st0;
		}
		_st37:
		if ( p == eof )
			goto _out37;
		p+= 1;
		st_case_37:
		if ( p == pe && p != eof )
			goto _out37;
		if ( p == eof ) {
			goto _st37;}
		else {
			switch( ( (*( p))) ) {
				case 82: {
					goto _st38;
				}
				case 114: {
					goto _st38;
				}
			}
			goto _st0;
		}
		_st38:
		if ( p == eof )
			goto _out38;
		p+= 1;
		st_case_38:
		if ( p == pe && p != eof )
			goto _out38;
		if ( p == eof ) {
			goto _st38;}
		else {
			switch( ( (*( p))) ) {
				case 86: {
					goto _st39;
				}
				case 118: {
					goto _st39;
				}
			}
			goto _st0;
		}
		_st39:
		if ( p == eof )
			goto _out39;
		p+= 1;
		st_case_39:
		if ( p == pe && p != eof )
			goto _out39;
		if ( p == eof ) {
			goto _st39;}
		else {
			switch( ( (*( p))) ) {
				case 65: {
					goto _st40;
				}
				case 97: {
					goto _st40;
				}
			}
			goto _st0;
		}
		_st40:
		if ( p == eof )
			goto _out40;
		p+= 1;
		st_case_40:
		if ( p == pe && p != eof )
			goto _out40;
		if ( p == eof ) {
			goto _st40;}
		else {
			switch( ( (*( p))) ) {
				case 76: {
					goto _st41;
				}
				case 108: {
					goto _st41;
				}
			}
			goto _st0;
		}
		_st41:
		if ( p == eof )
			goto _out41;
		p+= 1;
		st_case_41:
		if ( p == pe && p != eof )
			goto _out41;
		if ( p == eof ) {
			goto _st41;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st41;
				}
				case 32: {
					goto _st41;
				}
				case 61: {
					goto _st42;
				}
			}
			goto _st0;
		}
		_ctr281:
		{
#line 461 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_TIME())) ncs.v_time += 86400 * *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 42; goto _out;}
			}
		}
		
#line 3359 "crontab.cpp"
		
		goto _st42;
		_ctr284:
		{
#line 455 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_TIME())) ncs.v_time += 3600 * *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 42; goto _out;}
			}
		}
		
#line 3372 "crontab.cpp"
		
		goto _st42;
		_ctr287:
		{
#line 449 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_TIME())) ncs.v_time += 60 * *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 42; goto _out;}
			}
		}
		
#line 3385 "crontab.cpp"
		
		goto _st42;
		_ctr290:
		{
#line 443 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_TIME())) ncs.v_time += *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 42; goto _out;}
			}
		}
		
#line 3398 "crontab.cpp"
		
		goto _st42;
		_ctr293:
		{
#line 467 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_TIME())) ncs.v_time += 604800 * *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 42; goto _out;}
			}
		}
		
#line 3411 "crontab.cpp"
		
		goto _st42;
		_st42:
		if ( p == eof )
			goto _out42;
		p+= 1;
		st_case_42:
		if ( p == pe && p != eof )
			goto _out42;
		if ( p == eof ) {
			goto _st42;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st42;
				}
				case 32: {
					goto _st42;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr62;
			}
			goto _st0;
		}
		_ctr62:
		{
#line 442 "crontab.rl"
			ncs.time_st = p; ncs.v_time = 0; }
		
#line 3442 "crontab.cpp"
		
		goto _st43;
		_ctr282:
		{
#line 461 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_TIME())) ncs.v_time += 86400 * *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 43; goto _out;}
			}
		}
		
#line 3455 "crontab.cpp"
		
		{
#line 442 "crontab.rl"
			ncs.time_st = p; ncs.v_time = 0; }
		
#line 3461 "crontab.cpp"
		
		goto _st43;
		_ctr285:
		{
#line 455 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_TIME())) ncs.v_time += 3600 * *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 43; goto _out;}
			}
		}
		
#line 3474 "crontab.cpp"
		
		{
#line 442 "crontab.rl"
			ncs.time_st = p; ncs.v_time = 0; }
		
#line 3480 "crontab.cpp"
		
		goto _st43;
		_ctr288:
		{
#line 449 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_TIME())) ncs.v_time += 60 * *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 43; goto _out;}
			}
		}
		
#line 3493 "crontab.cpp"
		
		{
#line 442 "crontab.rl"
			ncs.time_st = p; ncs.v_time = 0; }
		
#line 3499 "crontab.cpp"
		
		goto _st43;
		_ctr291:
		{
#line 443 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_TIME())) ncs.v_time += *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 43; goto _out;}
			}
		}
		
#line 3512 "crontab.cpp"
		
		{
#line 442 "crontab.rl"
			ncs.time_st = p; ncs.v_time = 0; }
		
#line 3518 "crontab.cpp"
		
		goto _st43;
		_ctr294:
		{
#line 467 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_TIME())) ncs.v_time += 604800 * *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 43; goto _out;}
			}
		}
		
#line 3531 "crontab.cpp"
		
		{
#line 442 "crontab.rl"
			ncs.time_st = p; ncs.v_time = 0; }
		
#line 3537 "crontab.cpp"
		
		goto _st43;
		_st43:
		if ( p == eof )
			goto _out43;
		p+= 1;
		st_case_43:
		if ( p == pe && p != eof )
			goto _out43;
		if ( p == eof ) {
			goto _st43;}
		else {
			switch( ( (*( p))) ) {
				case 100: {
					goto _st209;
				}
				case 104: {
					goto _st210;
				}
				case 109: {
					goto _st211;
				}
				case 115: {
					goto _st212;
				}
				case 119: {
					goto _st213;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st43;
			}
			goto _st0;
		}
		_ctr280:
		{
#line 461 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_TIME())) ncs.v_time += 86400 * *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 209; goto _out;}
			}
		}
		
#line 3582 "crontab.cpp"
		
		{
#line 569 "crontab.rl"
			ncs.ce->interval = ncs.v_time; }
		
#line 3588 "crontab.cpp"
		
		goto _st209;
		_st209:
		if ( p == eof )
			goto _out209;
		p+= 1;
		st_case_209:
		if ( p == pe && p != eof )
			goto _out209;
		if ( p == eof ) {
			goto _ctr280;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _ctr281;
				}
				case 32: {
					goto _ctr281;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr282;
			}
			goto _st0;
		}
		_ctr283:
		{
#line 455 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_TIME())) ncs.v_time += 3600 * *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 210; goto _out;}
			}
		}
		
#line 3624 "crontab.cpp"
		
		{
#line 569 "crontab.rl"
			ncs.ce->interval = ncs.v_time; }
		
#line 3630 "crontab.cpp"
		
		goto _st210;
		_st210:
		if ( p == eof )
			goto _out210;
		p+= 1;
		st_case_210:
		if ( p == pe && p != eof )
			goto _out210;
		if ( p == eof ) {
			goto _ctr283;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _ctr284;
				}
				case 32: {
					goto _ctr284;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr285;
			}
			goto _st0;
		}
		_ctr286:
		{
#line 449 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_TIME())) ncs.v_time += 60 * *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 211; goto _out;}
			}
		}
		
#line 3666 "crontab.cpp"
		
		{
#line 569 "crontab.rl"
			ncs.ce->interval = ncs.v_time; }
		
#line 3672 "crontab.cpp"
		
		goto _st211;
		_st211:
		if ( p == eof )
			goto _out211;
		p+= 1;
		st_case_211:
		if ( p == pe && p != eof )
			goto _out211;
		if ( p == eof ) {
			goto _ctr286;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _ctr287;
				}
				case 32: {
					goto _ctr287;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr288;
			}
			goto _st0;
		}
		_ctr289:
		{
#line 443 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_TIME())) ncs.v_time += *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 212; goto _out;}
			}
		}
		
#line 3708 "crontab.cpp"
		
		{
#line 569 "crontab.rl"
			ncs.ce->interval = ncs.v_time; }
		
#line 3714 "crontab.cpp"
		
		goto _st212;
		_st212:
		if ( p == eof )
			goto _out212;
		p+= 1;
		st_case_212:
		if ( p == pe && p != eof )
			goto _out212;
		if ( p == eof ) {
			goto _ctr289;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _ctr290;
				}
				case 32: {
					goto _ctr290;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr291;
			}
			goto _st0;
		}
		_ctr292:
		{
#line 467 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_TIME())) ncs.v_time += 604800 * *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 213; goto _out;}
			}
		}
		
#line 3750 "crontab.cpp"
		
		{
#line 569 "crontab.rl"
			ncs.ce->interval = ncs.v_time; }
		
#line 3756 "crontab.cpp"
		
		goto _st213;
		_st213:
		if ( p == eof )
			goto _out213;
		p+= 1;
		st_case_213:
		if ( p == pe && p != eof )
			goto _out213;
		if ( p == eof ) {
			goto _ctr292;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _ctr293;
				}
				case 32: {
					goto _ctr293;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr294;
			}
			goto _st0;
		}
		_st44:
		if ( p == eof )
			goto _out44;
		p+= 1;
		st_case_44:
		if ( p == pe && p != eof )
			goto _out44;
		if ( p == eof ) {
			goto _st44;}
		else {
			switch( ( (*( p))) ) {
				case 79: {
					goto _st45;
				}
				case 111: {
					goto _st45;
				}
			}
			goto _st0;
		}
		_st45:
		if ( p == eof )
			goto _out45;
		p+= 1;
		st_case_45:
		if ( p == pe && p != eof )
			goto _out45;
		if ( p == eof ) {
			goto _st45;}
		else {
			switch( ( (*( p))) ) {
				case 85: {
					goto _st46;
				}
				case 117: {
					goto _st46;
				}
			}
			goto _st0;
		}
		_st46:
		if ( p == eof )
			goto _out46;
		p+= 1;
		st_case_46:
		if ( p == pe && p != eof )
			goto _out46;
		if ( p == eof ) {
			goto _st46;}
		else {
			switch( ( (*( p))) ) {
				case 82: {
					goto _st47;
				}
				case 114: {
					goto _st47;
				}
			}
			goto _st0;
		}
		_st47:
		if ( p == eof )
			goto _out47;
		p+= 1;
		st_case_47:
		if ( p == pe && p != eof )
			goto _out47;
		if ( p == eof ) {
			goto _st47;}
		else {
			switch( ( (*( p))) ) {
				case 78: {
					goto _st48;
				}
				case 110: {
					goto _st48;
				}
			}
			goto _st0;
		}
		_st48:
		if ( p == eof )
			goto _out48;
		p+= 1;
		st_case_48:
		if ( p == pe && p != eof )
			goto _out48;
		if ( p == eof ) {
			goto _st48;}
		else {
			switch( ( (*( p))) ) {
				case 65: {
					goto _st49;
				}
				case 97: {
					goto _st49;
				}
			}
			goto _st0;
		}
		_st49:
		if ( p == eof )
			goto _out49;
		p+= 1;
		st_case_49:
		if ( p == pe && p != eof )
			goto _out49;
		if ( p == eof ) {
			goto _st49;}
		else {
			switch( ( (*( p))) ) {
				case 76: {
					goto _st214;
				}
				case 108: {
					goto _st214;
				}
			}
			goto _st0;
		}
		_ctr295:
		{
#line 520 "crontab.rl"
			ncs.ce->journal = true; }
		
#line 3907 "crontab.cpp"
		
		goto _st214;
		_st214:
		if ( p == eof )
			goto _out214;
		p+= 1;
		st_case_214:
		if ( p == pe && p != eof )
			goto _out214;
		if ( p == eof ) {
			goto _ctr295;}
		else {
			goto _st0;
		}
		_st50:
		if ( p == eof )
			goto _out50;
		p+= 1;
		st_case_50:
		if ( p == pe && p != eof )
			goto _out50;
		if ( p == eof ) {
			goto _st50;}
		else {
			if ( ( (*( p))) == 95 ) {
				goto _st51;
			}
			goto _st0;
		}
		_st51:
		if ( p == eof )
			goto _out51;
		p+= 1;
		st_case_51:
		if ( p == pe && p != eof )
			goto _out51;
		if ( p == eof ) {
			goto _st51;}
		else {
			switch( ( (*( p))) ) {
				case 65: {
					goto _st52;
				}
				case 67: {
					goto _st56;
				}
				case 68: {
					goto _st66;
				}
				case 70: {
					goto _st72;
				}
				case 77: {
					goto _st79;
				}
				case 78: {
					goto _st97;
				}
				case 82: {
					goto _st116;
				}
				case 83: {
					goto _st134;
				}
				case 97: {
					goto _st52;
				}
				case 99: {
					goto _st56;
				}
				case 100: {
					goto _st66;
				}
				case 102: {
					goto _st72;
				}
				case 109: {
					goto _st79;
				}
				case 110: {
					goto _st97;
				}
				case 114: {
					goto _st116;
				}
				case 115: {
					goto _st134;
				}
			}
			goto _st0;
		}
		_st52:
		if ( p == eof )
			goto _out52;
		p+= 1;
		st_case_52:
		if ( p == pe && p != eof )
			goto _out52;
		if ( p == eof ) {
			goto _st52;}
		else {
			switch( ( (*( p))) ) {
				case 83: {
					goto _st53;
				}
				case 115: {
					goto _st53;
				}
			}
			goto _st0;
		}
		_st53:
		if ( p == eof )
			goto _out53;
		p+= 1;
		st_case_53:
		if ( p == pe && p != eof )
			goto _out53;
		if ( p == eof ) {
			goto _st53;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st53;
				}
				case 32: {
					goto _st53;
				}
				case 61: {
					goto _st54;
				}
			}
			goto _st0;
		}
		_st54:
		if ( p == eof )
			goto _out54;
		p+= 1;
		st_case_54:
		if ( p == pe && p != eof )
			goto _out54;
		if ( p == eof ) {
			goto _st54;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st54;
				}
				case 32: {
					goto _st54;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr86;
			}
			goto _st0;
		}
		_ctr86:
		{
#line 474 "crontab.rl"
			
			ncs.intv_st = p;
			ncs.v_int = ncs.v_int2 = 0;
			ncs.intv2_exist = false;
		}
		
#line 4074 "crontab.cpp"
		
		goto _st215;
		_ctr296:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 215; goto _out;}
			}
		}
		
#line 4087 "crontab.cpp"
		
		{
#line 537 "crontab.rl"
			ncs.setlim(RLIMIT_AS); }
		
#line 4093 "crontab.cpp"
		
		goto _st215;
		_st215:
		if ( p == eof )
			goto _out215;
		p+= 1;
		st_case_215:
		if ( p == pe && p != eof )
			goto _out215;
		if ( p == eof ) {
			goto _ctr296;}
		else {
			if ( ( (*( p))) == 44 ) {
				goto _ctr297;
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st215;
			}
			goto _st0;
		}
		_ctr297:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 55; goto _out;}
			}
		}
		
#line 4124 "crontab.cpp"
		
		goto _st55;
		_st55:
		if ( p == eof )
			goto _out55;
		p+= 1;
		st_case_55:
		if ( p == pe && p != eof )
			goto _out55;
		if ( p == eof ) {
			goto _st55;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr88;
			}
			goto _st0;
		}
		_ctr88:
		{
#line 485 "crontab.rl"
			ncs.intv2_st = p; }
		
#line 4147 "crontab.cpp"
		
		goto _st216;
		_ctr299:
		{
#line 486 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV2())) ncs.v_int2 = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 216; goto _out;}
			}
			ncs.intv2_exist = true;
		}
		
#line 4161 "crontab.cpp"
		
		{
#line 537 "crontab.rl"
			ncs.setlim(RLIMIT_AS); }
		
#line 4167 "crontab.cpp"
		
		goto _st216;
		_st216:
		if ( p == eof )
			goto _out216;
		p+= 1;
		st_case_216:
		if ( p == pe && p != eof )
			goto _out216;
		if ( p == eof ) {
			goto _ctr299;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st216;
			}
			goto _st0;
		}
		_st56:
		if ( p == eof )
			goto _out56;
		p+= 1;
		st_case_56:
		if ( p == pe && p != eof )
			goto _out56;
		if ( p == eof ) {
			goto _st56;}
		else {
			switch( ( (*( p))) ) {
				case 79: {
					goto _st57;
				}
				case 80: {
					goto _st62;
				}
				case 111: {
					goto _st57;
				}
				case 112: {
					goto _st62;
				}
			}
			goto _st0;
		}
		_st57:
		if ( p == eof )
			goto _out57;
		p+= 1;
		st_case_57:
		if ( p == pe && p != eof )
			goto _out57;
		if ( p == eof ) {
			goto _st57;}
		else {
			switch( ( (*( p))) ) {
				case 82: {
					goto _st58;
				}
				case 114: {
					goto _st58;
				}
			}
			goto _st0;
		}
		_st58:
		if ( p == eof )
			goto _out58;
		p+= 1;
		st_case_58:
		if ( p == pe && p != eof )
			goto _out58;
		if ( p == eof ) {
			goto _st58;}
		else {
			switch( ( (*( p))) ) {
				case 69: {
					goto _st59;
				}
				case 101: {
					goto _st59;
				}
			}
			goto _st0;
		}
		_st59:
		if ( p == eof )
			goto _out59;
		p+= 1;
		st_case_59:
		if ( p == pe && p != eof )
			goto _out59;
		if ( p == eof ) {
			goto _st59;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st59;
				}
				case 32: {
					goto _st59;
				}
				case 61: {
					goto _st60;
				}
			}
			goto _st0;
		}
		_st60:
		if ( p == eof )
			goto _out60;
		p+= 1;
		st_case_60:
		if ( p == pe && p != eof )
			goto _out60;
		if ( p == eof ) {
			goto _st60;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st60;
				}
				case 32: {
					goto _st60;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr94;
			}
			goto _st0;
		}
		_ctr94:
		{
#line 474 "crontab.rl"
			
			ncs.intv_st = p;
			ncs.v_int = ncs.v_int2 = 0;
			ncs.intv2_exist = false;
		}
		
#line 4306 "crontab.cpp"
		
		goto _st217;
		_ctr301:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 217; goto _out;}
			}
		}
		
#line 4319 "crontab.cpp"
		
		{
#line 542 "crontab.rl"
			ncs.setlim(RLIMIT_CORE); }
		
#line 4325 "crontab.cpp"
		
		goto _st217;
		_st217:
		if ( p == eof )
			goto _out217;
		p+= 1;
		st_case_217:
		if ( p == pe && p != eof )
			goto _out217;
		if ( p == eof ) {
			goto _ctr301;}
		else {
			if ( ( (*( p))) == 44 ) {
				goto _ctr302;
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st217;
			}
			goto _st0;
		}
		_ctr302:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 61; goto _out;}
			}
		}
		
#line 4356 "crontab.cpp"
		
		goto _st61;
		_st61:
		if ( p == eof )
			goto _out61;
		p+= 1;
		st_case_61:
		if ( p == pe && p != eof )
			goto _out61;
		if ( p == eof ) {
			goto _st61;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr96;
			}
			goto _st0;
		}
		_ctr96:
		{
#line 485 "crontab.rl"
			ncs.intv2_st = p; }
		
#line 4379 "crontab.cpp"
		
		goto _st218;
		_ctr304:
		{
#line 486 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV2())) ncs.v_int2 = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 218; goto _out;}
			}
			ncs.intv2_exist = true;
		}
		
#line 4393 "crontab.cpp"
		
		{
#line 542 "crontab.rl"
			ncs.setlim(RLIMIT_CORE); }
		
#line 4399 "crontab.cpp"
		
		goto _st218;
		_st218:
		if ( p == eof )
			goto _out218;
		p+= 1;
		st_case_218:
		if ( p == pe && p != eof )
			goto _out218;
		if ( p == eof ) {
			goto _ctr304;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st218;
			}
			goto _st0;
		}
		_st62:
		if ( p == eof )
			goto _out62;
		p+= 1;
		st_case_62:
		if ( p == pe && p != eof )
			goto _out62;
		if ( p == eof ) {
			goto _st62;}
		else {
			switch( ( (*( p))) ) {
				case 85: {
					goto _st63;
				}
				case 117: {
					goto _st63;
				}
			}
			goto _st0;
		}
		_st63:
		if ( p == eof )
			goto _out63;
		p+= 1;
		st_case_63:
		if ( p == pe && p != eof )
			goto _out63;
		if ( p == eof ) {
			goto _st63;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st63;
				}
				case 32: {
					goto _st63;
				}
				case 61: {
					goto _st64;
				}
			}
			goto _st0;
		}
		_st64:
		if ( p == eof )
			goto _out64;
		p+= 1;
		st_case_64:
		if ( p == pe && p != eof )
			goto _out64;
		if ( p == eof ) {
			goto _st64;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st64;
				}
				case 32: {
					goto _st64;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr99;
			}
			goto _st0;
		}
		_ctr99:
		{
#line 474 "crontab.rl"
			
			ncs.intv_st = p;
			ncs.v_int = ncs.v_int2 = 0;
			ncs.intv2_exist = false;
		}
		
#line 4492 "crontab.cpp"
		
		goto _st219;
		_ctr306:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 219; goto _out;}
			}
		}
		
#line 4505 "crontab.cpp"
		
		{
#line 546 "crontab.rl"
			ncs.setlim(RLIMIT_CPU); }
		
#line 4511 "crontab.cpp"
		
		goto _st219;
		_st219:
		if ( p == eof )
			goto _out219;
		p+= 1;
		st_case_219:
		if ( p == pe && p != eof )
			goto _out219;
		if ( p == eof ) {
			goto _ctr306;}
		else {
			if ( ( (*( p))) == 44 ) {
				goto _ctr307;
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st219;
			}
			goto _st0;
		}
		_ctr307:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 65; goto _out;}
			}
		}
		
#line 4542 "crontab.cpp"
		
		goto _st65;
		_st65:
		if ( p == eof )
			goto _out65;
		p+= 1;
		st_case_65:
		if ( p == pe && p != eof )
			goto _out65;
		if ( p == eof ) {
			goto _st65;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr101;
			}
			goto _st0;
		}
		_ctr101:
		{
#line 485 "crontab.rl"
			ncs.intv2_st = p; }
		
#line 4565 "crontab.cpp"
		
		goto _st220;
		_ctr309:
		{
#line 486 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV2())) ncs.v_int2 = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 220; goto _out;}
			}
			ncs.intv2_exist = true;
		}
		
#line 4579 "crontab.cpp"
		
		{
#line 546 "crontab.rl"
			ncs.setlim(RLIMIT_CPU); }
		
#line 4585 "crontab.cpp"
		
		goto _st220;
		_st220:
		if ( p == eof )
			goto _out220;
		p+= 1;
		st_case_220:
		if ( p == pe && p != eof )
			goto _out220;
		if ( p == eof ) {
			goto _ctr309;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st220;
			}
			goto _st0;
		}
		_st66:
		if ( p == eof )
			goto _out66;
		p+= 1;
		st_case_66:
		if ( p == pe && p != eof )
			goto _out66;
		if ( p == eof ) {
			goto _st66;}
		else {
			switch( ( (*( p))) ) {
				case 65: {
					goto _st67;
				}
				case 97: {
					goto _st67;
				}
			}
			goto _st0;
		}
		_st67:
		if ( p == eof )
			goto _out67;
		p+= 1;
		st_case_67:
		if ( p == pe && p != eof )
			goto _out67;
		if ( p == eof ) {
			goto _st67;}
		else {
			switch( ( (*( p))) ) {
				case 84: {
					goto _st68;
				}
				case 116: {
					goto _st68;
				}
			}
			goto _st0;
		}
		_st68:
		if ( p == eof )
			goto _out68;
		p+= 1;
		st_case_68:
		if ( p == pe && p != eof )
			goto _out68;
		if ( p == eof ) {
			goto _st68;}
		else {
			switch( ( (*( p))) ) {
				case 65: {
					goto _st69;
				}
				case 97: {
					goto _st69;
				}
			}
			goto _st0;
		}
		_st69:
		if ( p == eof )
			goto _out69;
		p+= 1;
		st_case_69:
		if ( p == pe && p != eof )
			goto _out69;
		if ( p == eof ) {
			goto _st69;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st69;
				}
				case 32: {
					goto _st69;
				}
				case 61: {
					goto _st70;
				}
			}
			goto _st0;
		}
		_st70:
		if ( p == eof )
			goto _out70;
		p+= 1;
		st_case_70:
		if ( p == pe && p != eof )
			goto _out70;
		if ( p == eof ) {
			goto _st70;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st70;
				}
				case 32: {
					goto _st70;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr106;
			}
			goto _st0;
		}
		_ctr106:
		{
#line 474 "crontab.rl"
			
			ncs.intv_st = p;
			ncs.v_int = ncs.v_int2 = 0;
			ncs.intv2_exist = false;
		}
		
#line 4718 "crontab.cpp"
		
		goto _st221;
		_ctr311:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 221; goto _out;}
			}
		}
		
#line 4731 "crontab.cpp"
		
		{
#line 544 "crontab.rl"
			ncs.setlim(RLIMIT_DATA); }
		
#line 4737 "crontab.cpp"
		
		goto _st221;
		_st221:
		if ( p == eof )
			goto _out221;
		p+= 1;
		st_case_221:
		if ( p == pe && p != eof )
			goto _out221;
		if ( p == eof ) {
			goto _ctr311;}
		else {
			if ( ( (*( p))) == 44 ) {
				goto _ctr312;
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st221;
			}
			goto _st0;
		}
		_ctr312:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 71; goto _out;}
			}
		}
		
#line 4768 "crontab.cpp"
		
		goto _st71;
		_st71:
		if ( p == eof )
			goto _out71;
		p+= 1;
		st_case_71:
		if ( p == pe && p != eof )
			goto _out71;
		if ( p == eof ) {
			goto _st71;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr108;
			}
			goto _st0;
		}
		_ctr108:
		{
#line 485 "crontab.rl"
			ncs.intv2_st = p; }
		
#line 4791 "crontab.cpp"
		
		goto _st222;
		_ctr314:
		{
#line 486 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV2())) ncs.v_int2 = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 222; goto _out;}
			}
			ncs.intv2_exist = true;
		}
		
#line 4805 "crontab.cpp"
		
		{
#line 544 "crontab.rl"
			ncs.setlim(RLIMIT_DATA); }
		
#line 4811 "crontab.cpp"
		
		goto _st222;
		_st222:
		if ( p == eof )
			goto _out222;
		p+= 1;
		st_case_222:
		if ( p == pe && p != eof )
			goto _out222;
		if ( p == eof ) {
			goto _ctr314;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st222;
			}
			goto _st0;
		}
		_st72:
		if ( p == eof )
			goto _out72;
		p+= 1;
		st_case_72:
		if ( p == pe && p != eof )
			goto _out72;
		if ( p == eof ) {
			goto _st72;}
		else {
			switch( ( (*( p))) ) {
				case 83: {
					goto _st73;
				}
				case 115: {
					goto _st73;
				}
			}
			goto _st0;
		}
		_st73:
		if ( p == eof )
			goto _out73;
		p+= 1;
		st_case_73:
		if ( p == pe && p != eof )
			goto _out73;
		if ( p == eof ) {
			goto _st73;}
		else {
			switch( ( (*( p))) ) {
				case 73: {
					goto _st74;
				}
				case 105: {
					goto _st74;
				}
			}
			goto _st0;
		}
		_st74:
		if ( p == eof )
			goto _out74;
		p+= 1;
		st_case_74:
		if ( p == pe && p != eof )
			goto _out74;
		if ( p == eof ) {
			goto _st74;}
		else {
			switch( ( (*( p))) ) {
				case 90: {
					goto _st75;
				}
				case 122: {
					goto _st75;
				}
			}
			goto _st0;
		}
		_st75:
		if ( p == eof )
			goto _out75;
		p+= 1;
		st_case_75:
		if ( p == pe && p != eof )
			goto _out75;
		if ( p == eof ) {
			goto _st75;}
		else {
			switch( ( (*( p))) ) {
				case 69: {
					goto _st76;
				}
				case 101: {
					goto _st76;
				}
			}
			goto _st0;
		}
		_st76:
		if ( p == eof )
			goto _out76;
		p+= 1;
		st_case_76:
		if ( p == pe && p != eof )
			goto _out76;
		if ( p == eof ) {
			goto _st76;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st76;
				}
				case 32: {
					goto _st76;
				}
				case 61: {
					goto _st77;
				}
			}
			goto _st0;
		}
		_st77:
		if ( p == eof )
			goto _out77;
		p+= 1;
		st_case_77:
		if ( p == pe && p != eof )
			goto _out77;
		if ( p == eof ) {
			goto _st77;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st77;
				}
				case 32: {
					goto _st77;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr114;
			}
			goto _st0;
		}
		_ctr114:
		{
#line 474 "crontab.rl"
			
			ncs.intv_st = p;
			ncs.v_int = ncs.v_int2 = 0;
			ncs.intv2_exist = false;
		}
		
#line 4964 "crontab.cpp"
		
		goto _st223;
		_ctr316:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 223; goto _out;}
			}
		}
		
#line 4977 "crontab.cpp"
		
		{
#line 545 "crontab.rl"
			ncs.setlim(RLIMIT_FSIZE); }
		
#line 4983 "crontab.cpp"
		
		goto _st223;
		_st223:
		if ( p == eof )
			goto _out223;
		p+= 1;
		st_case_223:
		if ( p == pe && p != eof )
			goto _out223;
		if ( p == eof ) {
			goto _ctr316;}
		else {
			if ( ( (*( p))) == 44 ) {
				goto _ctr317;
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st223;
			}
			goto _st0;
		}
		_ctr317:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 78; goto _out;}
			}
		}
		
#line 5014 "crontab.cpp"
		
		goto _st78;
		_st78:
		if ( p == eof )
			goto _out78;
		p+= 1;
		st_case_78:
		if ( p == pe && p != eof )
			goto _out78;
		if ( p == eof ) {
			goto _st78;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr116;
			}
			goto _st0;
		}
		_ctr116:
		{
#line 485 "crontab.rl"
			ncs.intv2_st = p; }
		
#line 5037 "crontab.cpp"
		
		goto _st224;
		_ctr319:
		{
#line 486 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV2())) ncs.v_int2 = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 224; goto _out;}
			}
			ncs.intv2_exist = true;
		}
		
#line 5051 "crontab.cpp"
		
		{
#line 545 "crontab.rl"
			ncs.setlim(RLIMIT_FSIZE); }
		
#line 5057 "crontab.cpp"
		
		goto _st224;
		_st224:
		if ( p == eof )
			goto _out224;
		p+= 1;
		st_case_224:
		if ( p == pe && p != eof )
			goto _out224;
		if ( p == eof ) {
			goto _ctr319;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st224;
			}
			goto _st0;
		}
		_st79:
		if ( p == eof )
			goto _out79;
		p+= 1;
		st_case_79:
		if ( p == pe && p != eof )
			goto _out79;
		if ( p == eof ) {
			goto _st79;}
		else {
			switch( ( (*( p))) ) {
				case 69: {
					goto _st80;
				}
				case 83: {
					goto _st88;
				}
				case 101: {
					goto _st80;
				}
				case 115: {
					goto _st88;
				}
			}
			goto _st0;
		}
		_st80:
		if ( p == eof )
			goto _out80;
		p+= 1;
		st_case_80:
		if ( p == pe && p != eof )
			goto _out80;
		if ( p == eof ) {
			goto _st80;}
		else {
			switch( ( (*( p))) ) {
				case 77: {
					goto _st81;
				}
				case 109: {
					goto _st81;
				}
			}
			goto _st0;
		}
		_st81:
		if ( p == eof )
			goto _out81;
		p+= 1;
		st_case_81:
		if ( p == pe && p != eof )
			goto _out81;
		if ( p == eof ) {
			goto _st81;}
		else {
			switch( ( (*( p))) ) {
				case 76: {
					goto _st82;
				}
				case 108: {
					goto _st82;
				}
			}
			goto _st0;
		}
		_st82:
		if ( p == eof )
			goto _out82;
		p+= 1;
		st_case_82:
		if ( p == pe && p != eof )
			goto _out82;
		if ( p == eof ) {
			goto _st82;}
		else {
			switch( ( (*( p))) ) {
				case 79: {
					goto _st83;
				}
				case 111: {
					goto _st83;
				}
			}
			goto _st0;
		}
		_st83:
		if ( p == eof )
			goto _out83;
		p+= 1;
		st_case_83:
		if ( p == pe && p != eof )
			goto _out83;
		if ( p == eof ) {
			goto _st83;}
		else {
			switch( ( (*( p))) ) {
				case 67: {
					goto _st84;
				}
				case 99: {
					goto _st84;
				}
			}
			goto _st0;
		}
		_st84:
		if ( p == eof )
			goto _out84;
		p+= 1;
		st_case_84:
		if ( p == pe && p != eof )
			goto _out84;
		if ( p == eof ) {
			goto _st84;}
		else {
			switch( ( (*( p))) ) {
				case 75: {
					goto _st85;
				}
				case 107: {
					goto _st85;
				}
			}
			goto _st0;
		}
		_st85:
		if ( p == eof )
			goto _out85;
		p+= 1;
		st_case_85:
		if ( p == pe && p != eof )
			goto _out85;
		if ( p == eof ) {
			goto _st85;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st85;
				}
				case 32: {
					goto _st85;
				}
				case 61: {
					goto _st86;
				}
			}
			goto _st0;
		}
		_st86:
		if ( p == eof )
			goto _out86;
		p+= 1;
		st_case_86:
		if ( p == pe && p != eof )
			goto _out86;
		if ( p == eof ) {
			goto _st86;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st86;
				}
				case 32: {
					goto _st86;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr125;
			}
			goto _st0;
		}
		_ctr125:
		{
#line 474 "crontab.rl"
			
			ncs.intv_st = p;
			ncs.v_int = ncs.v_int2 = 0;
			ncs.intv2_exist = false;
		}
		
#line 5256 "crontab.cpp"
		
		goto _st225;
		_ctr321:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 225; goto _out;}
			}
		}
		
#line 5269 "crontab.cpp"
		
		{
#line 538 "crontab.rl"
			ncs.setlim(RLIMIT_MEMLOCK); }
		
#line 5275 "crontab.cpp"
		
		goto _st225;
		_st225:
		if ( p == eof )
			goto _out225;
		p+= 1;
		st_case_225:
		if ( p == pe && p != eof )
			goto _out225;
		if ( p == eof ) {
			goto _ctr321;}
		else {
			if ( ( (*( p))) == 44 ) {
				goto _ctr322;
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st225;
			}
			goto _st0;
		}
		_ctr322:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 87; goto _out;}
			}
		}
		
#line 5306 "crontab.cpp"
		
		goto _st87;
		_st87:
		if ( p == eof )
			goto _out87;
		p+= 1;
		st_case_87:
		if ( p == pe && p != eof )
			goto _out87;
		if ( p == eof ) {
			goto _st87;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr127;
			}
			goto _st0;
		}
		_ctr127:
		{
#line 485 "crontab.rl"
			ncs.intv2_st = p; }
		
#line 5329 "crontab.cpp"
		
		goto _st226;
		_ctr324:
		{
#line 486 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV2())) ncs.v_int2 = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 226; goto _out;}
			}
			ncs.intv2_exist = true;
		}
		
#line 5343 "crontab.cpp"
		
		{
#line 538 "crontab.rl"
			ncs.setlim(RLIMIT_MEMLOCK); }
		
#line 5349 "crontab.cpp"
		
		goto _st226;
		_st226:
		if ( p == eof )
			goto _out226;
		p+= 1;
		st_case_226:
		if ( p == pe && p != eof )
			goto _out226;
		if ( p == eof ) {
			goto _ctr324;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st226;
			}
			goto _st0;
		}
		_st88:
		if ( p == eof )
			goto _out88;
		p+= 1;
		st_case_88:
		if ( p == pe && p != eof )
			goto _out88;
		if ( p == eof ) {
			goto _st88;}
		else {
			switch( ( (*( p))) ) {
				case 71: {
					goto _st89;
				}
				case 103: {
					goto _st89;
				}
			}
			goto _st0;
		}
		_st89:
		if ( p == eof )
			goto _out89;
		p+= 1;
		st_case_89:
		if ( p == pe && p != eof )
			goto _out89;
		if ( p == eof ) {
			goto _st89;}
		else {
			switch( ( (*( p))) ) {
				case 81: {
					goto _st90;
				}
				case 113: {
					goto _st90;
				}
			}
			goto _st0;
		}
		_st90:
		if ( p == eof )
			goto _out90;
		p+= 1;
		st_case_90:
		if ( p == pe && p != eof )
			goto _out90;
		if ( p == eof ) {
			goto _st90;}
		else {
			switch( ( (*( p))) ) {
				case 85: {
					goto _st91;
				}
				case 117: {
					goto _st91;
				}
			}
			goto _st0;
		}
		_st91:
		if ( p == eof )
			goto _out91;
		p+= 1;
		st_case_91:
		if ( p == pe && p != eof )
			goto _out91;
		if ( p == eof ) {
			goto _st91;}
		else {
			switch( ( (*( p))) ) {
				case 69: {
					goto _st92;
				}
				case 101: {
					goto _st92;
				}
			}
			goto _st0;
		}
		_st92:
		if ( p == eof )
			goto _out92;
		p+= 1;
		st_case_92:
		if ( p == pe && p != eof )
			goto _out92;
		if ( p == eof ) {
			goto _st92;}
		else {
			switch( ( (*( p))) ) {
				case 85: {
					goto _st93;
				}
				case 117: {
					goto _st93;
				}
			}
			goto _st0;
		}
		_st93:
		if ( p == eof )
			goto _out93;
		p+= 1;
		st_case_93:
		if ( p == pe && p != eof )
			goto _out93;
		if ( p == eof ) {
			goto _st93;}
		else {
			switch( ( (*( p))) ) {
				case 69: {
					goto _st94;
				}
				case 101: {
					goto _st94;
				}
			}
			goto _st0;
		}
		_st94:
		if ( p == eof )
			goto _out94;
		p+= 1;
		st_case_94:
		if ( p == pe && p != eof )
			goto _out94;
		if ( p == eof ) {
			goto _st94;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st94;
				}
				case 32: {
					goto _st94;
				}
				case 61: {
					goto _st95;
				}
			}
			goto _st0;
		}
		_st95:
		if ( p == eof )
			goto _out95;
		p+= 1;
		st_case_95:
		if ( p == pe && p != eof )
			goto _out95;
		if ( p == eof ) {
			goto _st95;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st95;
				}
				case 32: {
					goto _st95;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr135;
			}
			goto _st0;
		}
		_ctr135:
		{
#line 474 "crontab.rl"
			
			ncs.intv_st = p;
			ncs.v_int = ncs.v_int2 = 0;
			ncs.intv2_exist = false;
		}
		
#line 5542 "crontab.cpp"
		
		goto _st227;
		_ctr326:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 227; goto _out;}
			}
		}
		
#line 5555 "crontab.cpp"
		
		{
#line 547 "crontab.rl"
			ncs.setlim(RLIMIT_MSGQUEUE); }
		
#line 5561 "crontab.cpp"
		
		goto _st227;
		_st227:
		if ( p == eof )
			goto _out227;
		p+= 1;
		st_case_227:
		if ( p == pe && p != eof )
			goto _out227;
		if ( p == eof ) {
			goto _ctr326;}
		else {
			if ( ( (*( p))) == 44 ) {
				goto _ctr327;
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st227;
			}
			goto _st0;
		}
		_ctr327:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 96; goto _out;}
			}
		}
		
#line 5592 "crontab.cpp"
		
		goto _st96;
		_st96:
		if ( p == eof )
			goto _out96;
		p+= 1;
		st_case_96:
		if ( p == pe && p != eof )
			goto _out96;
		if ( p == eof ) {
			goto _st96;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr137;
			}
			goto _st0;
		}
		_ctr137:
		{
#line 485 "crontab.rl"
			ncs.intv2_st = p; }
		
#line 5615 "crontab.cpp"
		
		goto _st228;
		_ctr329:
		{
#line 486 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV2())) ncs.v_int2 = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 228; goto _out;}
			}
			ncs.intv2_exist = true;
		}
		
#line 5629 "crontab.cpp"
		
		{
#line 547 "crontab.rl"
			ncs.setlim(RLIMIT_MSGQUEUE); }
		
#line 5635 "crontab.cpp"
		
		goto _st228;
		_st228:
		if ( p == eof )
			goto _out228;
		p+= 1;
		st_case_228:
		if ( p == pe && p != eof )
			goto _out228;
		if ( p == eof ) {
			goto _ctr329;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st228;
			}
			goto _st0;
		}
		_st97:
		if ( p == eof )
			goto _out97;
		p+= 1;
		st_case_97:
		if ( p == pe && p != eof )
			goto _out97;
		if ( p == eof ) {
			goto _st97;}
		else {
			switch( ( (*( p))) ) {
				case 73: {
					goto _st98;
				}
				case 79: {
					goto _st103;
				}
				case 80: {
					goto _st110;
				}
				case 105: {
					goto _st98;
				}
				case 111: {
					goto _st103;
				}
				case 112: {
					goto _st110;
				}
			}
			goto _st0;
		}
		_st98:
		if ( p == eof )
			goto _out98;
		p+= 1;
		st_case_98:
		if ( p == pe && p != eof )
			goto _out98;
		if ( p == eof ) {
			goto _st98;}
		else {
			switch( ( (*( p))) ) {
				case 67: {
					goto _st99;
				}
				case 99: {
					goto _st99;
				}
			}
			goto _st0;
		}
		_st99:
		if ( p == eof )
			goto _out99;
		p+= 1;
		st_case_99:
		if ( p == pe && p != eof )
			goto _out99;
		if ( p == eof ) {
			goto _st99;}
		else {
			switch( ( (*( p))) ) {
				case 69: {
					goto _st100;
				}
				case 101: {
					goto _st100;
				}
			}
			goto _st0;
		}
		_st100:
		if ( p == eof )
			goto _out100;
		p+= 1;
		st_case_100:
		if ( p == pe && p != eof )
			goto _out100;
		if ( p == eof ) {
			goto _st100;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st100;
				}
				case 32: {
					goto _st100;
				}
				case 61: {
					goto _st101;
				}
			}
			goto _st0;
		}
		_st101:
		if ( p == eof )
			goto _out101;
		p+= 1;
		st_case_101:
		if ( p == pe && p != eof )
			goto _out101;
		if ( p == eof ) {
			goto _st101;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st101;
				}
				case 32: {
					goto _st101;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr144;
			}
			goto _st0;
		}
		_ctr144:
		{
#line 474 "crontab.rl"
			
			ncs.intv_st = p;
			ncs.v_int = ncs.v_int2 = 0;
			ncs.intv2_exist = false;
		}
		
#line 5780 "crontab.cpp"
		
		goto _st229;
		_ctr331:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 229; goto _out;}
			}
		}
		
#line 5793 "crontab.cpp"
		
		{
#line 548 "crontab.rl"
			ncs.setlim(RLIMIT_NICE); }
		
#line 5799 "crontab.cpp"
		
		goto _st229;
		_st229:
		if ( p == eof )
			goto _out229;
		p+= 1;
		st_case_229:
		if ( p == pe && p != eof )
			goto _out229;
		if ( p == eof ) {
			goto _ctr331;}
		else {
			if ( ( (*( p))) == 44 ) {
				goto _ctr332;
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st229;
			}
			goto _st0;
		}
		_ctr332:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 102; goto _out;}
			}
		}
		
#line 5830 "crontab.cpp"
		
		goto _st102;
		_st102:
		if ( p == eof )
			goto _out102;
		p+= 1;
		st_case_102:
		if ( p == pe && p != eof )
			goto _out102;
		if ( p == eof ) {
			goto _st102;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr146;
			}
			goto _st0;
		}
		_ctr146:
		{
#line 485 "crontab.rl"
			ncs.intv2_st = p; }
		
#line 5853 "crontab.cpp"
		
		goto _st230;
		_ctr334:
		{
#line 486 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV2())) ncs.v_int2 = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 230; goto _out;}
			}
			ncs.intv2_exist = true;
		}
		
#line 5867 "crontab.cpp"
		
		{
#line 548 "crontab.rl"
			ncs.setlim(RLIMIT_NICE); }
		
#line 5873 "crontab.cpp"
		
		goto _st230;
		_st230:
		if ( p == eof )
			goto _out230;
		p+= 1;
		st_case_230:
		if ( p == pe && p != eof )
			goto _out230;
		if ( p == eof ) {
			goto _ctr334;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st230;
			}
			goto _st0;
		}
		_st103:
		if ( p == eof )
			goto _out103;
		p+= 1;
		st_case_103:
		if ( p == pe && p != eof )
			goto _out103;
		if ( p == eof ) {
			goto _st103;}
		else {
			switch( ( (*( p))) ) {
				case 70: {
					goto _st104;
				}
				case 102: {
					goto _st104;
				}
			}
			goto _st0;
		}
		_st104:
		if ( p == eof )
			goto _out104;
		p+= 1;
		st_case_104:
		if ( p == pe && p != eof )
			goto _out104;
		if ( p == eof ) {
			goto _st104;}
		else {
			switch( ( (*( p))) ) {
				case 73: {
					goto _st105;
				}
				case 105: {
					goto _st105;
				}
			}
			goto _st0;
		}
		_st105:
		if ( p == eof )
			goto _out105;
		p+= 1;
		st_case_105:
		if ( p == pe && p != eof )
			goto _out105;
		if ( p == eof ) {
			goto _st105;}
		else {
			switch( ( (*( p))) ) {
				case 76: {
					goto _st106;
				}
				case 108: {
					goto _st106;
				}
			}
			goto _st0;
		}
		_st106:
		if ( p == eof )
			goto _out106;
		p+= 1;
		st_case_106:
		if ( p == pe && p != eof )
			goto _out106;
		if ( p == eof ) {
			goto _st106;}
		else {
			switch( ( (*( p))) ) {
				case 69: {
					goto _st107;
				}
				case 101: {
					goto _st107;
				}
			}
			goto _st0;
		}
		_st107:
		if ( p == eof )
			goto _out107;
		p+= 1;
		st_case_107:
		if ( p == pe && p != eof )
			goto _out107;
		if ( p == eof ) {
			goto _st107;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st107;
				}
				case 32: {
					goto _st107;
				}
				case 61: {
					goto _st108;
				}
			}
			goto _st0;
		}
		_st108:
		if ( p == eof )
			goto _out108;
		p+= 1;
		st_case_108:
		if ( p == pe && p != eof )
			goto _out108;
		if ( p == eof ) {
			goto _st108;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st108;
				}
				case 32: {
					goto _st108;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr152;
			}
			goto _st0;
		}
		_ctr152:
		{
#line 474 "crontab.rl"
			
			ncs.intv_st = p;
			ncs.v_int = ncs.v_int2 = 0;
			ncs.intv2_exist = false;
		}
		
#line 6026 "crontab.cpp"
		
		goto _st231;
		_ctr336:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 231; goto _out;}
			}
		}
		
#line 6039 "crontab.cpp"
		
		{
#line 539 "crontab.rl"
			ncs.setlim(RLIMIT_NOFILE); }
		
#line 6045 "crontab.cpp"
		
		goto _st231;
		_st231:
		if ( p == eof )
			goto _out231;
		p+= 1;
		st_case_231:
		if ( p == pe && p != eof )
			goto _out231;
		if ( p == eof ) {
			goto _ctr336;}
		else {
			if ( ( (*( p))) == 44 ) {
				goto _ctr337;
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st231;
			}
			goto _st0;
		}
		_ctr337:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 109; goto _out;}
			}
		}
		
#line 6076 "crontab.cpp"
		
		goto _st109;
		_st109:
		if ( p == eof )
			goto _out109;
		p+= 1;
		st_case_109:
		if ( p == pe && p != eof )
			goto _out109;
		if ( p == eof ) {
			goto _st109;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr154;
			}
			goto _st0;
		}
		_ctr154:
		{
#line 485 "crontab.rl"
			ncs.intv2_st = p; }
		
#line 6099 "crontab.cpp"
		
		goto _st232;
		_ctr339:
		{
#line 486 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV2())) ncs.v_int2 = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 232; goto _out;}
			}
			ncs.intv2_exist = true;
		}
		
#line 6113 "crontab.cpp"
		
		{
#line 539 "crontab.rl"
			ncs.setlim(RLIMIT_NOFILE); }
		
#line 6119 "crontab.cpp"
		
		goto _st232;
		_st232:
		if ( p == eof )
			goto _out232;
		p+= 1;
		st_case_232:
		if ( p == pe && p != eof )
			goto _out232;
		if ( p == eof ) {
			goto _ctr339;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st232;
			}
			goto _st0;
		}
		_st110:
		if ( p == eof )
			goto _out110;
		p+= 1;
		st_case_110:
		if ( p == pe && p != eof )
			goto _out110;
		if ( p == eof ) {
			goto _st110;}
		else {
			switch( ( (*( p))) ) {
				case 82: {
					goto _st111;
				}
				case 114: {
					goto _st111;
				}
			}
			goto _st0;
		}
		_st111:
		if ( p == eof )
			goto _out111;
		p+= 1;
		st_case_111:
		if ( p == pe && p != eof )
			goto _out111;
		if ( p == eof ) {
			goto _st111;}
		else {
			switch( ( (*( p))) ) {
				case 79: {
					goto _st112;
				}
				case 111: {
					goto _st112;
				}
			}
			goto _st0;
		}
		_st112:
		if ( p == eof )
			goto _out112;
		p+= 1;
		st_case_112:
		if ( p == pe && p != eof )
			goto _out112;
		if ( p == eof ) {
			goto _st112;}
		else {
			switch( ( (*( p))) ) {
				case 67: {
					goto _st113;
				}
				case 99: {
					goto _st113;
				}
			}
			goto _st0;
		}
		_st113:
		if ( p == eof )
			goto _out113;
		p+= 1;
		st_case_113:
		if ( p == pe && p != eof )
			goto _out113;
		if ( p == eof ) {
			goto _st113;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st113;
				}
				case 32: {
					goto _st113;
				}
				case 61: {
					goto _st114;
				}
			}
			goto _st0;
		}
		_st114:
		if ( p == eof )
			goto _out114;
		p+= 1;
		st_case_114:
		if ( p == pe && p != eof )
			goto _out114;
		if ( p == eof ) {
			goto _st114;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st114;
				}
				case 32: {
					goto _st114;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr159;
			}
			goto _st0;
		}
		_ctr159:
		{
#line 474 "crontab.rl"
			
			ncs.intv_st = p;
			ncs.v_int = ncs.v_int2 = 0;
			ncs.intv2_exist = false;
		}
		
#line 6252 "crontab.cpp"
		
		goto _st233;
		_ctr341:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 233; goto _out;}
			}
		}
		
#line 6265 "crontab.cpp"
		
		{
#line 540 "crontab.rl"
			ncs.setlim(RLIMIT_NPROC); }
		
#line 6271 "crontab.cpp"
		
		goto _st233;
		_st233:
		if ( p == eof )
			goto _out233;
		p+= 1;
		st_case_233:
		if ( p == pe && p != eof )
			goto _out233;
		if ( p == eof ) {
			goto _ctr341;}
		else {
			if ( ( (*( p))) == 44 ) {
				goto _ctr342;
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st233;
			}
			goto _st0;
		}
		_ctr342:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 115; goto _out;}
			}
		}
		
#line 6302 "crontab.cpp"
		
		goto _st115;
		_st115:
		if ( p == eof )
			goto _out115;
		p+= 1;
		st_case_115:
		if ( p == pe && p != eof )
			goto _out115;
		if ( p == eof ) {
			goto _st115;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr161;
			}
			goto _st0;
		}
		_ctr161:
		{
#line 485 "crontab.rl"
			ncs.intv2_st = p; }
		
#line 6325 "crontab.cpp"
		
		goto _st234;
		_ctr344:
		{
#line 486 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV2())) ncs.v_int2 = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 234; goto _out;}
			}
			ncs.intv2_exist = true;
		}
		
#line 6339 "crontab.cpp"
		
		{
#line 540 "crontab.rl"
			ncs.setlim(RLIMIT_NPROC); }
		
#line 6345 "crontab.cpp"
		
		goto _st234;
		_st234:
		if ( p == eof )
			goto _out234;
		p+= 1;
		st_case_234:
		if ( p == pe && p != eof )
			goto _out234;
		if ( p == eof ) {
			goto _ctr344;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st234;
			}
			goto _st0;
		}
		_st116:
		if ( p == eof )
			goto _out116;
		p+= 1;
		st_case_116:
		if ( p == pe && p != eof )
			goto _out116;
		if ( p == eof ) {
			goto _st116;}
		else {
			switch( ( (*( p))) ) {
				case 83: {
					goto _st117;
				}
				case 84: {
					goto _st121;
				}
				case 115: {
					goto _st117;
				}
				case 116: {
					goto _st121;
				}
			}
			goto _st0;
		}
		_st117:
		if ( p == eof )
			goto _out117;
		p+= 1;
		st_case_117:
		if ( p == pe && p != eof )
			goto _out117;
		if ( p == eof ) {
			goto _st117;}
		else {
			switch( ( (*( p))) ) {
				case 83: {
					goto _st118;
				}
				case 115: {
					goto _st118;
				}
			}
			goto _st0;
		}
		_st118:
		if ( p == eof )
			goto _out118;
		p+= 1;
		st_case_118:
		if ( p == pe && p != eof )
			goto _out118;
		if ( p == eof ) {
			goto _st118;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st118;
				}
				case 32: {
					goto _st118;
				}
				case 61: {
					goto _st119;
				}
			}
			goto _st0;
		}
		_st119:
		if ( p == eof )
			goto _out119;
		p+= 1;
		st_case_119:
		if ( p == pe && p != eof )
			goto _out119;
		if ( p == eof ) {
			goto _st119;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st119;
				}
				case 32: {
					goto _st119;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr166;
			}
			goto _st0;
		}
		_ctr166:
		{
#line 474 "crontab.rl"
			
			ncs.intv_st = p;
			ncs.v_int = ncs.v_int2 = 0;
			ncs.intv2_exist = false;
		}
		
#line 6464 "crontab.cpp"
		
		goto _st235;
		_ctr346:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 235; goto _out;}
			}
		}
		
#line 6477 "crontab.cpp"
		
		{
#line 541 "crontab.rl"
			ncs.setlim(RLIMIT_RSS); }
		
#line 6483 "crontab.cpp"
		
		goto _st235;
		_st235:
		if ( p == eof )
			goto _out235;
		p+= 1;
		st_case_235:
		if ( p == pe && p != eof )
			goto _out235;
		if ( p == eof ) {
			goto _ctr346;}
		else {
			if ( ( (*( p))) == 44 ) {
				goto _ctr347;
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st235;
			}
			goto _st0;
		}
		_ctr347:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 120; goto _out;}
			}
		}
		
#line 6514 "crontab.cpp"
		
		goto _st120;
		_st120:
		if ( p == eof )
			goto _out120;
		p+= 1;
		st_case_120:
		if ( p == pe && p != eof )
			goto _out120;
		if ( p == eof ) {
			goto _st120;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr168;
			}
			goto _st0;
		}
		_ctr168:
		{
#line 485 "crontab.rl"
			ncs.intv2_st = p; }
		
#line 6537 "crontab.cpp"
		
		goto _st236;
		_ctr349:
		{
#line 486 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV2())) ncs.v_int2 = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 236; goto _out;}
			}
			ncs.intv2_exist = true;
		}
		
#line 6551 "crontab.cpp"
		
		{
#line 541 "crontab.rl"
			ncs.setlim(RLIMIT_RSS); }
		
#line 6557 "crontab.cpp"
		
		goto _st236;
		_st236:
		if ( p == eof )
			goto _out236;
		p+= 1;
		st_case_236:
		if ( p == pe && p != eof )
			goto _out236;
		if ( p == eof ) {
			goto _ctr349;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st236;
			}
			goto _st0;
		}
		_st121:
		if ( p == eof )
			goto _out121;
		p+= 1;
		st_case_121:
		if ( p == pe && p != eof )
			goto _out121;
		if ( p == eof ) {
			goto _st121;}
		else {
			switch( ( (*( p))) ) {
				case 80: {
					goto _st122;
				}
				case 84: {
					goto _st128;
				}
				case 112: {
					goto _st122;
				}
				case 116: {
					goto _st128;
				}
			}
			goto _st0;
		}
		_st122:
		if ( p == eof )
			goto _out122;
		p+= 1;
		st_case_122:
		if ( p == pe && p != eof )
			goto _out122;
		if ( p == eof ) {
			goto _st122;}
		else {
			switch( ( (*( p))) ) {
				case 82: {
					goto _st123;
				}
				case 114: {
					goto _st123;
				}
			}
			goto _st0;
		}
		_st123:
		if ( p == eof )
			goto _out123;
		p+= 1;
		st_case_123:
		if ( p == pe && p != eof )
			goto _out123;
		if ( p == eof ) {
			goto _st123;}
		else {
			switch( ( (*( p))) ) {
				case 73: {
					goto _st124;
				}
				case 105: {
					goto _st124;
				}
			}
			goto _st0;
		}
		_st124:
		if ( p == eof )
			goto _out124;
		p+= 1;
		st_case_124:
		if ( p == pe && p != eof )
			goto _out124;
		if ( p == eof ) {
			goto _st124;}
		else {
			switch( ( (*( p))) ) {
				case 79: {
					goto _st125;
				}
				case 111: {
					goto _st125;
				}
			}
			goto _st0;
		}
		_st125:
		if ( p == eof )
			goto _out125;
		p+= 1;
		st_case_125:
		if ( p == pe && p != eof )
			goto _out125;
		if ( p == eof ) {
			goto _st125;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st125;
				}
				case 32: {
					goto _st125;
				}
				case 61: {
					goto _st126;
				}
			}
			goto _st0;
		}
		_st126:
		if ( p == eof )
			goto _out126;
		p+= 1;
		st_case_126:
		if ( p == pe && p != eof )
			goto _out126;
		if ( p == eof ) {
			goto _st126;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st126;
				}
				case 32: {
					goto _st126;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr175;
			}
			goto _st0;
		}
		_ctr175:
		{
#line 474 "crontab.rl"
			
			ncs.intv_st = p;
			ncs.v_int = ncs.v_int2 = 0;
			ncs.intv2_exist = false;
		}
		
#line 6716 "crontab.cpp"
		
		goto _st237;
		_ctr351:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 237; goto _out;}
			}
		}
		
#line 6729 "crontab.cpp"
		
		{
#line 550 "crontab.rl"
			ncs.setlim(RLIMIT_RTPRIO); }
		
#line 6735 "crontab.cpp"
		
		goto _st237;
		_st237:
		if ( p == eof )
			goto _out237;
		p+= 1;
		st_case_237:
		if ( p == pe && p != eof )
			goto _out237;
		if ( p == eof ) {
			goto _ctr351;}
		else {
			if ( ( (*( p))) == 44 ) {
				goto _ctr352;
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st237;
			}
			goto _st0;
		}
		_ctr352:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 127; goto _out;}
			}
		}
		
#line 6766 "crontab.cpp"
		
		goto _st127;
		_st127:
		if ( p == eof )
			goto _out127;
		p+= 1;
		st_case_127:
		if ( p == pe && p != eof )
			goto _out127;
		if ( p == eof ) {
			goto _st127;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr177;
			}
			goto _st0;
		}
		_ctr177:
		{
#line 485 "crontab.rl"
			ncs.intv2_st = p; }
		
#line 6789 "crontab.cpp"
		
		goto _st238;
		_ctr354:
		{
#line 486 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV2())) ncs.v_int2 = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 238; goto _out;}
			}
			ncs.intv2_exist = true;
		}
		
#line 6803 "crontab.cpp"
		
		{
#line 550 "crontab.rl"
			ncs.setlim(RLIMIT_RTPRIO); }
		
#line 6809 "crontab.cpp"
		
		goto _st238;
		_st238:
		if ( p == eof )
			goto _out238;
		p+= 1;
		st_case_238:
		if ( p == pe && p != eof )
			goto _out238;
		if ( p == eof ) {
			goto _ctr354;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st238;
			}
			goto _st0;
		}
		_st128:
		if ( p == eof )
			goto _out128;
		p+= 1;
		st_case_128:
		if ( p == pe && p != eof )
			goto _out128;
		if ( p == eof ) {
			goto _st128;}
		else {
			switch( ( (*( p))) ) {
				case 73: {
					goto _st129;
				}
				case 105: {
					goto _st129;
				}
			}
			goto _st0;
		}
		_st129:
		if ( p == eof )
			goto _out129;
		p+= 1;
		st_case_129:
		if ( p == pe && p != eof )
			goto _out129;
		if ( p == eof ) {
			goto _st129;}
		else {
			switch( ( (*( p))) ) {
				case 77: {
					goto _st130;
				}
				case 109: {
					goto _st130;
				}
			}
			goto _st0;
		}
		_st130:
		if ( p == eof )
			goto _out130;
		p+= 1;
		st_case_130:
		if ( p == pe && p != eof )
			goto _out130;
		if ( p == eof ) {
			goto _st130;}
		else {
			switch( ( (*( p))) ) {
				case 69: {
					goto _st131;
				}
				case 101: {
					goto _st131;
				}
			}
			goto _st0;
		}
		_st131:
		if ( p == eof )
			goto _out131;
		p+= 1;
		st_case_131:
		if ( p == pe && p != eof )
			goto _out131;
		if ( p == eof ) {
			goto _st131;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st131;
				}
				case 32: {
					goto _st131;
				}
				case 61: {
					goto _st132;
				}
			}
			goto _st0;
		}
		_st132:
		if ( p == eof )
			goto _out132;
		p+= 1;
		st_case_132:
		if ( p == pe && p != eof )
			goto _out132;
		if ( p == eof ) {
			goto _st132;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st132;
				}
				case 32: {
					goto _st132;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr182;
			}
			goto _st0;
		}
		_ctr182:
		{
#line 474 "crontab.rl"
			
			ncs.intv_st = p;
			ncs.v_int = ncs.v_int2 = 0;
			ncs.intv2_exist = false;
		}
		
#line 6942 "crontab.cpp"
		
		goto _st239;
		_ctr356:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 239; goto _out;}
			}
		}
		
#line 6955 "crontab.cpp"
		
		{
#line 549 "crontab.rl"
			ncs.setlim(RLIMIT_RTTIME); }
		
#line 6961 "crontab.cpp"
		
		goto _st239;
		_st239:
		if ( p == eof )
			goto _out239;
		p+= 1;
		st_case_239:
		if ( p == pe && p != eof )
			goto _out239;
		if ( p == eof ) {
			goto _ctr356;}
		else {
			if ( ( (*( p))) == 44 ) {
				goto _ctr357;
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st239;
			}
			goto _st0;
		}
		_ctr357:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 133; goto _out;}
			}
		}
		
#line 6992 "crontab.cpp"
		
		goto _st133;
		_st133:
		if ( p == eof )
			goto _out133;
		p+= 1;
		st_case_133:
		if ( p == pe && p != eof )
			goto _out133;
		if ( p == eof ) {
			goto _st133;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr184;
			}
			goto _st0;
		}
		_ctr184:
		{
#line 485 "crontab.rl"
			ncs.intv2_st = p; }
		
#line 7015 "crontab.cpp"
		
		goto _st240;
		_ctr359:
		{
#line 486 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV2())) ncs.v_int2 = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 240; goto _out;}
			}
			ncs.intv2_exist = true;
		}
		
#line 7029 "crontab.cpp"
		
		{
#line 549 "crontab.rl"
			ncs.setlim(RLIMIT_RTTIME); }
		
#line 7035 "crontab.cpp"
		
		goto _st240;
		_st240:
		if ( p == eof )
			goto _out240;
		p+= 1;
		st_case_240:
		if ( p == pe && p != eof )
			goto _out240;
		if ( p == eof ) {
			goto _ctr359;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st240;
			}
			goto _st0;
		}
		_st134:
		if ( p == eof )
			goto _out134;
		p+= 1;
		st_case_134:
		if ( p == pe && p != eof )
			goto _out134;
		if ( p == eof ) {
			goto _st134;}
		else {
			switch( ( (*( p))) ) {
				case 73: {
					goto _st135;
				}
				case 84: {
					goto _st146;
				}
				case 105: {
					goto _st135;
				}
				case 116: {
					goto _st146;
				}
			}
			goto _st0;
		}
		_st135:
		if ( p == eof )
			goto _out135;
		p+= 1;
		st_case_135:
		if ( p == pe && p != eof )
			goto _out135;
		if ( p == eof ) {
			goto _st135;}
		else {
			switch( ( (*( p))) ) {
				case 71: {
					goto _st136;
				}
				case 103: {
					goto _st136;
				}
			}
			goto _st0;
		}
		_st136:
		if ( p == eof )
			goto _out136;
		p+= 1;
		st_case_136:
		if ( p == pe && p != eof )
			goto _out136;
		if ( p == eof ) {
			goto _st136;}
		else {
			switch( ( (*( p))) ) {
				case 80: {
					goto _st137;
				}
				case 112: {
					goto _st137;
				}
			}
			goto _st0;
		}
		_st137:
		if ( p == eof )
			goto _out137;
		p+= 1;
		st_case_137:
		if ( p == pe && p != eof )
			goto _out137;
		if ( p == eof ) {
			goto _st137;}
		else {
			switch( ( (*( p))) ) {
				case 69: {
					goto _st138;
				}
				case 101: {
					goto _st138;
				}
			}
			goto _st0;
		}
		_st138:
		if ( p == eof )
			goto _out138;
		p+= 1;
		st_case_138:
		if ( p == pe && p != eof )
			goto _out138;
		if ( p == eof ) {
			goto _st138;}
		else {
			switch( ( (*( p))) ) {
				case 78: {
					goto _st139;
				}
				case 110: {
					goto _st139;
				}
			}
			goto _st0;
		}
		_st139:
		if ( p == eof )
			goto _out139;
		p+= 1;
		st_case_139:
		if ( p == pe && p != eof )
			goto _out139;
		if ( p == eof ) {
			goto _st139;}
		else {
			switch( ( (*( p))) ) {
				case 68: {
					goto _st140;
				}
				case 100: {
					goto _st140;
				}
			}
			goto _st0;
		}
		_st140:
		if ( p == eof )
			goto _out140;
		p+= 1;
		st_case_140:
		if ( p == pe && p != eof )
			goto _out140;
		if ( p == eof ) {
			goto _st140;}
		else {
			switch( ( (*( p))) ) {
				case 73: {
					goto _st141;
				}
				case 105: {
					goto _st141;
				}
			}
			goto _st0;
		}
		_st141:
		if ( p == eof )
			goto _out141;
		p+= 1;
		st_case_141:
		if ( p == pe && p != eof )
			goto _out141;
		if ( p == eof ) {
			goto _st141;}
		else {
			switch( ( (*( p))) ) {
				case 78: {
					goto _st142;
				}
				case 110: {
					goto _st142;
				}
			}
			goto _st0;
		}
		_st142:
		if ( p == eof )
			goto _out142;
		p+= 1;
		st_case_142:
		if ( p == pe && p != eof )
			goto _out142;
		if ( p == eof ) {
			goto _st142;}
		else {
			switch( ( (*( p))) ) {
				case 71: {
					goto _st143;
				}
				case 103: {
					goto _st143;
				}
			}
			goto _st0;
		}
		_st143:
		if ( p == eof )
			goto _out143;
		p+= 1;
		st_case_143:
		if ( p == pe && p != eof )
			goto _out143;
		if ( p == eof ) {
			goto _st143;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st143;
				}
				case 32: {
					goto _st143;
				}
				case 61: {
					goto _st144;
				}
			}
			goto _st0;
		}
		_st144:
		if ( p == eof )
			goto _out144;
		p+= 1;
		st_case_144:
		if ( p == pe && p != eof )
			goto _out144;
		if ( p == eof ) {
			goto _st144;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st144;
				}
				case 32: {
					goto _st144;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr196;
			}
			goto _st0;
		}
		_ctr196:
		{
#line 474 "crontab.rl"
			
			ncs.intv_st = p;
			ncs.v_int = ncs.v_int2 = 0;
			ncs.intv2_exist = false;
		}
		
#line 7294 "crontab.cpp"
		
		goto _st241;
		_ctr361:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 241; goto _out;}
			}
		}
		
#line 7307 "crontab.cpp"
		
		{
#line 551 "crontab.rl"
			ncs.setlim(RLIMIT_SIGPENDING); }
		
#line 7313 "crontab.cpp"
		
		goto _st241;
		_st241:
		if ( p == eof )
			goto _out241;
		p+= 1;
		st_case_241:
		if ( p == pe && p != eof )
			goto _out241;
		if ( p == eof ) {
			goto _ctr361;}
		else {
			if ( ( (*( p))) == 44 ) {
				goto _ctr362;
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st241;
			}
			goto _st0;
		}
		_ctr362:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 145; goto _out;}
			}
		}
		
#line 7344 "crontab.cpp"
		
		goto _st145;
		_st145:
		if ( p == eof )
			goto _out145;
		p+= 1;
		st_case_145:
		if ( p == pe && p != eof )
			goto _out145;
		if ( p == eof ) {
			goto _st145;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr198;
			}
			goto _st0;
		}
		_ctr198:
		{
#line 485 "crontab.rl"
			ncs.intv2_st = p; }
		
#line 7367 "crontab.cpp"
		
		goto _st242;
		_ctr364:
		{
#line 486 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV2())) ncs.v_int2 = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 242; goto _out;}
			}
			ncs.intv2_exist = true;
		}
		
#line 7381 "crontab.cpp"
		
		{
#line 551 "crontab.rl"
			ncs.setlim(RLIMIT_SIGPENDING); }
		
#line 7387 "crontab.cpp"
		
		goto _st242;
		_st242:
		if ( p == eof )
			goto _out242;
		p+= 1;
		st_case_242:
		if ( p == pe && p != eof )
			goto _out242;
		if ( p == eof ) {
			goto _ctr364;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st242;
			}
			goto _st0;
		}
		_st146:
		if ( p == eof )
			goto _out146;
		p+= 1;
		st_case_146:
		if ( p == pe && p != eof )
			goto _out146;
		if ( p == eof ) {
			goto _st146;}
		else {
			switch( ( (*( p))) ) {
				case 65: {
					goto _st147;
				}
				case 97: {
					goto _st147;
				}
			}
			goto _st0;
		}
		_st147:
		if ( p == eof )
			goto _out147;
		p+= 1;
		st_case_147:
		if ( p == pe && p != eof )
			goto _out147;
		if ( p == eof ) {
			goto _st147;}
		else {
			switch( ( (*( p))) ) {
				case 67: {
					goto _st148;
				}
				case 99: {
					goto _st148;
				}
			}
			goto _st0;
		}
		_st148:
		if ( p == eof )
			goto _out148;
		p+= 1;
		st_case_148:
		if ( p == pe && p != eof )
			goto _out148;
		if ( p == eof ) {
			goto _st148;}
		else {
			switch( ( (*( p))) ) {
				case 75: {
					goto _st149;
				}
				case 107: {
					goto _st149;
				}
			}
			goto _st0;
		}
		_st149:
		if ( p == eof )
			goto _out149;
		p+= 1;
		st_case_149:
		if ( p == pe && p != eof )
			goto _out149;
		if ( p == eof ) {
			goto _st149;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st149;
				}
				case 32: {
					goto _st149;
				}
				case 61: {
					goto _st150;
				}
			}
			goto _st0;
		}
		_st150:
		if ( p == eof )
			goto _out150;
		p+= 1;
		st_case_150:
		if ( p == pe && p != eof )
			goto _out150;
		if ( p == eof ) {
			goto _st150;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st150;
				}
				case 32: {
					goto _st150;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr203;
			}
			goto _st0;
		}
		_ctr203:
		{
#line 474 "crontab.rl"
			
			ncs.intv_st = p;
			ncs.v_int = ncs.v_int2 = 0;
			ncs.intv2_exist = false;
		}
		
#line 7520 "crontab.cpp"
		
		goto _st243;
		_ctr366:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 243; goto _out;}
			}
		}
		
#line 7533 "crontab.cpp"
		
		{
#line 543 "crontab.rl"
			ncs.setlim(RLIMIT_STACK); }
		
#line 7539 "crontab.cpp"
		
		goto _st243;
		_st243:
		if ( p == eof )
			goto _out243;
		p+= 1;
		st_case_243:
		if ( p == pe && p != eof )
			goto _out243;
		if ( p == eof ) {
			goto _ctr366;}
		else {
			if ( ( (*( p))) == 44 ) {
				goto _ctr367;
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st243;
			}
			goto _st0;
		}
		_ctr367:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 151; goto _out;}
			}
		}
		
#line 7570 "crontab.cpp"
		
		goto _st151;
		_st151:
		if ( p == eof )
			goto _out151;
		p+= 1;
		st_case_151:
		if ( p == pe && p != eof )
			goto _out151;
		if ( p == eof ) {
			goto _st151;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr205;
			}
			goto _st0;
		}
		_ctr205:
		{
#line 485 "crontab.rl"
			ncs.intv2_st = p; }
		
#line 7593 "crontab.cpp"
		
		goto _st244;
		_ctr369:
		{
#line 486 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV2())) ncs.v_int2 = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 244; goto _out;}
			}
			ncs.intv2_exist = true;
		}
		
#line 7607 "crontab.cpp"
		
		{
#line 543 "crontab.rl"
			ncs.setlim(RLIMIT_STACK); }
		
#line 7613 "crontab.cpp"
		
		goto _st244;
		_st244:
		if ( p == eof )
			goto _out244;
		p+= 1;
		st_case_244:
		if ( p == pe && p != eof )
			goto _out244;
		if ( p == eof ) {
			goto _ctr369;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st244;
			}
			goto _st0;
		}
		_st152:
		if ( p == eof )
			goto _out152;
		p+= 1;
		st_case_152:
		if ( p == pe && p != eof )
			goto _out152;
		if ( p == eof ) {
			goto _st152;}
		else {
			switch( ( (*( p))) ) {
				case 65: {
					goto _st153;
				}
				case 73: {
					goto _st160;
				}
				case 79: {
					goto _st167;
				}
				case 97: {
					goto _st153;
				}
				case 105: {
					goto _st160;
				}
				case 111: {
					goto _st167;
				}
			}
			goto _st0;
		}
		_st153:
		if ( p == eof )
			goto _out153;
		p+= 1;
		st_case_153:
		if ( p == pe && p != eof )
			goto _out153;
		if ( p == eof ) {
			goto _st153;}
		else {
			switch( ( (*( p))) ) {
				case 88: {
					goto _st154;
				}
				case 120: {
					goto _st154;
				}
			}
			goto _st0;
		}
		_st154:
		if ( p == eof )
			goto _out154;
		p+= 1;
		st_case_154:
		if ( p == pe && p != eof )
			goto _out154;
		if ( p == eof ) {
			goto _st154;}
		else {
			switch( ( (*( p))) ) {
				case 82: {
					goto _st155;
				}
				case 114: {
					goto _st155;
				}
			}
			goto _st0;
		}
		_st155:
		if ( p == eof )
			goto _out155;
		p+= 1;
		st_case_155:
		if ( p == pe && p != eof )
			goto _out155;
		if ( p == eof ) {
			goto _st155;}
		else {
			switch( ( (*( p))) ) {
				case 85: {
					goto _st156;
				}
				case 117: {
					goto _st156;
				}
			}
			goto _st0;
		}
		_st156:
		if ( p == eof )
			goto _out156;
		p+= 1;
		st_case_156:
		if ( p == pe && p != eof )
			goto _out156;
		if ( p == eof ) {
			goto _st156;}
		else {
			switch( ( (*( p))) ) {
				case 78: {
					goto _st157;
				}
				case 110: {
					goto _st157;
				}
			}
			goto _st0;
		}
		_st157:
		if ( p == eof )
			goto _out157;
		p+= 1;
		st_case_157:
		if ( p == pe && p != eof )
			goto _out157;
		if ( p == eof ) {
			goto _st157;}
		else {
			switch( ( (*( p))) ) {
				case 83: {
					goto _st158;
				}
				case 115: {
					goto _st158;
				}
			}
			goto _st0;
		}
		_st158:
		if ( p == eof )
			goto _out158;
		p+= 1;
		st_case_158:
		if ( p == pe && p != eof )
			goto _out158;
		if ( p == eof ) {
			goto _st158;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st158;
				}
				case 32: {
					goto _st158;
				}
				case 61: {
					goto _st159;
				}
			}
			goto _st0;
		}
		_st159:
		if ( p == eof )
			goto _out159;
		p+= 1;
		st_case_159:
		if ( p == pe && p != eof )
			goto _out159;
		if ( p == eof ) {
			goto _st159;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st159;
				}
				case 32: {
					goto _st159;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr215;
			}
			goto _st0;
		}
		_ctr215:
		{
#line 474 "crontab.rl"
			
			ncs.intv_st = p;
			ncs.v_int = ncs.v_int2 = 0;
			ncs.intv2_exist = false;
		}
		
#line 7818 "crontab.cpp"
		
		goto _st245;
		_ctr371:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 245; goto _out;}
			}
		}
		
#line 7831 "crontab.cpp"
		
		{
#line 529 "crontab.rl"
			
			if (!ncs.runat)
			ncs.ce->maxruns = ncs.v_int > 0 ? static_cast<unsigned>(ncs.v_int) : 0;
		}
		
#line 7840 "crontab.cpp"
		
		goto _st245;
		_st245:
		if ( p == eof )
			goto _out245;
		p+= 1;
		st_case_245:
		if ( p == pe && p != eof )
			goto _out245;
		if ( p == eof ) {
			goto _ctr371;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st245;
			}
			goto _st0;
		}
		_st160:
		if ( p == eof )
			goto _out160;
		p+= 1;
		st_case_160:
		if ( p == pe && p != eof )
			goto _out160;
		if ( p == eof ) {
			goto _st160;}
		else {
			switch( ( (*( p))) ) {
				case 78: {
					goto _st161;
				}
				case 110: {
					goto _st161;
				}
			}
			goto _st0;
		}
		_st161:
		if ( p == eof )
			goto _out161;
		p+= 1;
		st_case_161:
		if ( p == pe && p != eof )
			goto _out161;
		if ( p == eof ) {
			goto _st161;}
		else {
			switch( ( (*( p))) ) {
				case 85: {
					goto _st162;
				}
				case 117: {
					goto _st162;
				}
			}
			goto _st0;
		}
		_st162:
		if ( p == eof )
			goto _out162;
		p+= 1;
		st_case_162:
		if ( p == pe && p != eof )
			goto _out162;
		if ( p == eof ) {
			goto _st162;}
		else {
			switch( ( (*( p))) ) {
				case 84: {
					goto _st163;
				}
				case 116: {
					goto _st163;
				}
			}
			goto _st0;
		}
		_st163:
		if ( p == eof )
			goto _out163;
		p+= 1;
		st_case_163:
		if ( p == pe && p != eof )
			goto _out163;
		if ( p == eof ) {
			goto _st163;}
		else {
			switch( ( (*( p))) ) {
				case 69: {
					goto _st164;
				}
				case 101: {
					goto _st164;
				}
			}
			goto _st0;
		}
		_st164:
		if ( p == eof )
			goto _out164;
		p+= 1;
		st_case_164:
		if ( p == pe && p != eof )
			goto _out164;
		if ( p == eof ) {
			goto _st164;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st164;
				}
				case 32: {
					goto _st164;
				}
				case 61: {
					goto _st165;
				}
			}
			goto _st0;
		}
		_st165:
		if ( p == eof )
			goto _out165;
		p+= 1;
		st_case_165:
		if ( p == pe && p != eof )
			goto _out165;
		if ( p == eof ) {
			goto _st165;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st165;
				}
				case 32: {
					goto _st165;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr221;
			}
			goto _st0;
		}
		_ctr221:
		{
#line 474 "crontab.rl"
			
			ncs.intv_st = p;
			ncs.v_int = ncs.v_int2 = 0;
			ncs.intv2_exist = false;
		}
		
#line 7993 "crontab.cpp"
		
		goto _st246;
		_ctr373:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 246; goto _out;}
			}
		}
		
#line 8006 "crontab.cpp"
		
		{
#line 577 "crontab.rl"
			addcstlist(ncs, ncs.ce->minute, 60, 0, 59); }
		
#line 8012 "crontab.cpp"
		
		goto _st246;
		_st246:
		if ( p == eof )
			goto _out246;
		p+= 1;
		st_case_246:
		if ( p == pe && p != eof )
			goto _out246;
		if ( p == eof ) {
			goto _ctr373;}
		else {
			if ( ( (*( p))) == 44 ) {
				goto _ctr374;
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st246;
			}
			goto _st0;
		}
		_ctr374:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 166; goto _out;}
			}
		}
		
#line 8043 "crontab.cpp"
		
		goto _st166;
		_st166:
		if ( p == eof )
			goto _out166;
		p+= 1;
		st_case_166:
		if ( p == pe && p != eof )
			goto _out166;
		if ( p == eof ) {
			goto _st166;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr223;
			}
			goto _st0;
		}
		_ctr223:
		{
#line 485 "crontab.rl"
			ncs.intv2_st = p; }
		
#line 8066 "crontab.cpp"
		
		goto _st247;
		_ctr376:
		{
#line 486 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV2())) ncs.v_int2 = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 247; goto _out;}
			}
			ncs.intv2_exist = true;
		}
		
#line 8080 "crontab.cpp"
		
		{
#line 577 "crontab.rl"
			addcstlist(ncs, ncs.ce->minute, 60, 0, 59); }
		
#line 8086 "crontab.cpp"
		
		goto _st247;
		_st247:
		if ( p == eof )
			goto _out247;
		p+= 1;
		st_case_247:
		if ( p == pe && p != eof )
			goto _out247;
		if ( p == eof ) {
			goto _ctr376;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st247;
			}
			goto _st0;
		}
		_st167:
		if ( p == eof )
			goto _out167;
		p+= 1;
		st_case_167:
		if ( p == pe && p != eof )
			goto _out167;
		if ( p == eof ) {
			goto _st167;}
		else {
			switch( ( (*( p))) ) {
				case 78: {
					goto _st168;
				}
				case 110: {
					goto _st168;
				}
			}
			goto _st0;
		}
		_st168:
		if ( p == eof )
			goto _out168;
		p+= 1;
		st_case_168:
		if ( p == pe && p != eof )
			goto _out168;
		if ( p == eof ) {
			goto _st168;}
		else {
			switch( ( (*( p))) ) {
				case 84: {
					goto _st169;
				}
				case 116: {
					goto _st169;
				}
			}
			goto _st0;
		}
		_st169:
		if ( p == eof )
			goto _out169;
		p+= 1;
		st_case_169:
		if ( p == pe && p != eof )
			goto _out169;
		if ( p == eof ) {
			goto _st169;}
		else {
			switch( ( (*( p))) ) {
				case 72: {
					goto _st170;
				}
				case 104: {
					goto _st170;
				}
			}
			goto _st0;
		}
		_st170:
		if ( p == eof )
			goto _out170;
		p+= 1;
		st_case_170:
		if ( p == pe && p != eof )
			goto _out170;
		if ( p == eof ) {
			goto _st170;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st170;
				}
				case 32: {
					goto _st170;
				}
				case 61: {
					goto _st171;
				}
			}
			goto _st0;
		}
		_st171:
		if ( p == eof )
			goto _out171;
		p+= 1;
		st_case_171:
		if ( p == pe && p != eof )
			goto _out171;
		if ( p == eof ) {
			goto _st171;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st171;
				}
				case 32: {
					goto _st171;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr228;
			}
			goto _st0;
		}
		_ctr228:
		{
#line 474 "crontab.rl"
			
			ncs.intv_st = p;
			ncs.v_int = ncs.v_int2 = 0;
			ncs.intv2_exist = false;
		}
		
#line 8219 "crontab.cpp"
		
		goto _st248;
		_ctr378:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 248; goto _out;}
			}
		}
		
#line 8232 "crontab.cpp"
		
		{
#line 573 "crontab.rl"
			addcstlist(ncs, ncs.ce->month, 0, 1, 12); }
		
#line 8238 "crontab.cpp"
		
		goto _st248;
		_st248:
		if ( p == eof )
			goto _out248;
		p+= 1;
		st_case_248:
		if ( p == pe && p != eof )
			goto _out248;
		if ( p == eof ) {
			goto _ctr378;}
		else {
			if ( ( (*( p))) == 44 ) {
				goto _ctr379;
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st248;
			}
			goto _st0;
		}
		_ctr379:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 172; goto _out;}
			}
		}
		
#line 8269 "crontab.cpp"
		
		goto _st172;
		_st172:
		if ( p == eof )
			goto _out172;
		p+= 1;
		st_case_172:
		if ( p == pe && p != eof )
			goto _out172;
		if ( p == eof ) {
			goto _st172;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr230;
			}
			goto _st0;
		}
		_ctr230:
		{
#line 485 "crontab.rl"
			ncs.intv2_st = p; }
		
#line 8292 "crontab.cpp"
		
		goto _st249;
		_ctr381:
		{
#line 486 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV2())) ncs.v_int2 = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 249; goto _out;}
			}
			ncs.intv2_exist = true;
		}
		
#line 8306 "crontab.cpp"
		
		{
#line 573 "crontab.rl"
			addcstlist(ncs, ncs.ce->month, 0, 1, 12); }
		
#line 8312 "crontab.cpp"
		
		goto _st249;
		_st249:
		if ( p == eof )
			goto _out249;
		p+= 1;
		st_case_249:
		if ( p == pe && p != eof )
			goto _out249;
		if ( p == eof ) {
			goto _ctr381;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st249;
			}
			goto _st0;
		}
		_st173:
		if ( p == eof )
			goto _out173;
		p+= 1;
		st_case_173:
		if ( p == pe && p != eof )
			goto _out173;
		if ( p == eof ) {
			goto _st173;}
		else {
			switch( ( (*( p))) ) {
				case 65: {
					goto _st174;
				}
				case 97: {
					goto _st174;
				}
			}
			goto _st0;
		}
		_st174:
		if ( p == eof )
			goto _out174;
		p+= 1;
		st_case_174:
		if ( p == pe && p != eof )
			goto _out174;
		if ( p == eof ) {
			goto _st174;}
		else {
			switch( ( (*( p))) ) {
				case 84: {
					goto _st175;
				}
				case 116: {
					goto _st175;
				}
			}
			goto _st0;
		}
		_st175:
		if ( p == eof )
			goto _out175;
		p+= 1;
		st_case_175:
		if ( p == pe && p != eof )
			goto _out175;
		if ( p == eof ) {
			goto _st175;}
		else {
			switch( ( (*( p))) ) {
				case 72: {
					goto _st176;
				}
				case 104: {
					goto _st176;
				}
			}
			goto _st0;
		}
		_st176:
		if ( p == eof )
			goto _out176;
		p+= 1;
		st_case_176:
		if ( p == pe && p != eof )
			goto _out176;
		if ( p == eof ) {
			goto _st176;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st176;
				}
				case 32: {
					goto _st176;
				}
				case 61: {
					goto _st177;
				}
			}
			goto _st0;
		}
		_st177:
		if ( p == eof )
			goto _out177;
		p+= 1;
		st_case_177:
		if ( p == pe && p != eof )
			goto _out177;
		if ( p == eof ) {
			goto _st177;}
		else {
			switch( ( (*( p))) ) {
				case 0: {
					goto _st0;
				}
				case 9: {
					goto _ctr236;
				}
				case 10: {
					goto _st0;
				}
				case 32: {
					goto _ctr236;
				}
			}
			goto _ctr235;
		}
		_ctr235:
		{
#line 494 "crontab.rl"
			ncs.strv_st = p; ncs.v_strlen = 0; }
		
#line 8444 "crontab.cpp"
		
		goto _st250;
		_ctr383:
		{
#line 495 "crontab.rl"
			
			ncs.v_strlen = p > ncs.strv_st ? static_cast<size_t>(p - ncs.strv_st) : 0;
			if (ncs.v_strlen <= INT_MAX) {
				ssize_t snl = snprintf(ncs.v_str, sizeof ncs.v_str,
				"%.*s", (int)ncs.v_strlen, ncs.strv_st);
				if (snl < 0 || (size_t)snl >= sizeof ncs.v_str) {
					log_line("error parsing line %zu in crontab; too long?", ncs.linenum);
					std::exit(EXIT_FAILURE);
				}
			}
		}
		
#line 8462 "crontab.cpp"
		
		{
#line 591 "crontab.rl"
			
			ncs.ce->path = std::string(ncs.v_str, ncs.v_strlen);
		}
		
#line 8470 "crontab.cpp"
		
		goto _st250;
		_st250:
		if ( p == eof )
			goto _out250;
		p+= 1;
		st_case_250:
		if ( p == pe && p != eof )
			goto _out250;
		if ( p == eof ) {
			goto _ctr383;}
		else {
			switch( ( (*( p))) ) {
				case 0: {
					goto _st0;
				}
				case 10: {
					goto _st0;
				}
			}
			goto _st250;
		}
		_ctr236:
		{
#line 494 "crontab.rl"
			ncs.strv_st = p; ncs.v_strlen = 0; }
		
#line 8498 "crontab.cpp"
		
		goto _st251;
		_ctr385:
		{
#line 495 "crontab.rl"
			
			ncs.v_strlen = p > ncs.strv_st ? static_cast<size_t>(p - ncs.strv_st) : 0;
			if (ncs.v_strlen <= INT_MAX) {
				ssize_t snl = snprintf(ncs.v_str, sizeof ncs.v_str,
				"%.*s", (int)ncs.v_strlen, ncs.strv_st);
				if (snl < 0 || (size_t)snl >= sizeof ncs.v_str) {
					log_line("error parsing line %zu in crontab; too long?", ncs.linenum);
					std::exit(EXIT_FAILURE);
				}
			}
		}
		
#line 8516 "crontab.cpp"
		
		{
#line 591 "crontab.rl"
			
			ncs.ce->path = std::string(ncs.v_str, ncs.v_strlen);
		}
		
#line 8524 "crontab.cpp"
		
		goto _st251;
		_st251:
		if ( p == eof )
			goto _out251;
		p+= 1;
		st_case_251:
		if ( p == pe && p != eof )
			goto _out251;
		if ( p == eof ) {
			goto _ctr385;}
		else {
			switch( ( (*( p))) ) {
				case 0: {
					goto _st0;
				}
				case 9: {
					goto _ctr236;
				}
				case 10: {
					goto _st0;
				}
				case 32: {
					goto _ctr236;
				}
			}
			goto _ctr235;
		}
		_st178:
		if ( p == eof )
			goto _out178;
		p+= 1;
		st_case_178:
		if ( p == pe && p != eof )
			goto _out178;
		if ( p == eof ) {
			goto _st178;}
		else {
			switch( ( (*( p))) ) {
				case 85: {
					goto _st179;
				}
				case 117: {
					goto _st179;
				}
			}
			goto _st0;
		}
		_st179:
		if ( p == eof )
			goto _out179;
		p+= 1;
		st_case_179:
		if ( p == pe && p != eof )
			goto _out179;
		if ( p == eof ) {
			goto _st179;}
		else {
			switch( ( (*( p))) ) {
				case 78: {
					goto _st180;
				}
				case 110: {
					goto _st180;
				}
			}
			goto _st0;
		}
		_st180:
		if ( p == eof )
			goto _out180;
		p+= 1;
		st_case_180:
		if ( p == pe && p != eof )
			goto _out180;
		if ( p == eof ) {
			goto _st180;}
		else {
			switch( ( (*( p))) ) {
				case 65: {
					goto _st181;
				}
				case 97: {
					goto _st181;
				}
			}
			goto _st0;
		}
		_st181:
		if ( p == eof )
			goto _out181;
		p+= 1;
		st_case_181:
		if ( p == pe && p != eof )
			goto _out181;
		if ( p == eof ) {
			goto _st181;}
		else {
			switch( ( (*( p))) ) {
				case 84: {
					goto _st182;
				}
				case 116: {
					goto _st182;
				}
			}
			goto _st0;
		}
		_st182:
		if ( p == eof )
			goto _out182;
		p+= 1;
		st_case_182:
		if ( p == pe && p != eof )
			goto _out182;
		if ( p == eof ) {
			goto _st182;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st182;
				}
				case 32: {
					goto _st182;
				}
				case 61: {
					goto _st183;
				}
			}
			goto _st0;
		}
		_st183:
		if ( p == eof )
			goto _out183;
		p+= 1;
		st_case_183:
		if ( p == pe && p != eof )
			goto _out183;
		if ( p == eof ) {
			goto _st183;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st183;
				}
				case 32: {
					goto _st183;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr242;
			}
			goto _st0;
		}
		_ctr242:
		{
#line 474 "crontab.rl"
			
			ncs.intv_st = p;
			ncs.v_int = ncs.v_int2 = 0;
			ncs.intv2_exist = false;
		}
		
#line 8688 "crontab.cpp"
		
		goto _st252;
		_ctr386:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 252; goto _out;}
			}
		}
		
#line 8701 "crontab.cpp"
		
		{
#line 523 "crontab.rl"
			
			ncs.runat = true;
			ncs.ce->exectime = ncs.v_int;
			ncs.ce->maxruns = 1;
			ncs.ce->journal = true;
		}
		
#line 8712 "crontab.cpp"
		
		goto _st252;
		_st252:
		if ( p == eof )
			goto _out252;
		p+= 1;
		st_case_252:
		if ( p == pe && p != eof )
			goto _out252;
		if ( p == eof ) {
			goto _ctr386;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st252;
			}
			goto _st0;
		}
		_st184:
		if ( p == eof )
			goto _out184;
		p+= 1;
		st_case_184:
		if ( p == pe && p != eof )
			goto _out184;
		if ( p == eof ) {
			goto _st184;}
		else {
			switch( ( (*( p))) ) {
				case 83: {
					goto _st185;
				}
				case 115: {
					goto _st185;
				}
			}
			goto _st0;
		}
		_st185:
		if ( p == eof )
			goto _out185;
		p+= 1;
		st_case_185:
		if ( p == pe && p != eof )
			goto _out185;
		if ( p == eof ) {
			goto _st185;}
		else {
			switch( ( (*( p))) ) {
				case 69: {
					goto _st186;
				}
				case 101: {
					goto _st186;
				}
			}
			goto _st0;
		}
		_st186:
		if ( p == eof )
			goto _out186;
		p+= 1;
		st_case_186:
		if ( p == pe && p != eof )
			goto _out186;
		if ( p == eof ) {
			goto _st186;}
		else {
			switch( ( (*( p))) ) {
				case 82: {
					goto _st187;
				}
				case 114: {
					goto _st187;
				}
			}
			goto _st0;
		}
		_st187:
		if ( p == eof )
			goto _out187;
		p+= 1;
		st_case_187:
		if ( p == pe && p != eof )
			goto _out187;
		if ( p == eof ) {
			goto _st187;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st187;
				}
				case 32: {
					goto _st187;
				}
				case 61: {
					goto _st188;
				}
			}
			goto _st0;
		}
		_st188:
		if ( p == eof )
			goto _out188;
		p+= 1;
		st_case_188:
		if ( p == pe && p != eof )
			goto _out188;
		if ( p == eof ) {
			goto _st188;}
		else {
			switch( ( (*( p))) ) {
				case 0: {
					goto _st0;
				}
				case 9: {
					goto _ctr248;
				}
				case 10: {
					goto _st0;
				}
				case 32: {
					goto _ctr248;
				}
			}
			goto _ctr247;
		}
		_ctr247:
		{
#line 494 "crontab.rl"
			ncs.strv_st = p; ncs.v_strlen = 0; }
		
#line 8844 "crontab.cpp"
		
		goto _st253;
		_ctr388:
		{
#line 495 "crontab.rl"
			
			ncs.v_strlen = p > ncs.strv_st ? static_cast<size_t>(p - ncs.strv_st) : 0;
			if (ncs.v_strlen <= INT_MAX) {
				ssize_t snl = snprintf(ncs.v_str, sizeof ncs.v_str,
				"%.*s", (int)ncs.v_strlen, ncs.strv_st);
				if (snl < 0 || (size_t)snl >= sizeof ncs.v_str) {
					log_line("error parsing line %zu in crontab; too long?", ncs.linenum);
					std::exit(EXIT_FAILURE);
				}
			}
		}
		
#line 8862 "crontab.cpp"
		
		{
#line 586 "crontab.rl"
			ncs.setuserv(); }
		
#line 8868 "crontab.cpp"
		
		goto _st253;
		_st253:
		if ( p == eof )
			goto _out253;
		p+= 1;
		st_case_253:
		if ( p == pe && p != eof )
			goto _out253;
		if ( p == eof ) {
			goto _ctr388;}
		else {
			switch( ( (*( p))) ) {
				case 0: {
					goto _st0;
				}
				case 10: {
					goto _st0;
				}
			}
			goto _st253;
		}
		_ctr248:
		{
#line 494 "crontab.rl"
			ncs.strv_st = p; ncs.v_strlen = 0; }
		
#line 8896 "crontab.cpp"
		
		goto _st254;
		_ctr390:
		{
#line 495 "crontab.rl"
			
			ncs.v_strlen = p > ncs.strv_st ? static_cast<size_t>(p - ncs.strv_st) : 0;
			if (ncs.v_strlen <= INT_MAX) {
				ssize_t snl = snprintf(ncs.v_str, sizeof ncs.v_str,
				"%.*s", (int)ncs.v_strlen, ncs.strv_st);
				if (snl < 0 || (size_t)snl >= sizeof ncs.v_str) {
					log_line("error parsing line %zu in crontab; too long?", ncs.linenum);
					std::exit(EXIT_FAILURE);
				}
			}
		}
		
#line 8914 "crontab.cpp"
		
		{
#line 586 "crontab.rl"
			ncs.setuserv(); }
		
#line 8920 "crontab.cpp"
		
		goto _st254;
		_st254:
		if ( p == eof )
			goto _out254;
		p+= 1;
		st_case_254:
		if ( p == pe && p != eof )
			goto _out254;
		if ( p == eof ) {
			goto _ctr390;}
		else {
			switch( ( (*( p))) ) {
				case 0: {
					goto _st0;
				}
				case 9: {
					goto _ctr248;
				}
				case 10: {
					goto _st0;
				}
				case 32: {
					goto _ctr248;
				}
			}
			goto _ctr247;
		}
		_st189:
		if ( p == eof )
			goto _out189;
		p+= 1;
		st_case_189:
		if ( p == pe && p != eof )
			goto _out189;
		if ( p == eof ) {
			goto _st189;}
		else {
			switch( ( (*( p))) ) {
				case 69: {
					goto _st190;
				}
				case 101: {
					goto _st190;
				}
			}
			goto _st0;
		}
		_st190:
		if ( p == eof )
			goto _out190;
		p+= 1;
		st_case_190:
		if ( p == pe && p != eof )
			goto _out190;
		if ( p == eof ) {
			goto _st190;}
		else {
			switch( ( (*( p))) ) {
				case 69: {
					goto _st191;
				}
				case 101: {
					goto _st191;
				}
			}
			goto _st0;
		}
		_st191:
		if ( p == eof )
			goto _out191;
		p+= 1;
		st_case_191:
		if ( p == pe && p != eof )
			goto _out191;
		if ( p == eof ) {
			goto _st191;}
		else {
			switch( ( (*( p))) ) {
				case 75: {
					goto _st192;
				}
				case 107: {
					goto _st192;
				}
			}
			goto _st0;
		}
		_st192:
		if ( p == eof )
			goto _out192;
		p+= 1;
		st_case_192:
		if ( p == pe && p != eof )
			goto _out192;
		if ( p == eof ) {
			goto _st192;}
		else {
			switch( ( (*( p))) ) {
				case 68: {
					goto _st193;
				}
				case 100: {
					goto _st193;
				}
			}
			goto _st0;
		}
		_st193:
		if ( p == eof )
			goto _out193;
		p+= 1;
		st_case_193:
		if ( p == pe && p != eof )
			goto _out193;
		if ( p == eof ) {
			goto _st193;}
		else {
			switch( ( (*( p))) ) {
				case 65: {
					goto _st194;
				}
				case 97: {
					goto _st194;
				}
			}
			goto _st0;
		}
		_st194:
		if ( p == eof )
			goto _out194;
		p+= 1;
		st_case_194:
		if ( p == pe && p != eof )
			goto _out194;
		if ( p == eof ) {
			goto _st194;}
		else {
			switch( ( (*( p))) ) {
				case 89: {
					goto _st195;
				}
				case 121: {
					goto _st195;
				}
			}
			goto _st0;
		}
		_st195:
		if ( p == eof )
			goto _out195;
		p+= 1;
		st_case_195:
		if ( p == pe && p != eof )
			goto _out195;
		if ( p == eof ) {
			goto _st195;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st195;
				}
				case 32: {
					goto _st195;
				}
				case 61: {
					goto _st196;
				}
			}
			goto _st0;
		}
		_st196:
		if ( p == eof )
			goto _out196;
		p+= 1;
		st_case_196:
		if ( p == pe && p != eof )
			goto _out196;
		if ( p == eof ) {
			goto _st196;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _st196;
				}
				case 32: {
					goto _st196;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr256;
			}
			goto _st0;
		}
		_ctr256:
		{
#line 474 "crontab.rl"
			
			ncs.intv_st = p;
			ncs.v_int = ncs.v_int2 = 0;
			ncs.intv2_exist = false;
		}
		
#line 9124 "crontab.cpp"
		
		goto _st255;
		_ctr391:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 255; goto _out;}
			}
		}
		
#line 9137 "crontab.cpp"
		
		{
#line 575 "crontab.rl"
			addcstlist(ncs, ncs.ce->weekday, 0, 1, 7); }
		
#line 9143 "crontab.cpp"
		
		goto _st255;
		_st255:
		if ( p == eof )
			goto _out255;
		p+= 1;
		st_case_255:
		if ( p == pe && p != eof )
			goto _out255;
		if ( p == eof ) {
			goto _ctr391;}
		else {
			if ( ( (*( p))) == 44 ) {
				goto _ctr392;
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st255;
			}
			goto _st0;
		}
		_ctr392:
		{
#line 479 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 197; goto _out;}
			}
		}
		
#line 9174 "crontab.cpp"
		
		goto _st197;
		_st197:
		if ( p == eof )
			goto _out197;
		p+= 1;
		st_case_197:
		if ( p == pe && p != eof )
			goto _out197;
		if ( p == eof ) {
			goto _st197;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr258;
			}
			goto _st0;
		}
		_ctr258:
		{
#line 485 "crontab.rl"
			ncs.intv2_st = p; }
		
#line 9197 "crontab.cpp"
		
		goto _st256;
		_ctr394:
		{
#line 486 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV2())) ncs.v_int2 = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 256; goto _out;}
			}
			ncs.intv2_exist = true;
		}
		
#line 9211 "crontab.cpp"
		
		{
#line 575 "crontab.rl"
			addcstlist(ncs, ncs.ce->weekday, 0, 1, 7); }
		
#line 9217 "crontab.cpp"
		
		goto _st256;
		_st256:
		if ( p == eof )
			goto _out256;
		p+= 1;
		st_case_256:
		if ( p == pe && p != eof )
			goto _out256;
		if ( p == eof ) {
			goto _ctr394;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st256;
			}
			goto _st0;
		}
		_out1: ncs.cs = 1; goto _out; 
		_out0: ncs.cs = 0; goto _out; 
		_out2: ncs.cs = 2; goto _out; 
		_out198: ncs.cs = 198; goto _out; 
		_out3: ncs.cs = 3; goto _out; 
		_out4: ncs.cs = 4; goto _out; 
		_out5: ncs.cs = 5; goto _out; 
		_out6: ncs.cs = 6; goto _out; 
		_out7: ncs.cs = 7; goto _out; 
		_out8: ncs.cs = 8; goto _out; 
		_out9: ncs.cs = 9; goto _out; 
		_out199: ncs.cs = 199; goto _out; 
		_out200: ncs.cs = 200; goto _out; 
		_out10: ncs.cs = 10; goto _out; 
		_out11: ncs.cs = 11; goto _out; 
		_out12: ncs.cs = 12; goto _out; 
		_out13: ncs.cs = 13; goto _out; 
		_out14: ncs.cs = 14; goto _out; 
		_out15: ncs.cs = 15; goto _out; 
		_out16: ncs.cs = 16; goto _out; 
		_out201: ncs.cs = 201; goto _out; 
		_out202: ncs.cs = 202; goto _out; 
		_out17: ncs.cs = 17; goto _out; 
		_out18: ncs.cs = 18; goto _out; 
		_out19: ncs.cs = 19; goto _out; 
		_out20: ncs.cs = 20; goto _out; 
		_out203: ncs.cs = 203; goto _out; 
		_out21: ncs.cs = 21; goto _out; 
		_out204: ncs.cs = 204; goto _out; 
		_out22: ncs.cs = 22; goto _out; 
		_out23: ncs.cs = 23; goto _out; 
		_out24: ncs.cs = 24; goto _out; 
		_out25: ncs.cs = 25; goto _out; 
		_out26: ncs.cs = 26; goto _out; 
		_out27: ncs.cs = 27; goto _out; 
		_out205: ncs.cs = 205; goto _out; 
		_out206: ncs.cs = 206; goto _out; 
		_out28: ncs.cs = 28; goto _out; 
		_out29: ncs.cs = 29; goto _out; 
		_out30: ncs.cs = 30; goto _out; 
		_out31: ncs.cs = 31; goto _out; 
		_out32: ncs.cs = 32; goto _out; 
		_out207: ncs.cs = 207; goto _out; 
		_out33: ncs.cs = 33; goto _out; 
		_out208: ncs.cs = 208; goto _out; 
		_out34: ncs.cs = 34; goto _out; 
		_out35: ncs.cs = 35; goto _out; 
		_out36: ncs.cs = 36; goto _out; 
		_out37: ncs.cs = 37; goto _out; 
		_out38: ncs.cs = 38; goto _out; 
		_out39: ncs.cs = 39; goto _out; 
		_out40: ncs.cs = 40; goto _out; 
		_out41: ncs.cs = 41; goto _out; 
		_out42: ncs.cs = 42; goto _out; 
		_out43: ncs.cs = 43; goto _out; 
		_out209: ncs.cs = 209; goto _out; 
		_out210: ncs.cs = 210; goto _out; 
		_out211: ncs.cs = 211; goto _out; 
		_out212: ncs.cs = 212; goto _out; 
		_out213: ncs.cs = 213; goto _out; 
		_out44: ncs.cs = 44; goto _out; 
		_out45: ncs.cs = 45; goto _out; 
		_out46: ncs.cs = 46; goto _out; 
		_out47: ncs.cs = 47; goto _out; 
		_out48: ncs.cs = 48; goto _out; 
		_out49: ncs.cs = 49; goto _out; 
		_out214: ncs.cs = 214; goto _out; 
		_out50: ncs.cs = 50; goto _out; 
		_out51: ncs.cs = 51; goto _out; 
		_out52: ncs.cs = 52; goto _out; 
		_out53: ncs.cs = 53; goto _out; 
		_out54: ncs.cs = 54; goto _out; 
		_out215: ncs.cs = 215; goto _out; 
		_out55: ncs.cs = 55; goto _out; 
		_out216: ncs.cs = 216; goto _out; 
		_out56: ncs.cs = 56; goto _out; 
		_out57: ncs.cs = 57; goto _out; 
		_out58: ncs.cs = 58; goto _out; 
		_out59: ncs.cs = 59; goto _out; 
		_out60: ncs.cs = 60; goto _out; 
		_out217: ncs.cs = 217; goto _out; 
		_out61: ncs.cs = 61; goto _out; 
		_out218: ncs.cs = 218; goto _out; 
		_out62: ncs.cs = 62; goto _out; 
		_out63: ncs.cs = 63; goto _out; 
		_out64: ncs.cs = 64; goto _out; 
		_out219: ncs.cs = 219; goto _out; 
		_out65: ncs.cs = 65; goto _out; 
		_out220: ncs.cs = 220; goto _out; 
		_out66: ncs.cs = 66; goto _out; 
		_out67: ncs.cs = 67; goto _out; 
		_out68: ncs.cs = 68; goto _out; 
		_out69: ncs.cs = 69; goto _out; 
		_out70: ncs.cs = 70; goto _out; 
		_out221: ncs.cs = 221; goto _out; 
		_out71: ncs.cs = 71; goto _out; 
		_out222: ncs.cs = 222; goto _out; 
		_out72: ncs.cs = 72; goto _out; 
		_out73: ncs.cs = 73; goto _out; 
		_out74: ncs.cs = 74; goto _out; 
		_out75: ncs.cs = 75; goto _out; 
		_out76: ncs.cs = 76; goto _out; 
		_out77: ncs.cs = 77; goto _out; 
		_out223: ncs.cs = 223; goto _out; 
		_out78: ncs.cs = 78; goto _out; 
		_out224: ncs.cs = 224; goto _out; 
		_out79: ncs.cs = 79; goto _out; 
		_out80: ncs.cs = 80; goto _out; 
		_out81: ncs.cs = 81; goto _out; 
		_out82: ncs.cs = 82; goto _out; 
		_out83: ncs.cs = 83; goto _out; 
		_out84: ncs.cs = 84; goto _out; 
		_out85: ncs.cs = 85; goto _out; 
		_out86: ncs.cs = 86; goto _out; 
		_out225: ncs.cs = 225; goto _out; 
		_out87: ncs.cs = 87; goto _out; 
		_out226: ncs.cs = 226; goto _out; 
		_out88: ncs.cs = 88; goto _out; 
		_out89: ncs.cs = 89; goto _out; 
		_out90: ncs.cs = 90; goto _out; 
		_out91: ncs.cs = 91; goto _out; 
		_out92: ncs.cs = 92; goto _out; 
		_out93: ncs.cs = 93; goto _out; 
		_out94: ncs.cs = 94; goto _out; 
		_out95: ncs.cs = 95; goto _out; 
		_out227: ncs.cs = 227; goto _out; 
		_out96: ncs.cs = 96; goto _out; 
		_out228: ncs.cs = 228; goto _out; 
		_out97: ncs.cs = 97; goto _out; 
		_out98: ncs.cs = 98; goto _out; 
		_out99: ncs.cs = 99; goto _out; 
		_out100: ncs.cs = 100; goto _out; 
		_out101: ncs.cs = 101; goto _out; 
		_out229: ncs.cs = 229; goto _out; 
		_out102: ncs.cs = 102; goto _out; 
		_out230: ncs.cs = 230; goto _out; 
		_out103: ncs.cs = 103; goto _out; 
		_out104: ncs.cs = 104; goto _out; 
		_out105: ncs.cs = 105; goto _out; 
		_out106: ncs.cs = 106; goto _out; 
		_out107: ncs.cs = 107; goto _out; 
		_out108: ncs.cs = 108; goto _out; 
		_out231: ncs.cs = 231; goto _out; 
		_out109: ncs.cs = 109; goto _out; 
		_out232: ncs.cs = 232; goto _out; 
		_out110: ncs.cs = 110; goto _out; 
		_out111: ncs.cs = 111; goto _out; 
		_out112: ncs.cs = 112; goto _out; 
		_out113: ncs.cs = 113; goto _out; 
		_out114: ncs.cs = 114; goto _out; 
		_out233: ncs.cs = 233; goto _out; 
		_out115: ncs.cs = 115; goto _out; 
		_out234: ncs.cs = 234; goto _out; 
		_out116: ncs.cs = 116; goto _out; 
		_out117: ncs.cs = 117; goto _out; 
		_out118: ncs.cs = 118; goto _out; 
		_out119: ncs.cs = 119; goto _out; 
		_out235: ncs.cs = 235; goto _out; 
		_out120: ncs.cs = 120; goto _out; 
		_out236: ncs.cs = 236; goto _out; 
		_out121: ncs.cs = 121; goto _out; 
		_out122: ncs.cs = 122; goto _out; 
		_out123: ncs.cs = 123; goto _out; 
		_out124: ncs.cs = 124; goto _out; 
		_out125: ncs.cs = 125; goto _out; 
		_out126: ncs.cs = 126; goto _out; 
		_out237: ncs.cs = 237; goto _out; 
		_out127: ncs.cs = 127; goto _out; 
		_out238: ncs.cs = 238; goto _out; 
		_out128: ncs.cs = 128; goto _out; 
		_out129: ncs.cs = 129; goto _out; 
		_out130: ncs.cs = 130; goto _out; 
		_out131: ncs.cs = 131; goto _out; 
		_out132: ncs.cs = 132; goto _out; 
		_out239: ncs.cs = 239; goto _out; 
		_out133: ncs.cs = 133; goto _out; 
		_out240: ncs.cs = 240; goto _out; 
		_out134: ncs.cs = 134; goto _out; 
		_out135: ncs.cs = 135; goto _out; 
		_out136: ncs.cs = 136; goto _out; 
		_out137: ncs.cs = 137; goto _out; 
		_out138: ncs.cs = 138; goto _out; 
		_out139: ncs.cs = 139; goto _out; 
		_out140: ncs.cs = 140; goto _out; 
		_out141: ncs.cs = 141; goto _out; 
		_out142: ncs.cs = 142; goto _out; 
		_out143: ncs.cs = 143; goto _out; 
		_out144: ncs.cs = 144; goto _out; 
		_out241: ncs.cs = 241; goto _out; 
		_out145: ncs.cs = 145; goto _out; 
		_out242: ncs.cs = 242; goto _out; 
		_out146: ncs.cs = 146; goto _out; 
		_out147: ncs.cs = 147; goto _out; 
		_out148: ncs.cs = 148; goto _out; 
		_out149: ncs.cs = 149; goto _out; 
		_out150: ncs.cs = 150; goto _out; 
		_out243: ncs.cs = 243; goto _out; 
		_out151: ncs.cs = 151; goto _out; 
		_out244: ncs.cs = 244; goto _out; 
		_out152: ncs.cs = 152; goto _out; 
		_out153: ncs.cs = 153; goto _out; 
		_out154: ncs.cs = 154; goto _out; 
		_out155: ncs.cs = 155; goto _out; 
		_out156: ncs.cs = 156; goto _out; 
		_out157: ncs.cs = 157; goto _out; 
		_out158: ncs.cs = 158; goto _out; 
		_out159: ncs.cs = 159; goto _out; 
		_out245: ncs.cs = 245; goto _out; 
		_out160: ncs.cs = 160; goto _out; 
		_out161: ncs.cs = 161; goto _out; 
		_out162: ncs.cs = 162; goto _out; 
		_out163: ncs.cs = 163; goto _out; 
		_out164: ncs.cs = 164; goto _out; 
		_out165: ncs.cs = 165; goto _out; 
		_out246: ncs.cs = 246; goto _out; 
		_out166: ncs.cs = 166; goto _out; 
		_out247: ncs.cs = 247; goto _out; 
		_out167: ncs.cs = 167; goto _out; 
		_out168: ncs.cs = 168; goto _out; 
		_out169: ncs.cs = 169; goto _out; 
		_out170: ncs.cs = 170; goto _out; 
		_out171: ncs.cs = 171; goto _out; 
		_out248: ncs.cs = 248; goto _out; 
		_out172: ncs.cs = 172; goto _out; 
		_out249: ncs.cs = 249; goto _out; 
		_out173: ncs.cs = 173; goto _out; 
		_out174: ncs.cs = 174; goto _out; 
		_out175: ncs.cs = 175; goto _out; 
		_out176: ncs.cs = 176; goto _out; 
		_out177: ncs.cs = 177; goto _out; 
		_out250: ncs.cs = 250; goto _out; 
		_out251: ncs.cs = 251; goto _out; 
		_out178: ncs.cs = 178; goto _out; 
		_out179: ncs.cs = 179; goto _out; 
		_out180: ncs.cs = 180; goto _out; 
		_out181: ncs.cs = 181; goto _out; 
		_out182: ncs.cs = 182; goto _out; 
		_out183: ncs.cs = 183; goto _out; 
		_out252: ncs.cs = 252; goto _out; 
		_out184: ncs.cs = 184; goto _out; 
		_out185: ncs.cs = 185; goto _out; 
		_out186: ncs.cs = 186; goto _out; 
		_out187: ncs.cs = 187; goto _out; 
		_out188: ncs.cs = 188; goto _out; 
		_out253: ncs.cs = 253; goto _out; 
		_out254: ncs.cs = 254; goto _out; 
		_out189: ncs.cs = 189; goto _out; 
		_out190: ncs.cs = 190; goto _out; 
		_out191: ncs.cs = 191; goto _out; 
		_out192: ncs.cs = 192; goto _out; 
		_out193: ncs.cs = 193; goto _out; 
		_out194: ncs.cs = 194; goto _out; 
		_out195: ncs.cs = 195; goto _out; 
		_out196: ncs.cs = 196; goto _out; 
		_out255: ncs.cs = 255; goto _out; 
		_out197: ncs.cs = 197; goto _out; 
		_out256: ncs.cs = 256; goto _out; 
		_out: {}
	}
	
#line 629 "crontab.rl"
	
	
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
		log_line("%s: failed to open file: '%s': %s", __func__, path.c_str(), strerror(errno));
		std::exit(EXIT_FAILURE);
	}
	SCOPE_EXIT{ fclose(f); };
	while (!feof(f)) {
		if (!fgets(buf, sizeof buf, f)) {
			if (!feof(f))
				log_line("%s: io error fetching line of '%s'", __func__, path.c_str());
			break;
		}
		auto llen = strlen(buf);
		if (llen == 0)
			continue;
		if (buf[llen-1] == '\n')
			buf[--llen] = 0;
		++ncs.linenum;
		if (do_parse_config(ncs, buf, llen) < 0) {
			log_line("%s: do_parse_config(%s) failed at line %zu", __func__, path.c_str(), ncs.linenum);
			std::exit(EXIT_FAILURE);
		}
	}
	std::make_heap(stk.begin(), stk.end(), GtCronEntry);
	history_map.clear();
	cfg_reload = 1;
}

