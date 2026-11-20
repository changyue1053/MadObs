#include "fixture.h"

int main(int argc, char** argv) {
  CHECK(argc==3);
  const madobs_event_rule_v1 own_rule={.event_id=CONTENDER_EVENT,.max_severity=4};
  const madobs_privacy_policy_v1 own_policy={sizeof(own_policy),MADOBS_ABI_VERSION,1,&own_rule};
  const madobs_event_rule_v1 shared_rule={.event_id=HOLDER_EVENT,.max_severity=4};
  const madobs_privacy_policy_v1 shared_policy={sizeof(shared_policy),MADOBS_ABI_VERSION,1,&shared_rule};
  sink own={.policy=&own_policy,.accept=1}, mismatch={.policy=&own_policy,.accept=0};
  sink shared={.policy=&shared_policy,.accept=0}; /* do not acknowledge retained historical data */
  madobs_private_open_result reason=MADOBS_PRIVATE_OPEN_INTERNAL;
  const madobs_private_handle_t b=madobs_private_open_ex(argv[2],local_sink,&own,&own_policy,&reason);
  CHECK(b && reason==MADOBS_PRIVATE_OPEN_OK);
  const madobs_event_v1 rejected=event(HOLDER_EVENT), message=event(CONTENDER_EVENT);
  CHECK(madobs_private_emit(b,&rejected)==MADOBS_E_SCHEMA_REJECTED);
  CHECK(madobs_private_emit(b,&message)==MADOBS_OK);
  wait_for(b,&own,1);
  CHECK(atomic_load(&own.valid)==1);
  CHECK(!madobs_private_open_ex(argv[1],local_sink,&shared,&shared_policy,&reason));
  CHECK(reason==MADOBS_PRIVATE_OPEN_WAL_BUSY);
  CHECK(!atomic_load(&shared.valid) && !atomic_load(&shared.invalid));
  signal_parent('B');

  wait_parent('T'); /* A has stopped but not destroyed its handle or exited. */
  CHECK(!madobs_private_open_ex(argv[1],local_sink,&mismatch,&own_policy,&reason));
  CHECK(reason==MADOBS_PRIVATE_OPEN_POLICY_OR_MANIFEST_MISMATCH);
  CHECK(!atomic_load(&mismatch.valid) && !atomic_load(&mismatch.invalid));
  const madobs_private_handle_t lease=madobs_private_open_ex(argv[1],local_sink,&shared,&shared_policy,&reason);
  CHECK(lease && reason==MADOBS_PRIVATE_OPEN_OK);
  CHECK(madobs_private_emit(lease,&message)==MADOBS_E_SCHEMA_REJECTED);
  CHECK(madobs_private_stop(lease)==MADOBS_OK);
  CHECK(!atomic_load(&shared.invalid)); /* replay counts are deliberately unspecified */
  assert_gate_clear(lease);
  CHECK(madobs_private_destroy(lease)==MADOBS_OK);

  CHECK(madobs_private_emit(b,&message)==MADOBS_OK);
  wait_for(b,&own,2);
  CHECK(madobs_private_stop(b)==MADOBS_OK);
  const madobs_generic_stats_v1 final=stats_for(b);
  CHECK(final.enqueued==2 && final.drained==2 && no_drops(&final));
  CHECK(atomic_load(&own.valid)==2 && !atomic_load(&own.invalid));
  assert_gate_clear(b);
  CHECK(madobs_private_destroy(b)==MADOBS_OK);
  signal_parent('T');
  return 0;
}
