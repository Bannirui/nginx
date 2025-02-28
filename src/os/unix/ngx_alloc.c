
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>


ngx_uint_t  ngx_pagesize;
ngx_uint_t  ngx_pagesize_shift;
ngx_uint_t  ngx_cacheline_size;


/**
 * 对malloc的封装
 * @param size 申请的内存大小
 */
void *
ngx_alloc(size_t size, ngx_log_t *log)
{
    void  *p;

    p = malloc(size);
    if (p == NULL) {
        ngx_log_error(NGX_LOG_EMERG, log, ngx_errno,
                      "malloc(%uz) failed", size);
    }

    ngx_log_debug2(NGX_LOG_DEBUG_ALLOC, log, 0, "malloc: %p:%uz", p, size);

    return p;
}


void *
ngx_calloc(size_t size, ngx_log_t *log)
{
    void  *p;

    p = ngx_alloc(size, log);

    if (p) {
        ngx_memzero(p, size);
    }

    return p;
}


#if (NGX_HAVE_POSIX_MEMALIGN)

/**
 * 内存分配
 * 为什么不用alloc系列
 * <ul>
 *   <li>alloc等函数自动处理了内存对齐 32位系统对齐边界8Byte 64位系统对齐边界16Byte 但是alloc申请内存有限制 上限4KB</li>
 *   <li>需要动态对齐边界</li>
 * </ul>
 * 内存对齐的含义是 分配好的内存空间起始地址是alignment的倍数
 * 内存边界对齐的意义
 * <ul>
 *  <li>提高处理器访问内存的性能</li>
 *  <li>一些处理器的硬件要求</li>
 *  <li>一些库或框架的硬性要求</li>
 *  <li>避免内存访问错误</li>
 * </ul>
 * @param alignment 内存对齐边界 alignment是2的幂次方
 * @param size 申请的内存大小
 * @param log 内存分配失败打印日志
 * @return 分配成功的内存地址
 */
void *
ngx_memalign(size_t alignment, size_t size, ngx_log_t *log)
{
    void  *p;
    int    err;

    /*
     * malloc calloc realloc等内存分配函数都处理了内存对齐问题 并且这些系统函数分配的内存大小有限制 上限4KB
     * 32位系统以8字节为边界对齐
     * 64位系统以16字节为边界对齐
     * 更大边界的话就需要动态处理
     * 系统调用分配内存 动态指定需要的对齐边界
     * @param p 分配成功的内存地址
     * @param alignment 对齐边界是几字节 必须是2的幂次方 分配成功的内存起始地址是alignment的倍数
     * @param size 向系统申请的内存空间大小
     */
    err = posix_memalign(&p, alignment, size);

    if (err) {
        ngx_log_error(NGX_LOG_EMERG, log, err,
                      "posix_memalign(%uz, %uz) failed", alignment, size);
        p = NULL;
    }

    ngx_log_debug3(NGX_LOG_DEBUG_ALLOC, log, 0,
                   "posix_memalign: %p:%uz @%uz", p, size, alignment);

    return p;
}

#elif (NGX_HAVE_MEMALIGN)

void *
ngx_memalign(size_t alignment, size_t size, ngx_log_t *log)
{
    void  *p;

    p = memalign(alignment, size);
    if (p == NULL) {
        ngx_log_error(NGX_LOG_EMERG, log, ngx_errno,
                      "memalign(%uz, %uz) failed", alignment, size);
    }

    ngx_log_debug3(NGX_LOG_DEBUG_ALLOC, log, 0,
                   "memalign: %p:%uz @%uz", p, size, alignment);

    return p;
}

#endif
