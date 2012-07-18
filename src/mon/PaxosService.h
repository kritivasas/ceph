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

#ifndef CEPH_PAXOSSERVICE_H
#define CEPH_PAXOSSERVICE_H

#include "messages/PaxosServiceMessage.h"
#include "include/Context.h"
#include "Paxos.h"
#include "Monitor.h"
#include "MonitorDBStore.h"

class Monitor;
class Paxos;

/**
 * A Paxos Service is an abstraction that easily allows one to obtain an
 * association between a Monitor and a Paxos class, in order to implement any
 * service.
 */
class PaxosService {
  /**
   * @defgroup PaxosService_h_class Paxos Service
   * @{
   */
 public:
  /**
   * The Monitor to which this class is associated with
   */
  Monitor *mon;
  /**
   * The Paxos instance to which this class is associated with
   */
  Paxos *paxos;
  /**
   * Our name. This will be associated with the class implementing us, and will
   * be used mainly for store-related operations.
   */
  string service_name;
  /**
   * If we are or have queued anything for proposal, this variable will be true
   * until our proposal has been finished.
   */
  atomic_t proposing;

 protected:
  /**
   * Services implementing us used to depend on the Paxos version, back when
   * each service would have a Paxos instance for itself. However, now we only
   * have a single Paxos instance, shared by all the services. Each service now
   * must keep its own version, if so they wish. This variable should be used
   * for that purpose.
   */
  version_t service_version;

 private:
  /**
   * Event callback responsible for proposing our pending value once a timer 
   * runs out and fires.
   */
  Context *proposal_timer;
  /**
   * If the implementation class has anything pending to be proposed to Paxos,
   * then have_pending should be true; otherwise, false.
   */
  bool have_pending; 

protected:
  /**
   * @defgroup PaxosService_h_callbacks Callback classes
   * @{
   */
  /**
   * Retry dispatching a given service message
   *
   * This callback class is used when we had to wait for some condition to
   * become true while we were dispatching it.
   *
   * For instance, if the message's version isn't readable, according to Paxos,
   * then we must wait for it to become readable. So, we just queue an
   * instance of this class onto the Paxos::wait_for_readable function, and
   * we will retry the whole dispatch again once the callback is fired.
   */
  class C_RetryMessage : public Context {
    PaxosService *svc;
    PaxosServiceMessage *m;
  public:
    C_RetryMessage(PaxosService *s, PaxosServiceMessage *m_) : svc(s), m(m_) {}
    void finish(int r) {
      svc->dispatch(m);
    }
  };

  /**
   * Callback used to make sure we call the PaxosService::_active function
   * whenever a condition is fulfilled.
   *
   * This is used in multiple situations, from waiting for the Paxos to commit
   * our proposed value, to waiting for the Paxos to become active once an
   * election is finished.
   */
  class C_Active : public Context {
    PaxosService *svc;
  public:
    C_Active(PaxosService *s);// : svc(s) {}
    void finish(int r);/* {
      if (r >= 0) 
	svc->_active();
    }*/
  };

  /**
   * Callback class used to propose the pending value once the proposal_timer
   * fires up.
   */
  class C_Propose : public Context {
    PaxosService *ps;
  public:
    C_Propose(PaxosService *p) : ps(p) { }
    void finish(int r) { 
      ps->proposal_timer = 0;
      ps->propose_pending(); 
    }
  };

  class C_Committed : public Context {
    PaxosService *ps;
  public:
    C_Committed(PaxosService *p) : ps(p) { }
    void finish(int r) {
      ps->proposing.set(0);
      ps->wakeup_proposing_waiters();
    }
  }
  /**
   * @}
   */
  friend class C_Propose;
  

public:
  /**
   * @param mn A Monitor instance
   * @param p A Paxos instance
   * @parem name Our service's name.
   */
  PaxosService(Monitor *mn, Paxos *p, string name) 
    : mon(mn), paxos(p), service_name(name),
      service_version(0), proposal_timer(0), have_pending(false),
      last_committed_name("last_committed"),
      first_committed_name("first_committed"),
      last_accepted_name("last_accepted"),
      mkfs_name("mkfs"),
      full_version_name("full"), full_latest_name("latest")
  {
    proposing.set(0);
  }

  virtual ~PaxosService() {}

  /**
   * Get the service's name.
   *
   * @returns The service's name.
   */
  string get_service_name() { return service_name; }
  
  // i implement and you ignore
  /**
   * Informs this instance that it should consider itself restarted.
   *
   * This means that we will cancel our proposal_timer event, if any exists.
   */
  void restart();
  /**
   * Informs this instance that an election has finished.
   *
   * This means that we will invoke a PaxosService::discard_pending while
   * setting have_pending to false (basically, ignore our pending state) and
   * we will then make sure we obtain a new state.
   *
   * Our state shall be updated by PaxosService::_active if the Paxos is
   * active; otherwise, we will wait for it to become active by adding a 
   * PaxosService::C_Active callback to it.
   */
  void election_finished();
  /**
   * Informs this instance that it is supposed to shutdown.
   *
   * Basically, it will instruct Paxos to cancel all events/callbacks and then
   * will cancel the proposal_timer event if any exists.
   */
  void shutdown();

private:
  /**
   * Update our state by updating it from Paxos, and then creating a new
   * pending state if need be.
   *
   * @remarks We only create a pending state we our Monitor is the Leader.
   *
   * @pre Paxos is active
   * @post have_pending is true iif our Monitor is the Leader and Paxos is
   *	   active
   */
  void _active();

public:
  /**
   * Propose a new value through Paxos.
   *
   * This function should be called by the classes implementing 
   * PaxosService, in order to propose a new value through Paxos.
   *
   * @pre The implementation class implements the encode_pending function.
   * @pre have_pending is true
   * @pre Our monitor is the Leader
   * @pre Paxos is active
   * @post Cancel the proposal timer, if any
   * @post have_pending is false
   * @post propose pending value through Paxos
   *
   * @note This function depends on the implementation of encode_pending on
   *	   the class that is implementing PaxosService
   */
  void propose_pending();
  /**
   * Dispatch a message by passing it to several different functions that are
   * either implemented directly by this service, or that should be implemented
   * by the class implementing this service.
   *
   * @param m A message
   * @returns 'true' on successful dispatch; 'false' otherwise.
   */
  bool dispatch(PaxosServiceMessage *m);

  /**
   * @defgroup PaxosService_h_override_funcs Functions that should be
   *					     overridden.
   *
   * These functions should be overridden at will by the class implementing
   * this service.
   * @{
   */
  /**
   * Create the initial state for your system.
   *
   * In some of ours the state is actually set up elsewhere so this does
   * nothing.
   */
  virtual void create_initial() = 0;

  /**
   * Query the Paxos system for the latest state and apply it if it's newer
   * than the current Monitor state.
   *
   * @returns 'true' on success; 'false' otherwise.
   */
  virtual void update_from_paxos() = 0;

  /**
   * Create the pending state.
   *
   * @invariant This function is only called on a Leader.
   * @remarks This created state is then modified by incoming messages.
   * @remarks Called at startup and after every Paxos ratification round.
   */
  virtual void create_pending() = 0;

  /**
   * Encode the pending state into a bufferlist for ratification and
   * transmission as the next state.
   *
   * @invariant This function is only called on a Leader.
   *
   * @param t The transaction to hold all changes.
   */
  virtual void encode_pending(MonitorDBStore::Transaction *t) = 0;

  /**
   * Discard the pending state
   *
   * @invariant This function is only called on a Leader.
   *
   * @remarks This function is NOT overridden in any of our code, but it is
   *	      called in PaxosService::election_finished if have_pending is
   *	      true.
   */
  virtual void discard_pending() { }

  /**
   * Look at the query; if the query can be handled without changing state,
   * do so.
   *
   * @param m A query message
   * @returns 'true' if the query was handled (e.g., was a read that got
   *	      answered, was a state change that has no effect); 'false' 
   *	      otherwise.
   */
  virtual bool preprocess_query(PaxosServiceMessage *m) = 0;

  /**
   * Apply the message to the pending state.
   *
   * @invariant This function is only called on a Leader.
   *
   * @param m An update message
   * @returns 'true' if the update message was handled (e.g., a command that
   *	      went through); 'false' otherwise.
   */
  virtual bool prepare_update(PaxosServiceMessage *m) = 0;
  /**
   * @}
   */

  /**
   * Determine if the Paxos system should vote on pending, and if so how long
   * it should wait to vote.
   *
   * @param[out] delay The wait time, used so we can limit the update traffic
   *		       spamming.
   * @returns 'true' if the Paxos system should propose; 'false' otherwise.
   */
  virtual bool should_propose(double &delay);

  /**
   * @defgroup PaxosService_h_courtesy Courtesy functions
   *
   * Courtesy functions, in case the class implementing this service has
   * anything it wants/needs to do at these times.
   * @{
   */
  /**
   * This is called when the Paxos state goes to active.
   *
   * @remarks It's a courtesy method, in case the class implementing this 
   *	      service has anything it wants/needs to do at that time.
   *
   * @note This function may get called twice in certain recovery cases.
   */
  virtual void on_active() { }

  /**
   * Called when the Paxos system enters a Leader election.
   *
   * @remarks It's a courtesy method, in case the class implementing this
   *	      service has anything it wants/needs to do at that time.
   */
  virtual void on_restart() { }
  /**
   * @}
   */

  /**
   * Tick.
   */
  virtual void tick() {}

  /**
   * Get health information
   *
   * @param summary list of summary strings and associated severity
   * @param detail optional list of detailed problem reports; may be NULL
   */
  virtual void get_health(list<pair<health_status_t,string> >& summary,
			  list<pair<health_status_t,string> > *detail) const { }

 private:
  /**
   * @defgroup PaxosService_h_store_keys Set of keys that are usually used on
   *					 all the services implementing this
   *					 class, and, being almost the only keys
   *					 used, should be standardized to avoid
   *					 mistakes.
   * @{
   */
  const string last_committed_name;
  const string first_committed_name;
  const string last_accepted_name;
  const string mkfs_name;
  const string full_version_name;
  const string full_latest_name;
  /**
   * @}
   */

  /**
   */
  list<Context*> waiting_for_finished_proposal;

 public:

  bool is_proposing() {
    return ((int) proposing.read() == 1);
  }

  /**
   * Check if we are in the Paxos ACTIVE state.
   *
   * @note This function is a wrapper for Paxos::is_active
   *
   * @returns true if in state ACTIVE; false otherwise.
   */
  bool is_active() {
    return (!is_proposing() && !paxos->is_recovering());
  }

  /**
   * TODO: FIXME: Most of this is no longer valid. Rework it. Seriously. I just
   * TODO: FIXME: don't have the time right now.
   *
   * Check if we are readable.
   *
   * The services implementing us used to rely on this function to check if
   * Paxos was readable, back when we had a Paxos instance for each service.
   * Now, we only have a single Paxos instance, shared by all the services.
   *
   * Since each service longer have their own private Paxos, they can no
   * longer assume each Paxos version as theirs, thus we cannot make this
   * function a plain wrapper for the Paxos::is_readable as is. Currently,
   * Paxos versions are Paxos'; our versions are ours'. There's no more
   * in-breeding here.
   *
   * So, since we cannot infer which Paxos version the version @p ver have been
   * "mapped" to (i.e., which Paxos version its proposal turned out to be), we
   * are forced to make some new rules, although highly based on Paxos'.
   *
   * Since "being readable" is a state tightly-knit with the Paxos state, we
   * are going to say that a given version @p ver is readable iif:
   *
   *  - ver <= last_committed (same as in Paxos, but this is *our*
   *  last_committed, instead of Paxos' last_committed); and
   *  - Paxos::is_readable(0) returns true.
   *
   * We are relying on passing '0' as an argument to Paxos::is_readable because
   * we know it will then perform all the checks we are really interested in.
   * Feel free to head out to Paxos.h for further explanation on this function.
   *
   * @param ver The version we want to check if is readable
   * @returns true if it is readable; false otherwise
   */
  bool is_readable(version_t ver = 0) {
    if (ver > get_last_committed())
      return false;

    return ((mon->is_peon() || mon->is_leader())
      && (!is_proposing() && !paxos->is_recovering())
      && (get_last_committed() > 0)
      && ((mon->get_quorum().size == 1) || paxos->is_valid_lease()));
  }

  /**
   * Check if we are writeable.
   *
   * @note This function is a wrapper for Paxos::is_writeable
   ;*
   * @returns true if writeable; false otherwise
   */
  bool is_writeable() {
    return (!is_proposing() && mon->is_leader() && paxos->is_lease_valid());
  }

  void wait_for_finished_proposal(Context *c) {
    waiting_for_finished_proposal.push_back(c);
  }

  void wait_for_active(Context *c) {
    if (is_proposing())
      wait_for_finished_proposal(c);
    else
      paxos->wait_for_active(c);
  }

  void wait_for_readable(Context *c) {
    if (is_proposing())
      wait_for_finished_proposal(c)
    else
      paxos->wait_for_readable(c);
  }

  void wait_for_writeable(Context *c) {
    if (is_proposing())
      wait_for_finished_proposal(c);
    else
      paxos->wait_for_writeable(c);
  }

  void wakeup_proposing_waiters();

  /**
   * Trim our log. This implies getting rid of versions on the k/v store.
   * Services implementing us don't have to implement this function if they
   * don't want to, but we won't implement it for them either.
   *
   * This function had to be inheritted from the Paxos, since the existing
   * services made use of it. This function should be tuned for each service's
   * needs. We have it in this interface to make sure its usage and purpose is
   * well understood by the underlying services.
   *
   * @param first The version that should become the first one in the log.
   * @param force Optional. Each service may use it as it sees fit, but the
   *		  expected behavior is that, when 'true', we will remove all
   *		  the log versions even if we don't have a full map in store.
   */
  void trim_to(version_t first, bool force = false);


  /**
   * Cancel events.
   *
   * @note This function is a wrapper for Paxos::cancel_events
   */
  void cancel_events() {
    paxos->cancel_events();
  }

  /**
   * @defgroup PaxosService_h_store_funcs Back storage interface functions
   * @{
   */
  /**
   * @defgroup PaxosService_h_store_modify Wrapper function interface to access
   *					   the back store for modification
   *					   purposes
   * @{
   */
  void put_first_committed(MonitorDBStore::Transaction *t, version_t ver) {
    t->put(get_service_name(), first_committed_name, ver);
  }
  /**
   * Set the last committed version to @p ver
   *
   * @param t A transaction to which we add this put operation
   * @param ver The last committed version number being put
   */
  void put_last_committed(MonitorDBStore::Transaction *t, version_t ver) {
    t->put(get_service_name(), last_committed_name, ver);
  }
  /**
   * Put the contents of @p bl into version @p ver
   *
   * @param t A transaction to which we will add this put operation
   * @param ver The version to which we will add the value
   * @param bl A bufferlist containing the version's value
   */
  void put_version(MonitorDBStore::Transaction *t, version_t ver,
		   bufferlist& bl) {
    t->put(get_service_name(), ver, bl);
  }
  /**
   * Put the contents of @p bl into version @p ver (prefixed with @p prefix)
   *
   * @param t A transaction to which we will add this put operation
   * @param prefix The version's prefix
   * @param ver The version to which we will add the value
   * @param bl A bufferlist containing the version's value
   */
  void put_version(MonitorDBStore::Transaction *t, 
		   const string& prefix, version_t ver, bufferlist& bl);
  /**
   * Put a version number into a key composed by @p prefix and @p name
   * combined.
   *
   * @param t The transaction to which we will add this put operation
   * @param prefix The key's prefix
   * @param name The key's suffix
   * @param ver A version number
   */
  void put_version(MonitorDBStore::Transaction *t,
		   const string& prefix, const string& name, version_t ver) {
    string key = mon->store->combine_strings(prefix, name);
    t->put(get_service_name(), key, ver);
  }
  /**
   * Put the contents of @p bl into a full version key for this service, that
   * will be created with @p ver in mind.
   *
   * @param t The transaction to which we will add this put operation
   * @param ver A version number
   * @param bl A bufferlist containing the version's value
   */
  void put_version_full(MonitorDBStore::Transaction *t,
			version_t ver, bufferlist& bl) {
    string key = mon->store->combine_strings(full_version_name, ver);
    t->put(get_service_name(), key, bl);
  }
  /**
   * Put the version number in @p ver into the key pointing to the latest full
   * version of this service.
   *
   * @param t The transaction to which we will add this put operation
   * @param ver A version number
   */
  void put_version_latest_full(MonitorDBStore::Transaction *t, version_t ver) {
    string key =
      mon->store->combine_strings(full_version_name, full_latest_name);
    t->put(get_service_name(), key, ver);
  }
  /**
   * Put the contents of @p bl into the key @p key.
   *
   * @param t A transaction to which we will add this put operation
   * @param key The key to which we will add the value
   * @param bl A bufferlist containing the value
   */
  void put_value(MonitorDBStore::Transaction *t,
		 const string& key, bufferlist& bl) {
    t->put(get_service_name(), key, bl);
  }
  /**
   * Put the contents of @p bl into a key composed of @p prefix and @p name
   * concatenated.
   *
   * @param t A transaction to which we will add this put operation
   * @param prefix The key's prefix
   * @param name The key's suffix
   * @param bl A bufferlist containing the value
   */
  void put_value(MonitorDBStore::Transaction *t,
		 const string& prefix, const string& name, bufferlist& bl) {
    string key = mon->store->combine_strings(prefix, name);
    t->put(get_service_name(), key, bl);
  }
  /**
   * Remove our mkfs entry from the store
   *
   * @param t A transaction to which we will add this erase operation
   */
  void erase_mkfs(MonitorDBStore::Transaction *t) {
    t->erase(mkfs_name, get_service_name());
  }
  /**
   * @}
   */

  /**
   * @defgroup PaxosService_h_store_get Wrapper function interface to access
   *					the back store for reading purposes
   * @{
   */
  /**
   * Get the first committed version
   *
   * @returns Our first committed version (that is available)
   */
  version_t get_first_committed() {
    return mon->store->get(get_service_name(), first_committed_name);
  }
  /**
   * Get the last committed version
   *
   * @returns Our last committed version
   */
  version_t get_last_committed() {
    return mon->store->get(get_service_name(), last_committed_name);
  }
  /**
   * Get our current version
   *
   * @returns Our current version
   */
  version_t get_version() {
    return get_last_committed();
  }
  /**
   * Get the contents of a given version @p ver
   *
   * @param ver The version being obtained
   * @param bl The bufferlist to be populated
   * @return 0 on success; <0 otherwise
   */
  int get_version(version_t ver, bufferlist& bl) {
    return mon->store->get(get_service_name(), ver, bl);
  }
  /**
   * Get the contents of a given version @p ver with a given prefix @p prefix
   *
   * @param prefix The intended prefix
   * @param ver The version being obtained
   * @param bl The bufferlist to be populated
   * @return 0 on success; <0 otherwise
   */
  int get_version(const string& prefix, version_t ver, bufferlist& bl);
  /**
   * Get a version number from a given key, whose name is composed by
   * @p prefix and @p name combined.
   *
   * @param prefix Key's prefix
   * @param name Key's suffix
   * @returns A version number
   */
  version_t get_version(const string& prefix, const string& name) {
    string key = mon->store->combine_strings(prefix, name);
    return mon->store->get(get_service_name(), key);
  }
  /**
   * Get the contents of a given full version of this service.
   *
   * @param ver A version number
   * @param bl The bufferlist to be populated
   * @returns 0 on success; <0 otherwise
   */
  int get_version_full(version_t ver, bufferlist& bl) {
    string key = mon->store->combine_strings(full_version_name, ver);
    return mon->store->get(get_service_name(), key, bl);
  }
  /**
   * Get the latest full version number
   *
   * @returns A version number
   */
  version_t get_version_latest_full() {
    return get_version(full_version_name, full_latest_name);
  }
  /**
   * Get a value from a given key, composed by @p prefix and @p name combined.
   *
   * @param[in] prefix Key's prefix
   * @param[in] name Key's suffix
   * @param[out] bl The bufferlist to be populated with the value
   */
  int get_value(const string& prefix, const string& name, bufferlist& bl) {
    string key = mon->store->combine_strings(prefix, name);
    return mon->store->get(get_service_name(), key, bl);
  }
  /**
   * Get a value from a given key.
   *
   * @param[in] key The key
   * @param[out] bl The bufferlist to be populated with the value
   */
  int get_value(const string& key, bufferlist& bl) {
    return mon->store->get(get_service_name(), key, bl);
  }
  /**
   * Get the contents of our mkfs entry
   *
   * @param bl A bufferlist to populate with the contents of the entry
   * @return 0 on success; <0 otherwise
   */
  int get_mkfs(bufferlist& bl) {
    return mon->store->get(mkfs_name, get_service_name(), bl);
  }
  /**
   * Checks if a given key composed by @p prefix and @p name exists.
   *
   * @param prefix Key's prefix
   * @param name Key's suffix
   * @returns true if it exists; false otherwise.
   */
  bool exists_key(const string& prefix, const string& name) {
    string key = mon->store->combine_strings(prefix, name);
    return mon->store->exists(get_service_name(), key);
  }
  /**
   * @}
   */
  /**
   * @}
   */
};

#endif

