#line 1 "crontab.rl"
// Copyright 2003-2024 Nicholas J. Kain <njkain at gmail dot com>
// SPDX-License-Identifier: MIT
#include <algorithm>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <nk/string_replace_all.hpp>
#include <nk/from_string.hpp>
#include <nk/scopeguard.hpp>
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

static int cfg_reload;    /* 0 on first call, 1 on subsequent calls */
extern int gflags_debug;

std::vector<Job> g_jobs;

static void get_history(Job &item);

struct ParseCfgState
{
	ParseCfgState(std::string_view ef, std::vector<StackItem> *stk,
	std::vector<StackItem> *dstk)
	: stack(stk), deadstack(dstk), execfile(ef.data(), ef.size())
	{
		memset(v_str, 0, sizeof v_str);
	}
	char v_str[MAX_LINE];
	
	std::vector<StackItem> *stack;
	std::vector<StackItem> *deadstack;
	
	Job ce;
	
	std::string execfile;
	
	const char *jobid_st = nullptr;
	const char *time_st = nullptr;
	const char *intv_st = nullptr;
	const char *intv2_st = nullptr;
	const char *strv_st = nullptr;
	
	size_t v_strlen = 0;
	size_t linenum = 0;
	
	unsigned int v_time;
	
	int v_int = 0;
	int v_int2 = 0;
	
	int cs = 0;
	int cmdret = 0;
	
	bool intv2_exist = false;
	bool runat = false;
	
	bool parse_error = false;
	
	void create_ce()
	{
		ce.clear();
		cmdret = 0;
		runat = false;
	}
	
	inline void debug_print_ce() const
	{
		if (!gflags_debug)
			return;
		log_line("-=- finish_ce -=-");
		log_line("id: %u", ce.id);
		log_line("command: %s", ce.command.c_str());
		log_line("args: %s", ce.args.c_str());
		log_line("numruns: %u", ce.numruns);
		log_line("maxruns: %u", ce.maxruns);
		log_line("journal: %s", ce.journal ? "true" : "false");
		for (const auto &i: ce.month)
		log_line("month: [%u,%u]", i.first, i.second);
		for (const auto &i: ce.day)
		log_line("day: [%u,%u]", i.first, i.second);
		for (const auto &i: ce.weekday)
		log_line("weekday: [%u,%u]", i.first, i.second);
		for (const auto &i: ce.hour)
		log_line("hour: [%u,%u]", i.first, i.second);
		for (const auto &i: ce.minute)
		log_line("minute: [%u,%u]", i.first, i.second);
		log_line("interval: %u", ce.interval);
		log_line("exectime: %lu", ce.exectime);
		log_line("lasttime: %lu", ce.lasttime);
	}
	
	inline void debug_print_ce_history() const
	{
		if (!gflags_debug)
			return;
		log_line("[%u]->numruns = %u", ce.id, ce.numruns);
		log_line("[%u]->exectime = %lu", ce.id, ce.exectime);
		log_line("[%u]->lasttime = %lu", ce.id, ce.lasttime);
	}
	
	void finish_ce()
	{
		debug_print_ce();
		
		if (ce.id <= 0
			|| (ce.interval <= 0 && ce.exectime <= 0)
		|| ce.command.empty() || cmdret < 1) {
			if (gflags_debug)
				log_line("===> IGNORE");
			ce.clear();
			return;
		}
		if (gflags_debug)
			log_line("===> ADD");
		
		/* we have a job to insert */
		if (runat) { /* runat task */
			auto forced_exectime = ce.exectime;
			get_history(ce);
			ce.exectime = forced_exectime;
			debug_print_ce_history();
			
			auto numruns = ce.numruns;
			g_jobs.emplace_back(std::move(ce));
			ce.clear();
			
			/* insert iif we haven't exceeded maxruns */
			assert(g_jobs.size() > 0);
			if (!numruns)
				stack->emplace_back(g_jobs.size() - 1);
			else
				deadstack->emplace_back(g_jobs.size() - 1);
		} else { /* interval task */
			get_history(ce);
			debug_print_ce_history();
			set_initial_exectime(ce);
			
			auto numruns = ce.numruns;
			auto maxruns = ce.maxruns;
			auto exectime = ce.exectime;
			g_jobs.emplace_back(std::move(ce));
			ce.clear();
			
			/* insert iif numruns < maxruns and no constr error */
			assert(g_jobs.size() > 0);
			if ((maxruns == 0 || numruns < maxruns)
				&& exectime != 0)
			stack->emplace_back(g_jobs.size() - 1);
			else
				deadstack->emplace_back(g_jobs.size() - 1);
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
	hstm() {}
	const char *st = nullptr;
	int cs = 0;
	unsigned int id = 0;
	item_history h;
	bool parse_error = false;
};

struct history_entry
{
	history_entry() {}
	history_entry(unsigned int id_, item_history h_) : id(id_), h(std::move(h_)) {}
	
	unsigned int id;
	item_history h;
};
static std::vector<history_entry> history_lut;

#define MARKED_HST() hst.st, (p > hst.st ? static_cast<size_t>(p - hst.st) : 0)


#line 246 "crontab.rl"



#line 207 "crontab.cpp"
static const signed char _history_m_actions[] = {
	0, 1, 0, 1, 1, 1, 2, 1,
	3, 1, 4, 0
};

static const signed char _history_m_key_offsets[] = {
	0, 0, 2, 7, 9, 11, 13, 18,
	23, 0
};

static const char _history_m_trans_keys[] = {
	48, 57, 58, 61, 124, 48, 57, 48,
	57, 48, 57, 48, 57, 58, 61, 124,
	48, 57, 58, 61, 124, 48, 57, 58,
	61, 124, 48, 57, 0
};

static const signed char _history_m_single_lengths[] = {
	0, 0, 3, 0, 0, 0, 3, 3,
	3, 0
};

static const signed char _history_m_range_lengths[] = {
	0, 1, 1, 1, 1, 1, 1, 1,
	1, 0
};

static const signed char _history_m_index_offsets[] = {
	0, 0, 2, 7, 9, 11, 13, 18,
	23, 0
};

static const signed char _history_m_cond_targs[] = {
	2, 0, 3, 4, 5, 2, 0, 6,
	0, 7, 0, 8, 0, 3, 4, 5,
	6, 0, 3, 4, 5, 7, 0, 3,
	4, 5, 8, 0, 0, 1, 2, 3,
	4, 5, 6, 7, 8, 0
};

static const signed char _history_m_cond_actions[] = {
	1, 0, 9, 9, 9, 0, 0, 1,
	0, 1, 0, 1, 0, 5, 5, 5,
	0, 0, 7, 7, 7, 0, 0, 3,
	3, 3, 0, 0, 0, 0, 0, 0,
	0, 0, 5, 7, 3, 0
};

static const signed char _history_m_eof_trans[] = {
	29, 30, 31, 32, 33, 34, 35, 36,
	37, 0
};

static const int history_m_start = 1;
static const int history_m_first_final = 6;
static const int history_m_error = 0;

static const int history_m_en_main = 1;


#line 248 "crontab.rl"


static int do_parse_history(hstm &hst, const char *p, size_t plen)
{
	const char *pe = p + plen;
	const char *eof = pe;
	

#line 274 "crontab.cpp"
	{
		hst.cs = (int)history_m_start;
	}
	
#line 255 "crontab.rl"


#line 279 "crontab.cpp"
	{
		int _klen;
		unsigned int _trans = 0;
		const char * _keys;
		const signed char * _acts;
		unsigned int _nacts;
		_resume: {}
		if ( p == pe && p != eof )
			goto _out;
		if ( p == eof ) {
			if ( _history_m_eof_trans[hst.cs] > 0 ) {
				_trans = (unsigned int)_history_m_eof_trans[hst.cs] - 1;
			}
		}
		else {
			_keys = ( _history_m_trans_keys + (_history_m_key_offsets[hst.cs]));
			_trans = (unsigned int)_history_m_index_offsets[hst.cs];
			
			_klen = (int)_history_m_single_lengths[hst.cs];
			if ( _klen > 0 ) {
				const char *_lower = _keys;
				const char *_upper = _keys + _klen - 1;
				const char *_mid;
				while ( 1 ) {
					if ( _upper < _lower ) {
						_keys += _klen;
						_trans += (unsigned int)_klen;
						break;
					}
					
					_mid = _lower + ((_upper-_lower) >> 1);
					if ( ( (*( p))) < (*( _mid)) )
						_upper = _mid - 1;
					else if ( ( (*( p))) > (*( _mid)) )
						_lower = _mid + 1;
					else {
						_trans += (unsigned int)(_mid - _keys);
						goto _match;
					}
				}
			}
			
			_klen = (int)_history_m_range_lengths[hst.cs];
			if ( _klen > 0 ) {
				const char *_lower = _keys;
				const char *_upper = _keys + (_klen<<1) - 2;
				const char *_mid;
				while ( 1 ) {
					if ( _upper < _lower ) {
						_trans += (unsigned int)_klen;
						break;
					}
					
					_mid = _lower + (((_upper-_lower) >> 1) & ~1);
					if ( ( (*( p))) < (*( _mid)) )
						_upper = _mid - 2;
					else if ( ( (*( p))) > (*( _mid + 1)) )
						_lower = _mid + 2;
					else {
						_trans += (unsigned int)((_mid - _keys)>>1);
						break;
					}
				}
			}
			
			_match: {}
		}
		hst.cs = (int)_history_m_cond_targs[_trans];
		
		if ( _history_m_cond_actions[_trans] != 0 ) {
			
			_acts = ( _history_m_actions + (_history_m_cond_actions[_trans]));
			_nacts = (unsigned int)(*( _acts));
			_acts += 1;
			while ( _nacts > 0 ) {
				switch ( (*( _acts)) )
				{
					case 0:  {
							{
#line 207 "crontab.rl"
							hst.st = p; }
						
#line 361 "crontab.cpp"

						break; 
					}
					case 1:  {
							{
#line 208 "crontab.rl"
							
							if (auto t = nk::from_string<time_t>(MARKED_HST())) {
								hst.h.set_lasttime(*t);
							} else {
								hst.parse_error = true;
								{p += 1; goto _out; }
							}
						}
						
#line 376 "crontab.cpp"

						break; 
					}
					case 2:  {
							{
#line 216 "crontab.rl"
							
							if (auto t = nk::from_string<unsigned>(MARKED_HST())) {
								hst.h.set_numruns(*t);
							} else {
								hst.parse_error = true;
								{p += 1; goto _out; }
							}
						}
						
#line 391 "crontab.cpp"

						break; 
					}
					case 3:  {
							{
#line 224 "crontab.rl"
							
							if (auto t = nk::from_string<time_t>(MARKED_HST())) {
								hst.h.set_exectime(*t);
							} else {
								hst.parse_error = true;
								{p += 1; goto _out; }
							}
						}
						
#line 406 "crontab.cpp"

						break; 
					}
					case 4:  {
							{
#line 232 "crontab.rl"
							
							if (auto t = nk::from_string<unsigned>(MARKED_HST())) {
								hst.id = *t;
							} else {
								hst.parse_error = true;
								{p += 1; goto _out; }
							}
						}
						
#line 421 "crontab.cpp"

						break; 
					}
				}
				_nacts -= 1;
				_acts += 1;
			}
			
		}
		
		if ( p == eof ) {
			if ( hst.cs >= 6 )
				goto _out;
		}
		else {
			if ( hst.cs != 0 ) {
				p += 1;
				goto _resume;
			}
		}
		_out: {}
	}
	
#line 256 "crontab.rl"

	
	if (hst.parse_error) return -1;
		if (hst.cs >= history_m_first_final)
		return 1;
	if (hst.cs == history_m_error)
		return -1;
	return -2;
}

static void parse_history(std::string_view path)
{
	char buf[MAX_LINE];
	auto f = fopen(path.data(), "r");
	if (!f) {
		log_line("%s: failed to open history file \"%s\" for read: %s",
		__func__, path.data(), strerror(errno));
		return;
	}
	SCOPE_EXIT{ fclose(f); };
	size_t linenum = 0;
	while (!feof(f)) {
		if (!fgets(buf, sizeof buf, f)) {
			if (!feof(f))
				log_line("%s: io error fetching line of '%s'", __func__, path.data());
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
		history_lut.emplace_back(h.id, std::move(h.h));
	}
}

static void get_history(Job &item)
{
	for (const auto &i: history_lut) {
		if (i.id == item.id) {
			if (const auto exectm = i.h.exectime())
				item.exectime = *exectm > 0 ? *exectm : 0;
			if (const auto lasttm = i.h.lasttime())
				item.lasttime = *lasttm > 0 ? *lasttm : 0;
			if (const auto t = i.h.numruns())
				item.numruns = *t;
			return;
		}
	}
}

static void addcstlist(ParseCfgState &ncs, Job::cst_list &list,
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

struct Pckm {
	Pckm() {}
	char *st = nullptr;
	int cs = 0;
};

#define MARKED_PCKM() pckm.st, (p > pckm.st ? static_cast<size_t>(p - pckm.st) : 0)


#line 371 "crontab.rl"



#line 545 "crontab.cpp"
static const signed char _parse_cmd_key_m_actions[] = {
	0, 1, 0, 1, 1, 1, 2, 2,
	0, 2, 2, 1, 0, 2, 1, 2,
	3, 1, 0, 2, 0
};

static const signed char _parse_cmd_key_m_key_offsets[] = {
	0, 0, 4, 8, 11, 12, 16, 20,
	24, 0
};

static const char _parse_cmd_key_m_trans_keys[] = {
	0, 9, 32, 92, 0, 9, 32, 92,
	0, 9, 32, 0, 0, 9, 32, 92,
	0, 9, 32, 92, 0, 9, 32, 92,
	0, 9, 32, 92, 0
};

static const signed char _parse_cmd_key_m_single_lengths[] = {
	0, 4, 4, 3, 1, 4, 4, 4,
	4, 0
};

static const signed char _parse_cmd_key_m_range_lengths[] = {
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0
};

static const signed char _parse_cmd_key_m_index_offsets[] = {
	0, 0, 5, 10, 14, 16, 21, 26,
	31, 0
};

static const signed char _parse_cmd_key_m_cond_targs[] = {
	0, 1, 1, 5, 2, 0, 3, 3,
	5, 2, 0, 3, 3, 4, 0, 4,
	0, 3, 6, 5, 2, 0, 3, 3,
	8, 7, 0, 3, 3, 8, 7, 0,
	3, 6, 8, 7, 0, 1, 2, 3,
	4, 5, 6, 7, 8, 0
};

static const signed char _parse_cmd_key_m_cond_actions[] = {
	0, 0, 0, 1, 1, 0, 3, 3,
	0, 0, 0, 1, 1, 1, 0, 0,
	0, 3, 3, 0, 0, 0, 10, 10,
	1, 1, 0, 3, 3, 0, 0, 0,
	3, 3, 0, 0, 0, 0, 3, 7,
	5, 3, 16, 13, 13, 0
};

static const signed char _parse_cmd_key_m_eof_trans[] = {
	37, 38, 39, 40, 41, 42, 43, 44,
	45, 0
};

static const int parse_cmd_key_m_start = 1;
static const int parse_cmd_key_m_first_final = 2;
static const int parse_cmd_key_m_error = 0;

static const int parse_cmd_key_m_en_main = 1;


#line 373 "crontab.rl"


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
	
	Pckm pckm;
	
	if (ncs.cmdret != 0) {
		ncs.cmdret = -3;
		log_line("Duplicate 'command' value at line %zu", ncs.linenum);
		exit(EXIT_FAILURE);
	}
	

#line 629 "crontab.cpp"
	{
		pckm.cs = (int)parse_cmd_key_m_start;
	}
	
#line 394 "crontab.rl"


#line 634 "crontab.cpp"
	{
		int _klen;
		unsigned int _trans = 0;
		const char * _keys;
		const signed char * _acts;
		unsigned int _nacts;
		_resume: {}
		if ( p == pe && p != eof )
			goto _out;
		if ( p == eof ) {
			if ( _parse_cmd_key_m_eof_trans[pckm.cs] > 0 ) {
				_trans = (unsigned int)_parse_cmd_key_m_eof_trans[pckm.cs] - 1;
			}
		}
		else {
			_keys = ( _parse_cmd_key_m_trans_keys + (_parse_cmd_key_m_key_offsets[pckm.cs]));
			_trans = (unsigned int)_parse_cmd_key_m_index_offsets[pckm.cs];
			
			_klen = (int)_parse_cmd_key_m_single_lengths[pckm.cs];
			if ( _klen > 0 ) {
				const char *_lower = _keys;
				const char *_upper = _keys + _klen - 1;
				const char *_mid;
				while ( 1 ) {
					if ( _upper < _lower ) {
						_keys += _klen;
						_trans += (unsigned int)_klen;
						break;
					}
					
					_mid = _lower + ((_upper-_lower) >> 1);
					if ( ( (*( p))) < (*( _mid)) )
						_upper = _mid - 1;
					else if ( ( (*( p))) > (*( _mid)) )
						_lower = _mid + 1;
					else {
						_trans += (unsigned int)(_mid - _keys);
						goto _match;
					}
				}
			}
			
			_klen = (int)_parse_cmd_key_m_range_lengths[pckm.cs];
			if ( _klen > 0 ) {
				const char *_lower = _keys;
				const char *_upper = _keys + (_klen<<1) - 2;
				const char *_mid;
				while ( 1 ) {
					if ( _upper < _lower ) {
						_trans += (unsigned int)_klen;
						break;
					}
					
					_mid = _lower + (((_upper-_lower) >> 1) & ~1);
					if ( ( (*( p))) < (*( _mid)) )
						_upper = _mid - 2;
					else if ( ( (*( p))) > (*( _mid + 1)) )
						_lower = _mid + 2;
					else {
						_trans += (unsigned int)((_mid - _keys)>>1);
						break;
					}
				}
			}
			
			_match: {}
		}
		pckm.cs = (int)_parse_cmd_key_m_cond_targs[_trans];
		
		if ( _parse_cmd_key_m_cond_actions[_trans] != 0 ) {
			
			_acts = ( _parse_cmd_key_m_actions + (_parse_cmd_key_m_cond_actions[_trans]));
			_nacts = (unsigned int)(*( _acts));
			_acts += 1;
			while ( _nacts > 0 ) {
				switch ( (*( _acts)) )
				{
					case 0:  {
							{
#line 358 "crontab.rl"
							pckm.st = p; }
						
#line 716 "crontab.cpp"

						break; 
					}
					case 1:  {
							{
#line 359 "crontab.rl"
							
							ncs.ce.command = std::string(MARKED_PCKM());
							string_replace_all(ncs.ce.command, "\\ ", 2, " ");
							string_replace_all(ncs.ce.command, "\\\\", 2, "\\");
						}
						
#line 728 "crontab.cpp"

						break; 
					}
					case 2:  {
							{
#line 364 "crontab.rl"
							ncs.ce.args = std::string(MARKED_PCKM()); }
						
#line 736 "crontab.cpp"

						break; 
					}
				}
				_nacts -= 1;
				_acts += 1;
			}
			
		}
		
		if ( p == eof ) {
			if ( pckm.cs >= 2 )
				goto _out;
		}
		else {
			if ( pckm.cs != 0 ) {
				p += 1;
				goto _resume;
			}
		}
		_out: {}
	}
	
#line 395 "crontab.rl"

	
	if (pckm.cs == parse_cmd_key_m_error) {
		ncs.cmdret = -1;
		log_line("Malformed 'command' value at line %zu", ncs.linenum);
		exit(EXIT_FAILURE);
	} else if (pckm.cs >= parse_cmd_key_m_first_final)
	ncs.cmdret = 1;
	else {
		ncs.cmdret = -2;
		log_line("Incomplete 'command' value at line %zu", ncs.linenum);
		exit(EXIT_FAILURE);
	}
}

#define MARKED_TIME() ncs.time_st, (p > (ncs.time_st + 1) ? static_cast<size_t>(p - ncs.time_st - 1) : 0)
#define MARKED_INTV1() ncs.intv_st, (p > ncs.intv_st ? static_cast<size_t>(p - ncs.intv_st) : 0)
#define MARKED_INTV2() ncs.intv2_st, (p > ncs.intv2_st ? static_cast<size_t>(p - ncs.intv2_st) : 0)
#define MARKED_JOBID() ncs.jobid_st, (p > ncs.jobid_st ? static_cast<size_t>(p - ncs.jobid_st) : 0)


#line 566 "crontab.rl"



#line 782 "crontab.cpp"
static const signed char _ncrontab_actions[] = {
	0, 1, 0, 1, 1, 1, 2, 1,
	3, 1, 4, 1, 5, 1, 6, 1,
	7, 1, 8, 1, 10, 1, 12, 1,
	22, 1, 23, 1, 24, 2, 1, 0,
	2, 1, 15, 2, 2, 0, 2, 2,
	15, 2, 3, 0, 2, 3, 15, 2,
	4, 0, 2, 4, 15, 2, 5, 0,
	2, 5, 15, 2, 7, 13, 2, 7,
	14, 2, 7, 16, 2, 7, 17, 2,
	7, 18, 2, 7, 19, 2, 7, 20,
	2, 9, 16, 2, 9, 17, 2, 9,
	18, 2, 9, 19, 2, 9, 20, 2,
	11, 21, 0
};

static const short _ncrontab_key_offsets[] = {
	0, 0, 17, 19, 21, 23, 25, 27,
	29, 31, 34, 38, 40, 42, 45, 49,
	51, 53, 55, 57, 60, 64, 66, 68,
	70, 72, 74, 76, 78, 80, 83, 87,
	94, 96, 98, 100, 102, 104, 106, 112,
	114, 116, 118, 120, 122, 125, 129, 131,
	133, 135, 137, 140, 144, 146, 148, 150,
	152, 155, 159, 161, 163, 165, 167, 169,
	172, 176, 178, 180, 182, 184, 186, 188,
	191, 195, 197, 199, 201, 205, 208, 210,
	213, 215, 219, 223, 227, 231, 235, 235,
	237, 240, 242, 245, 247, 249, 252, 0
};

static const char _ncrontab_trans_keys[] = {
	33, 67, 68, 72, 73, 74, 77, 82,
	87, 99, 100, 104, 105, 106, 109, 114,
	119, 48, 57, 79, 111, 77, 109, 77,
	109, 65, 97, 78, 110, 68, 100, 9,
	32, 61, 0, 9, 10, 32, 65, 97,
	89, 121, 9, 32, 61, 9, 32, 48,
	57, 48, 57, 79, 111, 85, 117, 82,
	114, 9, 32, 61, 9, 32, 48, 57,
	48, 57, 78, 110, 84, 116, 69, 101,
	82, 114, 86, 118, 65, 97, 76, 108,
	9, 32, 61, 9, 32, 48, 57, 100,
	104, 109, 115, 119, 48, 57, 79, 111,
	85, 117, 82, 114, 78, 110, 65, 97,
	76, 108, 65, 73, 79, 97, 105, 111,
	88, 120, 82, 114, 85, 117, 78, 110,
	83, 115, 9, 32, 61, 9, 32, 48,
	57, 78, 110, 85, 117, 84, 116, 69,
	101, 9, 32, 61, 9, 32, 48, 57,
	48, 57, 78, 110, 84, 116, 72, 104,
	9, 32, 61, 9, 32, 48, 57, 48,
	57, 85, 117, 78, 110, 65, 97, 84,
	116, 9, 32, 61, 9, 32, 48, 57,
	69, 101, 69, 101, 75, 107, 68, 100,
	65, 97, 89, 121, 9, 32, 61, 9,
	32, 48, 57, 48, 57, 48, 57, 0,
	10, 0, 9, 10, 32, 44, 48, 57,
	48, 57, 44, 48, 57, 48, 57, 9,
	32, 48, 57, 9, 32, 48, 57, 9,
	32, 48, 57, 9, 32, 48, 57, 9,
	32, 48, 57, 48, 57, 44, 48, 57,
	48, 57, 44, 48, 57, 48, 57, 48,
	57, 44, 48, 57, 48, 57, 0
};

static const signed char _ncrontab_single_lengths[] = {
	0, 17, 0, 2, 2, 2, 2, 2,
	2, 3, 4, 2, 2, 3, 2, 0,
	2, 2, 2, 3, 2, 0, 2, 2,
	2, 2, 2, 2, 2, 3, 2, 5,
	2, 2, 2, 2, 2, 2, 6, 2,
	2, 2, 2, 2, 3, 2, 2, 2,
	2, 2, 3, 2, 0, 2, 2, 2,
	3, 2, 0, 2, 2, 2, 2, 3,
	2, 2, 2, 2, 2, 2, 2, 3,
	2, 0, 0, 2, 4, 1, 0, 1,
	0, 2, 2, 2, 2, 2, 0, 0,
	1, 0, 1, 0, 0, 1, 0, 0
};

static const signed char _ncrontab_range_lengths[] = {
	0, 0, 1, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 1, 1,
	0, 0, 0, 0, 1, 1, 0, 0,
	0, 0, 0, 0, 0, 0, 1, 1,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 1, 0, 0,
	0, 0, 0, 1, 1, 0, 0, 0,
	0, 1, 1, 0, 0, 0, 0, 0,
	1, 0, 0, 0, 0, 0, 0, 0,
	1, 1, 1, 0, 0, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 0, 1,
	1, 1, 1, 1, 1, 1, 1, 0
};

static const short _ncrontab_index_offsets[] = {
	0, 0, 18, 20, 23, 26, 29, 32,
	35, 38, 42, 47, 50, 53, 57, 61,
	63, 66, 69, 72, 76, 80, 82, 85,
	88, 91, 94, 97, 100, 103, 107, 111,
	118, 121, 124, 127, 130, 133, 136, 143,
	146, 149, 152, 155, 158, 162, 166, 169,
	172, 175, 178, 182, 186, 188, 191, 194,
	197, 201, 205, 207, 210, 213, 216, 219,
	223, 227, 230, 233, 236, 239, 242, 245,
	249, 253, 255, 257, 260, 265, 268, 270,
	273, 275, 279, 283, 287, 291, 295, 296,
	298, 301, 303, 306, 308, 310, 313, 0
};

static const signed char _ncrontab_cond_targs[] = {
	2, 3, 11, 16, 22, 32, 38, 59,
	65, 3, 11, 16, 22, 32, 38, 59,
	65, 0, 74, 0, 4, 4, 0, 5,
	5, 0, 6, 6, 0, 7, 7, 0,
	8, 8, 0, 9, 9, 0, 9, 9,
	10, 0, 0, 76, 0, 76, 75, 12,
	12, 0, 13, 13, 0, 13, 13, 14,
	0, 14, 14, 77, 0, 78, 0, 17,
	17, 0, 18, 18, 0, 19, 19, 0,
	19, 19, 20, 0, 20, 20, 79, 0,
	80, 0, 23, 23, 0, 24, 24, 0,
	25, 25, 0, 26, 26, 0, 27, 27,
	0, 28, 28, 0, 29, 29, 0, 29,
	29, 30, 0, 30, 30, 31, 0, 81,
	82, 83, 84, 85, 31, 0, 33, 33,
	0, 34, 34, 0, 35, 35, 0, 36,
	36, 0, 37, 37, 0, 86, 86, 0,
	39, 46, 53, 39, 46, 53, 0, 40,
	40, 0, 41, 41, 0, 42, 42, 0,
	43, 43, 0, 44, 44, 0, 44, 44,
	45, 0, 45, 45, 87, 0, 47, 47,
	0, 48, 48, 0, 49, 49, 0, 50,
	50, 0, 50, 50, 51, 0, 51, 51,
	88, 0, 89, 0, 54, 54, 0, 55,
	55, 0, 56, 56, 0, 56, 56, 57,
	0, 57, 57, 90, 0, 91, 0, 60,
	60, 0, 61, 61, 0, 62, 62, 0,
	63, 63, 0, 63, 63, 64, 0, 64,
	64, 92, 0, 66, 66, 0, 67, 67,
	0, 68, 68, 0, 69, 69, 0, 70,
	70, 0, 71, 71, 0, 71, 71, 72,
	0, 72, 72, 93, 0, 94, 0, 74,
	0, 0, 0, 75, 0, 76, 0, 76,
	75, 15, 77, 0, 78, 0, 21, 79,
	0, 80, 0, 30, 30, 31, 0, 30,
	30, 31, 0, 30, 30, 31, 0, 30,
	30, 31, 0, 30, 30, 31, 0, 0,
	87, 0, 52, 88, 0, 89, 0, 58,
	90, 0, 91, 0, 92, 0, 73, 93,
	0, 94, 0, 0, 1, 2, 3, 4,
	5, 6, 7, 8, 9, 10, 11, 12,
	13, 14, 15, 16, 17, 18, 19, 20,
	21, 22, 23, 24, 25, 26, 27, 28,
	29, 30, 31, 32, 33, 34, 35, 36,
	37, 38, 39, 40, 41, 42, 43, 44,
	45, 46, 47, 48, 49, 50, 51, 52,
	53, 54, 55, 56, 57, 58, 59, 60,
	61, 62, 63, 64, 65, 66, 67, 68,
	69, 70, 71, 72, 73, 74, 75, 76,
	77, 78, 79, 80, 81, 82, 83, 84,
	85, 86, 87, 88, 89, 90, 91, 92,
	93, 94, 0
};

static const signed char _ncrontab_cond_actions[] = {
	27, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 23, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 19, 0, 19, 19, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 13, 0, 17, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 13, 0,
	17, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 1, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 13, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	13, 0, 17, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 13, 0, 17, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 13, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 13, 0, 17, 0, 0,
	0, 0, 0, 0, 0, 19, 0, 19,
	19, 15, 0, 0, 0, 0, 15, 0,
	0, 0, 0, 9, 9, 47, 0, 7,
	7, 41, 0, 5, 5, 35, 0, 3,
	3, 29, 0, 11, 11, 53, 0, 0,
	0, 0, 15, 0, 0, 0, 0, 15,
	0, 0, 0, 0, 0, 0, 15, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 25, 95, 95,
	68, 83, 74, 89, 50, 44, 38, 32,
	56, 21, 62, 77, 92, 65, 80, 59,
	71, 86, 0
};

static const short _ncrontab_eof_trans[] = {
	316, 317, 318, 319, 320, 321, 322, 323,
	324, 325, 326, 327, 328, 329, 330, 331,
	332, 333, 334, 335, 336, 337, 338, 339,
	340, 341, 342, 343, 344, 345, 346, 347,
	348, 349, 350, 351, 352, 353, 354, 355,
	356, 357, 358, 359, 360, 361, 362, 363,
	364, 365, 366, 367, 368, 369, 370, 371,
	372, 373, 374, 375, 376, 377, 378, 379,
	380, 381, 382, 383, 384, 385, 386, 387,
	388, 389, 390, 391, 392, 393, 394, 395,
	396, 397, 398, 399, 400, 401, 402, 403,
	404, 405, 406, 407, 408, 409, 410, 0
};

static const int ncrontab_start = 1;
static const int ncrontab_first_final = 74;
static const int ncrontab_error = 0;

static const int ncrontab_en_main = 1;


#line 568 "crontab.rl"


static int do_parse_config(ParseCfgState &ncs, const char *p, size_t plen)
{
	const char *pe = p + plen;
	const char *eof = pe;
	

#line 1032 "crontab.cpp"
	{
		ncs.cs = (int)ncrontab_start;
	}
	
#line 575 "crontab.rl"


#line 1037 "crontab.cpp"
	{
		int _klen;
		unsigned int _trans = 0;
		const char * _keys;
		const signed char * _acts;
		unsigned int _nacts;
		_resume: {}
		if ( p == pe && p != eof )
			goto _out;
		if ( p == eof ) {
			if ( _ncrontab_eof_trans[ncs.cs] > 0 ) {
				_trans = (unsigned int)_ncrontab_eof_trans[ncs.cs] - 1;
			}
		}
		else {
			_keys = ( _ncrontab_trans_keys + (_ncrontab_key_offsets[ncs.cs]));
			_trans = (unsigned int)_ncrontab_index_offsets[ncs.cs];
			
			_klen = (int)_ncrontab_single_lengths[ncs.cs];
			if ( _klen > 0 ) {
				const char *_lower = _keys;
				const char *_upper = _keys + _klen - 1;
				const char *_mid;
				while ( 1 ) {
					if ( _upper < _lower ) {
						_keys += _klen;
						_trans += (unsigned int)_klen;
						break;
					}
					
					_mid = _lower + ((_upper-_lower) >> 1);
					if ( ( (*( p))) < (*( _mid)) )
						_upper = _mid - 1;
					else if ( ( (*( p))) > (*( _mid)) )
						_lower = _mid + 1;
					else {
						_trans += (unsigned int)(_mid - _keys);
						goto _match;
					}
				}
			}
			
			_klen = (int)_ncrontab_range_lengths[ncs.cs];
			if ( _klen > 0 ) {
				const char *_lower = _keys;
				const char *_upper = _keys + (_klen<<1) - 2;
				const char *_mid;
				while ( 1 ) {
					if ( _upper < _lower ) {
						_trans += (unsigned int)_klen;
						break;
					}
					
					_mid = _lower + (((_upper-_lower) >> 1) & ~1);
					if ( ( (*( p))) < (*( _mid)) )
						_upper = _mid - 2;
					else if ( ( (*( p))) > (*( _mid + 1)) )
						_lower = _mid + 2;
					else {
						_trans += (unsigned int)((_mid - _keys)>>1);
						break;
					}
				}
			}
			
			_match: {}
		}
		ncs.cs = (int)_ncrontab_cond_targs[_trans];
		
		if ( _ncrontab_cond_actions[_trans] != 0 ) {
			
			_acts = ( _ncrontab_actions + (_ncrontab_cond_actions[_trans]));
			_nacts = (unsigned int)(*( _acts));
			_acts += 1;
			while ( _nacts > 0 ) {
				switch ( (*( _acts)) )
				{
					case 0:  {
							{
#line 422 "crontab.rl"
							ncs.time_st = p; ncs.v_time = 0; }
						
#line 1119 "crontab.cpp"

						break; 
					}
					case 1:  {
							{
#line 423 "crontab.rl"
							
							if (auto t = nk::from_string<unsigned>(MARKED_TIME())) {
								ncs.v_time += *t;
							} else {
								ncs.parse_error = true;
								{p += 1; goto _out; }
							}
						}
						
#line 1134 "crontab.cpp"

						break; 
					}
					case 2:  {
							{
#line 431 "crontab.rl"
							
							if (auto t = nk::from_string<unsigned>(MARKED_TIME())) {
								ncs.v_time += 60 * *t;
							} else {
								ncs.parse_error = true;
								{p += 1; goto _out; }
							}
						}
						
#line 1149 "crontab.cpp"

						break; 
					}
					case 3:  {
							{
#line 439 "crontab.rl"
							
							if (auto t = nk::from_string<unsigned>(MARKED_TIME())) {
								ncs.v_time += 3600 * *t;
							} else {
								ncs.parse_error = true;
								{p += 1; goto _out; }
							}
						}
						
#line 1164 "crontab.cpp"

						break; 
					}
					case 4:  {
							{
#line 447 "crontab.rl"
							
							if (auto t = nk::from_string<unsigned>(MARKED_TIME())) {
								ncs.v_time += 86400 * *t;
							} else {
								ncs.parse_error = true;
								{p += 1; goto _out; }
							}
						}
						
#line 1179 "crontab.cpp"

						break; 
					}
					case 5:  {
							{
#line 455 "crontab.rl"
							
							if (auto t = nk::from_string<unsigned>(MARKED_TIME())) {
								ncs.v_time += 604800 * *t;
							} else {
								ncs.parse_error = true;
								{p += 1; goto _out; }
							}
						}
						
#line 1194 "crontab.cpp"

						break; 
					}
					case 6:  {
							{
#line 464 "crontab.rl"
							
							ncs.intv_st = p;
							ncs.v_int = ncs.v_int2 = 0;
							ncs.intv2_exist = false;
						}
						
#line 1206 "crontab.cpp"

						break; 
					}
					case 7:  {
							{
#line 469 "crontab.rl"
							
							if (auto t = nk::from_string<int>(MARKED_INTV1())) {
								ncs.v_int = *t;
							} else {
								ncs.parse_error = true;
								{p += 1; goto _out; }
							}
						}
						
#line 1221 "crontab.cpp"

						break; 
					}
					case 8:  {
							{
#line 477 "crontab.rl"
							ncs.intv2_st = p; }
						
#line 1229 "crontab.cpp"

						break; 
					}
					case 9:  {
							{
#line 478 "crontab.rl"
							
							if (auto t = nk::from_string<int>(MARKED_INTV2())) {
								ncs.v_int2 = *t;
							} else {
								ncs.parse_error = true;
								{p += 1; goto _out; }
							}
							ncs.intv2_exist = true;
						}
						
#line 1245 "crontab.cpp"

						break; 
					}
					case 10:  {
							{
#line 488 "crontab.rl"
							ncs.strv_st = p; ncs.v_strlen = 0; }
						
#line 1253 "crontab.cpp"

						break; 
					}
					case 11:  {
							{
#line 489 "crontab.rl"
							
							ncs.v_strlen = p > ncs.strv_st ? static_cast<size_t>(p - ncs.strv_st) : 0;
							if (ncs.v_strlen >= sizeof ncs.v_str) {
								log_line("error parsing line %zu in crontab: too long", ncs.linenum);
								exit(EXIT_FAILURE);
							}
							memcpy(ncs.v_str, ncs.strv_st, ncs.v_strlen);
							ncs.v_str[ncs.v_strlen] = 0;
						}
						
#line 1269 "crontab.cpp"

						break; 
					}
					case 12:  {
							{
#line 512 "crontab.rl"
							ncs.ce.journal = true; }
						
#line 1277 "crontab.cpp"

						break; 
					}
					case 13:  {
							{
#line 515 "crontab.rl"
							
							ncs.runat = true;
							ncs.ce.exectime = ncs.v_int;
							ncs.ce.maxruns = 1;
							ncs.ce.journal = true;
						}
						
#line 1290 "crontab.cpp"

						break; 
					}
					case 14:  {
							{
#line 521 "crontab.rl"
							
							if (!ncs.runat)
							ncs.ce.maxruns = ncs.v_int > 0 ? static_cast<unsigned>(ncs.v_int) : 0;
						}
						
#line 1301 "crontab.cpp"

						break; 
					}
					case 15:  {
							{
#line 529 "crontab.rl"
							ncs.ce.interval = ncs.v_time; }
						
#line 1309 "crontab.cpp"

						break; 
					}
					case 16:  {
							{
#line 533 "crontab.rl"
							addcstlist(ncs, ncs.ce.month, 0, 1, 12); }
						
#line 1317 "crontab.cpp"

						break; 
					}
					case 17:  {
							{
#line 534 "crontab.rl"
							addcstlist(ncs, ncs.ce.day, 0, 1, 31); }
						
#line 1325 "crontab.cpp"

						break; 
					}
					case 18:  {
							{
#line 535 "crontab.rl"
							addcstlist(ncs, ncs.ce.weekday, 0, 1, 7); }
						
#line 1333 "crontab.cpp"

						break; 
					}
					case 19:  {
							{
#line 536 "crontab.rl"
							addcstlist(ncs, ncs.ce.hour, 24, 0, 23); }
						
#line 1341 "crontab.cpp"

						break; 
					}
					case 20:  {
							{
#line 537 "crontab.rl"
							addcstlist(ncs, ncs.ce.minute, 60, 0, 59); }
						
#line 1349 "crontab.cpp"

						break; 
					}
					case 21:  {
							{
#line 545 "crontab.rl"
							parse_command_key(ncs); }
						
#line 1357 "crontab.cpp"

						break; 
					}
					case 22:  {
							{
#line 552 "crontab.rl"
							ncs.jobid_st = p; }
						
#line 1365 "crontab.cpp"

						break; 
					}
					case 23:  {
							{
#line 553 "crontab.rl"
							
							if (auto t = nk::from_string<unsigned>(MARKED_JOBID())) {
								ncs.ce.id = *t;
							} else {
								ncs.parse_error = true;
								{p += 1; goto _out; }
							}
						}
						
#line 1380 "crontab.cpp"

						break; 
					}
					case 24:  {
							{
#line 561 "crontab.rl"
							ncs.finish_ce(); ncs.create_ce(); }
						
#line 1388 "crontab.cpp"

						break; 
					}
				}
				_nacts -= 1;
				_acts += 1;
			}
			
		}
		
		if ( p == eof ) {
			if ( ncs.cs >= 74 )
				goto _out;
		}
		else {
			if ( ncs.cs != 0 ) {
				p += 1;
				goto _resume;
			}
		}
		_out: {}
	}
	
#line 576 "crontab.rl"

	
	if (ncs.parse_error) return -1;
		if (ncs.cs == ncrontab_error)
		return -1;
	if (ncs.cs >= ncrontab_first_final)
		return 1;
	return 0;
}

void parse_config(std::string_view path, std::string_view execfile,
std::vector<StackItem> *stk,
std::vector<StackItem> *deadstk)
{
	g_jobs.clear();
	ParseCfgState ncs(execfile, stk, deadstk);
	parse_history(ncs.execfile);
	
	char buf[MAX_LINE];
	auto f = fopen(path.data(), "r");
	if (!f) {
		log_line("%s: failed to open file: '%s': %s", __func__, path.data(), strerror(errno));
		exit(EXIT_FAILURE);
	}
	SCOPE_EXIT{ fclose(f); };
	while (!feof(f)) {
		if (!fgets(buf, sizeof buf, f)) {
			if (!feof(f))
				log_line("%s: io error fetching line of '%s'", __func__, path.data());
			break;
		}
		auto llen = strlen(buf);
		if (llen == 0)
			continue;
		if (buf[llen-1] == '\n')
			buf[--llen] = 0;
		++ncs.linenum;
		if (do_parse_config(ncs, buf, llen) < 0) {
			log_line("%s: do_parse_config(%s) failed at line %zu", __func__, path.data(), ncs.linenum);
			exit(EXIT_FAILURE);
		}
	}
	std::sort(stk->begin(), stk->end(), LtCronEntry);
	history_lut.clear();
	cfg_reload = 1;
}

