include_directories(${ENGINE_COMMON_INCLUDES} ${ENGINE_UTILS_UDFSDK_INCLUDE} ${ENGINE_DATATYPES_INCLUDE})

# ########## next target ###############

set(execplan_LIB_SRCS
    calpontsystemcatalog.cpp
    aggregatecolumn.cpp
    arithmeticcolumn.cpp
    arithmeticoperator.cpp
    calpontexecutionplan.cpp
    calpontexecutionplanfactory.cpp
    calpontselectexecutionplan.cpp
    mcsanalyzetableexecutionplan.cpp
    clientrotator.cpp
    constantcolumn.cpp
    constantfilter.cpp
    existsfilter.cpp
    expressionparser.cpp
    filter.cpp
    functioncolumn.cpp
    groupconcatcolumn.cpp
    intervalcolumn.cpp
    logicoperator.cpp
    mysqlexecutionplan.cpp
    objectidmanager.cpp
    objectreader.cpp
    operator.cpp
    oracleexecutionplan.cpp
    outerjoinonfilter.cpp
    predicateoperator.cpp
    pseudocolumn.cpp
    range.cpp
    returnedcolumn.cpp
    rewrites.cpp
    rowcolumn.cpp
    selectfilter.cpp
    sessionmanager.cpp
    simplecolumn.cpp
    simplefilter.cpp
    simplescalarfilter.cpp
    treenode.cpp
    treenodeimpl.cpp
    vendorexecutionplan.cpp
    windowfunctioncolumn.cpp
    udafcolumn.cpp
)

columnstore_library(execplan ${execplan_LIB_SRCS})
columnstore_link(
    execplan
    messageqcpp
    ${ENGINE_DT_LIB}
    pron
    oamcpp
    loggingcpp
    querytele
)
