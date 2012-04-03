/*
 * Ceph - scalable distributed file system
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */
#include "MessengerDriver.h"
#include "msg/SimpleMessenger.h"

int MessengerDriver::init()
{
  int ret = 0;
  messenger->add_dispatcher_head(this);
  ret = messenger->start();
  if (ret) { // umm, that shouldn't have happened...
    state = FAILED;
  } else {
    state = RUNNING;
  }
  return ret;
}

int MessengerDriver::stop()
{
  int ret = 0;
  if (state == RUNNING) { // turn off the Messenger
    ret = messenger->shutdown();
    if (ret) {// uh-oh, failure!? :(
      state = FAILED;
      assert(0);
      return ret;
    }
    messenger->wait();
    state = STOPPED;
  } else if (state == STOPPED) {
    ret = -1; // invalid, already stopped
  } else {
    state = STOPPED;
  }
  return ret;
}

int MessengerDriver::send_message(Message *message, const entity_inst_t& dest)
{
  if (state != RUNNING) {
    message->put();
    return -1;
  }
  return messenger->send_message(message, dest);
}

int MessengerDriver::establish_connection(const entity_inst_t& dest)
{
  if (state != RUNNING) {
    return -1;
  }
  messenger->get_connection(dest)->put(); // we don't want the ref, so put it
  return 0;
}

int MessengerDriver::break_connection(const entity_inst_t& dest)
{
  if (state != RUNNING) {
    return -1;
  }

  Connection *con = messenger->get_connection(dest);
  if (!con) {
    return -ENOTCONN;
  }
  messenger->mark_down(con);
  con->put();
  return 0;
}

int MessengerDriver::break_socket(const entity_inst_t& other)
{
  if (state != RUNNING) {
    return -1;
  }

  Connection *con = messenger->get_connection(other);
  if (!con) {
    return -ENOTCONN;
  }

  SimpleMessenger::Pipe * pipe = (SimpleMessenger::Pipe*) con->get_pipe();
  if (pipe == NULL) {
    return -ENOENT;
  }
  pipe->fail_on_socket = true;
  pipe->put();
  con->put();
  return 0;
}

void MessengerDriver::register_alert(StateAlert alert)
{
  // right now we can't handle Messenger states
  assert(statetracker->is_my_state(alert->get_watched_state()));

  my_alerts[alert->get_watched_state()->state_id].push_back(alert);
}

bool MessengerDriver::ms_dispatch(Message *m)
{
  received_messages.push_back(m);
  list<StateAlert>::iterator i = my_alerts[message_received].begin();
  while (i != my_alerts[message_received].end()) {
    (*i)->set_state_reached(m->get());
    my_alerts[message_received].erase(i++);
  }
  return true;
}

bool MessengerDriver::ms_handle_reset(Connection *c)
{
  list<StateAlert>::iterator i = my_alerts[lossy_connection_broke].begin();
  while (i != my_alerts[lossy_connection_broke].end()) {
    (*i)->set_state_reached();
    my_alerts[lossy_connection_broke].erase(i++);
  }
  return true;
}

void MessengerDriver::ms_handle_remote_reset(Connection *c)
{
  list<StateAlert>::iterator i = my_alerts[remote_reset_connection].begin();
  while (i != my_alerts[remote_reset_connection].end()) {
    (*i)->set_state_reached();
    my_alerts[remote_reset_connection].erase(i++);
  }
  return;
}

StateMaker MessengerDriver::get_subsystem_maker(const char *system)
{
  return modular_maker->create_maker(system);
}

// TODO: make this function support super states.
int MessengerDriver::create_messenger_state(StateMaker maker, const char *state)
{
  int super = -1;
  int state_id = maker->create_new_state(state, super);
  return state_id;
}

int MessengerDriver::report_state_changed(const char *system,
                                          int id, const char *state)
{
  StateMaker maker = get_subsystem_maker(system);
  int state_id = maker->retrieve_state_id(state);
  if (state_id < 0) {
    if (state_id == -ENOENT) {
      state_id = create_messenger_state(maker, state);
    } else {
      assert(0); // retrieve_state_id doesn't return anything else!
    }
  }
  report_state_changed(system, id, state_id);
  return state_id;
}

void MessengerDriver::report_state_changed(const char *system, int id, int state)
{
  // update the messenger state listing
  StateMaker maker = get_subsystem_maker(system);
  const State *state_obj = maker->retrieve_state(state);
  assert(state_obj != NULL);
  messenger_states[system][id] = state_obj;

  // check for StateAlerts to activate
  map<string, map<int, list<StateAlert> > >::iterator iter =
      messenger_alerts.find(system);
  if (iter != messenger_alerts.end()) {
    map<int, list<StateAlert> >::iterator state_iter =
        iter->second.find(state);
    if (state_iter != iter->second.end()) {
      list<StateAlert>::iterator alert_iter = state_iter->second.begin();
      while (alert_iter != state_iter->second.end()) {
        (*alert_iter)->set_state_reached(NULL);
        state_iter->second.erase(alert_iter++);
      }
    }
  }
}