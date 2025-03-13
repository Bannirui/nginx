
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
     *   <li>键值对从桶顶依次往下放</li>
     *   <li>桶底放个指针 是NULL 作为桶与桶之间的分隔符</li>
     * </ul>
     * 为什么要这么设计呢
     * 可以向其他hash表的实现一个在hash桶中用链表组织键值对 但是链表形式太耗内存了
     * 所有的组织形式中数组是最简单高效的
     * 因为hash表的键值对空间是一次性分配的 也就是hash桶是连在一起的 因此只要在hash桶之间有明确分割 顺着桶顶往下遍历 遇到分割符停下就行 就完成了一次hash桶键值对的扫描
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


/*
 * 函数指针 用于计算关键字data的hash值
 * @param data 键
 * @param len 键的长度
 */
typedef ngx_uint_t (*ngx_hash_key_pt) (u_char *data, size_t len);


typedef struct {
    ngx_hash_t            hash;
    ngx_hash_wildcard_t  *wc_head;
    ngx_hash_wildcard_t  *wc_tail;
} ngx_hash_combined_t;


typedef struct {
    // hash表
    ngx_hash_t       *hash;
    // 计算键hash值的函数指针
    ngx_hash_key_pt   key;
    // hash表最多bucket数量
    ngx_uint_t        max_size;
    /*
     * hash桶大小上限
     * 这个桶的大小是在实例化时候调用方指定的
     * 由2个部分组成
     * <ul>
     *   <li>真正的键值对数据 键值对从桶顶开始往下放</li>
     *   <li>指针 这个指针放NULL 紧随桶里面最后一个键值对 放在桶底</li>
     * </ul>
     * 因为所有hash桶的内存是一次性申请的整片内存 那么怎么区分桶与桶呢 就靠的这个NULL标识两个桶的分隔符
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
