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

#include "PaxosService.h"
#include "common/Clock.h"
#include "Monitor.h"
#include "MonitorDBStore.h"


#include "common/config.h"
#include "common/Formatter.h"

#define dout_subsys ceph_subsys_paxos
#undef dout_prefix
#define dout_prefix _prefix(_dout, mon, paxos, service_name)
static ostream& _prefix(std::ostream *_dout, Monitor *mon, Paxos *paxos, string service_name) {
  return *_dout << "mon." << mon->name << "@" << mon->rank
		<< "(" << mon->get_state_name()
		<< ").paxosservice(" << service_name << ") ";
}

bool PaxosService::is_readable(version_t ver)
{
  if (ver > get_last_committed()) {
    dout(20) << __func__ << " v" << ver
	     << " > last committed v" << get_last_committed() << dendl;
    return false;
  }

  if (!mon->is_peon() && !mon->is_leader()) {
    dout(20) << __func__ << " peon = " << mon->is_peon()
	     << "; leader = " << mon->is_leader() << dendl;
    return false;
  }

  if (is_proposing() || paxos->is_recovering()) {
    dout(20) << __func__ << " proposing = " << is_proposing()
	     << "; recovering = " << paxos->is_recovering() << dendl;
    if (is_proposing())
      dout(20) << __func__ << " paxos state: active = " << paxos->is_active()
	       << "; updating = " << paxos->is_updating() << dendl;
    return false;
  }

  if (get_last_committed() <= 0) {
    dout(20) << __func__ << " last committed <= 0" << dendl;
    return false;
  }

  if ((mon->get_quorum().size() != 1) && !paxos->is_lease_valid()) {
    dout(20) << __func__ << " quorum = " << mon->get_quorum().size()
	     << "; valid lease = " << paxos->is_lease_valid() << dendl;
    return false;
  }
  return true;
}

bool PaxosService::dispatch(PaxosServiceMessage *m)
{
  dout(10) << "dispatch " << *m << " from " << m->get_orig_source_inst() << dendl;
  // make sure our map is readable and up to date
  if (!is_readable(m->version)) {
    dout(10) << " waiting for paxos -> readable (v" << m->version << ")" << dendl;
    wait_for_readable(new C_RetryMessage(this, m), m->version);
    return true;
  }

  // make sure service has latest from paxos.
  update_from_paxos();

  // preprocess
  if (preprocess_query(m)) 
    return true;  // easy!

  // leader?
  if (!mon->is_leader()) {
    mon->forward_request_leader(m);
    return true;
  }
  
  // writeable?
  if (!is_writeable()) {
    dout(10) << " waiting for paxos -> writeable" << dendl;
    wait_for_writeable(new C_RetryMessage(this, m));
    return true;
  }

  // update
  if (prepare_update(m)) {
    double delay = 0.0;
    if (should_propose(delay)) {
      if (delay == 0.0) {
	propose_pending();
      } else {
	// delay a bit
	if (!proposal_timer) {
	  dout(10) << " setting propose timer with delay of " << delay << dendl;
	  proposal_timer = new C_Propose(this);
	  mon->timer.add_event_after(delay, proposal_timer);
	} else { 
	  dout(10) << " propose timer already set" << dendl;
	}
      }
    } else {
      dout(10) << " not proposing" << dendl;
    }
  }     
  return true;
}

bool PaxosService::should_propose(double& delay)
{
  // simple default policy: quick startup, then some damping.
  if (get_last_committed() <= 1)
    delay = 0.0;
  else {
    utime_t now = ceph_clock_now(g_ceph_context);
    if ((now - paxos->last_commit_time) > g_conf->paxos_propose_interval)
      delay = (double)g_conf->paxos_min_wait;
    else
      delay = (double)(g_conf->paxos_propose_interval + paxos->last_commit_time
		       - now);
  }
  return true;
}


void PaxosService::propose_pending()
{
  dout(10) << "propose_pending" << dendl;
  assert(have_pending);
#if 0
  if (!have_pending) {
    /* If we do not have a pending value, it means we must have recently
     * proposed and we are not yet ready to propose again; hence, return.
     */
    dout(10) << __func__ << " do not propose just yet!" << dendl;
    return;
  }
#endif
  assert(mon->is_leader());
  assert(is_active());
  if (!is_active())
    return;

  if (proposal_timer) {
    mon->timer.cancel_event(proposal_timer);
    proposal_timer = 0;
  }

  /**
   * @note The value we propose is encoded in a bufferlist, passed to 
   *	   Paxos::propose_new_value and it is obtained by calling a 
   *	   function that must be implemented by the class implementing us.
   *	   I.e., the function encode_pending will be the one responsible
   *	   to encode whatever is pending on the implementation class into a
   *	   bufferlist, so we can then propose that as a value through Paxos.
   */
  MonitorDBStore::Transaction t;
  bufferlist bl;
  encode_pending(&t);
  have_pending = false;

  JSONFormatter f(true);
  t.dump(&f);
  dout(30) << __func__ << " transaction dump:\n";
  f.flush(*_dout);
  *_dout << dendl;
  

  t.encode(bl);

  // apply to paxos
//  paxos->wait_for_commit_front(new C_Active(this));
  proposing.set(1);
  paxos->propose_new_value(bl, new C_Committed(this));
}



void PaxosService::restart()
{
  dout(10) << "restart" << dendl;
  if (proposal_timer) {
    mon->timer.cancel_event(proposal_timer);
    proposal_timer = 0;
  }
  // ignore any callbacks waiting for us to finish our proposal
  waiting_for_finished_proposal.clear();

  on_restart();
}

void PaxosService::election_finished()
{
  dout(10) << "election_finished" << dendl;

  if (have_pending) {
    discard_pending();
    have_pending = false;
  }
  proposing.set(0);

  // ignore any callbacks waiting for us to finish our proposal
  waiting_for_finished_proposal.clear();

  // make sure we update our state
  if (is_active())
    _active();
  else
    wait_for_active(new C_Active(this));
}

void PaxosService::_active()
{
  if (!is_active()) {
    dout(10) << "_active - not active" << dendl;
    wait_for_active(new C_Active(this));
    return;
  }
  dout(10) << "_active" << dendl;

  // pull latest from paxos
  update_from_paxos();

  // create pending state?
  if (mon->is_leader() && is_active()) {
    dout(7) << "_active creating new pending" << dendl;
    if (!have_pending) {
      create_pending();
      have_pending = true;
    }

    if (get_version() == 0) {
      // create initial state
      create_initial();
      propose_pending();
      return;
    }
  } else {
    if (!mon->is_leader()) {
      dout(7) << __func__ << " we are not the leader, hence we propose nothing!" << dendl;
    } else if (!is_active()) {
      dout(7) << __func__ << " we are not active, hence we propose nothing!" << dendl;
    }
  }

  /* wake people up before calling on_active(). We don't know how long we'll be
   * on the service's on_active(), and we really should wake people up!
   */
  wakeup_proposing_waiters();
  // NOTE: it's possible that this will get called twice if we commit
  // an old paxos value.  Implementations should be mindful of that.
  if (is_active())
    on_active();
}


void PaxosService::shutdown()
{
  cancel_events();

  if (proposal_timer) {
    mon->timer.cancel_event(proposal_timer);
    proposal_timer = 0;
  }
  // ignore any callbacks waiting for us to finish our proposal
  waiting_for_finished_proposal.clear();
}

void PaxosService::put_version(MonitorDBStore::Transaction *t,
			       const string& prefix, version_t ver,
			       bufferlist& bl)
{
  ostringstream os;
  os << ver;
  string key = mon->store->combine_strings(prefix, os.str());
  t->put(get_service_name(), key, bl);
}

int PaxosService::get_version(const string& prefix, version_t ver,
			      bufferlist& bl)
{
  ostringstream os;
  os << ver;
  string key = mon->store->combine_strings(prefix, os.str());
  return mon->store->get(get_service_name(), key, bl);
}

void PaxosService::wakeup_proposing_waiters()
{
  finish_contexts(g_ceph_context, waiting_for_finished_proposal);
}

void PaxosService::trim_to(version_t first, bool force)
{
  version_t first_committed = get_first_committed();
  version_t latest_full = get_version("full", "latest");

  string latest_key = mon->store->combine_strings("full", latest_full);
  bool has_full = mon->store->exists(get_service_name(), latest_key);

  dout(10) << __func__ << " " << first << " (was " << first_committed << ")"
	   << ", latest full " << latest_full << dendl;

  if (first_committed >= first)
    return;

  MonitorDBStore::Transaction t;
  while ((first_committed < first)
      && (force || (first_committed < latest_full))) {
    dout(20) << __func__ << first_committed << dendl;
    t.erase(get_service_name(), first_committed);

    if (has_full) {
      latest_key = mon->store->combine_strings("full", first_committed);
      if (mon->store->exists(get_service_name(), latest_key))
	t.erase(get_service_name(), latest_key);
    }

    first_committed++;
  }
  put_first_committed(&t, first_committed);
  mon->store->apply_transaction(t);
}

PaxosService::C_Active::C_Active(PaxosService *s) : svc(s)
{
#undef dout_prefix
#define dout_prefix _prefix(_dout, s->mon, s->paxos, s->service_name)
  dout(10) << __func__ << " Creating active callback for " << s->get_service_name() << dendl;
#undef dout_prefix
#define dout_prefix _prefix(_dout, mon, paxos, service_name)
}

void PaxosService::C_Active::finish(int r)
{
  if (r >= 0) {
#undef dout_prefix
#define dout_prefix _prefix(_dout, svc->mon, svc->paxos, svc->service_name)
    dout(10) << __func__ << " Going active for " << svc->get_service_name() << dendl;
#undef dout_prefix
#define dout_prefix _prefix(_dout, mon, paxos, service_name)
    svc->_active();
  }
}

