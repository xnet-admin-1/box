cmake_minimum_required(VERSION 3.10)
project(ztproxy C CXX)

# libzt source tree — set LIBZT_DIR externally or default to sibling
if(NOT DEFINED LIBZT_DIR)
    set(LIBZT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../xnet-zt/externals/libzt)
endif()

set(ZTO_DIR ${LIBZT_DIR}/ext/ZeroTierOne)
set(LWIP_SRC ${LIBZT_DIR}/ext/lwip/src)
set(LWIP_PORT ${LIBZT_DIR}/ext/lwip-contrib/ports/unix/port)

set(ZT_FLAGS "-DZT_USE_MINIUPNPC=1 -DZT_SOFTWARE_UPDATE_DEFAULT=0 -D_USING_LWIP_DEFINITIONS_=0 -DZT_SDK=1 -DZT_ENABLE_CONTROLLER=1")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${ZT_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${ZT_FLAGS} -std=c++14")

include_directories(
    ${ZTO_DIR} ${ZTO_DIR}/node ${ZTO_DIR}/osdep ${ZTO_DIR}/include
    ${ZTO_DIR}/ext/miniupnpc ${ZTO_DIR}/ext/libnatpmp
    ${ZTO_DIR}/controller
    ${LIBZT_DIR}/src ${LIBZT_DIR}/include ${LIBZT_DIR}/ext/concurrentqueue
    ${LWIP_SRC}/include ${LWIP_PORT}/include
)

file(GLOB ZTO_SRC ${ZTO_DIR}/node/*.cpp ${ZTO_DIR}/osdep/OSUtils.cpp
    ${ZTO_DIR}/osdep/PortMapper.cpp ${ZTO_DIR}/osdep/ManagedRoute.cpp)

set(NATPMP_SRC
    ${ZTO_DIR}/ext/libnatpmp/natpmp.c
    ${ZTO_DIR}/ext/libnatpmp/wingettimeofday.c
    ${ZTO_DIR}/ext/libnatpmp/getgateway.c)

set(UPNPC_SRC
    ${ZTO_DIR}/ext/miniupnpc/connecthostport.c
    ${ZTO_DIR}/ext/miniupnpc/igd_desc_parse.c
    ${ZTO_DIR}/ext/miniupnpc/minisoap.c
    ${ZTO_DIR}/ext/miniupnpc/minissdpc.c
    ${ZTO_DIR}/ext/miniupnpc/miniupnpc.c
    ${ZTO_DIR}/ext/miniupnpc/miniwget.c
    ${ZTO_DIR}/ext/miniupnpc/minixml.c
    ${ZTO_DIR}/ext/miniupnpc/portlistingparse.c
    ${ZTO_DIR}/ext/miniupnpc/receivedata.c
    ${ZTO_DIR}/ext/miniupnpc/upnpcommands.c
    ${ZTO_DIR}/ext/miniupnpc/upnpdev.c
    ${ZTO_DIR}/ext/miniupnpc/upnperrors.c
    ${ZTO_DIR}/ext/miniupnpc/upnpreplyparse.c)

file(GLOB LWIP_SRC_FILES
    ${LWIP_SRC}/netif/*.c ${LWIP_SRC}/api/*.c ${LWIP_PORT}/sys_arch.c
    ${LWIP_SRC}/core/*.c ${LWIP_SRC}/core/ipv4/*.c ${LWIP_SRC}/core/ipv6/*.c)

file(GLOB LIBZT_SRC ${LIBZT_DIR}/src/*.cpp)

# Controller
set(CONTROLLER_SRC
    ${ZTO_DIR}/controller/EmbeddedNetworkController.cpp
    ${ZTO_DIR}/controller/DB.cpp
    ${ZTO_DIR}/controller/FileDB.cpp
    ${ZTO_DIR}/controller/LFDB.cpp)

# sio stubs needed by lwip
set(SIO_STUBS ${CMAKE_CURRENT_SOURCE_DIR}/sio_stubs.c)

add_executable(ztproxy
    main.c
    ${SIO_STUBS}
    ${ZTO_SRC}
    ${NATPMP_SRC}
    ${UPNPC_SRC}
    ${LWIP_SRC_FILES}
    ${LIBZT_SRC}
    ${CONTROLLER_SRC}
)

target_link_libraries(ztproxy log)
set_target_properties(ztproxy PROPERTIES LINK_FLAGS "-Wl,-z,max-page-size=16384")
