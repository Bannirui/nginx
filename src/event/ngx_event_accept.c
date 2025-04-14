
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


static ngx_int_t ngx_disable_accept_events(ngx_cycle_t *cycle, ngx_uint_t all);
#if (NGX_HAVE_EPOLLEXCLUSIVE)
static void ngx_reorder_accept_events(ngx_listening_t *ls);
#endif
static void ngx_close_accepted_connection(ngx_connection_t *c);


void
ngx_event_accept(ngx_event_t *ev)
{
    socklen_t          socklen;
    ngx_err_t          err;
    ngx_log_t         *log;
    ngx_uint_t         level;
    ngx_socket_t       s;
    ngx_event_t       *rev, *wev;
    ngx_sockaddr_t     sa;
    ngx_listening_t   *ls;
    ngx_connection_t  *c, *lc;
    ngx_event_conf_t  *ecf;
#if (NGX_HAVE_ACCEPT4)
    static ngx_uint_t  use_accept4 = 1;
#endif

    if (ev->timedout) {
        if (ngx_enable_accept_events((ngx_cycle_t *) ngx_cycle) != NGX_OK) {
            return;
        }

        ev->timedout = 0;
    }

    ecf = ngx_event_get_conf(ngx_cycle->conf_ctx, ngx_event_core_module);

    if (!(ngx_event_flags & NGX_USE_KQUEUE_EVENT)) {
        ev->available = ecf->multi_accept;
    }

    lc = ev->data;
    ls = lc->listening;
    ev->ready = 0;

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                   "accept on %V, ready: %d", &ls->addr_text, ev->available);

    do {
        socklen = sizeof(ngx_sockaddr_t);

#if (NGX_HAVE_ACCEPT4)
        if (use_accept4) {
            s = accept4(lc->fd, &sa.sockaddr, &socklen, SOCK_NONBLOCK);
        } else {
            s = accept(lc->fd, &sa.sockaddr, &socklen);
        }
#else
        s = accept(lc->fd, &sa.sockaddr, &socklen);
#endif

        if (s == (ngx_socket_t) -1) {
            err = ngx_socket_errno;

            if (err == NGX_EAGAIN) {
                ngx_log_debug0(NGX_LOG_DEBUG_EVENT, ev->log, err,
                               "accept() not ready");
                return;
            }

            level = NGX_LOG_ALERT;

            if (err == NGX_ECONNABORTED) {
                level = NGX_LOG_ERR;

            } else if (err == NGX_EMFILE || err == NGX_ENFILE) {
                level = NGX_LOG_CRIT;
            }

#if (NGX_HAVE_ACCEPT4)
            ngx_log_error(level, ev->log, err,
                          use_accept4 ? "accept4() failed" : "accept() failed");

            if (use_accept4 && err == NGX_ENOSYS) {
                use_accept4 = 0;
                ngx_inherited_nonblocking = 0;
                continue;
            }
#else
            ngx_log_error(level, ev->log, err, "accept() failed");
#endif

            if (err == NGX_ECONNABORTED) {
                if (ngx_event_flags & NGX_USE_KQUEUE_EVENT) {
                    ev->available--;
                }

                if (ev->available) {
                    continue;
                }
            }

            if (err == NGX_EMFILE || err == NGX_ENFILE) {
                if (ngx_disable_accept_events((ngx_cycle_t *) ngx_cycle, 1)
                    != NGX_OK)
                {
                    return;
                }

                if (ngx_use_accept_mutex) {
                    if (ngx_accept_mutex_held) {
                        ngx_shmtx_unlock(&ngx_accept_mutex);
                        ngx_accept_mutex_held = 0;
                    }

                    ngx_accept_disabled = 1;

                } else {
                    ngx_add_timer(ev, ecf->accept_mutex_delay);
                }
            }

            return;
        }

#if (NGX_STAT_STUB)
        (void) ngx_atomic_fetch_add(ngx_stat_accepted, 1);
#endif

        ngx_accept_disabled = ngx_cycle->connection_n / 8
                              - ngx_cycle->free_connection_n;

        c = ngx_get_connection(s, ev->log);

        if (c == NULL) {
            if (ngx_close_socket(s) == -1) {
                ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_socket_errno,
                              ngx_close_socket_n " failed");
            }

            return;
        }

        c->type = SOCK_STREAM;

#if (NGX_STAT_STUB)
        (void) ngx_atomic_fetch_add(ngx_stat_active, 1);
#endif

        c->pool = ngx_create_pool(ls->pool_size, ev->log);
        if (c->pool == NULL) {
            ngx_close_accepted_connection(c);
            return;
        }

        if (socklen > (socklen_t) sizeof(ngx_sockaddr_t)) {
            socklen = sizeof(ngx_sockaddr_t);
        }

        c->sockaddr = ngx_palloc(c->pool, socklen);
        if (c->sockaddr == NULL) {
            ngx_close_accepted_connection(c);
            return;
        }

        ngx_memcpy(c->sockaddr, &sa, socklen);

        log = ngx_palloc(c->pool, sizeof(ngx_log_t));
        if (log == NULL) {
            ngx_close_accepted_connection(c);
            return;
        }

        /* set a blocking mode for iocp and non-blocking mode for others */

        if (ngx_inherited_nonblocking) {
            if (ngx_event_flags & NGX_USE_IOCP_EVENT) {
                if (ngx_blocking(s) == -1) {
                    ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_socket_errno,
                                  ngx_blocking_n " failed");
                    ngx_close_accepted_connection(c);
                    return;
                }
            }

        } else {
            if (!(ngx_event_flags & NGX_USE_IOCP_EVENT)) {
                if (ngx_nonblocking(s) == -1) {
                    ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_socket_errno,
                                  ngx_nonblocking_n " failed");
                    ngx_close_accepted_connection(c);
                    return;
                }
            }
        }

        *log = ls->log;

        c->recv = ngx_recv;
        c->send = ngx_send;
        c->recv_chain = ngx_recv_chain;
        c->send_chain = ngx_send_chain;

        c->log = log;
        c->pool->log = log;

        c->socklen = socklen;
        c->listening = ls;
        c->local_sockaddr = ls->sockaddr;
        c->local_socklen = ls->socklen;

#if (NGX_HAVE_UNIX_DOMAIN)
        if (c->sockaddr->sa_family == AF_UNIX) {
            c->tcp_nopush = NGX_TCP_NOPUSH_DISABLED;
            c->tcp_nodelay = NGX_TCP_NODELAY_DISABLED;
#if (NGX_SOLARIS)
            /* Solaris's sendfilev() supports AF_NCA, AF_INET, and AF_INET6 */
            c->sendfile = 0;
#endif
        }
#endif

        rev = c->read;
        wev = c->write;

        wev->ready = 1;

        if (ngx_event_flags & NGX_USE_IOCP_EVENT) {
            rev->ready = 1;
        }

        if (ev->deferred_accept) {
            rev->ready = 1;
#if (NGX_HAVE_KQUEUE || NGX_HAVE_EPOLLRDHUP)
            rev->available = 1;
#endif
        }

        rev->log = log;
        wev->log = log;

        /*
         * TODO: MT: - ngx_atomic_fetch_add()
         *             or protection by critical section or light mutex
         *
         * TODO: MP: - allocated in a shared memory
         *           - ngx_atomic_fetch_add()
         *             or protection by critical section or light mutex
         */

        c->number = ngx_atomic_fetch_add(ngx_connection_counter, 1);

        c->start_time = ngx_current_msec;

#if (NGX_STAT_STUB)
        (void) ngx_atomic_fetch_add(ngx_stat_handled, 1);
#endif

        if (ls->addr_ntop) {
            c->addr_text.data = ngx_pnalloc(c->pool, ls->addr_text_max_len);
            if (c->addr_text.data == NULL) {
                ngx_close_accepted_connection(c);
                return;
            }

            c->addr_text.len = ngx_sock_ntop(c->sockaddr, c->socklen,
                                             c->addr_text.data,
                                             ls->addr_text_max_len, 0);
            if (c->addr_text.len == 0) {
                ngx_close_accepted_connection(c);
                return;
            }
        }

#if (NGX_DEBUG)
        {
        ngx_str_t  addr;
        u_char     text[NGX_SOCKADDR_STRLEN];

        ngx_debug_accepted_connection(ecf, c);

        if (log->log_level & NGX_LOG_DEBUG_EVENT) {
            addr.data = text;
            addr.len = ngx_sock_ntop(c->sockaddr, c->socklen, text,
                                     NGX_SOCKADDR_STRLEN, 1);

            ngx_log_debug3(NGX_LOG_DEBUG_EVENT, log, 0,
                           "*%uA accept: %V fd:%d", c->number, &addr, s);
        }

        }
#endif

        if (ngx_add_conn && (ngx_event_flags & NGX_USE_EPOLL_EVENT) == 0) {
            if (ngx_add_conn(c) == NGX_ERROR) {
                ngx_close_accepted_connection(c);
                return;
            }
        }

        log->data = NULL;
        log->handler = NULL;

        ls->handler(c);

        if (ngx_event_flags & NGX_USE_KQUEUE_EVENT) {
            ev->available--;
        }

    } while (ev->available);

#if (NGX_HAVE_EPOLLEXCLUSIVE)
    ngx_reorder_accept_events(ls);
#endif
}

/*
 * 什么时候需要抢锁 这个一个方案设计
 * 目的 防止多进程下accept惊群
 * cycle中listening存放一份监听端口 共享给所有worker进程 这种情况需要引入accept锁
 * 每个worker进程启动后不要第一时间注册连接事件
 * 而是等到进入事件循环 每一轮事件循环开始都
 */
ngx_int_t
ngx_trylock_accept_mutex(ngx_cycle_t *cycle)
{
    if (ngx_shmtx_trylock(&ngx_accept_mutex)) {

        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                       "accept mutex locked");

        if (ngx_accept_mutex_held && ngx_accept_events == 0) {
            return NGX_OK;
        }
        /*
         * 抢锁成功的进程才有资格监听连接 找cycle中listening列表中没有监听的连接全部注册到自己的事件循环器上
         */
        if (ngx_enable_accept_events(cycle) == NGX_ERROR) {
            ngx_shmtx_unlock(&ngx_accept_mutex);
            return NGX_ERROR;
        }

        ngx_accept_events = 0;
		// 标识抢到了锁
        ngx_accept_mutex_held = 1;

        return NGX_OK;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                   "accept mutex lock failed: %ui", ngx_accept_mutex_held);

    if (ngx_accept_mutex_held) {
        /*
         * worker抢锁失败 要把自己注册过的监听的连接事件移除掉
         * 这个地方移除连接事件监听会不会导致连接丢失
         * <ul>
         *   <li></li>
         * </ul>
         */
        if (ngx_disable_accept_events(cycle, 0) == NGX_ERROR) {
            return NGX_ERROR;
        }
		// 当前进程抢锁失败 放弃锁占用
        ngx_accept_mutex_held = 0;
    }

    return NGX_OK;
}

/*
 * master-worker多进程下抢到锁的worker进程才会执行到这
 * 到cycle的listening中拿没有被监听的socket注册到worker自己的事件循环器上
 */
ngx_int_t
ngx_enable_accept_events(ngx_cycle_t *cycle)
{
    ngx_uint_t         i;
    ngx_listening_t   *ls;
    ngx_connection_t  *c;

    ls = cycle->listening.elts;
    for (i = 0; i < cycle->listening.nelts; i++) {

        c = ls[i].connection;

        if (c == NULL || c->read->active) {
            // 找到候选监听端口没被注册的
            continue;
        }
		// 注册连接事件
        if (ngx_add_event(c->read, NGX_READ_EVENT, 0) == NGX_ERROR) {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}

/*
 * 从内核多路复用器移除事件的监听
 * <ul>
 *   <li>复用器红黑树上有这个事件才会发生移除<ul>
 *     为什么说明移除事件对真正的连接请求不会有影响 无非就是连接请求的接收比原来有一些滞后 但是这一点时间已经多发生的内核复用器系统调用对于accept惊群 都是小事
 *     <li>虽然注册了连接事件 但是一直没有连接请求进来 那么直接移除 将来别的worker注册连接事件的监听 这是最简单的场景 肯定不会有问题</li>
 *     <li>当前worker注册了连接事件 有连接请求进来 被accept过了 等于是已经发生过的事情 再把连接事件删除 将来别的worker进程注册 这也不会有问题</li>
 *     <li>当前worker注册了连接事件 有连接请求进来 自己还没处理 等于现在情况是连接已经进来在backlog中 事件也被内核放到复用器的ready list上 此时系统调用从复用器红黑树上移除事件 内核会从红黑树和ready list上都移除这个事件 因为实际连接已经到了backlog中 将来别的worker进程注册连接事件后 内核会立马把这个事件放到它的ready list中等着那个worker进程处理</li>
 *     <li></li>
 *   </ul></li>
 *   <li>复用器红黑树没有注册过这个事件 就等于是一次空调用 什么也不会发生</li>
 * </ul>
 */
static ngx_int_t
ngx_disable_accept_events(ngx_cycle_t *cycle, ngx_uint_t all)
{
    ngx_uint_t         i;
    ngx_listening_t   *ls;
    ngx_connection_t  *c;

    ls = cycle->listening.elts;
    for (i = 0; i < cycle->listening.nelts; i++) {

        c = ls[i].connection;

        if (c == NULL || !c->read->active) {
            /*
             * 还没被注册过监听的不用管 假设候选端口有8个 其中有5个是被注册过的 那么当前进程注册监听的一定在这5个之中
             * 所以不用讨论具体情况 直接把这5个都从自己的内核上删除
             * <ul>
             *   <li>自己内核注册过 自然会被移除</li>
             *   <li>自己内核没有注册过 什么也不会发生</li>
             * </ul>
             */
            continue;
        }

#if (NGX_HAVE_REUSEPORT)

        /*
         * do not disable accept on worker's own sockets
         * when disabling accept events due to accept mutex
         */

        if (ls[i].reuseport && !all) {
            continue;
        }

#endif
        /*
         * 从内核多路复用器移除事件的监听
         * <ul>
         *   <li>复用器红黑树上有这个事件才会发生移除<ul>
         *     为什么说明移除事件对真正的连接请求不会有影响 无非就是连接请求的接收比原来有一些滞后 但是这一点时间已经多发生的内核复用器系统调用对于accept惊群 都是小事
         *     <li>虽然注册了连接事件 但是一直没有连接请求进来 那么直接移除 将来别的worker注册连接事件的监听 这是最简单的场景 肯定不会有问题</li>
         *     <li>当前worker注册了连接事件 有连接请求进来 被accept过了 等于是已经发生过的事情 再把连接事件删除 将来别的worker进程注册 这也不会有问题</li>
         *     <li>当前worker注册了连接事件 有连接请求进来 自己还没处理 等于现在情况是连接已经进来在backlog中 事件也被内核放到复用器的ready list上 此时系统调用从复用器红黑树上移除事件 内核会从红黑树和ready list上都移除这个事件 因为实际连接已经到了backlog中 将来别的worker进程注册连接事件后 内核会立马把这个事件放到它的ready list中等着那个worker进程处理</li>
         *   </ul></li>
         *   <li>复用器红黑树没有注册过这个事件 就等于是一次空调用 什么也不会发生</li>
         * </ul>
         */
        if (ngx_del_event(c->read, NGX_READ_EVENT, NGX_DISABLE_EVENT)
            == NGX_ERROR)
        {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}


#if (NGX_HAVE_EPOLLEXCLUSIVE)

static void
ngx_reorder_accept_events(ngx_listening_t *ls)
{
    ngx_connection_t  *c;

    /*
     * Linux with EPOLLEXCLUSIVE usually notifies only the process which
     * was first to add the listening socket to the epoll instance.  As
     * a result most of the connections are handled by the first worker
     * process.  To fix this, we re-add the socket periodically, so other
     * workers will get a chance to accept connections.
     */

    if (!ngx_use_exclusive_accept) {
        return;
    }

#if (NGX_HAVE_REUSEPORT)

    if (ls->reuseport) {
        return;
    }

#endif

    c = ls->connection;

    if (c->requests++ % 16 != 0
        && ngx_accept_disabled <= 0)
    {
        return;
    }

    if (ngx_del_event(c->read, NGX_READ_EVENT, NGX_DISABLE_EVENT)
        == NGX_ERROR)
    {
        return;
    }

    if (ngx_add_event(c->read, NGX_READ_EVENT, NGX_EXCLUSIVE_EVENT)
        == NGX_ERROR)
    {
        return;
    }
}

#endif


static void
ngx_close_accepted_connection(ngx_connection_t *c)
{
    ngx_socket_t  fd;

    ngx_free_connection(c);

    fd = c->fd;
    c->fd = (ngx_socket_t) -1;

    if (ngx_close_socket(fd) == -1) {
        ngx_log_error(NGX_LOG_ALERT, c->log, ngx_socket_errno,
                      ngx_close_socket_n " failed");
    }

    if (c->pool) {
        ngx_destroy_pool(c->pool);
    }

#if (NGX_STAT_STUB)
    (void) ngx_atomic_fetch_add(ngx_stat_active, -1);
#endif
}


u_char *
ngx_accept_log_error(ngx_log_t *log, u_char *buf, size_t len)
{
    return ngx_snprintf(buf, len, " while accepting new connection on %V",
                        log->data);
}


#if (NGX_DEBUG)

void
ngx_debug_accepted_connection(ngx_event_conf_t *ecf, ngx_connection_t *c)
{
    struct sockaddr_in   *sin;
    ngx_cidr_t           *cidr;
    ngx_uint_t            i;
#if (NGX_HAVE_INET6)
    struct sockaddr_in6  *sin6;
    ngx_uint_t            n;
#endif

    cidr = ecf->debug_connection.elts;
    for (i = 0; i < ecf->debug_connection.nelts; i++) {
        if (cidr[i].family != (ngx_uint_t) c->sockaddr->sa_family) {
            goto next;
        }

        switch (cidr[i].family) {

#if (NGX_HAVE_INET6)
        case AF_INET6:
            sin6 = (struct sockaddr_in6 *) c->sockaddr;
            for (n = 0; n < 16; n++) {
                if ((sin6->sin6_addr.s6_addr[n]
                    & cidr[i].u.in6.mask.s6_addr[n])
                    != cidr[i].u.in6.addr.s6_addr[n])
                {
                    goto next;
                }
            }
            break;
#endif

#if (NGX_HAVE_UNIX_DOMAIN)
        case AF_UNIX:
            break;
#endif

        default: /* AF_INET */
            sin = (struct sockaddr_in *) c->sockaddr;
            if ((sin->sin_addr.s_addr & cidr[i].u.in.mask)
                != cidr[i].u.in.addr)
            {
                goto next;
            }
            break;
        }

        c->log->log_level = NGX_LOG_DEBUG_CONNECTION|NGX_LOG_DEBUG_ALL;
        break;

    next:
        continue;
    }
}

#endif
