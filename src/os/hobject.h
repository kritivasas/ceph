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

#ifndef __CEPH_OS_HOBJECT_H
#define __CEPH_OS_HOBJECT_H

#include "include/object.h"
#include "include/cmp.h"

#include "json_spirit/json_spirit_value.h"
#include "include/assert.h"   // spirit clobbers it!

typedef uint64_t filestore_hobject_key_t;

namespace ceph {
  class Formatter;
}

struct hobject_t {
  object_t oid;
  snapid_t snap;
  uint32_t hash;
  bool max;
  int64_t pool;
  string nspace;

private:
  string key;

public:
  const string &get_key() const {
    return key;
  }
  
  hobject_t() : snap(0), hash(0), max(false), pool(-1) {}

  hobject_t(object_t oid, const string& key, snapid_t snap, uint64_t hash,
	    int64_t pool) : 
    oid(oid), snap(snap), hash(hash), max(false),
    pool(pool),
    key(oid.name == key ? string() : key) {}

  hobject_t(const sobject_t &soid, const string &key, uint32_t hash,
	    int64_t pool) : 
    oid(soid.oid), snap(soid.snap), hash(hash), max(false),
    pool(pool),
    key(soid.oid.name == key ? string() : key) {}

  /* Do not use when a particular hash function is needed */
  explicit hobject_t(const sobject_t &o) :
    oid(o.oid), snap(o.snap), max(false), pool(-1) {
    hash = __gnu_cxx::hash<sobject_t>()(o);
  }

  // maximum sorted value.
  static hobject_t get_max() {
    hobject_t h;
    h.max = true;
    return h;
  }
  bool is_max() const {
    return max;
  }

  static uint32_t _reverse_nibbles(uint32_t retval) {
    // reverse nibbles
    retval = ((retval & 0x0f0f0f0f) << 4) | ((retval & 0xf0f0f0f0) >> 4);
    retval = ((retval & 0x00ff00ff) << 8) | ((retval & 0xff00ff00) >> 8);
    retval = ((retval & 0x0000ffff) << 16) | ((retval & 0xffff0000) >> 16);
    return retval;
  }

  filestore_hobject_key_t get_filestore_key() const {
    if (max)
      return 0x100000000ull;
    else
      return _reverse_nibbles(hash);
  }
  void set_filestore_key(uint32_t v) {
    hash = _reverse_nibbles(v);
  }

  const string& get_effective_key() const {
    if (key.length())
      return key;
    return oid.name;
  }

  void swap(hobject_t &o) {
    hobject_t temp(o);
    o = (*this);
    (*this) = temp;
  }

  void encode(bufferlist& bl) const;
  void decode(bufferlist::iterator& bl);
  void decode(json_spirit::Value& v);
  void dump(Formatter *f) const;
  static void generate_test_instances(list<hobject_t*>& o);
};
WRITE_CLASS_ENCODER(hobject_t)

namespace __gnu_cxx {
  template<> struct hash<hobject_t> {
    size_t operator()(const hobject_t &r) const {
      static hash<object_t> H;
      static rjhash<uint64_t> I;
      return H(r.oid) ^ I(r.snap);
    }
  };
}

ostream& operator<<(ostream& out, const hobject_t& o);

WRITE_EQ_OPERATORS_7(hobject_t, oid, get_key(), snap, hash, max, pool, nspace)
// sort hobject_t's by <max, get_filestore_key(hash), key, oid, snapid>
WRITE_CMP_OPERATORS_7(hobject_t,
		      max,
		      get_filestore_key(),
		      nspace,
		      pool,
		      get_effective_key(),
		      oid,
		      snap)

#endif
