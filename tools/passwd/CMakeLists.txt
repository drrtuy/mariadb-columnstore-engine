include_directories(${ENGINE_COMMON_INCLUDES} ${ENGINE_UTILS_COMMON_INCLUDE})

# ########## next target ###############
set(cspasswd_SRCS cspasswd.cpp secrets.cpp)
set(cskeys_SRCS cskeys.cpp secrets.cpp)

columnstore_executable(cspasswd ${cspasswd_SRCS})
columnstore_executable(cskeys ${cskeys_SRCS})

target_include_directories(cspasswd BEFORE PUBLIC ${OPENSSL_INCLUDE_DIR})
target_include_directories(cskeys BEFORE PUBLIC ${OPENSSL_INCLUDE_DIR})
columnstore_link(cspasswd ${ENGINE_LDFLAGS} ${ENGINE_EXEC_LIBS} ${SSL_LIBRARIES})
columnstore_link(cskeys ${ENGINE_LDFLAGS} ${ENGINE_EXEC_LIBS} ${SSL_LIBRARIES})
