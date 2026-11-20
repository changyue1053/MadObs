#ifndef MADOBS_INSTALLED_LEASE_FIXTURE_H
#define MADOBS_INSTALLED_LEASE_FIXTURE_H
#define _POSIX_C_SOURCE 200809L
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "madobs/private.h"

#define CHECK(condition) do {                                               \
  if (!(condition)) {                                                       \
    fprintf(stderr, "FAIL: fixture assertion %d\n", __LINE__);                \
    exit(1);                                                                \
  }                                                                         \
} while (0)

enum { HOLDER_EVENT=9, CONTENDER_EVENT=21 };
typedef struct {
  const madobs_privacy_policy_v1* policy;
  int accept;
  atomic_uint valid, invalid;
} sink;

static void local_sink(void* context, uint8_t priority, uint64_t id,
                       const uint8_t* data, size_t size, madobs_send_done_fn done,
                       void* done_context) {
  sink* s=context;
  (void)priority;
  (void)id;
  const int valid=madobs_check_private_batch(data,size,s->policy)==MADOBS_PRIVATE_BATCH_OK;
  // Borrowed bytes are consumed only here and never written or returned.
  done(done_context, valid && s->accept);
  atomic_fetch_add(valid ? &s->valid : &s->invalid, 1);
}

static madobs_event_v1 event(uint64_t id) {
  madobs_event_v1 e={0};
  e.struct_size=sizeof(e);
  e.abi_version=MADOBS_ABI_VERSION;
  e.event_id=id;
  e.severity=4; /* high priority, not dependent on a low-priority flush timer */
  e.monotonic_ns=73; /* synthetic: binding must canonicalize this before export */
  return e;
}
static int no_drops(const madobs_generic_stats_v1* stats) {
  return !stats->queue_dropped && !stats->wal_dropped;
}
static madobs_generic_stats_v1 stats_for(madobs_private_handle_t h) {
  madobs_generic_stats_v1 stats={0};
  CHECK(madobs_private_get_stats(h,&stats,sizeof(stats))==MADOBS_OK);
  return stats;
}
static void assert_gate_clear(madobs_private_handle_t h) {
  madobs_private_gate_stats_v1 stats={0};
  CHECK(madobs_private_get_gate_stats(h,&stats,sizeof(stats))==MADOBS_OK);
  CHECK(!stats.rejected_attempts);
}
static void wait_for(madobs_private_handle_t h, const sink* s, unsigned count) {
  const struct timespec delay={0, 5000000};
  for (unsigned i=0; i<1200; ++i) {
    const madobs_generic_stats_v1 v=stats_for(h);
    CHECK(!atomic_load(&s->invalid) && v.enqueued==count && v.drained<=count && no_drops(&v));
    if (v.drained==count && atomic_load(&s->valid)>=count) return;
    nanosleep(&delay,NULL);
  }
  CHECK(0);
}
static void signal_parent(int id) {
  CHECK(putchar(id)!=EOF && putchar('\n')!=EOF && fflush(stdout)==0);
}
static void wait_parent(int id) {
  CHECK(getchar()==id && getchar()=='\n');
}
#endif
