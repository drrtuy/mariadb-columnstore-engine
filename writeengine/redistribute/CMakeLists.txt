include_directories(${ENGINE_COMMON_INCLUDES})

# ########## next target ###############

set(writeengineredistribute_LIB_SRCS we_redistribute.cpp we_redistributecontrol.cpp we_redistributecontrolthread.cpp
                                     we_redistributeworkerthread.cpp
)

columnstore_library(writeengineredistribute ${writeengineredistribute_LIB_SRCS})

add_dependencies(writeengineredistribute loggingcpp)

columnstore_link(writeengineredistribute ${NETSNMP_LIBRARIES})

target_compile_definitions(writeengineredistribute PUBLIC BOOST_NO_CXX11_SCOPED_ENUMS)
