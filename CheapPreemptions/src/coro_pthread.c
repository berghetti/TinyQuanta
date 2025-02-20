#include <pthread.h>
#include "ci_lib.h"
#include <errno.h>

int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
  int ret;
  for(;;) {
    //if (mutex->__data.__owner == cp_pid) {
    //    // mutex is held by the same thread, yield
    //    (*intvActionHook)(0);
    //    continue;
    //}
    ret = pthread_mutex_trylock(mutex);
    if(ret == EBUSY) {
        // mutex is held, yield
    //    (*intvActionHook)(ret);
        continue;
    }
    // for other cases, we are done
    return ret;
  }
}

