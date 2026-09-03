set(POSIXIPC_CORE_PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}/include")

configure_file(
    posixipc_config.h.in
    "${POSIXIPC_CORE_PUBLIC}/posixipc_config.h"
    @ONLY)

add_library(posixipc_core OBJECT
    posixipc_core.c
    posixipc_time.c
    posixipc_layout.c
    posixipc_shm.c
    posixipc_clocksym.c
    posixipc_mutex.c
    posixipc_rwlock.c
    posixipc_cond.c
    posixipc_sem.c
    posixipc_barrier.c
    posixipc_spin.c
    posixipc_nsem.c
    posixipc_queue.c
    posixipc_mq.c
    posixipc_futex.c
    posixipc_eventfd.c
    posixipc_memfd.c
    posixipc_futexq.c)
target_include_directories(posixipc_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${POSIXIPC_CORE_PUBLIC}")
target_link_libraries(posixipc_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(POSIXIPC_RT_LIBRARY)
    target_link_libraries(posixipc_core PUBLIC ${POSIXIPC_RT_LIBRARY})
endif()
posixipc_apply_warnings(posixipc_core)
