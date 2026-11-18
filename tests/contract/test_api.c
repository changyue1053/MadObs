#include "madobs/observer.h"
#include <assert.h>
static unsigned calls;
static void capture(void* ctx, const madobs_event_v1* ev) {
  (void)ctx; assert(ev->event_id==42); ++calls;
}
int main(void) {
  madobs_observer_v1 o=madobs_noop_observer();
  assert(madobs_observer_emit(NULL,NULL)==MADOBS_NOOP);
  assert(madobs_observer_emit(&o,NULL)==MADOBS_NOOP);
  madobs_event_v1 e={0}; e.struct_size=sizeof(e); e.abi_version=1; e.event_id=42;
  o.emit=capture;
  assert(madobs_observer_emit(&o,&e)==MADOBS_EMITTED && calls==1);
  e.attribute_count=9; assert(madobs_observer_emit(&o,&e)==MADOBS_INVALID);
  e.attribute_count=0; e.abi_version=2; assert(madobs_observer_emit(&o,&e)==MADOBS_INVALID);
  o.abi_version=2; assert(madobs_observer_emit(&o,&e)==MADOBS_INVALID);
  return 0;
}
