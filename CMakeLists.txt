cmake_minimum_required(VERSION 3.30)
project(nginx LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 11)
# 架构ARM
set(CMAKE_OSX_ARCHITECTURES "arm64")

# 安装路径 /usr/local
set(NGX_PREFIX "${CMAKE_INSTALL_PREFIX}" CACHE STRING "nginx install prefix")
set(NGX_SBIN_PATH "${NGX_PREFIX}/bin/nginx" CACHE STRING "")
set(NGX_MODULES_PATH "${NGX_PREFIX}/modules" CACHE STRING "")
# nginx的配置文件
set(NGX_CONF_PATH "${NGX_PREFIX}/conf/nginx.conf" CACHE STRING "")
set(NGX_ERROR_LOG_PATH "${NGX_PREFIX}/logs/error.log" CACHE STRING "")
set(NGX_PID_PATH "${NGX_PREFIX}/logs/nginx.pid" CACHE STRING "")
set(NGX_LOCK_PATH "${NGX_PREFIX}/logs/nginx.lock" CACHE STRING "")
set(NGX_PLATFORM "" CACHE STRING "NGX platform to build for")

include(CheckCSourceCompiles)
include(CheckCSourceRuns)
include(CheckTypeSize)
include(TestBigEndian)

# nginx配置文件所在目录 /usr/local/conf
get_filename_component(NGX_CONF_PREFIX "${NGX_CONF_PATH}" DIRECTORY)

# 模板生成文件
configure_file(ngx_auto_headers.h.in ${CMAKE_CURRENT_BINARY_DIR}/ngx_auto_headers.h)
configure_file(ngx_auto_config.h.in ${CMAKE_CURRENT_BINARY_DIR}/ngx_auto_config.h)
configure_file(ngx_modules.c.in ${CMAKE_CURRENT_BINARY_DIR}/ngx_modules.c)

# core源码
set(CORE_SRCS
        src/core/nginx.c
        src/core/ngx_log.c
        src/core/ngx_palloc.c
        src/core/ngx_array.c
        src/core/ngx_list.c
        src/core/ngx_hash.c
        src/core/ngx_buf.c
        src/core/ngx_queue.c
        src/core/ngx_output_chain.c
        src/core/ngx_string.c
        src/core/ngx_parse.c
        src/core/ngx_parse_time.c
        src/core/ngx_inet.c
        src/core/ngx_file.c
        src/core/ngx_crc32.c
        src/core/ngx_murmurhash.c
        src/core/ngx_md5.c
        src/core/ngx_sha1.c
        src/core/ngx_rbtree.c
        src/core/ngx_radix_tree.c
        src/core/ngx_slab.c
        src/core/ngx_times.c
        src/core/ngx_shmtx.c
        src/core/ngx_connection.c
        src/core/ngx_cycle.c
        src/core/ngx_spinlock.c
        src/core/ngx_rwlock.c
        src/core/ngx_cpuinfo.c
        src/core/ngx_conf_file.c
        src/core/ngx_module.c
        src/core/ngx_resolver.c
        src/core/ngx_open_file_cache.c
        src/core/ngx_crypt.c
        src/core/ngx_proxy_protocol.c
        src/core/ngx_syslog.c
        src/core/ngx_regex.c
)

# event源码
set(EVENT_SRCS
        src/event/ngx_event.c
        src/event/ngx_event_timer.c
        src/event/ngx_event_posted.c
        src/event/ngx_event_accept.c
        src/event/ngx_event_udp.c
        src/event/ngx_event_connect.c
        src/event/ngx_event_pipe.c
)

# kq源码
set(KQ_SRCS src/event/modules/ngx_kqueue_module.c)

# unix系统
set(UNIX_SRCS
        src/os/unix/ngx_time.c
        src/os/unix/ngx_errno.c
        src/os/unix/ngx_alloc.c
        src/os/unix/ngx_files.c
        src/os/unix/ngx_socket.c
        src/os/unix/ngx_recv.c
        src/os/unix/ngx_readv_chain.c
        src/os/unix/ngx_udp_recv.c
        src/os/unix/ngx_send.c
        src/os/unix/ngx_writev_chain.c
        src/os/unix/ngx_udp_send.c
        src/os/unix/ngx_udp_sendmsg_chain.c
        src/os/unix/ngx_channel.c
        src/os/unix/ngx_shmem.c
        src/os/unix/ngx_process.c
        src/os/unix/ngx_daemon.c
        src/os/unix/ngx_setaffinity.c
        src/os/unix/ngx_setproctitle.c
        src/os/unix/ngx_posix_init.c
        src/os/unix/ngx_user.c
        src/os/unix/ngx_dlopen.c
        src/os/unix/ngx_process_cycle.c
        src/os/unix/ngx_darwin_init.c
        src/os/unix/ngx_darwin_sendfile_chain.c
)

# mac系统
set(MAC_SRCS
        ${CORE_SRCS}
        ${EVENT_SRCS}
        ${UNIX_SRCS}
        ${KQ_SRCS}
)

# 模版文件源码
set(NGX_MODULE_SRCS ${CMAKE_BINARY_DIR}/ngx_modules.c)

# http模块源码
set(HTTP_SRCS
        src/http/ngx_http.c
        src/http/ngx_http_core_module.c
        src/http/ngx_http_special_response.c
        src/http/ngx_http_request.c
        src/http/ngx_http_parse.c
        src/http/modules/ngx_http_log_module.c
        src/http/ngx_http_request_body.c
        src/http/ngx_http_variables.c
        src/http/ngx_http_script.c
        src/http/ngx_http_upstream.c
        src/http/ngx_http_upstream_round_robin.c
        src/http/ngx_http_file_cache.c
        src/http/ngx_http_write_filter_module.c
        src/http/ngx_http_header_filter_module.c
        src/http/modules/ngx_http_chunked_filter_module.c
        src/http/modules/ngx_http_range_filter_module.c
        src/http/modules/ngx_http_gzip_filter_module.c
        src/http/ngx_http_postpone_filter_module.c
        src/http/modules/ngx_http_ssi_filter_module.c
        src/http/modules/ngx_http_charset_filter_module.c
        src/http/modules/ngx_http_userid_filter_module.c
        src/http/modules/ngx_http_headers_filter_module.c
        src/http/ngx_http_copy_filter_module.c
        src/http/modules/ngx_http_not_modified_filter_module.c
        src/http/modules/ngx_http_static_module.c
        src/http/modules/ngx_http_autoindex_module.c
        src/http/modules/ngx_http_index_module.c
        src/http/modules/ngx_http_mirror_module.c
        src/http/modules/ngx_http_try_files_module.c
        src/http/modules/ngx_http_auth_basic_module.c
        src/http/modules/ngx_http_access_module.c
        src/http/modules/ngx_http_limit_conn_module.c
        src/http/modules/ngx_http_limit_req_module.c
        src/http/modules/ngx_http_geo_module.c
        src/http/modules/ngx_http_map_module.c
        src/http/modules/ngx_http_split_clients_module.c
        src/http/modules/ngx_http_referer_module.c
        src/http/modules/ngx_http_rewrite_module.c
        src/http/modules/ngx_http_proxy_module.c
        src/http/modules/ngx_http_fastcgi_module.c
        src/http/modules/ngx_http_uwsgi_module.c
        src/http/modules/ngx_http_scgi_module.c
        src/http/modules/ngx_http_memcached_module.c
        src/http/modules/ngx_http_empty_gif_module.c
        src/http/modules/ngx_http_browser_module.c
        src/http/modules/ngx_http_upstream_hash_module.c
        src/http/modules/ngx_http_upstream_ip_hash_module.c
        src/http/modules/ngx_http_upstream_least_conn_module.c
        src/http/modules/ngx_http_upstream_random_module.c
        src/http/modules/ngx_http_upstream_keepalive_module.c
        src/http/modules/ngx_http_upstream_zone_module.c
)

if (NGX_PLATFORM STREQUAL "")
    execute_process(COMMAND uname -s
            OUTPUT_VARIABLE NGX_SYSTEM
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND uname -r
            OUTPUT_VARIABLE NGX_RELEASE
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND uname -s
            OUTPUT_VARIABLE NGX_MACHINE
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(NGX_PLATFORM "${NGX_SYSTEM}:${NGX_RELEASE}:${NGX_MACHINE}")
endif ()
message(STATUS "ngx: checking for OS, building for ${NGX_PLATFORM}")

# 指定宏
add_definitions(-D_GNU_SOURCE -D_FILE_OFFSET_BITS=64)
set(CMAKE_REQUIRED_DEFINITIONS "-D_GNU_SOURCE -D_FILE_OFFSET_BITS=64")

# cpu架构arm64
set(NGX_CPU_CACHE_LINE 64)

TEST_BIG_ENDIAN(SYSTEM_IS_BIG_ENDIAN)

# 启用pkg-config模块
find_package(PkgConfig REQUIRED)
# pkg检测库
list(APPEND REQ_LIB_LIST libpcre zlib)
message(STATUS "要链接的库 ${REQ_LIB_LIST}")
# 依赖的库别名REQ
pkg_check_modules(REQ REQUIRED ${REQ_LIB_LIST})
# 所有依赖库的头文件路径
message(STATUS "链接库的头文件路径 ${REQ_INCLUDE_DIRS}")
include_directories(${REQ_INCLUDE_DIRS})
# 所有依赖库的库文件路径
message(STATUS "链接库的路径 ${REQ_LIBRARY_DIRS}")
link_directories(${REQ_LIBRARY_DIRS})

# 编译参数
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-unused-parameter")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-parameter")

# 头文件路径
include_directories(src/core)
include_directories(src/event)
include_directories(src/event/modules)
include_directories(src/event/quic)
include_directories(src/os/unix)
include_directories(src/http)
include_directories(src/http/modules)
# 模板文件
include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(/opt/homebrew/include)
include_directories(/Users/dingrui/MyApp/zlib/zlib-1.3.1)

# 编译可执行文件nginx
add_executable(${PROJECT_NAME}
        ${MAC_SRCS}
        ${NGX_MODULE_SRCS}
        ${HTTP_SRCS}
)

# 链接库
message(STATUS "链接库 ${REQ_LIBRARIES}")
target_link_libraries(${PROJECT_NAME}
        ${REQ_LIBRARIES}
)

# 安装 /usr/local/bin
install(TARGETS nginx DESTINATION "${NGX_PREFIX}/bin")

get_filename_component(NGX_HTTP_LOG_DIR "${NGX_HTTP_LOG_PATH}" DIRECTORY)
install(DIRECTORY DESTINATION "${NGX_HTTP_LOG_DIR}")

install(DIRECTORY DESTINATION "${NGX_HTTP_CLIENT_TEMP_PATH}")
install(DIRECTORY DESTINATION "${NGX_HTTP_PROXY_TEMP_PATH}")
install(DIRECTORY DESTINATION "${NGX_HTTP_FASTCGI_TEMP_PATH}")
install(DIRECTORY DESTINATION "${NGX_HTTP_UWSGI_TEMP_PATH}")
install(DIRECTORY DESTINATION "${NGX_HTTP_SCGI_TEMP_PATH}")