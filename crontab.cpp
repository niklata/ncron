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
#include "nk/nstrcpy.h"
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
		log_line("path: %s", ce->path.c_str());
		log_line("numruns: %u", ce->numruns);
		log_line("maxruns: %u", ce->maxruns);
		log_line("journal: %s", ce->journal ? "true" : "false");
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


#line 236 "crontab.rl"



#line 208 "crontab.cpp"
static const int history_m_start = 1;
static const int history_m_first_final = 6;
static const int history_m_error = 0;

static const int history_m_en_main = 1;


#line 238 "crontab.rl"


static int do_parse_history(hstm &hst, const char *p, size_t plen)
{
	const char *pe = p + plen;
	const char *eof = pe;
	
	
#line 225 "crontab.cpp"
	{
		hst.cs = (int)history_m_start;
	}
	
#line 245 "crontab.rl"
	
	
#line 233 "crontab.cpp"
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
#line 205 "crontab.rl"
			hst.st = p; }
		
#line 280 "crontab.cpp"
		
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
#line 224 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_HST())) hst.id = *t; else {
				hst.parse_error = true;
				{p+= 1; hst.cs = 3; goto _out;}
			}
		}
		
#line 319 "crontab.cpp"
		
		goto _st3;
		_ctr15:
		{
#line 212 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_HST())) hst.h.set_numruns(*t); else {
				hst.parse_error = true;
				{p+= 1; hst.cs = 3; goto _out;}
			}
		}
		
#line 332 "crontab.cpp"
		
		goto _st3;
		_ctr20:
		{
#line 218 "crontab.rl"
			
			if (auto t = nk::from_string<time_t>(MARKED_HST())) hst.h.set_exectime(*t); else {
				hst.parse_error = true;
				{p+= 1; hst.cs = 3; goto _out;}
			}
		}
		
#line 345 "crontab.cpp"
		
		goto _st3;
		_ctr25:
		{
#line 206 "crontab.rl"
			
			if (auto t = nk::from_string<time_t>(MARKED_HST())) hst.h.set_lasttime(*t); else {
				hst.parse_error = true;
				{p+= 1; hst.cs = 3; goto _out;}
			}
		}
		
#line 358 "crontab.cpp"
		
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
#line 205 "crontab.rl"
			hst.st = p; }
		
#line 381 "crontab.cpp"
		
		goto _st6;
		_ctr13:
		{
#line 212 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_HST())) hst.h.set_numruns(*t); else {
				hst.parse_error = true;
				{p+= 1; hst.cs = 6; goto _out;}
			}
		}
		
#line 394 "crontab.cpp"
		
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
#line 224 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_HST())) hst.id = *t; else {
				hst.parse_error = true;
				{p+= 1; hst.cs = 4; goto _out;}
			}
		}
		
#line 433 "crontab.cpp"
		
		goto _st4;
		_ctr16:
		{
#line 212 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_HST())) hst.h.set_numruns(*t); else {
				hst.parse_error = true;
				{p+= 1; hst.cs = 4; goto _out;}
			}
		}
		
#line 446 "crontab.cpp"
		
		goto _st4;
		_ctr21:
		{
#line 218 "crontab.rl"
			
			if (auto t = nk::from_string<time_t>(MARKED_HST())) hst.h.set_exectime(*t); else {
				hst.parse_error = true;
				{p+= 1; hst.cs = 4; goto _out;}
			}
		}
		
#line 459 "crontab.cpp"
		
		goto _st4;
		_ctr26:
		{
#line 206 "crontab.rl"
			
			if (auto t = nk::from_string<time_t>(MARKED_HST())) hst.h.set_lasttime(*t); else {
				hst.parse_error = true;
				{p+= 1; hst.cs = 4; goto _out;}
			}
		}
		
#line 472 "crontab.cpp"
		
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
#line 205 "crontab.rl"
			hst.st = p; }
		
#line 495 "crontab.cpp"
		
		goto _st7;
		_ctr18:
		{
#line 218 "crontab.rl"
			
			if (auto t = nk::from_string<time_t>(MARKED_HST())) hst.h.set_exectime(*t); else {
				hst.parse_error = true;
				{p+= 1; hst.cs = 7; goto _out;}
			}
		}
		
#line 508 "crontab.cpp"
		
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
#line 224 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_HST())) hst.id = *t; else {
				hst.parse_error = true;
				{p+= 1; hst.cs = 5; goto _out;}
			}
		}
		
#line 547 "crontab.cpp"
		
		goto _st5;
		_ctr17:
		{
#line 212 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_HST())) hst.h.set_numruns(*t); else {
				hst.parse_error = true;
				{p+= 1; hst.cs = 5; goto _out;}
			}
		}
		
#line 560 "crontab.cpp"
		
		goto _st5;
		_ctr22:
		{
#line 218 "crontab.rl"
			
			if (auto t = nk::from_string<time_t>(MARKED_HST())) hst.h.set_exectime(*t); else {
				hst.parse_error = true;
				{p+= 1; hst.cs = 5; goto _out;}
			}
		}
		
#line 573 "crontab.cpp"
		
		goto _st5;
		_ctr27:
		{
#line 206 "crontab.rl"
			
			if (auto t = nk::from_string<time_t>(MARKED_HST())) hst.h.set_lasttime(*t); else {
				hst.parse_error = true;
				{p+= 1; hst.cs = 5; goto _out;}
			}
		}
		
#line 586 "crontab.cpp"
		
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
#line 205 "crontab.rl"
			hst.st = p; }
		
#line 609 "crontab.cpp"
		
		goto _st8;
		_ctr23:
		{
#line 206 "crontab.rl"
			
			if (auto t = nk::from_string<time_t>(MARKED_HST())) hst.h.set_lasttime(*t); else {
				hst.parse_error = true;
				{p+= 1; hst.cs = 8; goto _out;}
			}
		}
		
#line 622 "crontab.cpp"
		
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
	
#line 246 "crontab.rl"
	
	
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


#line 363 "crontab.rl"



#line 769 "crontab.cpp"
static const int parse_cmd_key_m_start = 1;
static const int parse_cmd_key_m_first_final = 2;
static const int parse_cmd_key_m_error = 0;

static const int parse_cmd_key_m_en_main = 1;


#line 365 "crontab.rl"


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
	
	
#line 800 "crontab.cpp"
	{
		pckm.cs = (int)parse_cmd_key_m_start;
	}
	
#line 386 "crontab.rl"
	
	
#line 808 "crontab.cpp"
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
#line 350 "crontab.rl"
			pckm.st = p; }
		
#line 861 "crontab.cpp"
		
		goto _st2;
		_ctr4:
		{
#line 351 "crontab.rl"
			
			ncs.ce->command = std::string(MARKED_PCKM());
			string_replace_all(ncs.ce->command, "\\ ", 2, " ");
			string_replace_all(ncs.ce->command, "\\\\", 2, "\\");
		}
		
#line 873 "crontab.cpp"
		
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
#line 350 "crontab.rl"
			pckm.st = p; }
		
#line 912 "crontab.cpp"
		
		goto _st3;
		_ctr6:
		{
#line 351 "crontab.rl"
			
			ncs.ce->command = std::string(MARKED_PCKM());
			string_replace_all(ncs.ce->command, "\\ ", 2, " ");
			string_replace_all(ncs.ce->command, "\\\\", 2, "\\");
		}
		
#line 924 "crontab.cpp"
		
		goto _st3;
		_ctr8:
		{
#line 350 "crontab.rl"
			pckm.st = p; }
		
#line 932 "crontab.cpp"
		
		{
#line 356 "crontab.rl"
			ncs.ce->args = std::string(MARKED_PCKM()); }
		
#line 938 "crontab.cpp"
		
		goto _st3;
		_ctr17:
		{
#line 351 "crontab.rl"
			
			ncs.ce->command = std::string(MARKED_PCKM());
			string_replace_all(ncs.ce->command, "\\ ", 2, " ");
			string_replace_all(ncs.ce->command, "\\\\", 2, "\\");
		}
		
#line 950 "crontab.cpp"
		
		{
#line 350 "crontab.rl"
			pckm.st = p; }
		
#line 956 "crontab.cpp"
		
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
#line 350 "crontab.rl"
			pckm.st = p; }
		
#line 987 "crontab.cpp"
		
		goto _st4;
		_ctr11:
		{
#line 356 "crontab.rl"
			ncs.ce->args = std::string(MARKED_PCKM()); }
		
#line 995 "crontab.cpp"
		
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
#line 350 "crontab.rl"
			pckm.st = p; }
		
#line 1018 "crontab.cpp"
		
		goto _st5;
		_ctr13:
		{
#line 351 "crontab.rl"
			
			ncs.ce->command = std::string(MARKED_PCKM());
			string_replace_all(ncs.ce->command, "\\ ", 2, " ");
			string_replace_all(ncs.ce->command, "\\\\", 2, "\\");
		}
		
#line 1030 "crontab.cpp"
		
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
#line 351 "crontab.rl"
			
			ncs.ce->command = std::string(MARKED_PCKM());
			string_replace_all(ncs.ce->command, "\\ ", 2, " ");
			string_replace_all(ncs.ce->command, "\\\\", 2, "\\");
		}
		
#line 1068 "crontab.cpp"
		
		goto _st6;
		_ctr15:
		{
#line 351 "crontab.rl"
			
			ncs.ce->command = std::string(MARKED_PCKM());
			string_replace_all(ncs.ce->command, "\\ ", 2, " ");
			string_replace_all(ncs.ce->command, "\\\\", 2, "\\");
		}
		
#line 1080 "crontab.cpp"
		
		{
#line 350 "crontab.rl"
			pckm.st = p; }
		
#line 1086 "crontab.cpp"
		
		{
#line 356 "crontab.rl"
			ncs.ce->args = std::string(MARKED_PCKM()); }
		
#line 1092 "crontab.cpp"
		
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
#line 350 "crontab.rl"
			pckm.st = p; }
		
#line 1126 "crontab.cpp"
		
		goto _st7;
		_ctr19:
		{
#line 351 "crontab.rl"
			
			ncs.ce->command = std::string(MARKED_PCKM());
			string_replace_all(ncs.ce->command, "\\ ", 2, " ");
			string_replace_all(ncs.ce->command, "\\\\", 2, "\\");
		}
		
#line 1138 "crontab.cpp"
		
		{
#line 356 "crontab.rl"
			ncs.ce->args = std::string(MARKED_PCKM()); }
		
#line 1144 "crontab.cpp"
		
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
#line 350 "crontab.rl"
			pckm.st = p; }
		
#line 1178 "crontab.cpp"
		
		goto _st8;
		_ctr22:
		{
#line 351 "crontab.rl"
			
			ncs.ce->command = std::string(MARKED_PCKM());
			string_replace_all(ncs.ce->command, "\\ ", 2, " ");
			string_replace_all(ncs.ce->command, "\\\\", 2, "\\");
		}
		
#line 1190 "crontab.cpp"
		
		{
#line 356 "crontab.rl"
			ncs.ce->args = std::string(MARKED_PCKM()); }
		
#line 1196 "crontab.cpp"
		
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
	
#line 387 "crontab.rl"
	
	
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


#line 544 "crontab.rl"



#line 1263 "crontab.cpp"
static const int ncrontab_start = 1;
static const int ncrontab_first_final = 79;
static const int ncrontab_error = 0;

static const int ncrontab_en_main = 1;


#line 546 "crontab.rl"


static int do_parse_config(ParseCfgState &ncs, const char *p, size_t plen)
{
	const char *pe = p + plen;
	const char *eof = pe;
	
	
#line 1280 "crontab.cpp"
	{
		ncs.cs = (int)ncrontab_start;
	}
	
#line 553 "crontab.rl"
	
	
#line 1288 "crontab.cpp"
	{
		switch ( ncs.cs ) {
			case 1:
			goto st_case_1;
			case 0:
			goto st_case_0;
			case 2:
			goto st_case_2;
			case 79:
			goto st_case_79;
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
			case 10:
			goto st_case_10;
			case 80:
			goto st_case_80;
			case 81:
			goto st_case_81;
			case 11:
			goto st_case_11;
			case 12:
			goto st_case_12;
			case 13:
			goto st_case_13;
			case 14:
			goto st_case_14;
			case 82:
			goto st_case_82;
			case 15:
			goto st_case_15;
			case 83:
			goto st_case_83;
			case 16:
			goto st_case_16;
			case 17:
			goto st_case_17;
			case 18:
			goto st_case_18;
			case 19:
			goto st_case_19;
			case 20:
			goto st_case_20;
			case 84:
			goto st_case_84;
			case 21:
			goto st_case_21;
			case 85:
			goto st_case_85;
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
			case 28:
			goto st_case_28;
			case 29:
			goto st_case_29;
			case 30:
			goto st_case_30;
			case 31:
			goto st_case_31;
			case 86:
			goto st_case_86;
			case 87:
			goto st_case_87;
			case 88:
			goto st_case_88;
			case 89:
			goto st_case_89;
			case 90:
			goto st_case_90;
			case 32:
			goto st_case_32;
			case 33:
			goto st_case_33;
			case 34:
			goto st_case_34;
			case 35:
			goto st_case_35;
			case 36:
			goto st_case_36;
			case 37:
			goto st_case_37;
			case 91:
			goto st_case_91;
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
			case 44:
			goto st_case_44;
			case 45:
			goto st_case_45;
			case 92:
			goto st_case_92;
			case 46:
			goto st_case_46;
			case 47:
			goto st_case_47;
			case 48:
			goto st_case_48;
			case 49:
			goto st_case_49;
			case 50:
			goto st_case_50;
			case 51:
			goto st_case_51;
			case 93:
			goto st_case_93;
			case 52:
			goto st_case_52;
			case 94:
			goto st_case_94;
			case 53:
			goto st_case_53;
			case 54:
			goto st_case_54;
			case 55:
			goto st_case_55;
			case 56:
			goto st_case_56;
			case 57:
			goto st_case_57;
			case 95:
			goto st_case_95;
			case 58:
			goto st_case_58;
			case 96:
			goto st_case_96;
			case 59:
			goto st_case_59;
			case 60:
			goto st_case_60;
			case 61:
			goto st_case_61;
			case 62:
			goto st_case_62;
			case 63:
			goto st_case_63;
			case 97:
			goto st_case_97;
			case 98:
			goto st_case_98;
			case 64:
			goto st_case_64;
			case 65:
			goto st_case_65;
			case 66:
			goto st_case_66;
			case 67:
			goto st_case_67;
			case 68:
			goto st_case_68;
			case 69:
			goto st_case_69;
			case 99:
			goto st_case_99;
			case 70:
			goto st_case_70;
			case 71:
			goto st_case_71;
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
			case 100:
			goto st_case_100;
			case 78:
			goto st_case_78;
			case 101:
			goto st_case_101;
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
					goto _st11;
				}
				case 72: {
					goto _st16;
				}
				case 73: {
					goto _st22;
				}
				case 74: {
					goto _st32;
				}
				case 77: {
					goto _st38;
				}
				case 80: {
					goto _st59;
				}
				case 82: {
					goto _st64;
				}
				case 87: {
					goto _st70;
				}
				case 99: {
					goto _st3;
				}
				case 100: {
					goto _st11;
				}
				case 104: {
					goto _st16;
				}
				case 105: {
					goto _st22;
				}
				case 106: {
					goto _st32;
				}
				case 109: {
					goto _st38;
				}
				case 112: {
					goto _st59;
				}
				case 114: {
					goto _st64;
				}
				case 119: {
					goto _st70;
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
#line 539 "crontab.rl"
			ncs.finish_ce(); ncs.create_ce(); }
		
#line 1577 "crontab.cpp"
		
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
				goto _ctr13;
			}
			goto _st0;
		}
		_ctr13:
		{
#line 532 "crontab.rl"
			ncs.jobid_st = p; }
		
#line 1600 "crontab.cpp"
		
		goto _st79;
		_ctr104:
		{
#line 533 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_JOBID())) ncs.ce->id = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 79; goto _out;}
			}
		}
		
#line 1613 "crontab.cpp"
		
		goto _st79;
		_st79:
		if ( p == eof )
			goto _out79;
		p+= 1;
		st_case_79:
		if ( p == pe && p != eof )
			goto _out79;
		if ( p == eof ) {
			goto _ctr104;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st79;
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
				case 79: {
					goto _st4;
				}
				case 111: {
					goto _st4;
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
				case 77: {
					goto _st5;
				}
				case 109: {
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
				case 77: {
					goto _st6;
				}
				case 109: {
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
				case 65: {
					goto _st7;
				}
				case 97: {
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
				case 78: {
					goto _st8;
				}
				case 110: {
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
				case 68: {
					goto _st9;
				}
				case 100: {
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
				case 9: {
					goto _st9;
				}
				case 32: {
					goto _st9;
				}
				case 61: {
					goto _st10;
				}
			}
			goto _st0;
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
				case 0: {
					goto _st0;
				}
				case 9: {
					goto _ctr22;
				}
				case 10: {
					goto _st0;
				}
				case 32: {
					goto _ctr22;
				}
			}
			goto _ctr21;
		}
		_ctr21:
		{
#line 466 "crontab.rl"
			ncs.strv_st = p; ncs.v_strlen = 0; }
		
#line 1805 "crontab.cpp"
		
		goto _st80;
		_ctr106:
		{
#line 467 "crontab.rl"
			
			ncs.v_strlen = p > ncs.strv_st ? static_cast<size_t>(p - ncs.strv_st) : 0;
			if (!nstrcpyl(ncs.v_str, sizeof ncs.v_str, ncs.strv_st, ncs.v_strlen)) {
				log_line("error parsing line %zu in crontab; too long?", ncs.linenum);
				std::exit(EXIT_FAILURE);
			}
		}
		
#line 1819 "crontab.cpp"
		
		{
#line 521 "crontab.rl"
			parse_command_key(ncs); }
		
#line 1825 "crontab.cpp"
		
		goto _st80;
		_st80:
		if ( p == eof )
			goto _out80;
		p+= 1;
		st_case_80:
		if ( p == pe && p != eof )
			goto _out80;
		if ( p == eof ) {
			goto _ctr106;}
		else {
			switch( ( (*( p))) ) {
				case 0: {
					goto _st0;
				}
				case 10: {
					goto _st0;
				}
			}
			goto _st80;
		}
		_ctr22:
		{
#line 466 "crontab.rl"
			ncs.strv_st = p; ncs.v_strlen = 0; }
		
#line 1853 "crontab.cpp"
		
		goto _st81;
		_ctr108:
		{
#line 467 "crontab.rl"
			
			ncs.v_strlen = p > ncs.strv_st ? static_cast<size_t>(p - ncs.strv_st) : 0;
			if (!nstrcpyl(ncs.v_str, sizeof ncs.v_str, ncs.strv_st, ncs.v_strlen)) {
				log_line("error parsing line %zu in crontab; too long?", ncs.linenum);
				std::exit(EXIT_FAILURE);
			}
		}
		
#line 1867 "crontab.cpp"
		
		{
#line 521 "crontab.rl"
			parse_command_key(ncs); }
		
#line 1873 "crontab.cpp"
		
		goto _st81;
		_st81:
		if ( p == eof )
			goto _out81;
		p+= 1;
		st_case_81:
		if ( p == pe && p != eof )
			goto _out81;
		if ( p == eof ) {
			goto _ctr108;}
		else {
			switch( ( (*( p))) ) {
				case 0: {
					goto _st0;
				}
				case 9: {
					goto _ctr22;
				}
				case 10: {
					goto _st0;
				}
				case 32: {
					goto _ctr22;
				}
			}
			goto _ctr21;
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
				case 65: {
					goto _st12;
				}
				case 97: {
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
				case 89: {
					goto _st13;
				}
				case 121: {
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
				case 9: {
					goto _st13;
				}
				case 32: {
					goto _st13;
				}
				case 61: {
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
				case 9: {
					goto _st14;
				}
				case 32: {
					goto _st14;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr26;
			}
			goto _st0;
		}
		_ctr26:
		{
#line 446 "crontab.rl"
			
			ncs.intv_st = p;
			ncs.v_int = ncs.v_int2 = 0;
			ncs.intv2_exist = false;
		}
		
#line 1997 "crontab.cpp"
		
		goto _st82;
		_ctr109:
		{
#line 451 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 82; goto _out;}
			}
		}
		
#line 2010 "crontab.cpp"
		
		{
#line 510 "crontab.rl"
			addcstlist(ncs, ncs.ce->day, 0, 1, 31); }
		
#line 2016 "crontab.cpp"
		
		goto _st82;
		_st82:
		if ( p == eof )
			goto _out82;
		p+= 1;
		st_case_82:
		if ( p == pe && p != eof )
			goto _out82;
		if ( p == eof ) {
			goto _ctr109;}
		else {
			if ( ( (*( p))) == 44 ) {
				goto _ctr110;
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st82;
			}
			goto _st0;
		}
		_ctr110:
		{
#line 451 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 15; goto _out;}
			}
		}
		
#line 2047 "crontab.cpp"
		
		goto _st15;
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
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr28;
			}
			goto _st0;
		}
		_ctr28:
		{
#line 457 "crontab.rl"
			ncs.intv2_st = p; }
		
#line 2070 "crontab.cpp"
		
		goto _st83;
		_ctr112:
		{
#line 458 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV2())) ncs.v_int2 = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 83; goto _out;}
			}
			ncs.intv2_exist = true;
		}
		
#line 2084 "crontab.cpp"
		
		{
#line 510 "crontab.rl"
			addcstlist(ncs, ncs.ce->day, 0, 1, 31); }
		
#line 2090 "crontab.cpp"
		
		goto _st83;
		_st83:
		if ( p == eof )
			goto _out83;
		p+= 1;
		st_case_83:
		if ( p == pe && p != eof )
			goto _out83;
		if ( p == eof ) {
			goto _ctr112;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st83;
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
				case 79: {
					goto _st17;
				}
				case 111: {
					goto _st17;
				}
			}
			goto _st0;
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
				case 85: {
					goto _st18;
				}
				case 117: {
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
				case 82: {
					goto _st19;
				}
				case 114: {
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
				goto _ctr33;
			}
			goto _st0;
		}
		_ctr33:
		{
#line 446 "crontab.rl"
			
			ncs.intv_st = p;
			ncs.v_int = ncs.v_int2 = 0;
			ncs.intv2_exist = false;
		}
		
#line 2223 "crontab.cpp"
		
		goto _st84;
		_ctr114:
		{
#line 451 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 84; goto _out;}
			}
		}
		
#line 2236 "crontab.cpp"
		
		{
#line 512 "crontab.rl"
			addcstlist(ncs, ncs.ce->hour, 24, 0, 23); }
		
#line 2242 "crontab.cpp"
		
		goto _st84;
		_st84:
		if ( p == eof )
			goto _out84;
		p+= 1;
		st_case_84:
		if ( p == pe && p != eof )
			goto _out84;
		if ( p == eof ) {
			goto _ctr114;}
		else {
			if ( ( (*( p))) == 44 ) {
				goto _ctr115;
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st84;
			}
			goto _st0;
		}
		_ctr115:
		{
#line 451 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 21; goto _out;}
			}
		}
		
#line 2273 "crontab.cpp"
		
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
				goto _ctr35;
			}
			goto _st0;
		}
		_ctr35:
		{
#line 457 "crontab.rl"
			ncs.intv2_st = p; }
		
#line 2296 "crontab.cpp"
		
		goto _st85;
		_ctr117:
		{
#line 458 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV2())) ncs.v_int2 = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 85; goto _out;}
			}
			ncs.intv2_exist = true;
		}
		
#line 2310 "crontab.cpp"
		
		{
#line 512 "crontab.rl"
			addcstlist(ncs, ncs.ce->hour, 24, 0, 23); }
		
#line 2316 "crontab.cpp"
		
		goto _st85;
		_st85:
		if ( p == eof )
			goto _out85;
		p+= 1;
		st_case_85:
		if ( p == pe && p != eof )
			goto _out85;
		if ( p == eof ) {
			goto _ctr117;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st85;
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
				case 78: {
					goto _st23;
				}
				case 110: {
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
				case 84: {
					goto _st24;
				}
				case 116: {
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
				case 69: {
					goto _st25;
				}
				case 101: {
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
				case 82: {
					goto _st26;
				}
				case 114: {
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
				case 86: {
					goto _st27;
				}
				case 118: {
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
				case 65: {
					goto _st28;
				}
				case 97: {
					goto _st28;
				}
			}
			goto _st0;
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
				case 76: {
					goto _st29;
				}
				case 108: {
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
				case 9: {
					goto _st29;
				}
				case 32: {
					goto _st29;
				}
				case 61: {
					goto _st30;
				}
			}
			goto _st0;
		}
		_ctr120:
		{
#line 433 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_TIME())) ncs.v_time += 86400 * *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 30; goto _out;}
			}
		}
		
#line 2507 "crontab.cpp"
		
		goto _st30;
		_ctr123:
		{
#line 427 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_TIME())) ncs.v_time += 3600 * *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 30; goto _out;}
			}
		}
		
#line 2520 "crontab.cpp"
		
		goto _st30;
		_ctr126:
		{
#line 421 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_TIME())) ncs.v_time += 60 * *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 30; goto _out;}
			}
		}
		
#line 2533 "crontab.cpp"
		
		goto _st30;
		_ctr129:
		{
#line 415 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_TIME())) ncs.v_time += *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 30; goto _out;}
			}
		}
		
#line 2546 "crontab.cpp"
		
		goto _st30;
		_ctr132:
		{
#line 439 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_TIME())) ncs.v_time += 604800 * *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 30; goto _out;}
			}
		}
		
#line 2559 "crontab.cpp"
		
		goto _st30;
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
				case 9: {
					goto _st30;
				}
				case 32: {
					goto _st30;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr44;
			}
			goto _st0;
		}
		_ctr44:
		{
#line 414 "crontab.rl"
			ncs.time_st = p; ncs.v_time = 0; }
		
#line 2590 "crontab.cpp"
		
		goto _st31;
		_ctr121:
		{
#line 433 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_TIME())) ncs.v_time += 86400 * *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 31; goto _out;}
			}
		}
		
#line 2603 "crontab.cpp"
		
		{
#line 414 "crontab.rl"
			ncs.time_st = p; ncs.v_time = 0; }
		
#line 2609 "crontab.cpp"
		
		goto _st31;
		_ctr124:
		{
#line 427 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_TIME())) ncs.v_time += 3600 * *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 31; goto _out;}
			}
		}
		
#line 2622 "crontab.cpp"
		
		{
#line 414 "crontab.rl"
			ncs.time_st = p; ncs.v_time = 0; }
		
#line 2628 "crontab.cpp"
		
		goto _st31;
		_ctr127:
		{
#line 421 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_TIME())) ncs.v_time += 60 * *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 31; goto _out;}
			}
		}
		
#line 2641 "crontab.cpp"
		
		{
#line 414 "crontab.rl"
			ncs.time_st = p; ncs.v_time = 0; }
		
#line 2647 "crontab.cpp"
		
		goto _st31;
		_ctr130:
		{
#line 415 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_TIME())) ncs.v_time += *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 31; goto _out;}
			}
		}
		
#line 2660 "crontab.cpp"
		
		{
#line 414 "crontab.rl"
			ncs.time_st = p; ncs.v_time = 0; }
		
#line 2666 "crontab.cpp"
		
		goto _st31;
		_ctr133:
		{
#line 439 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_TIME())) ncs.v_time += 604800 * *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 31; goto _out;}
			}
		}
		
#line 2679 "crontab.cpp"
		
		{
#line 414 "crontab.rl"
			ncs.time_st = p; ncs.v_time = 0; }
		
#line 2685 "crontab.cpp"
		
		goto _st31;
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
				case 100: {
					goto _st86;
				}
				case 104: {
					goto _st87;
				}
				case 109: {
					goto _st88;
				}
				case 115: {
					goto _st89;
				}
				case 119: {
					goto _st90;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st31;
			}
			goto _st0;
		}
		_ctr119:
		{
#line 433 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_TIME())) ncs.v_time += 86400 * *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 86; goto _out;}
			}
		}
		
#line 2730 "crontab.cpp"
		
		{
#line 505 "crontab.rl"
			ncs.ce->interval = ncs.v_time; }
		
#line 2736 "crontab.cpp"
		
		goto _st86;
		_st86:
		if ( p == eof )
			goto _out86;
		p+= 1;
		st_case_86:
		if ( p == pe && p != eof )
			goto _out86;
		if ( p == eof ) {
			goto _ctr119;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _ctr120;
				}
				case 32: {
					goto _ctr120;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr121;
			}
			goto _st0;
		}
		_ctr122:
		{
#line 427 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_TIME())) ncs.v_time += 3600 * *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 87; goto _out;}
			}
		}
		
#line 2772 "crontab.cpp"
		
		{
#line 505 "crontab.rl"
			ncs.ce->interval = ncs.v_time; }
		
#line 2778 "crontab.cpp"
		
		goto _st87;
		_st87:
		if ( p == eof )
			goto _out87;
		p+= 1;
		st_case_87:
		if ( p == pe && p != eof )
			goto _out87;
		if ( p == eof ) {
			goto _ctr122;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _ctr123;
				}
				case 32: {
					goto _ctr123;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr124;
			}
			goto _st0;
		}
		_ctr125:
		{
#line 421 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_TIME())) ncs.v_time += 60 * *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 88; goto _out;}
			}
		}
		
#line 2814 "crontab.cpp"
		
		{
#line 505 "crontab.rl"
			ncs.ce->interval = ncs.v_time; }
		
#line 2820 "crontab.cpp"
		
		goto _st88;
		_st88:
		if ( p == eof )
			goto _out88;
		p+= 1;
		st_case_88:
		if ( p == pe && p != eof )
			goto _out88;
		if ( p == eof ) {
			goto _ctr125;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _ctr126;
				}
				case 32: {
					goto _ctr126;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr127;
			}
			goto _st0;
		}
		_ctr128:
		{
#line 415 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_TIME())) ncs.v_time += *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 89; goto _out;}
			}
		}
		
#line 2856 "crontab.cpp"
		
		{
#line 505 "crontab.rl"
			ncs.ce->interval = ncs.v_time; }
		
#line 2862 "crontab.cpp"
		
		goto _st89;
		_st89:
		if ( p == eof )
			goto _out89;
		p+= 1;
		st_case_89:
		if ( p == pe && p != eof )
			goto _out89;
		if ( p == eof ) {
			goto _ctr128;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _ctr129;
				}
				case 32: {
					goto _ctr129;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr130;
			}
			goto _st0;
		}
		_ctr131:
		{
#line 439 "crontab.rl"
			
			if (auto t = nk::from_string<unsigned>(MARKED_TIME())) ncs.v_time += 604800 * *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 90; goto _out;}
			}
		}
		
#line 2898 "crontab.cpp"
		
		{
#line 505 "crontab.rl"
			ncs.ce->interval = ncs.v_time; }
		
#line 2904 "crontab.cpp"
		
		goto _st90;
		_st90:
		if ( p == eof )
			goto _out90;
		p+= 1;
		st_case_90:
		if ( p == pe && p != eof )
			goto _out90;
		if ( p == eof ) {
			goto _ctr131;}
		else {
			switch( ( (*( p))) ) {
				case 9: {
					goto _ctr132;
				}
				case 32: {
					goto _ctr132;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr133;
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
				case 79: {
					goto _st33;
				}
				case 111: {
					goto _st33;
				}
			}
			goto _st0;
		}
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
			switch( ( (*( p))) ) {
				case 85: {
					goto _st34;
				}
				case 117: {
					goto _st34;
				}
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
				case 82: {
					goto _st35;
				}
				case 114: {
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
				case 78: {
					goto _st36;
				}
				case 110: {
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
				case 65: {
					goto _st37;
				}
				case 97: {
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
				case 76: {
					goto _st91;
				}
				case 108: {
					goto _st91;
				}
			}
			goto _st0;
		}
		_ctr134:
		{
#line 488 "crontab.rl"
			ncs.ce->journal = true; }
		
#line 3055 "crontab.cpp"
		
		goto _st91;
		_st91:
		if ( p == eof )
			goto _out91;
		p+= 1;
		st_case_91:
		if ( p == pe && p != eof )
			goto _out91;
		if ( p == eof ) {
			goto _ctr134;}
		else {
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
				case 65: {
					goto _st39;
				}
				case 73: {
					goto _st46;
				}
				case 79: {
					goto _st53;
				}
				case 97: {
					goto _st39;
				}
				case 105: {
					goto _st46;
				}
				case 111: {
					goto _st53;
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
				case 88: {
					goto _st40;
				}
				case 120: {
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
				case 82: {
					goto _st41;
				}
				case 114: {
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
				case 85: {
					goto _st42;
				}
				case 117: {
					goto _st42;
				}
			}
			goto _st0;
		}
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
				case 78: {
					goto _st43;
				}
				case 110: {
					goto _st43;
				}
			}
			goto _st0;
		}
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
				case 83: {
					goto _st44;
				}
				case 115: {
					goto _st44;
				}
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
				case 9: {
					goto _st44;
				}
				case 32: {
					goto _st44;
				}
				case 61: {
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
				case 9: {
					goto _st45;
				}
				case 32: {
					goto _st45;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr66;
			}
			goto _st0;
		}
		_ctr66:
		{
#line 446 "crontab.rl"
			
			ncs.intv_st = p;
			ncs.v_int = ncs.v_int2 = 0;
			ncs.intv2_exist = false;
		}
		
#line 3257 "crontab.cpp"
		
		goto _st92;
		_ctr135:
		{
#line 451 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 92; goto _out;}
			}
		}
		
#line 3270 "crontab.cpp"
		
		{
#line 497 "crontab.rl"
			
			if (!ncs.runat)
			ncs.ce->maxruns = ncs.v_int > 0 ? static_cast<unsigned>(ncs.v_int) : 0;
		}
		
#line 3279 "crontab.cpp"
		
		goto _st92;
		_st92:
		if ( p == eof )
			goto _out92;
		p+= 1;
		st_case_92:
		if ( p == pe && p != eof )
			goto _out92;
		if ( p == eof ) {
			goto _ctr135;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st92;
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
				case 78: {
					goto _st47;
				}
				case 110: {
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
				case 85: {
					goto _st48;
				}
				case 117: {
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
				case 84: {
					goto _st49;
				}
				case 116: {
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
				case 69: {
					goto _st50;
				}
				case 101: {
					goto _st50;
				}
			}
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
			switch( ( (*( p))) ) {
				case 9: {
					goto _st50;
				}
				case 32: {
					goto _st50;
				}
				case 61: {
					goto _st51;
				}
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
				case 9: {
					goto _st51;
				}
				case 32: {
					goto _st51;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr72;
			}
			goto _st0;
		}
		_ctr72:
		{
#line 446 "crontab.rl"
			
			ncs.intv_st = p;
			ncs.v_int = ncs.v_int2 = 0;
			ncs.intv2_exist = false;
		}
		
#line 3432 "crontab.cpp"
		
		goto _st93;
		_ctr137:
		{
#line 451 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 93; goto _out;}
			}
		}
		
#line 3445 "crontab.cpp"
		
		{
#line 513 "crontab.rl"
			addcstlist(ncs, ncs.ce->minute, 60, 0, 59); }
		
#line 3451 "crontab.cpp"
		
		goto _st93;
		_st93:
		if ( p == eof )
			goto _out93;
		p+= 1;
		st_case_93:
		if ( p == pe && p != eof )
			goto _out93;
		if ( p == eof ) {
			goto _ctr137;}
		else {
			if ( ( (*( p))) == 44 ) {
				goto _ctr138;
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st93;
			}
			goto _st0;
		}
		_ctr138:
		{
#line 451 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 52; goto _out;}
			}
		}
		
#line 3482 "crontab.cpp"
		
		goto _st52;
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
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr74;
			}
			goto _st0;
		}
		_ctr74:
		{
#line 457 "crontab.rl"
			ncs.intv2_st = p; }
		
#line 3505 "crontab.cpp"
		
		goto _st94;
		_ctr140:
		{
#line 458 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV2())) ncs.v_int2 = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 94; goto _out;}
			}
			ncs.intv2_exist = true;
		}
		
#line 3519 "crontab.cpp"
		
		{
#line 513 "crontab.rl"
			addcstlist(ncs, ncs.ce->minute, 60, 0, 59); }
		
#line 3525 "crontab.cpp"
		
		goto _st94;
		_st94:
		if ( p == eof )
			goto _out94;
		p+= 1;
		st_case_94:
		if ( p == pe && p != eof )
			goto _out94;
		if ( p == eof ) {
			goto _ctr140;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st94;
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
				case 78: {
					goto _st54;
				}
				case 110: {
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
				case 84: {
					goto _st55;
				}
				case 116: {
					goto _st55;
				}
			}
			goto _st0;
		}
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
			switch( ( (*( p))) ) {
				case 72: {
					goto _st56;
				}
				case 104: {
					goto _st56;
				}
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
				case 9: {
					goto _st56;
				}
				case 32: {
					goto _st56;
				}
				case 61: {
					goto _st57;
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
				case 9: {
					goto _st57;
				}
				case 32: {
					goto _st57;
				}
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr79;
			}
			goto _st0;
		}
		_ctr79:
		{
#line 446 "crontab.rl"
			
			ncs.intv_st = p;
			ncs.v_int = ncs.v_int2 = 0;
			ncs.intv2_exist = false;
		}
		
#line 3658 "crontab.cpp"
		
		goto _st95;
		_ctr142:
		{
#line 451 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 95; goto _out;}
			}
		}
		
#line 3671 "crontab.cpp"
		
		{
#line 509 "crontab.rl"
			addcstlist(ncs, ncs.ce->month, 0, 1, 12); }
		
#line 3677 "crontab.cpp"
		
		goto _st95;
		_st95:
		if ( p == eof )
			goto _out95;
		p+= 1;
		st_case_95:
		if ( p == pe && p != eof )
			goto _out95;
		if ( p == eof ) {
			goto _ctr142;}
		else {
			if ( ( (*( p))) == 44 ) {
				goto _ctr143;
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st95;
			}
			goto _st0;
		}
		_ctr143:
		{
#line 451 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 58; goto _out;}
			}
		}
		
#line 3708 "crontab.cpp"
		
		goto _st58;
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
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr81;
			}
			goto _st0;
		}
		_ctr81:
		{
#line 457 "crontab.rl"
			ncs.intv2_st = p; }
		
#line 3731 "crontab.cpp"
		
		goto _st96;
		_ctr145:
		{
#line 458 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV2())) ncs.v_int2 = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 96; goto _out;}
			}
			ncs.intv2_exist = true;
		}
		
#line 3745 "crontab.cpp"
		
		{
#line 509 "crontab.rl"
			addcstlist(ncs, ncs.ce->month, 0, 1, 12); }
		
#line 3751 "crontab.cpp"
		
		goto _st96;
		_st96:
		if ( p == eof )
			goto _out96;
		p+= 1;
		st_case_96:
		if ( p == pe && p != eof )
			goto _out96;
		if ( p == eof ) {
			goto _ctr145;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st96;
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
				case 65: {
					goto _st60;
				}
				case 97: {
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
				case 84: {
					goto _st61;
				}
				case 116: {
					goto _st61;
				}
			}
			goto _st0;
		}
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
			switch( ( (*( p))) ) {
				case 72: {
					goto _st62;
				}
				case 104: {
					goto _st62;
				}
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
				case 9: {
					goto _st62;
				}
				case 32: {
					goto _st62;
				}
				case 61: {
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
				case 0: {
					goto _st0;
				}
				case 9: {
					goto _ctr87;
				}
				case 10: {
					goto _st0;
				}
				case 32: {
					goto _ctr87;
				}
			}
			goto _ctr86;
		}
		_ctr86:
		{
#line 466 "crontab.rl"
			ncs.strv_st = p; ncs.v_strlen = 0; }
		
#line 3883 "crontab.cpp"
		
		goto _st97;
		_ctr147:
		{
#line 467 "crontab.rl"
			
			ncs.v_strlen = p > ncs.strv_st ? static_cast<size_t>(p - ncs.strv_st) : 0;
			if (!nstrcpyl(ncs.v_str, sizeof ncs.v_str, ncs.strv_st, ncs.v_strlen)) {
				log_line("error parsing line %zu in crontab; too long?", ncs.linenum);
				std::exit(EXIT_FAILURE);
			}
		}
		
#line 3897 "crontab.cpp"
		
		{
#line 522 "crontab.rl"
			
			ncs.ce->path = std::string(ncs.v_str, ncs.v_strlen);
		}
		
#line 3905 "crontab.cpp"
		
		goto _st97;
		_st97:
		if ( p == eof )
			goto _out97;
		p+= 1;
		st_case_97:
		if ( p == pe && p != eof )
			goto _out97;
		if ( p == eof ) {
			goto _ctr147;}
		else {
			switch( ( (*( p))) ) {
				case 0: {
					goto _st0;
				}
				case 10: {
					goto _st0;
				}
			}
			goto _st97;
		}
		_ctr87:
		{
#line 466 "crontab.rl"
			ncs.strv_st = p; ncs.v_strlen = 0; }
		
#line 3933 "crontab.cpp"
		
		goto _st98;
		_ctr149:
		{
#line 467 "crontab.rl"
			
			ncs.v_strlen = p > ncs.strv_st ? static_cast<size_t>(p - ncs.strv_st) : 0;
			if (!nstrcpyl(ncs.v_str, sizeof ncs.v_str, ncs.strv_st, ncs.v_strlen)) {
				log_line("error parsing line %zu in crontab; too long?", ncs.linenum);
				std::exit(EXIT_FAILURE);
			}
		}
		
#line 3947 "crontab.cpp"
		
		{
#line 522 "crontab.rl"
			
			ncs.ce->path = std::string(ncs.v_str, ncs.v_strlen);
		}
		
#line 3955 "crontab.cpp"
		
		goto _st98;
		_st98:
		if ( p == eof )
			goto _out98;
		p+= 1;
		st_case_98:
		if ( p == pe && p != eof )
			goto _out98;
		if ( p == eof ) {
			goto _ctr149;}
		else {
			switch( ( (*( p))) ) {
				case 0: {
					goto _st0;
				}
				case 9: {
					goto _ctr87;
				}
				case 10: {
					goto _st0;
				}
				case 32: {
					goto _ctr87;
				}
			}
			goto _ctr86;
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
				case 85: {
					goto _st65;
				}
				case 117: {
					goto _st65;
				}
			}
			goto _st0;
		}
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
			switch( ( (*( p))) ) {
				case 78: {
					goto _st66;
				}
				case 110: {
					goto _st66;
				}
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
				case 9: {
					goto _st68;
				}
				case 32: {
					goto _st68;
				}
				case 61: {
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
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _ctr93;
			}
			goto _st0;
		}
		_ctr93:
		{
#line 446 "crontab.rl"
			
			ncs.intv_st = p;
			ncs.v_int = ncs.v_int2 = 0;
			ncs.intv2_exist = false;
		}
		
#line 4119 "crontab.cpp"
		
		goto _st99;
		_ctr150:
		{
#line 451 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 99; goto _out;}
			}
		}
		
#line 4132 "crontab.cpp"
		
		{
#line 491 "crontab.rl"
			
			ncs.runat = true;
			ncs.ce->exectime = ncs.v_int;
			ncs.ce->maxruns = 1;
			ncs.ce->journal = true;
		}
		
#line 4143 "crontab.cpp"
		
		goto _st99;
		_st99:
		if ( p == eof )
			goto _out99;
		p+= 1;
		st_case_99:
		if ( p == pe && p != eof )
			goto _out99;
		if ( p == eof ) {
			goto _ctr150;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st99;
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
				case 69: {
					goto _st71;
				}
				case 101: {
					goto _st71;
				}
			}
			goto _st0;
		}
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
			switch( ( (*( p))) ) {
				case 69: {
					goto _st72;
				}
				case 101: {
					goto _st72;
				}
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
				case 75: {
					goto _st73;
				}
				case 107: {
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
				case 68: {
					goto _st74;
				}
				case 100: {
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
				case 65: {
					goto _st75;
				}
				case 97: {
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
				case 89: {
					goto _st76;
				}
				case 121: {
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
				goto _ctr101;
			}
			goto _st0;
		}
		_ctr101:
		{
#line 446 "crontab.rl"
			
			ncs.intv_st = p;
			ncs.v_int = ncs.v_int2 = 0;
			ncs.intv2_exist = false;
		}
		
#line 4336 "crontab.cpp"
		
		goto _st100;
		_ctr152:
		{
#line 451 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 100; goto _out;}
			}
		}
		
#line 4349 "crontab.cpp"
		
		{
#line 511 "crontab.rl"
			addcstlist(ncs, ncs.ce->weekday, 0, 1, 7); }
		
#line 4355 "crontab.cpp"
		
		goto _st100;
		_st100:
		if ( p == eof )
			goto _out100;
		p+= 1;
		st_case_100:
		if ( p == pe && p != eof )
			goto _out100;
		if ( p == eof ) {
			goto _ctr152;}
		else {
			if ( ( (*( p))) == 44 ) {
				goto _ctr153;
			}
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st100;
			}
			goto _st0;
		}
		_ctr153:
		{
#line 451 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV1())) ncs.v_int = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 78; goto _out;}
			}
		}
		
#line 4386 "crontab.cpp"
		
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
				goto _ctr103;
			}
			goto _st0;
		}
		_ctr103:
		{
#line 457 "crontab.rl"
			ncs.intv2_st = p; }
		
#line 4409 "crontab.cpp"
		
		goto _st101;
		_ctr155:
		{
#line 458 "crontab.rl"
			
			if (auto t = nk::from_string<int>(MARKED_INTV2())) ncs.v_int2 = *t; else {
				ncs.parse_error = true;
				{p+= 1; ncs.cs = 101; goto _out;}
			}
			ncs.intv2_exist = true;
		}
		
#line 4423 "crontab.cpp"
		
		{
#line 511 "crontab.rl"
			addcstlist(ncs, ncs.ce->weekday, 0, 1, 7); }
		
#line 4429 "crontab.cpp"
		
		goto _st101;
		_st101:
		if ( p == eof )
			goto _out101;
		p+= 1;
		st_case_101:
		if ( p == pe && p != eof )
			goto _out101;
		if ( p == eof ) {
			goto _ctr155;}
		else {
			if ( 48 <= ( (*( p))) && ( (*( p))) <= 57 ) {
				goto _st101;
			}
			goto _st0;
		}
		_out1: ncs.cs = 1; goto _out; 
		_out0: ncs.cs = 0; goto _out; 
		_out2: ncs.cs = 2; goto _out; 
		_out79: ncs.cs = 79; goto _out; 
		_out3: ncs.cs = 3; goto _out; 
		_out4: ncs.cs = 4; goto _out; 
		_out5: ncs.cs = 5; goto _out; 
		_out6: ncs.cs = 6; goto _out; 
		_out7: ncs.cs = 7; goto _out; 
		_out8: ncs.cs = 8; goto _out; 
		_out9: ncs.cs = 9; goto _out; 
		_out10: ncs.cs = 10; goto _out; 
		_out80: ncs.cs = 80; goto _out; 
		_out81: ncs.cs = 81; goto _out; 
		_out11: ncs.cs = 11; goto _out; 
		_out12: ncs.cs = 12; goto _out; 
		_out13: ncs.cs = 13; goto _out; 
		_out14: ncs.cs = 14; goto _out; 
		_out82: ncs.cs = 82; goto _out; 
		_out15: ncs.cs = 15; goto _out; 
		_out83: ncs.cs = 83; goto _out; 
		_out16: ncs.cs = 16; goto _out; 
		_out17: ncs.cs = 17; goto _out; 
		_out18: ncs.cs = 18; goto _out; 
		_out19: ncs.cs = 19; goto _out; 
		_out20: ncs.cs = 20; goto _out; 
		_out84: ncs.cs = 84; goto _out; 
		_out21: ncs.cs = 21; goto _out; 
		_out85: ncs.cs = 85; goto _out; 
		_out22: ncs.cs = 22; goto _out; 
		_out23: ncs.cs = 23; goto _out; 
		_out24: ncs.cs = 24; goto _out; 
		_out25: ncs.cs = 25; goto _out; 
		_out26: ncs.cs = 26; goto _out; 
		_out27: ncs.cs = 27; goto _out; 
		_out28: ncs.cs = 28; goto _out; 
		_out29: ncs.cs = 29; goto _out; 
		_out30: ncs.cs = 30; goto _out; 
		_out31: ncs.cs = 31; goto _out; 
		_out86: ncs.cs = 86; goto _out; 
		_out87: ncs.cs = 87; goto _out; 
		_out88: ncs.cs = 88; goto _out; 
		_out89: ncs.cs = 89; goto _out; 
		_out90: ncs.cs = 90; goto _out; 
		_out32: ncs.cs = 32; goto _out; 
		_out33: ncs.cs = 33; goto _out; 
		_out34: ncs.cs = 34; goto _out; 
		_out35: ncs.cs = 35; goto _out; 
		_out36: ncs.cs = 36; goto _out; 
		_out37: ncs.cs = 37; goto _out; 
		_out91: ncs.cs = 91; goto _out; 
		_out38: ncs.cs = 38; goto _out; 
		_out39: ncs.cs = 39; goto _out; 
		_out40: ncs.cs = 40; goto _out; 
		_out41: ncs.cs = 41; goto _out; 
		_out42: ncs.cs = 42; goto _out; 
		_out43: ncs.cs = 43; goto _out; 
		_out44: ncs.cs = 44; goto _out; 
		_out45: ncs.cs = 45; goto _out; 
		_out92: ncs.cs = 92; goto _out; 
		_out46: ncs.cs = 46; goto _out; 
		_out47: ncs.cs = 47; goto _out; 
		_out48: ncs.cs = 48; goto _out; 
		_out49: ncs.cs = 49; goto _out; 
		_out50: ncs.cs = 50; goto _out; 
		_out51: ncs.cs = 51; goto _out; 
		_out93: ncs.cs = 93; goto _out; 
		_out52: ncs.cs = 52; goto _out; 
		_out94: ncs.cs = 94; goto _out; 
		_out53: ncs.cs = 53; goto _out; 
		_out54: ncs.cs = 54; goto _out; 
		_out55: ncs.cs = 55; goto _out; 
		_out56: ncs.cs = 56; goto _out; 
		_out57: ncs.cs = 57; goto _out; 
		_out95: ncs.cs = 95; goto _out; 
		_out58: ncs.cs = 58; goto _out; 
		_out96: ncs.cs = 96; goto _out; 
		_out59: ncs.cs = 59; goto _out; 
		_out60: ncs.cs = 60; goto _out; 
		_out61: ncs.cs = 61; goto _out; 
		_out62: ncs.cs = 62; goto _out; 
		_out63: ncs.cs = 63; goto _out; 
		_out97: ncs.cs = 97; goto _out; 
		_out98: ncs.cs = 98; goto _out; 
		_out64: ncs.cs = 64; goto _out; 
		_out65: ncs.cs = 65; goto _out; 
		_out66: ncs.cs = 66; goto _out; 
		_out67: ncs.cs = 67; goto _out; 
		_out68: ncs.cs = 68; goto _out; 
		_out69: ncs.cs = 69; goto _out; 
		_out99: ncs.cs = 99; goto _out; 
		_out70: ncs.cs = 70; goto _out; 
		_out71: ncs.cs = 71; goto _out; 
		_out72: ncs.cs = 72; goto _out; 
		_out73: ncs.cs = 73; goto _out; 
		_out74: ncs.cs = 74; goto _out; 
		_out75: ncs.cs = 75; goto _out; 
		_out76: ncs.cs = 76; goto _out; 
		_out77: ncs.cs = 77; goto _out; 
		_out100: ncs.cs = 100; goto _out; 
		_out78: ncs.cs = 78; goto _out; 
		_out101: ncs.cs = 101; goto _out; 
		_out: {}
	}
	
#line 554 "crontab.rl"
	
	
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

