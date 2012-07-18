// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */


#include "LogMonitor.h"
#include "Monitor.h"
#include "MonitorDBStore.h"

#include "messages/MMonCommand.h"
#include "messages/MLog.h"
#include "messages/MLogAck.h"

#include "common/Timer.h"

#include "osd/osd_types.h"

#include "common/config.h"
#include <sstream>

#define dout_subsys ceph_subsys_mon
#undef dout_prefix
#define dout_prefix _prefix(_dout, mon, get_version())
static ostream& _prefix(std::ostream *_dout, Monitor *mon, version_t v) {
  return *_dout << "mon." << mon->name << "@" << mon->rank
		<< "(" << mon->get_state_name()
		<< ").log v" << v << " ";
}

ostream& operator<<(ostream& out, LogMonitor& pm)
{
  std::stringstream ss;
  /*
  for (hash_map<int,int>::iterator p = pm.pg_map.num_pg_by_state.begin();
       p != pm.pg_map.num_pg_by_state.end();
       ++p) {
    if (p != pm.pg_map.num_pg_by_state.begin())
      ss << ", ";
    ss << p->second << " " << pg_state_string(p->first);
  }
  string states = ss.str();
  return out << "v" << pm.pg_map.version << ": "
	     << pm.pg_map.pg_stat.size() << " pgs: "
	     << states << "; "
	     << kb_t(pm.pg_map.total_pg_kb()) << " data, " 
	     << kb_t(pm.pg_map.total_used_kb()) << " used, "
	     << kb_t(pm.pg_map.total_avail_kb()) << " / "
	     << kb_t(pm.pg_map.total_kb()) << " free";
  */
  return out << "log";
}

/*
 Tick function to update the map based on performance every N seconds
*/

void LogMonitor::tick() 
{
  if (!is_active()) return;

  update_from_paxos();
  dout(10) << *this << dendl;

  if (!mon->is_leader()) return; 

}

void LogMonitor::create_initial()
{
  dout(10) << "create_initial -- creating initial map" << dendl;
  LogEntry e;
  memset(&e.who, 0, sizeof(e.who));
  e.stamp = ceph_clock_now(g_ceph_context);
  e.type = CLOG_INFO;
  std::stringstream ss;
  ss << "mkfs " << mon->monmap->get_fsid();
  e.msg = ss.str();
  e.seq = 0;
  pending_log.insert(pair<utime_t,LogEntry>(e.stamp, e));
}

void LogMonitor::update_from_paxos()
{
  version_t version = get_version();
  if (version == summary.version)
    return;
  assert(version >= summary.version);

  bufferlist blog;
  bufferlist blogdebug;
  bufferlist bloginfo;
  bufferlist blogwarn;
  bufferlist blogerr;
  bufferlist blogsec;

  version_t latest_full = get_version_latest_full();
  if ((latest_full > 0) && (latest_full > summary.version)) {
      bufferlist latest_bl;
      get_version_full(latest_full, latest_bl);
      assert(latest_bl.length() != 0);
      dout(7) << __func__ << " loading summary e" << latest_full << dendl;
      bufferlist::iterator p = latest_bl.begin();
      ::decode(summary, p);
      dout(7) << __func__ << " loaded summary e" << summary.version << dendl;
  }

  // walk through incrementals
  while (version > summary.version) {
    bufferlist bl;
    int err = get_version(summary.version+1, bl);
    assert(err == 0);

    bufferlist::iterator p = bl.begin();
    __u8 v;
    ::decode(v, p);
    while (!p.end()) {
      LogEntry le;
      le.decode(p);
      dout(7) << "update_from_paxos applying incremental log " << summary.version+1 <<  " " << le << dendl;

      stringstream ss;
      ss << le;
      string s;
      getline(ss, s);
      s += "\n";

      blog.append(s);
      if (le.type >= CLOG_DEBUG)
	blogdebug.append(s);
      if (le.type >= CLOG_INFO)
	bloginfo.append(s);
      if (le.type == CLOG_SEC)
        blogsec.append(s);
      if (le.type >= CLOG_WARN)
	blogwarn.append(s);
      if (le.type >= CLOG_ERROR)
	blogerr.append(s);

      summary.add(le);
    }

    summary.version++;
  }

  MonitorDBStore::Transaction t;

  if (blog.length())
    store_do_append(&t, "log", blog);
  if (blogdebug.length())
    store_do_append(&t, "log.debug", blogdebug);
  if (bloginfo.length())
    store_do_append(&t, "log.info", bloginfo);
  if (blogsec.length())
    store_do_append(&t, "log.security", blogsec);
  if (blogwarn.length())
    store_do_append(&t, "log.warn", blogwarn);
  if (blogerr.length())
    store_do_append(&t, "log.err", blogerr);
  if (!t.empty())
    mon->store->apply_transaction(t);


  // trim
  unsigned max = g_conf->mon_max_log_epochs;
  if (mon->is_leader() && version > max)
    trim_to(version - max);

  check_subs();
}

void LogMonitor::store_do_append(MonitorDBStore::Transaction *t,
    const string& key, bufferlist& bl)
{
  bufferlist existing_bl;
  int err = get_value(key, existing_bl);
  assert(err == 0);

  existing_bl.append(bl);
  put_value(t, key, existing_bl);
}

void LogMonitor::create_pending()
{
  pending_log.clear();
  pending_summary = summary;
  dout(10) << "create_pending v " << (get_version() + 1) << dendl;
}

void LogMonitor::encode_pending(MonitorDBStore::Transaction *t)
{
  version_t version = get_version() + 1;
  bufferlist bl;
  dout(10) << __func__ << " v" << version << dendl;
  __u8 v = 1;
  ::encode(v, bl);
  multimap<utime_t,LogEntry>::iterator p;
  for (p = pending_log.begin(); p != pending_log.end(); p++)
    p->second.encode(bl);

  bufferlist summary_bl;
  ::encode(summary, summary_bl);

  put_version(t, version, bl);
  put_last_committed(t, version);

  put_version_full(t, version, summary_bl);
  put_version_latest_full(t, version);
}

bool LogMonitor::preprocess_query(PaxosServiceMessage *m)
{
  dout(10) << "preprocess_query " << *m << " from " << m->get_orig_source_inst() << dendl;
  switch (m->get_type()) {
  case MSG_MON_COMMAND:
    return preprocess_command((MMonCommand*)m);

  case MSG_LOG:
    return preprocess_log((MLog*)m);

  default:
    assert(0);
    m->put();
    return true;
  }
}

bool LogMonitor::prepare_update(PaxosServiceMessage *m)
{
  dout(10) << "prepare_update " << *m << " from " << m->get_orig_source_inst() << dendl;
  switch (m->get_type()) {
  case MSG_MON_COMMAND:
    return prepare_command((MMonCommand*)m);
  case MSG_LOG:
    return prepare_log((MLog*)m);
  default:
    assert(0);
    m->put();
    return false;
  }
}

bool LogMonitor::preprocess_log(MLog *m)
{
  dout(10) << "preprocess_log " << *m << " from " << m->get_orig_source() << dendl;
  int num_new = 0;

  MonSession *session = m->get_session();
  if (!session)
    goto done;
  if (!session->caps.check_privileges(PAXOS_LOG, MON_CAP_X)) {
    dout(0) << "preprocess_log got MLog from entity with insufficient privileges "
	    << session->caps << dendl;
    goto done;
  }
  
  for (deque<LogEntry>::iterator p = m->entries.begin();
       p != m->entries.end();
       p++) {
    if (!pending_summary.contains(p->key()))
      num_new++;
  }
  if (!num_new) {
    dout(10) << "  nothing new" << dendl;
    goto done;
  }

  return false;

 done:
  m->put();
  return true;
}

bool LogMonitor::prepare_log(MLog *m) 
{
  dout(10) << "prepare_log " << *m << " from " << m->get_orig_source() << dendl;

  if (m->fsid != mon->monmap->fsid) {
    dout(0) << "handle_log on fsid " << m->fsid << " != " << mon->monmap->fsid 
	    << dendl;
    m->put();
    return false;
  }

  for (deque<LogEntry>::iterator p = m->entries.begin();
       p != m->entries.end();
       p++) {
    dout(10) << " logging " << *p << dendl;
    if (!pending_summary.contains(p->key())) {
      pending_summary.add(*p);
      pending_log.insert(pair<utime_t,LogEntry>(p->stamp, *p));

    }
  }
  paxos->wait_for_commit(new C_Log(this, m));
  return true;
}

void LogMonitor::_updated_log(MLog *m)
{
  dout(7) << "_updated_log for " << m->get_orig_source_inst() << dendl;
  mon->send_reply(m, new MLogAck(m->fsid, m->entries.rbegin()->seq));

  m->put();
}



bool LogMonitor::preprocess_command(MMonCommand *m)
{
  int r = -1;
  bufferlist rdata;
  stringstream ss;

  if (r != -1) {
    string rs;
    getline(ss, rs);
    mon->reply_command(m, r, rs, rdata, get_version());
    return true;
  } else
    return false;
}


bool LogMonitor::prepare_command(MMonCommand *m)
{
  stringstream ss;
  string rs;
  int err = -EINVAL;

  // nothing here yet
  ss << "unrecognized command";

  getline(ss, rs);
  mon->reply_command(m, err, rs, get_version());
  return false;
}


void LogMonitor::check_subs()
{
  dout(10) << __func__ << dendl;

  map<string, xlist<Subscription*>*>::iterator subs_map_it;
  subs_map_it = mon->session_map.subs.begin();

  for (; subs_map_it != mon->session_map.subs.end(); subs_map_it++) {

    xlist<Subscription*> *subs_lst = subs_map_it->second;
    xlist<Subscription*>::iterator subs_lst_it = subs_lst->begin();

    for (; !subs_lst_it.end(); ++subs_lst_it)
      check_sub(*subs_lst_it);
  }
}

void LogMonitor::check_sub(Subscription *s)
{
  dout(10) << __func__ << " client wants " << s->type << " ver " << s->next << dendl;

  map<string, int> types;
  types["log-debug"]  = CLOG_DEBUG;
  types["log-info"]   = CLOG_INFO;
  types["log-sec"]    = CLOG_SEC;
  types["log-warn"]   = CLOG_WARN;
  types["log-error"]  = CLOG_ERROR;

  if (!types.count(s->type)) {
    dout(1) << __func__ << " sub " << s->type << " not log type " << dendl;
    return;
  }

  version_t summary_version = summary.version;

  if (s->next > summary_version) {
    dout(10) << __func__ << " client " << s->session->inst 
	    << " requested version (" << s->next << ") is greater than ours (" 
	    << summary_version << "), which means we already sent him" 
	    << " everything we have." << dendl;
    return;
  } 
 
  int sub_level = types[s->type];
  bool ret = true;
  MLog *mlog = new MLog(mon->monmap->fsid);

  if (s->next == 0) { 
    /* First timer, heh? */
    ret = _create_sub_summary(mlog, sub_level);
  } else {
    /* let us send you an incremental log... */
    ret = _create_sub_incremental(mlog, sub_level, s->next);
  }

  if (!ret) {
    dout(1) << __func__ << " ret = " << ret << dendl;
    mlog->put();
    return;
  }

  dout(1) << __func__ << " sending message to " << s->session->inst 
	  << " with " << mlog->entries.size() << " entries"
	  << " (version " << mlog->version << ")" << dendl;
  
  mon->messenger->send_message(mlog, s->session->inst);
  if (s->onetime)
    mon->session_map.remove_sub(s);
  else
    s->next = summary_version+1;
}

/**
 * Create a log message containing only the last message in the summary.
 *
 * @param mlog	Log message we'll send to the client.
 * @param level Maximum log level the client is interested in.
 * @return	'true' if we consider we successfully populated @mlog;
 *		'false' otherwise.
 */
bool LogMonitor::_create_sub_summary(MLog *mlog, int level)
{
  dout(10) << __func__ << dendl;

  assert(mlog != NULL);

  if (!summary.tail.size())
    return false;

  list<LogEntry>::reverse_iterator it = summary.tail.rbegin();
  for (; it != summary.tail.rend(); it++) {
    LogEntry e = *it;
    if (e.type < level)
      continue;

    mlog->entries.push_back(e);
    mlog->version = summary.version;
    break;
  }

  return true;
}

/**
 * Create an incremental log message from version @sv to @summary.version
 *
 * @param mlog	Log message we'll send to the client with the messages received
 *		since version @sv, inclusive.
 * @param level	The max log level of the messages the client is interested in.
 * @param sv	The version the client is looking for.
 * @return	'true' if we consider we successfully populated @mlog; 
 *		'false' otherwise.
 */
bool LogMonitor::_create_sub_incremental(MLog *mlog, int level, version_t sv)
{
  dout(10) << __func__ << " level " << level << " ver " << sv 
	  << " cur summary ver " << summary.version << dendl; 

  bool success = true;
  version_t summary_ver = summary.version;
  while (sv <= summary_ver) {
    bufferlist bl;
    success = paxos->read(sv, bl);
    if (!success) {
      dout(10) << __func__ << " paxos->read() unsuccessful" << dendl;
      break;
    }
    bufferlist::iterator p = bl.begin();
    __u8 v;
    ::decode(v,p);
    while (!p.end()) {
      LogEntry le;
      le.decode(p);

      if (le.type < level) {
	dout(20) << __func__ << " requested " << level 
		 << " entry " << le.type << dendl;
	continue;
      }

      mlog->entries.push_back(le);
    }
    mlog->version = sv++;
  }

  dout(10) << __func__ << " incremental message ready (" 
	  << mlog->entries.size() << " entries)" << dendl;

  return success;
}

