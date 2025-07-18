cmake_minimum_required(VERSION 3.10)

project(storagemanager)

include_directories(include ${CMAKE_BINARY_DIR}/include ${ENGINE_COMMON_INCLUDES} ${S3API_DIR})

set(storagemanager_SRCS
    src/AppendTask.cpp
    src/ClientRequestProcessor.cpp
    src/ListDirectoryTask.cpp
    src/OpenTask.cpp
    src/PingTask.cpp
    src/PosixTask.cpp
    src/ProcessTask.cpp
    src/ReadTask.cpp
    src/StatTask.cpp
    src/ThreadPool.cpp
    src/TruncateTask.cpp
    src/UnlinkTask.cpp
    src/WriteTask.cpp
    src/CopyTask.cpp
    src/IOCoordinator.cpp
    src/SessionManager.cpp
    src/Config.cpp
    src/CloudStorage.cpp
    src/S3Storage.cpp
    src/LocalStorage.cpp
    src/Cache.cpp
    src/SMLogging.cpp
    src/Downloader.cpp
    src/Synchronizer.cpp
    src/RWLock.cpp
    src/MetadataFile.cpp
    src/Replicator.cpp
    src/Utilities.cpp
    src/Ownership.cpp
    src/PrefixCache.cpp
    src/SyncTask.cpp
    src/ListIOTask.cpp
    src/TerminateIOTask.cpp
    ../utils/common/crashtrace.cpp
)

option(SM_TRACE "Enable some tracing output" OFF)
option(SM_LOG_TO_STDERR "Make the logging system also print to stderr" OFF)
if(SM_TRACE)
    add_definitions(-DSM_TRACE)
endif()

if(CMAKE_BUILD_TYPE STREQUAL Debug)
    add_definitions(-DDEBUG -Werror -Wall)
    set(S3_CONFIGURE_OPT --enable-debug)
endif()

if(SM_LOG_TO_STDERR)
    add_definitions(-DDEBUG)
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/storagemanager.cnf.in" "${CMAKE_CURRENT_SOURCE_DIR}/storagemanager.cnf" @ONLY
)

# get linkage right
link_directories(${CMAKE_BINARY_DIR}/lib)
set(CMAKE_INSTALL_RPATH $ORIGIN $ORIGIN/../lib)

columnstore_library(storagemanager ${storagemanager_SRCS})
add_dependencies(storagemanager marias3 external_boost)

target_compile_definitions(storagemanager PUBLIC BOOST_NO_CXX11_SCOPED_ENUMS)
columnstore_link(
    storagemanager
    boost_chrono
    boost_system
    boost_thread
    boost_filesystem
    boost_regex
    pthread
    ${S3API_DEPS}
)
target_include_directories(storagemanager PRIVATE ${Boost_INCLUDE_DIRS})

columnstore_executable(StorageManager src/main.cpp)
columnstore_link(StorageManager storagemanager)

set(TMPDIR ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_executable(unit_tests src/unit_tests.cpp)
target_compile_definitions(unit_tests PUBLIC BOOST_NO_CXX11_SCOPED_ENUMS)
columnstore_link(unit_tests storagemanager)

columnstore_executable(testS3Connection src/testS3Connection.cpp)
target_compile_definitions(testS3Connection PUBLIC BOOST_NO_CXX11_SCOPED_ENUMS)
columnstore_link(testS3Connection storagemanager boost_program_options)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${TMPDIR})

add_custom_command(
    TARGET unit_tests
    PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/test_data
)
add_custom_command(
    TARGET unit_tests
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/test_data
            ${CMAKE_CURRENT_BINARY_DIR}/test_data
)

# The includes and lib linkages required to link against cloudio ... pretty crazy.  When lib dependencies are eventually
# config'd right, change this to only include and link against cloudio.
include_directories(${ENGINE_SRC_DIR}/utils/cloudio ${ENGINE_COMMON_INCLUDES})
columnstore_executable(smcat src/smcat.cpp)
columnstore_link(smcat storagemanager cloudio ${ENGINE_LDFLAGS} ${ENGINE_EXEC_LIBS})

columnstore_executable(smput src/smput.cpp)
columnstore_link(smput storagemanager cloudio ${ENGINE_LDFLAGS} ${ENGINE_EXEC_LIBS})

columnstore_executable(smls src/smls.cpp)
columnstore_link(smls storagemanager cloudio ${ENGINE_LDFLAGS} ${ENGINE_EXEC_LIBS})

columnstore_executable(smrm src/smrm.cpp)
columnstore_link(smrm storagemanager cloudio ${ENGINE_LDFLAGS} ${ENGINE_EXEC_LIBS})

columnstore_executable(smps src/smps.cpp)
columnstore_link(smps storagemanager cloudio ${ENGINE_LDFLAGS} ${ENGINE_EXEC_LIBS})

columnstore_executable(smkill src/smkill.cpp)
columnstore_link(smkill storagemanager cloudio ${ENGINE_LDFLAGS} ${ENGINE_EXEC_LIBS})

columnstore_install_file(storagemanager.cnf ${ENGINE_SYSCONFDIR}/columnstore)

install(
    FILES storagemanager.cnf
    RENAME storagemanager.cnf.example
    DESTINATION ${ENGINE_SYSCONFDIR}/columnstore
    COMPONENT columnstore-engine
)
