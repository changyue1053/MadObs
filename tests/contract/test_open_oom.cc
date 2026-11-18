#include "madobs/private.h"
#include <cassert>
#include <cstdlib>
#include <new>
#include <string>
static int countdown=-1;
extern "C" void* __real__Znwm(size_t);
extern "C" void* __wrap__Znwm(size_t size) {
  if(countdown==0) { countdown=-1; throw std::bad_alloc(); }
  if(countdown>0) --countdown;
  return __real__Znwm(size);
}
static void Send(void*,uint8_t,uint64_t,const uint8_t*,size_t,madobs_send_done_fn done,void* ctx) { done(ctx,1); }
int main() {
  char parent[]="/tmp/madobs_open_oom_XXXXXX"; assert(mkdtemp(parent));
  madobs_event_rule_v1 rule{};
  madobs_privacy_policy_v1 policy{sizeof(policy),1,1,&rule};
  // Fail each of the first six C++ allocations: owner, shared control block,
  // and early policy-identity storage. No worker has started at these points.
  for(int i=0;i<6;++i) {
    const auto path=std::string(parent)+"/case"+std::to_string(i);
    madobs_private_open_result reason=MADOBS_PRIVATE_OPEN_OK;
    countdown=i;
    const auto failed=madobs_private_open_ex(path.c_str(),Send,nullptr,&policy,&reason);
    countdown=-1;
    assert(!failed && reason==MADOBS_PRIVATE_OPEN_INTERNAL);
    const auto retry=madobs_private_open_ex(path.c_str(),Send,nullptr,&policy,&reason);
    assert(retry && reason==MADOBS_PRIVATE_OPEN_OK);
    assert(madobs_private_destroy(retry)==MADOBS_OK);
  }
}
