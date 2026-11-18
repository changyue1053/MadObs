#include "madobs/observer.h"
#include <cassert>
#include <stdexcept>
int main() {
  auto o=madobs_noop_observer();
  o.emit=[](void*,const madobs_event_v1*){throw std::runtime_error("host failure");};
  madobs_event_v1 e{}; e.struct_size=sizeof(e); e.abi_version=1;
  assert(madobs_observer_emit(&o,&e)==MADOBS_CALLBACK_ERROR);
}
