#include "fixture.h"

int main(int argc, char** argv) {
  CHECK(argc==2);
  const madobs_event_rule_v1 rule={.event_id=HOLDER_EVENT,.max_severity=4};
  const madobs_privacy_policy_v1 policy={sizeof(policy),MADOBS_ABI_VERSION,1,&rule};
  sink own={.policy=&policy,.accept=0};
  madobs_private_open_result reason=MADOBS_PRIVATE_OPEN_INTERNAL;
  const madobs_private_handle_t h=madobs_private_open_ex(argv[1],local_sink,&own,&policy,&reason);
  CHECK(h && reason==MADOBS_PRIVATE_OPEN_OK);
  const madobs_event_v1 message=event(HOLDER_EVENT);
  CHECK(madobs_private_emit(h,&message)==MADOBS_OK);
  wait_for(h,&own,1); /* observed an actual policy-valid exporter refusal */
  assert_gate_clear(h);
  signal_parent('A');

  wait_parent('S'); /* the separate contender has actually seen WAL_BUSY */
  CHECK(madobs_private_stop(h)==MADOBS_OK);
  const madobs_generic_stats_v1 stopped=stats_for(h);
  CHECK(stopped.enqueued==1 && stopped.drained==1 && no_drops(&stopped));
  CHECK(madobs_private_emit(h,&message)==MADOBS_E_BAD_STATE);
  assert_gate_clear(h);
  const unsigned callbacks=atomic_load(&own.valid);
  CHECK(callbacks>=1 && !atomic_load(&own.invalid));
  signal_parent('S');

  /* The process and its stopped C handle remain alive during ownership transfer. */
  wait_parent('X');
  const madobs_generic_stats_v1 after=stats_for(h);
  CHECK(after.enqueued==stopped.enqueued && after.drained==stopped.drained && no_drops(&after));
  CHECK(atomic_load(&own.valid)==callbacks && !atomic_load(&own.invalid));
  CHECK(madobs_private_destroy(h)==MADOBS_OK);
  signal_parent('X');
  return 0;
}
