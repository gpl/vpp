/*
  Copyright (c) 2014 Cisco and/or its affiliates.

  * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/

/** @cond DOCUMENTATION_IS_IN_BIHASH_DOC_H */

/*
 * Note: to instantiate the template multiple times in a single file,
 * #undef __included_bihash_template_h__...
 */
#ifndef __included_bihash_template_h__
#define __included_bihash_template_h__

#include <vppinfra/heap.h>
#include <vppinfra/format.h>
#include <vppinfra/pool.h>
#include <vppinfra/cache.h>

#ifndef BIHASH_TYPE
#error BIHASH_TYPE not defined
#endif

#define _bv(a,b) a##b
#define __bv(a,b) _bv(a,b)
#define BV(a) __bv(a,BIHASH_TYPE)

#define _bvt(a,b) a##b##_t
#define __bvt(a,b) _bvt(a,b)
#define BVT(a) __bvt(a,BIHASH_TYPE)

typedef struct BV (clib_bihash_value)
{
  union
  {
    BVT (clib_bihash_kv) kvp[BIHASH_KVP_PER_PAGE];
    struct BV (clib_bihash_value) * next_free;
  };
} BVT (clib_bihash_value);

#if BIHASH_KVP_CACHE_SIZE > 5
#error Requested KVP cache LRU data exceeds 16 bits
#endif

typedef struct
{
  union
  {
    struct
    {
      u32 offset;
      u8 linear_search;
      u8 log2_pages;
      i16 refcnt;
    };
    u64 as_u64;
  };
#if BIHASH_KVP_CACHE_SIZE > 0
  u16 cache_lru;
    BVT (clib_bihash_kv) cache[BIHASH_KVP_CACHE_SIZE];
#endif
} BVT (clib_bihash_bucket);

typedef struct
{
  BVT (clib_bihash_value) * values;
  BVT (clib_bihash_bucket) * buckets;
  volatile u32 *writer_lock;

    BVT (clib_bihash_value) ** working_copies;
  int *working_copy_lengths;
    BVT (clib_bihash_bucket) saved_bucket;

  u32 nbuckets;
  u32 log2_nbuckets;
  u8 *name;

  u64 cache_hits;
  u64 cache_misses;

    BVT (clib_bihash_value) ** freelists;

  /*
   * Backing store allocation. Since bihash manages its own
   * freelists, we simple dole out memory at alloc_arena_next.
   */
  uword alloc_arena;
  uword alloc_arena_next;
  uword alloc_arena_size;

  /**
    * A custom format function to print the Key and Value of bihash_key instead of default hexdump
    */
  format_function_t *fmt_fn;

} BVT (clib_bihash);


static inline void
BV (clib_bihash_update_lru) (BVT (clib_bihash_bucket) * b, u8 slot)
{
#if BIHASH_KVP_CACHE_SIZE > 1
  u16 value, tmp, mask;
  u8 found_lru_pos;
  u16 save_hi;

  ASSERT (slot < BIHASH_KVP_CACHE_SIZE);

  /* First, find the slot in cache_lru */
  mask = slot;
  if (BIHASH_KVP_CACHE_SIZE > 1)
    mask |= slot << 3;
  if (BIHASH_KVP_CACHE_SIZE > 2)
    mask |= slot << 6;
  if (BIHASH_KVP_CACHE_SIZE > 3)
    mask |= slot << 9;
  if (BIHASH_KVP_CACHE_SIZE > 4)
    mask |= slot << 12;

  value = b->cache_lru;
  tmp = value ^ mask;

  /* Already the most-recently used? */
  if ((tmp & 7) == 0)
    return;

  found_lru_pos = ((tmp & (7 << 3)) == 0) ? 1 : 0;
  if (BIHASH_KVP_CACHE_SIZE > 2)
    found_lru_pos = ((tmp & (7 << 6)) == 0) ? 2 : found_lru_pos;
  if (BIHASH_KVP_CACHE_SIZE > 3)
    found_lru_pos = ((tmp & (7 << 9)) == 0) ? 3 : found_lru_pos;
  if (BIHASH_KVP_CACHE_SIZE > 4)
    found_lru_pos = ((tmp & (7 << 12)) == 0) ? 4 : found_lru_pos;

  ASSERT (found_lru_pos);

  /* create a mask to kill bits in or above slot */
  mask = 0xFFFF << found_lru_pos;
  mask <<= found_lru_pos;
  mask <<= found_lru_pos;
  mask ^= 0xFFFF;
  tmp = value & mask;

  /* Save bits above slot */
  mask ^= 0xFFFF;
  mask <<= 3;
  save_hi = value & mask;

  value = save_hi | (tmp << 3) | slot;

  b->cache_lru = value;
#endif
}

void
BV (clib_bihash_update_lru_not_inline) (BVT (clib_bihash_bucket) * b,
					u8 slot);

static inline u8 BV (clib_bihash_get_lru) (BVT (clib_bihash_bucket) * b)
{
#if BIHASH_KVP_CACHE_SIZE > 0
  return (b->cache_lru >> (3 * (BIHASH_KVP_CACHE_SIZE - 1))) & 7;
#else
  return 0;
#endif
}

static inline void BV (clib_bihash_reset_cache) (BVT (clib_bihash_bucket) * b)
{
#if BIHASH_KVP_CACHE_SIZE > 0
  u16 initial_lru_value;

  memset (b->cache, 0xff, sizeof (b->cache));

  /*
   * We'll want the cache to be loaded from slot 0 -> slot N, so
   * the initial LRU order is reverse index order.
   */
  if (BIHASH_KVP_CACHE_SIZE == 1)
    initial_lru_value = 0;
  else if (BIHASH_KVP_CACHE_SIZE == 2)
    initial_lru_value = (0 << 3) | (1 << 0);
  else if (BIHASH_KVP_CACHE_SIZE == 3)
    initial_lru_value = (0 << 6) | (1 << 3) | (2 << 0);
  else if (BIHASH_KVP_CACHE_SIZE == 4)
    initial_lru_value = (0 << 9) | (1 << 6) | (2 << 3) | (3 << 0);
  else if (BIHASH_KVP_CACHE_SIZE == 5)
    initial_lru_value = (0 << 12) | (1 << 9) | (2 << 6) | (3 << 3) | (4 << 0);

  b->cache_lru = initial_lru_value;
#endif
}

static inline int BV (clib_bihash_lock_bucket) (BVT (clib_bihash_bucket) * b)
{
#if BIHASH_KVP_CACHE_SIZE > 0
  u16 cache_lru_bit;
  u16 rv;

  cache_lru_bit = 1 << 15;

  rv = __sync_fetch_and_or (&b->cache_lru, cache_lru_bit);
  /* Was already locked? */
  if (rv & (1 << 15))
    return 0;
#endif
  return 1;
}

static inline void BV (clib_bihash_unlock_bucket)
  (BVT (clib_bihash_bucket) * b)
{
#if BIHASH_KVP_CACHE_SIZE > 0
  u16 cache_lru;

  cache_lru = b->cache_lru & ~(1 << 15);
  b->cache_lru = cache_lru;
#endif
}

static inline void *BV (clib_bihash_get_value) (BVT (clib_bihash) * h,
						uword offset)
{
  u8 *hp = (u8 *) h->alloc_arena;
  u8 *vp = hp + offset;

  return (void *) vp;
}

static inline uword BV (clib_bihash_get_offset) (BVT (clib_bihash) * h,
						 void *v)
{
  u8 *hp, *vp;

  hp = (u8 *) h->alloc_arena;
  vp = (u8 *) v;

  return vp - hp;
}

void BV (clib_bihash_init)
  (BVT (clib_bihash) * h, char *name, u32 nbuckets, uword memory_size);

void BV (clib_bihash_set_kvp_format_fn) (BVT (clib_bihash) * h,
					 format_function_t * fmt_fn);

void BV (clib_bihash_free) (BVT (clib_bihash) * h);

int BV (clib_bihash_add_del) (BVT (clib_bihash) * h,
			      BVT (clib_bihash_kv) * add_v, int is_add);
int BV (clib_bihash_search) (BVT (clib_bihash) * h,
			     BVT (clib_bihash_kv) * search_v,
			     BVT (clib_bihash_kv) * return_v);

void BV (clib_bihash_foreach_key_value_pair) (BVT (clib_bihash) * h,
					      void *callback, void *arg);

format_function_t BV (format_bihash);
format_function_t BV (format_bihash_kvp);
format_function_t BV (format_bihash_lru);

static inline int BV (clib_bihash_search_inline_with_hash)
  (BVT (clib_bihash) * h, u64 hash, BVT (clib_bihash_kv) * key_result)
{
  u32 bucket_index;
  BVT (clib_bihash_value) * v;
  BVT (clib_bihash_bucket) * b;
#if BIHASH_KVP_CACHE_SIZE > 0
  BVT (clib_bihash_kv) * kvp;
#endif
  int i, limit;

  bucket_index = hash & (h->nbuckets - 1);
  b = &h->buckets[bucket_index];

  if (b->offset == 0)
    return -1;

#if BIHASH_KVP_CACHE_SIZE > 0
  /* Check the cache, if not currently locked */
  if (PREDICT_TRUE ((b->cache_lru & (1 << 15)) == 0))
    {
      limit = BIHASH_KVP_CACHE_SIZE;
      kvp = b->cache;
      for (i = 0; i < limit; i++)
	{
	  if (BV (clib_bihash_key_compare) (kvp[i].key, key_result->key))
	    {
	      *key_result = kvp[i];
	      h->cache_hits++;
	      return 0;
	    }
	}
    }
#endif

  hash >>= h->log2_nbuckets;

  v = BV (clib_bihash_get_value) (h, b->offset);

  /* If the bucket has unresolvable collisions, use linear search */
  limit = BIHASH_KVP_PER_PAGE;
  v += (b->linear_search == 0) ? hash & ((1 << b->log2_pages) - 1) : 0;
  if (PREDICT_FALSE (b->linear_search))
    limit <<= b->log2_pages;

  for (i = 0; i < limit; i++)
    {
      if (BV (clib_bihash_key_compare) (v->kvp[i].key, key_result->key))
	{
	  *key_result = v->kvp[i];

#if BIHASH_KVP_CACHE_SIZE > 0
	  u8 cache_slot;
	  /* Try to lock the bucket */
	  if (BV (clib_bihash_lock_bucket) (b))
	    {
	      cache_slot = BV (clib_bihash_get_lru) (b);
	      b->cache[cache_slot] = v->kvp[i];
	      BV (clib_bihash_update_lru) (b, cache_slot);

	      /* Unlock the bucket */
	      BV (clib_bihash_unlock_bucket) (b);
	      h->cache_misses++;
	    }
#endif
	  return 0;
	}
    }
  return -1;
}

static inline int BV (clib_bihash_search_inline)
  (BVT (clib_bihash) * h, BVT (clib_bihash_kv) * key_result)
{
  u64 hash;

  hash = BV (clib_bihash_hash) (key_result);

  return BV (clib_bihash_search_inline_with_hash) (h, hash, key_result);
}

static inline void BV (clib_bihash_prefetch_bucket)
  (BVT (clib_bihash) * h, u64 hash)
{
  u32 bucket_index;
  BVT (clib_bihash_bucket) * b;

  bucket_index = hash & (h->nbuckets - 1);
  b = &h->buckets[bucket_index];

  CLIB_PREFETCH (b, CLIB_CACHE_LINE_BYTES, READ);
}

static inline void BV (clib_bihash_prefetch_data)
  (BVT (clib_bihash) * h, u64 hash)
{
  u32 bucket_index;
  BVT (clib_bihash_value) * v;
  BVT (clib_bihash_bucket) * b;

  bucket_index = hash & (h->nbuckets - 1);
  b = &h->buckets[bucket_index];

  if (PREDICT_FALSE (b->offset == 0))
    return;

  hash >>= h->log2_nbuckets;
  v = BV (clib_bihash_get_value) (h, b->offset);

  v += (b->linear_search == 0) ? hash & ((1 << b->log2_pages) - 1) : 0;

  CLIB_PREFETCH (v, CLIB_CACHE_LINE_BYTES, READ);
}

static inline int BV (clib_bihash_search_inline_2_with_hash)
  (BVT (clib_bihash) * h,
   u64 hash, BVT (clib_bihash_kv) * search_key, BVT (clib_bihash_kv) * valuep)
{
  u32 bucket_index;
  BVT (clib_bihash_value) * v;
  BVT (clib_bihash_bucket) * b;
#if BIHASH_KVP_CACHE_SIZE > 0
  BVT (clib_bihash_kv) * kvp;
#endif
  int i, limit;

  ASSERT (valuep);

  bucket_index = hash & (h->nbuckets - 1);
  b = &h->buckets[bucket_index];

  if (b->offset == 0)
    return -1;

  /* Check the cache, if currently unlocked */
#if BIHASH_KVP_CACHE_SIZE > 0
  if (PREDICT_TRUE ((b->cache_lru & (1 << 15)) == 0))
    {
      limit = BIHASH_KVP_CACHE_SIZE;
      kvp = b->cache;
      for (i = 0; i < limit; i++)
	{
	  if (BV (clib_bihash_key_compare) (kvp[i].key, search_key->key))
	    {
	      *valuep = kvp[i];
	      h->cache_hits++;
	      return 0;
	    }
	}
    }
#endif

  hash >>= h->log2_nbuckets;
  v = BV (clib_bihash_get_value) (h, b->offset);

  /* If the bucket has unresolvable collisions, use linear search */
  limit = BIHASH_KVP_PER_PAGE;
  v += (b->linear_search == 0) ? hash & ((1 << b->log2_pages) - 1) : 0;
  if (PREDICT_FALSE (b->linear_search))
    limit <<= b->log2_pages;

  for (i = 0; i < limit; i++)
    {
      if (BV (clib_bihash_key_compare) (v->kvp[i].key, search_key->key))
	{
	  *valuep = v->kvp[i];

#if BIHASH_KVP_CACHE_SIZE > 0
	  u8 cache_slot;

	  /* Try to lock the bucket */
	  if (BV (clib_bihash_lock_bucket) (b))
	    {
	      cache_slot = BV (clib_bihash_get_lru) (b);
	      b->cache[cache_slot] = v->kvp[i];
	      BV (clib_bihash_update_lru) (b, cache_slot);

	      /* Reenable the cache */
	      BV (clib_bihash_unlock_bucket) (b);
	      h->cache_misses++;
	    }
#endif
	  return 0;
	}
    }
  return -1;
}

static inline int BV (clib_bihash_search_inline_2)
  (BVT (clib_bihash) * h,
   BVT (clib_bihash_kv) * search_key, BVT (clib_bihash_kv) * valuep)
{
  u64 hash;

  hash = BV (clib_bihash_hash) (search_key);

  return BV (clib_bihash_search_inline_2_with_hash) (h, hash, search_key,
						     valuep);
}


#endif /* __included_bihash_template_h__ */

/** @endcond */

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
