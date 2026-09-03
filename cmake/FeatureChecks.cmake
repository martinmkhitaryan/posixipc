include(CheckSymbolExists)
include(CheckTypeSize)
include(CheckCSourceCompiles)

function(posixipc_check_symbol symbol header varname)
    check_symbol_exists(${symbol} "${header}" ${varname})
    if(${varname})
        set(${varname} 1 PARENT_SCOPE)
    else()
        set(${varname} 0 PARENT_SCOPE)
    endif()
endfunction()

function(posixipc_run_feature_checks)
    set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
    set(CMAKE_REQUIRED_LIBRARIES ${CMAKE_THREAD_LIBS_INIT})
    find_library(POSIXIPC_RT_LIBRARY rt)
    if(POSIXIPC_RT_LIBRARY)
        list(APPEND CMAKE_REQUIRED_LIBRARIES ${POSIXIPC_RT_LIBRARY})
    endif()

    posixipc_check_symbol(pthread_mutexattr_setrobust pthread.h
        POSIXIPC_HAVE_PTHREAD_MUTEXATTR_SETROBUST)
    posixipc_check_symbol(pthread_mutexattr_setpshared pthread.h
        POSIXIPC_HAVE_PTHREAD_MUTEXATTR_SETPSHARED)
    posixipc_check_symbol(pthread_mutexattr_setprotocol pthread.h
        POSIXIPC_HAVE_PTHREAD_MUTEXATTR_SETPROTOCOL)
    posixipc_check_symbol(pthread_rwlockattr_setpshared pthread.h
        POSIXIPC_HAVE_PTHREAD_RWLOCKATTR_SETPSHARED)
    posixipc_check_symbol(pthread_condattr_setpshared pthread.h
        POSIXIPC_HAVE_PTHREAD_CONDATTR_SETPSHARED)
    posixipc_check_symbol(pthread_condattr_setclock pthread.h
        POSIXIPC_HAVE_PTHREAD_CONDATTR_SETCLOCK)
    posixipc_check_symbol(pthread_barrier_wait pthread.h
        POSIXIPC_HAVE_PTHREAD_BARRIER_WAIT)
    posixipc_check_symbol(pthread_barrierattr_setpshared pthread.h
        POSIXIPC_HAVE_PTHREAD_BARRIERATTR_SETPSHARED)
    posixipc_check_symbol(pthread_spin_init pthread.h
        POSIXIPC_HAVE_PTHREAD_SPIN_INIT)
    posixipc_check_symbol(pthread_spin_trylock pthread.h
        POSIXIPC_HAVE_PTHREAD_SPIN_TRYLOCK)
    posixipc_check_symbol(pthread_mutex_timedlock pthread.h
        POSIXIPC_HAVE_PTHREAD_MUTEX_TIMEDLOCK)
    posixipc_check_symbol(pthread_mutex_clocklock pthread.h
        POSIXIPC_HAVE_PTHREAD_MUTEX_CLOCKLOCK)
    posixipc_check_symbol(pthread_rwlock_clockrdlock pthread.h
        POSIXIPC_HAVE_PTHREAD_RWLOCK_CLOCKRDLOCK)
    posixipc_check_symbol(pthread_rwlock_clockwrlock pthread.h
        POSIXIPC_HAVE_PTHREAD_RWLOCK_CLOCKWRLOCK)
    posixipc_check_symbol(pthread_cond_clockwait pthread.h
        POSIXIPC_HAVE_PTHREAD_COND_CLOCKWAIT)
    posixipc_check_symbol(sem_timedwait semaphore.h
        POSIXIPC_HAVE_SEM_TIMEDWAIT)
    posixipc_check_symbol(sem_clockwait semaphore.h
        POSIXIPC_HAVE_SEM_CLOCKWAIT)
    posixipc_check_symbol(sem_open semaphore.h
        POSIXIPC_HAVE_SEM_OPEN)
    posixipc_check_symbol(shm_open sys/mman.h
        POSIXIPC_HAVE_SHM_OPEN)
    posixipc_check_symbol(mq_open mqueue.h
        POSIXIPC_HAVE_MQ_OPEN)
    posixipc_check_symbol(memfd_create sys/mman.h
        POSIXIPC_HAVE_MEMFD_CREATE)
    posixipc_check_symbol(eventfd sys/eventfd.h
        POSIXIPC_HAVE_EVENTFD)

    check_c_source_compiles("
        #include <linux/futex.h>
        #include <sys/syscall.h>
        #include <unistd.h>
        int main(void) {
            (void)SYS_futex;
            (void)FUTEX_WAIT_BITSET;
            (void)FUTEX_WAKE;
            (void)FUTEX_BITSET_MATCH_ANY;
            return 0;
        }
    " POSIXIPC_HAVE_FUTEX)
    if(NOT POSIXIPC_HAVE_FUTEX)
        set(POSIXIPC_HAVE_FUTEX 0)
    endif()

    set(CMAKE_EXTRA_INCLUDE_FILES "pthread.h")
    check_type_size("pthread_mutex_t" POSIXIPC_SIZEOF_PTHREAD_MUTEX_T)
    check_type_size("pthread_rwlock_t" POSIXIPC_SIZEOF_PTHREAD_RWLOCK_T)
    check_type_size("pthread_cond_t" POSIXIPC_SIZEOF_PTHREAD_COND_T)
    if(POSIXIPC_HAVE_PTHREAD_BARRIER_WAIT)
        check_type_size("pthread_barrier_t" POSIXIPC_SIZEOF_PTHREAD_BARRIER_T)
    else()
        set(POSIXIPC_SIZEOF_PTHREAD_BARRIER_T 0)
    endif()
    if(POSIXIPC_HAVE_PTHREAD_SPIN_INIT)
        check_type_size("pthread_spinlock_t" POSIXIPC_SIZEOF_PTHREAD_SPINLOCK_T)
    else()
        set(POSIXIPC_SIZEOF_PTHREAD_SPINLOCK_T 0)
    endif()
    set(CMAKE_EXTRA_INCLUDE_FILES "semaphore.h")
    check_type_size("sem_t" POSIXIPC_SIZEOF_SEM_T)

    check_c_source_compiles("
        #include <features.h>
        #ifndef __GLIBC__
        #error not glibc
        #endif
        int main(void) { return 0; }
    " POSIXIPC_LIBC_IS_GLIBC)

    check_c_source_compiles("
        #include <stdarg.h>
        #if !defined(__DEFINED_va_list) && !defined(__MUSL__)
        #error not musl
        #endif
        int main(void) { return 0; }
    " POSIXIPC_LIBC_IS_MUSL)

    if(POSIXIPC_LIBC_IS_GLIBC)
        set(POSIXIPC_LIBC_FAMILY 1)
        set(POSIXIPC_LIBC_NAME "glibc")
    elseif(POSIXIPC_LIBC_IS_MUSL)
        set(POSIXIPC_LIBC_FAMILY 2)
        set(POSIXIPC_LIBC_NAME "musl")
    else()
        set(POSIXIPC_LIBC_FAMILY 3)
        set(POSIXIPC_LIBC_NAME "other")
    endif()

    if(NOT POSIXIPC_SIZEOF_PTHREAD_MUTEX_T)
        set(POSIXIPC_SIZEOF_PTHREAD_MUTEX_T 0)
    endif()
    if(NOT POSIXIPC_SIZEOF_PTHREAD_RWLOCK_T)
        set(POSIXIPC_SIZEOF_PTHREAD_RWLOCK_T 0)
    endif()
    if(NOT POSIXIPC_SIZEOF_PTHREAD_COND_T)
        set(POSIXIPC_SIZEOF_PTHREAD_COND_T 0)
    endif()
    if(NOT POSIXIPC_SIZEOF_SEM_T)
        set(POSIXIPC_SIZEOF_SEM_T 0)
    endif()

    set(POSIXIPC_ABI_SEED_STRING
        "${CMAKE_SYSTEM_PROCESSOR}|${CMAKE_SIZEOF_VOID_P}|${POSIXIPC_LIBC_FAMILY}|${POSIXIPC_CACHELINE_BYTES}|${POSIXIPC_SIZEOF_PTHREAD_MUTEX_T}|${POSIXIPC_SIZEOF_PTHREAD_RWLOCK_T}|${POSIXIPC_SIZEOF_PTHREAD_COND_T}|${POSIXIPC_SIZEOF_PTHREAD_BARRIER_T}|${POSIXIPC_SIZEOF_PTHREAD_SPINLOCK_T}|${POSIXIPC_SIZEOF_SEM_T}")
    string(SHA256 POSIXIPC_ABI_SEED_HASH "${POSIXIPC_ABI_SEED_STRING}")
    string(SUBSTRING "${POSIXIPC_ABI_SEED_HASH}" 0 8 POSIXIPC_ABI_TAG_SEED_HEX)
    math(EXPR POSIXIPC_ABI_TAG_SEED "0x${POSIXIPC_ABI_TAG_SEED_HEX}")

    foreach(var
        POSIXIPC_HAVE_PTHREAD_MUTEXATTR_SETROBUST
        POSIXIPC_HAVE_PTHREAD_MUTEXATTR_SETPSHARED
        POSIXIPC_HAVE_PTHREAD_MUTEXATTR_SETPROTOCOL
        POSIXIPC_HAVE_PTHREAD_RWLOCKATTR_SETPSHARED
        POSIXIPC_HAVE_PTHREAD_CONDATTR_SETPSHARED
        POSIXIPC_HAVE_PTHREAD_CONDATTR_SETCLOCK
        POSIXIPC_HAVE_PTHREAD_BARRIER_WAIT
        POSIXIPC_HAVE_PTHREAD_BARRIERATTR_SETPSHARED
        POSIXIPC_HAVE_PTHREAD_SPIN_INIT
        POSIXIPC_HAVE_PTHREAD_SPIN_TRYLOCK
        POSIXIPC_HAVE_PTHREAD_MUTEX_TIMEDLOCK
        POSIXIPC_HAVE_PTHREAD_MUTEX_CLOCKLOCK
        POSIXIPC_HAVE_PTHREAD_RWLOCK_CLOCKRDLOCK
        POSIXIPC_HAVE_PTHREAD_RWLOCK_CLOCKWRLOCK
        POSIXIPC_HAVE_PTHREAD_COND_CLOCKWAIT
        POSIXIPC_HAVE_SEM_TIMEDWAIT
        POSIXIPC_HAVE_SEM_CLOCKWAIT
        POSIXIPC_HAVE_SEM_OPEN
        POSIXIPC_HAVE_SHM_OPEN
        POSIXIPC_HAVE_MQ_OPEN
        POSIXIPC_HAVE_MEMFD_CREATE
        POSIXIPC_HAVE_EVENTFD
        POSIXIPC_HAVE_FUTEX
        POSIXIPC_SIZEOF_PTHREAD_MUTEX_T
        POSIXIPC_SIZEOF_PTHREAD_RWLOCK_T
        POSIXIPC_SIZEOF_PTHREAD_COND_T
        POSIXIPC_SIZEOF_PTHREAD_BARRIER_T
        POSIXIPC_SIZEOF_PTHREAD_SPINLOCK_T
        POSIXIPC_SIZEOF_SEM_T
        POSIXIPC_LIBC_FAMILY
        POSIXIPC_LIBC_NAME
        POSIXIPC_ABI_TAG_SEED
        POSIXIPC_RT_LIBRARY)
        set(${var} "${${var}}" PARENT_SCOPE)
    endforeach()
    set(POSIXIPC_CACHELINE_BYTES "${POSIXIPC_CACHELINE_BYTES}" PARENT_SCOPE)
endfunction()
