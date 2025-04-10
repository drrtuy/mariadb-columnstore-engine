SET (DEFAULT_USER "mysql")
SET (DEFAULT_GROUP "mysql")
SET(CORE_DUMPS "0")
OPTION(WITH_COREDUMPS "dump cores" OFF)

if (WITH_COREDUMPS)
  SET(CORE_DUMPS "infinity")
endif (WITH_COREDUMPS)


SET(LD_PRELOAD_STRING "LD_PRELOAD=$(ldconfig -p | grep -m1 libjemalloc | awk '{print $1}')")
SET(ALLOC_CONFIG "MALLOC_CONF=''")
SET(PRIMPROC_ALLOC_CONFIG ${ALLOC_CONFIG})
SET(DMLPROC_ALLOC_CONFIG ${ALLOC_CONFIG})
SET(DDLPROC_ALLOC_CONFIG ${ALLOC_CONFIG})
SET(WRITEENGINE_ALLOC_CONFIG ${ALLOC_CONFIG})
SET(CONTROLLERNODE_ALLOC_CONFIG ${ALLOC_CONFIG})
SET(WORKERNODE_ALLOC_CONFIG ${ALLOC_CONFIG})
SET(STORAGEMANAGER_ALLOC_CONFIG ${ALLOC_CONFIG})


if (WITH_COLUMNSTORE_ASAN)
  SET(ASAN_PATH "/tmp/asan")
  if (WITH_COLUMNSTORE_REPORT_PATH)
    SET(ASAN_PATH "${WITH_COLUMNSTORE_REPORT_PATH}/asan")
  endif (WITH_COLUMNSTORE_REPORT_PATH)


  SET(LD_PRELOAD_STRING "")
  SET(ALLOC_CONFIG "ASAN_OPTIONS=abort_on_error=1:disable_coredump=0,print_stats=false,detect_odr_violation=0,check_initialization_order=1,detect_stack_use_after_return=1,atexit=false,log_path=${ASAN_PATH}")
  SET(PRIMPROC_ALLOC_CONFIG ${ALLOC_CONFIG})
  SET(DMLPROC_ALLOC_CONFIG ${ALLOC_CONFIG})
  SET(DDLPROC_ALLOC_CONFIG ${ALLOC_CONFIG})
  SET(WRITEENGINE_ALLOC_CONFIG ${ALLOC_CONFIG})
  SET(CONTROLLERNODE_ALLOC_CONFIG ${ALLOC_CONFIG})
  SET(WORKERNODE_ALLOC_CONFIG ${ALLOC_CONFIG})
  SET(STORAGEMANAGER_ALLOC_CONFIG ${ALLOC_CONFIG})
endif()

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/columnstoreSyslogSetup.sh.in" "${CMAKE_CURRENT_SOURCE_DIR}/columnstoreSyslogSetup.sh" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/columnstore-post-install.in" "${CMAKE_CURRENT_SOURCE_DIR}/columnstore-post-install" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/columnstore-pre-uninstall.in" "${CMAKE_CURRENT_SOURCE_DIR}/columnstore-pre-uninstall" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/columnstoreLogRotate.in" "${CMAKE_CURRENT_SOURCE_DIR}/columnstoreLogRotate" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/mariadb-columnstore.service.in" "${CMAKE_CURRENT_SOURCE_DIR}/mariadb-columnstore.service" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/mariadb-columnstore-start.sh.in" "${CMAKE_CURRENT_SOURCE_DIR}/mariadb-columnstore-start.sh" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/mcs-workernode.service.in" "${CMAKE_CURRENT_SOURCE_DIR}/mcs-workernode.service" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/mcs-controllernode.service.in" "${CMAKE_CURRENT_SOURCE_DIR}/mcs-controllernode.service" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/mcs-primproc.service.in" "${CMAKE_CURRENT_SOURCE_DIR}/mcs-primproc.service" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/mcs-writeengineserver.service.in" "${CMAKE_CURRENT_SOURCE_DIR}/mcs-writeengineserver.service" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/mcs-dmlproc.service.in" "${CMAKE_CURRENT_SOURCE_DIR}/mcs-dmlproc.service" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/mcs-ddlproc.service.in" "${CMAKE_CURRENT_SOURCE_DIR}/mcs-ddlproc.service" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/mcs-loadbrm.service.in" "${CMAKE_CURRENT_SOURCE_DIR}/mcs-loadbrm.service" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/mcs-storagemanager.service.in" "${CMAKE_CURRENT_SOURCE_DIR}/mcs-storagemanager.service" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/mcs-stop-controllernode.sh.in" "${CMAKE_CURRENT_SOURCE_DIR}/mcs-stop-controllernode.sh" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/mcs-loadbrm.py.in" "${CMAKE_CURRENT_SOURCE_DIR}/mcs-loadbrm.py" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/mcs-savebrm.py.in" "${CMAKE_CURRENT_SOURCE_DIR}/mcs-savebrm.py" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/mcs-savebrm.py.in" "${CMAKE_CURRENT_SOURCE_DIR}/mcssavebrm.py" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/columnstoreSyslog.in" "${CMAKE_CURRENT_SOURCE_DIR}/columnstoreSyslog" @ONLY)

install(PROGRAMS columnstore-post-install
                columnstore-pre-uninstall
                columnstore_run.sh
                post-mysql-install
                post-mysqld-install
                columnstoreSyslogSetup.sh
                mcs-stop-controllernode.sh
                mcs-loadbrm.py
                mcs-savebrm.py
                mariadb-columnstore-start.sh
                mariadb-columnstore-stop.sh
                loop_process_starter.sh
                DESTINATION ${ENGINE_BINDIR} COMPONENT columnstore-engine)

install(FILES mariadb-columnstore.service
              columnstoreAlias
              columnstoreSyslog
              columnstoreSyslog7
              columnstoreSyslog-ng
              columnstoreLogRotate
              myCnf-include-args.text
              myCnf-exclude-args.text
              mcs-workernode.service
              mcs-controllernode.service
              mcs-primproc.service
              mcs-writeengineserver.service
              mcs-dmlproc.service
              mcs-ddlproc.service
              mcs-loadbrm.service
              mcs-storagemanager.service
              DESTINATION ${ENGINE_SUPPORTDIR} COMPONENT columnstore-engine)

install(FILES  module DESTINATION ${ENGINE_DATADIR}/local COMPONENT columnstore-engine)

find_package (Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME PythonUnitTests COMMAND ${Python3_EXECUTABLE} -m unittest test_mcs-savebrm.py)