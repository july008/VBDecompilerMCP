/*
 * khash.h - 轻量字符串哈希表（i18n 翻译表使用）
 *
 * 为什么自带而不是引入完整 khash：
 *   khash 上游是单头文件库（MIT），但完整版依赖大量宏（__ac_HASHMAP /
 *   KHASH_MAP_INIT_STR / kh_put / kh_get ...）与 C++ 不完全兼容的内部实现。
 *   本项目只需要"字符串键 → 指针值"这一个场景，因此这里提供一个
 *   **API 兼容子集**的紧凑实现：
 *
 *     khash_t / khint_t / kh_init_str / kh_put / kh_get / kh_destroy
 *     kh_size / kh_foreach / kh_str_hash_func / kh_str_hash_equal
 *
 *   语义与上游 khash 一致：
 *     - kh_put(h, key, val, &ret)：ret = 0 新建空槽 / 1 新增键 / 2 键已存在(值被覆盖)
 *     - 哈希表**不拥有** key/value 指针的内存（由调用方 strdup 并在销毁时 free）
 *     - 非线程安全：i18n 只在 UI 主线程访问
 *
 * 算法：FNV-1a + 开放寻址（线性探测）+ 负载因子 0.75 自动扩容。
 */
#ifndef VBMCP_KHASH_H
#define VBMCP_KHASH_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t khint_t;

/* 不透明句柄：调用方只通过下面的函数访问 */
typedef struct khash_t khash_t;

typedef khint_t (*kh_hash_fn)(const char *key);
typedef int      (*kh_equal_fn)(const char *a, const char *b);
/* 遍历回调：key / value / 用户上下文 */
typedef void     (*kh_foreach_fn)(const char *key, void *val, void *user);

/* ---------------- 哈希函数 ---------------- */

/* FNV-1a 32bit */
static khint_t kh_str_hash_func(const char *s)
{
    khint_t h = 2166136261u;
    if (!s) return 0;
    for (; *s; ++s) {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    return h ? h : 1u;   /* 0 保留给"空槽" */
}

static int kh_str_hash_equal(const char *a, const char *b)
{
    if (a == b) return 1;
    if (!a || !b) return 0;
    return strcmp(a, b) == 0;
}

/* ---------------- 内部桶 ---------------- */

typedef struct {
    khint_t hash;      /* 0 = 空槽 */
    const char *key;
    void *val;
} kh_bucket_t;

struct khash_t {
    kh_bucket_t *buckets;
    khint_t      n_buckets;   /* 总是 2 的幂 */
    khint_t      size;        /* 已用槽数 */
};

/* ---------------- 生命周期 ---------------- */

static khash_t *kh_init_str(kh_hash_fn hash_fn, kh_equal_fn equal_fn)
{
    (void)hash_fn; (void)equal_fn;   /* 当前固定为字符串语义，参数为兼容预留 */
    khash_t *h = (khash_t *)calloc(1, sizeof(khash_t));
    if (!h) return NULL;
    h->n_buckets = 64;
    h->buckets = (kh_bucket_t *)calloc(h->n_buckets, sizeof(kh_bucket_t));
    if (!h->buckets) { free(h); return NULL; }
    return h;
}

/* 只释放桶数组：不释放 key / value（所有权在调用方） */
static void kh_destroy(khash_t *h)
{
    if (!h) return;
    free(h->buckets);
    free(h);
}

static khint_t kh_size(const khash_t *h)
{
    return h ? h->size : 0;
}

/* 负载因子 > 0.75 时翻倍扩容 */
static int kh_resize(khash_t *h, khint_t new_n)
{
    kh_bucket_t *old = h->buckets;
    khint_t old_n = h->n_buckets;

    kh_bucket_t *nb = (kh_bucket_t *)calloc(new_n, sizeof(kh_bucket_t));
    if (!nb) return 0;                    /* 扩容失败：保持原状，仍可继续插入 */

    h->buckets = nb;
    h->n_buckets = new_n;
    h->size = 0;

    for (khint_t i = 0; i < old_n; ++i) {
        if (!old[i].hash) continue;
        khint_t j = old[i].hash & (new_n - 1);
        while (nb[j].hash) j = (j + 1) & (new_n - 1);
        nb[j] = old[i];
        h->size++;
    }
    free(old);
    return 1;
}

/* ---------------- 增删查 ---------------- */

/*
 * 插入 / 更新。ret（可选）：
 *   0 = 新建空槽（此前无此键）
 *   1 = 新增键
 *   2 = 键已存在（val 被覆盖，返回值仍为 2）
 * key 与 val 的内存所有权归调用方（通常 strdup），需在销毁前自行 free。
 */
static int kh_put(khash_t *h, const char *key, void *val, int *ret)
{
    int status = 0;
    if (ret) *ret = 0;
    if (!h || !key) return 0;

    if ((h->size + 1) * 4 >= h->n_buckets * 3) {
        kh_resize(h, h->n_buckets * 2);    /* 失败也继续（原表仍可用） */
    }

    khint_t hv = kh_str_hash_func(key);
    khint_t mask = h->n_buckets - 1;
    khint_t i = hv & mask;

    while (h->buckets[i].hash) {
        if (h->buckets[i].hash == hv && kh_str_hash_equal(h->buckets[i].key, key)) {
            h->buckets[i].val = val;
            if (ret) *ret = 2;
            return 1;
        }
        i = (i + 1) & mask;
    }

    h->buckets[i].hash = hv;
    h->buckets[i].key  = key;
    h->buckets[i].val  = val;
    h->size++;
    if (ret) *ret = 1;
    return 1;
}

/* 查找：命中返回 val（可以是 NULL），未命中返回 NULL。
 * 需要区分"存在但值为 NULL"时，请配合 kh_size / 遍历使用。 */
static void *kh_get(khash_t *h, const char *key)
{
    if (!h || !key) return NULL;
    khint_t hv = kh_str_hash_func(key);
    khint_t mask = h->n_buckets - 1;
    khint_t i = hv & mask;
    while (h->buckets[i].hash) {
        if (h->buckets[i].hash == hv && kh_str_hash_equal(h->buckets[i].key, key)) {
            return h->buckets[i].val;
        }
        i = (i + 1) & mask;
    }
    return NULL;
}


/* 遍历所有键值对（顺序不保证） */
static void kh_foreach(khash_t *h, kh_foreach_fn fn, void *user)
{
    if (!h || !fn) return;
    for (khint_t i = 0; i < h->n_buckets; ++i) {
        if (h->buckets[i].hash) fn(h->buckets[i].key, h->buckets[i].val, user);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* VBMCP_KHASH_H */