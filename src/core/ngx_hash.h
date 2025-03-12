
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_HASH_H_INCLUDED_
#define _NGX_HASH_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>

/*
 * 键值对
 */
typedef struct {
    // 值
    void             *value;
    // 键的长度
    u_short           len;
    /**
     * 键
     * 柔性数组 占位用 1个byte
     * 结构体本身这个成员只占用固定大小 在实际使用的时候分配更多内存来存放变长的name
     */
    u_char            name[1];
} ngx_hash_elt_t;

/*
 * 静态hash表
 * 初始化好了就不扩容了
 */
typedef struct {
    /*
     * hash桶数组
     * 所谓hash桶 就是hash表中一个数组 数组中每个元素就是一条键值对链表 存放链表的数组元素空间就是hash桶
     * hash桶里面里面放了两部分东西
     * <ul>
     *   <li>桶首元素 就是桶的最顶上放了个指针 这个指针存的是桶顶的内存地址</li>
     *   <li>其余才是真正的键值对</li>
     * </ul>
     * 为什么要这么设计呢
     * 可以向其他hash表的实现一个在hash桶中用链表组织键值对 但是链表形式太耗内存了
     * 所有的组织形式中数组是最简单高效的
     * 因此要记录第一个键值对的位置 再知晓有多少个键值对 就可以快速访问数组上的键值对元素
     */
    ngx_hash_elt_t  **buckets;
    // hash表中hash桶个数 就是数组长度 为了加快计算 规定size是2的幂次方 将来知道了key的hash值就可以位运算定位hash桶=hash&(size-1)
    ngx_uint_t        size;
} ngx_hash_t;


typedef struct {
    ngx_hash_t        hash;
    void             *value;
} ngx_hash_wildcard_t;

// 键值对
typedef struct {
    // 键
    ngx_str_t         key;
    // 键的hash值
    ngx_uint_t        key_hash;
    // 值
    void             *value;
} ngx_hash_key_t;


typedef ngx_uint_t (*ngx_hash_key_pt) (u_char *data, size_t len);


typedef struct {
    ngx_hash_t            hash;
    ngx_hash_wildcard_t  *wc_head;
    ngx_hash_wildcard_t  *wc_tail;
} ngx_hash_combined_t;


typedef struct {
    ngx_hash_t       *hash;
    ngx_hash_key_pt   key;
    // hash表最多bucket数量
    ngx_uint_t        max_size;
    /*
     * hash桶大小
     * 这个桶的大小是在实例化时候调用方指定的
     * 由2个部分组成
     * <ul>
     *   <li>指针 这个指针的用处是什么呢 指向桶的桶顶 读到这个指针就可以知道桶有内存地址从什么地方开始</li>
     *   <li>真正的键值对数据</li>
     * </ul>
     */
    ngx_uint_t        bucket_size;

    char             *name;
    ngx_pool_t       *pool;
    ngx_pool_t       *temp_pool;
} ngx_hash_init_t;


#define NGX_HASH_SMALL            1
#define NGX_HASH_LARGE            2

#define NGX_HASH_LARGE_ASIZE      16384
#define NGX_HASH_LARGE_HSIZE      10007

#define NGX_HASH_WILDCARD_KEY     1
#define NGX_HASH_READONLY_KEY     2


typedef struct {
    ngx_uint_t        hsize;

    ngx_pool_t       *pool;
    ngx_pool_t       *temp_pool;

    ngx_array_t       keys;
    ngx_array_t      *keys_hash;

    ngx_array_t       dns_wc_head;
    ngx_array_t      *dns_wc_head_hash;

    ngx_array_t       dns_wc_tail;
    ngx_array_t      *dns_wc_tail_hash;
} ngx_hash_keys_arrays_t;


typedef struct ngx_table_elt_s  ngx_table_elt_t;

struct ngx_table_elt_s {
    ngx_uint_t        hash;
    ngx_str_t         key;
    ngx_str_t         value;
    u_char           *lowcase_key;
    ngx_table_elt_t  *next;
};


void *ngx_hash_find(ngx_hash_t *hash, ngx_uint_t key, u_char *name, size_t len);
void *ngx_hash_find_wc_head(ngx_hash_wildcard_t *hwc, u_char *name, size_t len);
void *ngx_hash_find_wc_tail(ngx_hash_wildcard_t *hwc, u_char *name, size_t len);
void *ngx_hash_find_combined(ngx_hash_combined_t *hash, ngx_uint_t key,
    u_char *name, size_t len);

ngx_int_t ngx_hash_init(ngx_hash_init_t *hinit, ngx_hash_key_t *names,
    ngx_uint_t nelts);
ngx_int_t ngx_hash_wildcard_init(ngx_hash_init_t *hinit, ngx_hash_key_t *names,
    ngx_uint_t nelts);

#define ngx_hash(key, c)   ((ngx_uint_t) key * 31 + c)
ngx_uint_t ngx_hash_key(u_char *data, size_t len);
ngx_uint_t ngx_hash_key_lc(u_char *data, size_t len);
ngx_uint_t ngx_hash_strlow(u_char *dst, u_char *src, size_t n);


ngx_int_t ngx_hash_keys_array_init(ngx_hash_keys_arrays_t *ha, ngx_uint_t type);
ngx_int_t ngx_hash_add_key(ngx_hash_keys_arrays_t *ha, ngx_str_t *key,
    void *value, ngx_uint_t flags);


#endif /* _NGX_HASH_H_INCLUDED_ */
