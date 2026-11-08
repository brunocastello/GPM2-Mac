/* Mac OS 9 has no POSIX threads: the core runs on a single thread and locking becomes a no-op. */
#ifndef GPM2_OS9_PTHREAD_H
#define GPM2_OS9_PTHREAD_H
/* the pthread_t/mutex_t/cond_t types come from newlib's sys/_pthreadtypes.h (scalars) */
#define PTHREAD_MUTEX_INITIALIZER 0
#define PTHREAD_COND_INITIALIZER 0
#define pthread_mutex_init(m, a) (*(m) = 0)
#define pthread_mutex_destroy(m) 0
/* counter: a cooperative thread may yield while holding the lock (dpe_service_all uses trylock) */
#define pthread_mutex_lock(m) ((*(m))++, 0)
#define pthread_mutex_unlock(m) ((*(m))--, 0)
#define pthread_mutex_trylock(m) (*(m) ? 16 : ((*(m))++, 0))
#define pthread_cond_signal(c) 0
#define pthread_create(t, a, f, p) (-1)
#define pthread_join(t, r) 0
#define pthread_attr_init(a) 0
#define pthread_attr_setstacksize(a, s) 0
#endif
