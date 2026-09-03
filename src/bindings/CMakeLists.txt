python_add_library(_posixipc MODULE
    module.c
    py_errors.c
    py_wait.c
    py_shm.c
    py_mutex.c
    py_layout.c
    py_sync.c
    py_m3.c
    py_array.c
    py_queue.c
    py_linux.c
    WITH_SOABI)
target_link_libraries(_posixipc PRIVATE posixipc_core)
target_include_directories(_posixipc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(_posixipc SYSTEM PRIVATE ${Python_INCLUDE_DIRS})
posixipc_apply_warnings(_posixipc)
install(TARGETS _posixipc DESTINATION posixipc)
