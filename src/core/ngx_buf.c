
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>


/*
 * 实例化一个缓冲区
 * @param size buf的大小
 */
ngx_buf_t *
ngx_create_temp_buf(ngx_pool_t *pool, size_t size)
{
    ngx_buf_t *b;

    // 从内存池申请buf结构的内存
    b = ngx_calloc_buf(pool);
    if (b == NULL) {
        return NULL;
    }
    // 分配缓冲区内存 记录缓冲区的起始位置
    b->start = ngx_palloc(pool, size);
    if (b->start == NULL) {
        return NULL;
    }
    // 缓冲区刚创建好 还没填充数据 数据是空的
    b->pos = b->start;
    b->last = b->start;
    // 缓冲区结束位置
    b->end = b->last + size;
    // 标识缓冲区的数据是可以修改的
    b->temporary = 1;

    return b;
}


/*
 * 分配缓冲区链表节点的结构
 * <ul>
 *   <li>内存池空闲的就直接分配</li>
 *   <li>没有空闲的就新创建</li>
 * </ul>
 */
ngx_chain_t *
ngx_alloc_chain_link(ngx_pool_t *pool)
{
    ngx_chain_t  *cl;
    // 内存池空闲缓冲区链表有空闲的缓冲区
    cl = pool->chain;

    if (cl) {
        /*
         * 内存池有空闲缓冲区可以用就直接分配空闲缓冲区 避免向系统申请内存 提升性能
         * 更新标识 指向新的空闲缓冲区
         */
        pool->chain = cl->next;
        return cl;
    }
    // 内存池没有空闲缓冲区 创建新的缓冲区结构
    cl = ngx_palloc(pool, sizeof(ngx_chain_t));
    if (cl == NULL) {
        return NULL;
    }

    return cl;
}


/*
 * 从内存池分配多个缓冲区并挂成链表
 * @param pool 内存池
 * @param bufs 需要分配多少个缓冲区 每个缓冲区多大
 * @return 缓冲区挂成链表
 */
ngx_chain_t *
ngx_create_chain_of_bufs(ngx_pool_t *pool, ngx_bufs_t *bufs)
{
    u_char       *p;
    ngx_int_t     i;
    ngx_buf_t    *b;
    ngx_chain_t  *chain, *cl, **ll;
    // 所有缓冲区需要所有内存一次性分配
    p = ngx_palloc(pool, bufs->num * bufs->size);
    if (p == NULL) {
        return NULL;
    }

    ll = &chain;

    for (i = 0; i < bufs->num; i++) {
        // 缓冲区结构
        b = ngx_calloc_buf(pool);
        if (b == NULL) {
            return NULL;
        }
        // 初始化缓冲区
        // 缓冲区中数据是空的
        b->pos = p;
        b->last = p;
        // 标识缓冲区数据可修改
        b->temporary = 1;
        // 缓冲区起始位置
        b->start = p;
        // 一大片内存中抠掉分配完的缓冲区 剩下来继续分配给其他缓冲区
        p += bufs->size;
        // 缓冲区结束位置
        b->end = p;
        // 缓冲区链表节点
        cl = ngx_alloc_chain_link(pool);
        if (cl == NULL) {
            return NULL;
        }
        // 初始化缓冲区链表的节点
        cl->buf = b;
        *ll = cl;
        ll = &cl->next;
    }

    *ll = NULL;

    return chain;
}

/*
 * 缓冲区链表上的缓冲区引用复制到另一条缓冲区链表
 * @param chain 要复制到哪儿
 * @param in 要复制谁
 */
ngx_int_t
ngx_chain_add_copy(ngx_pool_t *pool, ngx_chain_t **chain, ngx_chain_t *in)
{
    ngx_chain_t  *cl, **ll;

    ll = chain;

    for (cl = *chain; cl; cl = cl->next) {
        ll = &cl->next;
    }
    // 遍历链表 逐个复制缓冲引用到新链表上
    while (in) {
        // 链表节点结构
        cl = ngx_alloc_chain_link(pool);
        if (cl == NULL) {
            *ll = NULL;
            return NGX_ERROR;
        }

        cl->buf = in->buf;
        *ll = cl;
        ll = &cl->next;
        in = in->next;
    }

    *ll = NULL;

    return NGX_OK;
}


/*
 * 从内存池分配个缓冲区
 * <ul>
 *   <li>优先从内存池空闲缓冲区找</li>
 *   <li>没有现成的空闲缓冲区就从内存当分配一个新的</li>
 * </ul>
 * @param p 内存池
 * @return 缓冲区
 */
ngx_chain_t *
ngx_chain_get_free_buf(ngx_pool_t *p, ngx_chain_t **free)
{
    ngx_chain_t  *cl;

    if (*free) {
        cl = *free;
        *free = cl->next;
        cl->next = NULL;
        return cl;
    }
    // 分配缓冲区链表节点结构
    cl = ngx_alloc_chain_link(p);
    if (cl == NULL) {
        return NULL;
    }
    // 分配缓冲区结构
    cl->buf = ngx_calloc_buf(p);
    if (cl->buf == NULL) {
        return NULL;
    }

    cl->next = NULL;

    return cl;
}


/*
 * 归类待处理缓冲区 要么被处理成正在使用缓冲区要么被处理成空闲缓冲区
 * 空闲缓冲区统一管理在内存池的空闲缓冲区链表上
 * @param p
 * @param free 空闲缓冲区
 * @param busy 正在使用的缓冲区
 * @param out 待处理的缓冲区
 * @param tag
 */
void
ngx_chain_update_chains(ngx_pool_t *p, ngx_chain_t **free, ngx_chain_t **busy,
    ngx_chain_t **out, ngx_buf_tag_t tag)
{
    ngx_chain_t  *cl;
    // 遍历待处理缓冲区 把所有待处理缓冲区先整成正在使用的缓冲区
    if (*out) {
        if (*busy == NULL) {
            *busy = *out;

        } else {
            for (cl = *busy; cl->next; cl = cl->next) { /* void */ }

            cl->next = *out;
        }

        *out = NULL;
    }
    /*
     * 遍历正在使用缓冲区
     */
    while (*busy) {
        cl = *busy;

        if (cl->buf->tag != tag) {
            *busy = cl->next;
            // 缓冲区放到内存池空闲链表供复用
            ngx_free_chain(p, cl);
            continue;
        }
        // 缓冲区中有数据
        if (ngx_buf_size(cl->buf) != 0) {
            break;
        }
        // 缓冲区中没数据 重置缓冲区状态到初始状态
        cl->buf->pos = cl->buf->start;
        cl->buf->last = cl->buf->start;

        *busy = cl->next;
        // 当前缓冲区被回收了 头插到空闲链表上
        cl->next = *free;
        *free = cl;
    }
}


off_t
ngx_chain_coalesce_file(ngx_chain_t **in, off_t limit)
{
    off_t         total, size, aligned, fprev;
    ngx_fd_t      fd;
    ngx_chain_t  *cl;

    total = 0;

    cl = *in;
    fd = cl->buf->file->fd;

    do {
        size = cl->buf->file_last - cl->buf->file_pos;

        if (size > limit - total) {
            size = limit - total;

            aligned = (cl->buf->file_pos + size + ngx_pagesize - 1)
                       & ~((off_t) ngx_pagesize - 1);

            if (aligned <= cl->buf->file_last) {
                size = aligned - cl->buf->file_pos;
            }

            total += size;
            break;
        }

        total += size;
        fprev = cl->buf->file_pos + size;
        cl = cl->next;

    } while (cl
             && cl->buf->in_file
             && total < limit
             && fd == cl->buf->file->fd
             && fprev == cl->buf->file_pos);

    *in = cl;

    return total;
}


ngx_chain_t *
ngx_chain_update_sent(ngx_chain_t *in, off_t sent)
{
    off_t  size;

    for ( /* void */ ; in; in = in->next) {

        if (ngx_buf_special(in->buf)) {
            continue;
        }

        if (sent == 0) {
            break;
        }

        size = ngx_buf_size(in->buf);

        if (sent >= size) {
            sent -= size;

            if (ngx_buf_in_memory(in->buf)) {
                in->buf->pos = in->buf->last;
            }

            if (in->buf->in_file) {
                in->buf->file_pos = in->buf->file_last;
            }

            continue;
        }

        if (ngx_buf_in_memory(in->buf)) {
            in->buf->pos += (size_t) sent;
        }

        if (in->buf->in_file) {
            in->buf->file_pos += sent;
        }

        break;
    }

    return in;
}
