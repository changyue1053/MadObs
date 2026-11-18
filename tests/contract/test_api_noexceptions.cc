#include <cassert>

#include "madobs/observer.h"
int main() {
  auto o = madobs_noop_observer();
  assert(madobs_observer_emit(&o, nullptr) == MADOBS_NOOP);
  int calls = 0;
  o.ctx = &calls;
  o.emit = [](void* p, const madobs_event_v1*) { ++*static_cast<int*>(p); };
  madobs_event_v1 e{};
  e.struct_size = sizeof(e);
  e.abi_version = 1;
  assert(madobs_observer_emit(&o, &e) == MADOBS_EMITTED && calls == 1);
}
