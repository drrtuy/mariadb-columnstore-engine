include_directories(${ENGINE_COMMON_INCLUDES} ../blockcache ../linux-port)

# ########## next target ###############

set(PrimProc_SRCS
    primproc.cpp
    batchprimitiveprocessor.cpp
    bppseeder.cpp
    bppsendthread.cpp
    columncommand.cpp
    command.cpp
    dictstep.cpp
    filtercommand.cpp
    logger.cpp
    passthrucommand.cpp
    primitiveserver.cpp
    pseudocc.cpp
    rtscommand.cpp
    umsocketselector.cpp
    serviceexemgr.cpp
    sqlfrontsessionthread.cpp
    rssmonfcn.cpp
    activestatementcounter.cpp
    femsghandler.cpp
    ../../utils/common/crashtrace.cpp
)

columnstore_executable(PrimProc ${PrimProc_SRCS})

target_include_directories(PrimProc PRIVATE ${Boost_INCLUDE_DIRS})
columnstore_link(
    PrimProc
    ${ENGINE_LDFLAGS}
    ${NETSNMP_LIBRARIES}
    ${ENGINE_WRITE_LIBS}
    threadpool
    cacheutils
    dbbc
    processor
    loggingcpp
)
