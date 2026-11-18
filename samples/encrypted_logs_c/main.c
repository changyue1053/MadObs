#include "madobs/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if(!(x)) return __LINE__; } while(0)
static void send_batch(void* c,uint8_t p,uint64_t id,const uint8_t* b,size_t n,madobs_send_done_fn done,void* dc) {
  (void)c; (void)p; (void)id; (void)b; (void)n; done(dc,1);
}
int main(void) {
  char dir[]="/tmp/madobs_encrypted_consumer_XXXXXX",logdir[128];
  CHECK(mkdtemp(dir)); snprintf(logdir,sizeof(logdir),"%s/logs",dir);
  uint8_t key[32]={0}; /* deterministic test-only key; never use in production */
  madobs_handle_t handle=madobs_create(dir,send_batch,NULL); CHECK(handle);
  CHECK(madobs_enable_logs(handle,logdir,4096,2,key)==MADOBS_OK);
  CHECK(madobs_start(handle)==MADOBS_OK);
  madobs_log(handle,2,1,"test fixture",12);
  CHECK(madobs_stop(handle)==MADOBS_OK); CHECK(madobs_destroy(handle)==MADOBS_OK);
  return 0;
}
