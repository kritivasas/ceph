// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
#ifndef CEPH_LIBRBD_AIOCOMPLETION_H
#define CEPH_LIBRBD_AIOCOMPLETION_H

#include "common/Cond.h"
#include "common/Mutex.h"
#include "common/ceph_context.h"
#include "common/perf_counters.h"
#include "include/Context.h"
#include "include/utime.h"
#include "include/rbd/librbd.hpp"

#include "librbd/ImageCtx.h"
#include "librbd/internal.h"

namespace librbd {

  class AioRead;

  typedef enum {
    AIO_TYPE_READ = 0,
    AIO_TYPE_WRITE,
    AIO_TYPE_DISCARD
  } aio_type_t;

  /**
   * AioCompletion is the overall completion for a single
   * rbd I/O request. It may be composed of many AioRequests,
   * which each go to a single object.
   *
   * The retrying of individual requests is handled at a lower level,
   * so all AioCompletion cares about is the count of outstanding
   * requests. Note that this starts at 1 to prevent the reference
   * count from reaching 0 while more requests are being added. When
   * all requests have been added, finish_adding_requests() releases
   * this initial reference.
   */
  struct AioCompletion {
    Mutex lock;
    Cond cond;
    bool done;
    ssize_t rval;
    callback_t complete_cb;
    void *complete_arg;
    rbd_completion_t rbd_comp;
    int pending_count;
    int ref;
    bool released;
    ImageCtx *ictx;
    utime_t start_time;
    aio_type_t aio_type;

    AioCompletion() : lock("AioCompletion::lock", true),
		      done(false), rval(0), complete_cb(NULL),
		      complete_arg(NULL), rbd_comp(NULL), pending_count(1),
		      ref(1), released(false) { 
    }
    ~AioCompletion() {
    }

    int wait_for_complete() {
      lock.Lock();
      while (!done)
	cond.Wait(lock);
      lock.Unlock();
      return 0;
    }

    void add_request() {
      lock.Lock();
      pending_count++;
      lock.Unlock();
      get();
    }

    void finish_adding_requests() {
      lock.Lock();
      assert(pending_count);
      int count = --pending_count;
      if (!count) {
	complete();
      }
      lock.Unlock();
    }

    void init_time(ImageCtx *i, aio_type_t t) {
      ictx = i;
      aio_type = t;
      start_time = ceph_clock_now(ictx->cct);
    }

    void complete() {
      utime_t elapsed;
      assert(lock.is_locked());
      elapsed = ceph_clock_now(ictx->cct) - start_time;
      if (complete_cb) {
	complete_cb(rbd_comp, complete_arg);
      }
      switch (aio_type) {
      case AIO_TYPE_READ: 
	ictx->perfcounter->finc(l_librbd_aio_rd_latency, elapsed); break;
      case AIO_TYPE_WRITE:
	ictx->perfcounter->finc(l_librbd_aio_wr_latency, elapsed); break;
      case AIO_TYPE_DISCARD:
	ictx->perfcounter->finc(l_librbd_aio_discard_latency, elapsed); break;
      default: break;
      }
      done = true;
      cond.Signal();
    }

    void set_complete_cb(void *cb_arg, callback_t cb) {
      complete_cb = cb;
      complete_arg = cb_arg;
    }

    void complete_request(CephContext *cct, ssize_t r);

    ssize_t get_return_value() {
      lock.Lock();
      ssize_t r = rval;
      lock.Unlock();
      return r;
    }

    void get() {
      lock.Lock();
      assert(ref > 0);
      ref++;
      lock.Unlock();
    }
    void release() {
      lock.Lock();
      assert(!released);
      released = true;
      put_unlock();
    }
    void put() {
      lock.Lock();
      put_unlock();
    }
    void put_unlock() {
      assert(ref > 0);
      int n = --ref;
      lock.Unlock();
      if (!n)
	delete this;
    }
  };

  class C_AioRead : public Context {
  public:
    C_AioRead(CephContext *cct, AioCompletion *completion, char *out_buf)
      : m_cct(cct), m_completion(completion), m_out_buf(out_buf) {}
    virtual ~C_AioRead() {}
    virtual void finish(int r);
    void set_req(AioRead *req) {
      m_req = req;
    }
  private:
    CephContext *m_cct;
    AioCompletion *m_completion;
    AioRead *m_req;
    char *m_out_buf;
  };

  class C_AioWrite : public Context {
  public:
    C_AioWrite(CephContext *cct, AioCompletion *completion)
      : m_cct(cct), m_completion(completion) {}
    virtual ~C_AioWrite() {}
    virtual void finish(int r) {
      m_completion->complete_request(m_cct, r);
    }
  private:
    CephContext *m_cct;
    AioCompletion *m_completion;
  };

  class C_CacheRead : public Context {
  public:
    C_CacheRead(Context *completion, AioRead *req)
      : m_completion(completion), m_req(req) {}
    virtual ~C_CacheRead() {}
    virtual void finish(int r);
  private:
    Context *m_completion;
    AioRead *m_req;
  };
}

#endif
