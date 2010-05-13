#include <reent.h>

/* Note that there is a copy of this in sys/reent.h.  */
#ifndef __ATTRIBUTE_IMPURE_PTR__
#define __ATTRIBUTE_IMPURE_PTR__
#endif

#ifndef __ATTRIBUTE_IMPURE_DATA__
#define __ATTRIBUTE_IMPURE_DATA__
#endif

static struct _reent __ATTRIBUTE_IMPURE_DATA__ global_impure_data = _REENT_INIT (global_impure_data);
static __thread struct _reent __ATTRIBUTE_IMPURE_DATA__ impure_data = _REENT_INIT (global_impure_data);
#ifdef __CYGWIN__
extern struct _reent reent_data __attribute__ ((alias("impure_data")));
#endif
__thread struct _reent *__ATTRIBUTE_IMPURE_PTR__ _impure_ptr = -1; //NULL; - try to avoid tbss until its alignment is fixed
struct _reent *_CONST __ATTRIBUTE_IMPURE_PTR__ _global_impure_ptr = &global_impure_data;

/*
 * This function should be called on thread startup (for each thread).
 */
void __newlib_thread_init()
{
#if 0
  /* NOTE: this code stems from the nacl x86 patch for newlib
   *       but does not quite work ARM
   */
  /*
   * Fix the initialization - REENT_INIT pointed
   * the pointers to the global structure.
   */
  impure_data._stdin = &impure_data.__sf[0];
  impure_data._stdout = &impure_data.__sf[1];
  impure_data._stderr = &impure_data.__sf[2];
#else
  __sinit(&impure_data);
#endif
  /* Set the pointer to point to the thread-specific structure. */
  _impure_ptr = &impure_data;
}
