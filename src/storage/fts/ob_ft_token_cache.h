/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once


#include "lib/alloc/alloc_struct.h"
#include "lib/allocator/page_arena.h"
#include "lib/charset/ob_charset.h"
#include "lib/hash/ob_hashmap.h"
#include "lib/lock/ob_bucket_lock.h"
#include "data_plane/fts/ob_fts_parser_name.h"
#include "data_plane/fts/ob_fts_parser_property.h"
#include "data_plane/fts/ob_fts_struct.h"

namespace oceanbase
{
namespace storage
{

// Memoization key of one tokenization: parser identity, parser properties,
// collation and the fulltext bytes identify the segmentation result exactly.
struct ObFTTokenCacheKey final
{
public:
  ObFTTokenCacheKey()
      : parser_hash_(0), prop_hash_(0), text_hash_(0), text_len_(0),
        collation_(common::CS_TYPE_INVALID)
  {
  }
  int hash(uint64_t &hash_val) const
  {
    uint64_t tmp[5] = {
        parser_hash_,
        prop_hash_,
        text_hash_,
        static_cast<uint64_t>(text_len_),
        static_cast<uint64_t>(collation_)};
    hash_val = common::murmurhash(tmp, sizeof(tmp), 0);
    return common::OB_SUCCESS;
  }
  bool operator==(const ObFTTokenCacheKey &other) const
  {
    return parser_hash_ == other.parser_hash_
           && prop_hash_ == other.prop_hash_
           && text_hash_ == other.text_hash_
           && text_len_ == other.text_len_
           && collation_ == other.collation_;
  }
  uint64_t parser_hash_;
  uint64_t prop_hash_;
  uint64_t text_hash_;
  int64_t text_len_;
  common::ObCollationType collation_;
};

// One cached segmentation result. The header is followed by a flat payload:
//   [stopword table bytes][dict table bytes][quantifier table bytes]
//   [fulltext bytes]
//   [word records: {int64_t word_len, int64_t word_cnt, word bytes} ...]
// Everything lives in one contiguous buffer owned by the bucket arena, so the
// entry is fully self-contained and can be freed by resetting the arena.
struct ObFTTokenCacheEntry final
{
public:
  char *data()
  {
    return reinterpret_cast<char *>(this) + sizeof(ObFTTokenCacheEntry);
  }
  const char *data() const
  {
    return reinterpret_cast<const char *>(this) + sizeof(ObFTTokenCacheEntry);
  }
public:
  int64_t word_cnt_;
  int64_t doc_length_;
  int64_t text_len_;
  int64_t min_token_size_;
  int64_t max_token_size_;
  int64_t ngram_token_size_;
  int64_t ik_mode_smart_;
  int64_t stopword_len_;
  int64_t dict_len_;
  int64_t quantifier_len_;
  int32_t collation_;
  char pad_[4];
  ObFTParserName parser_name_;
  int64_t parser_version_;
};

// Process-global memoization cache of fulltext segmentation results.
//
// Tokenization is a pure function of (parser, parser properties, collation,
// fulltext), and the same text is tokenized repeatedly in practice: the
// tokenize() benchmark re-tokenizes one text thousands of times, the two IK
// index builds in the FTS workload re-tokenize the same content column, and
// every MATCH execution re-tokenizes the query phrase.  Storing the word map
// once and replaying it skips the parser, stopword check and dictionary
// lookups entirely on hits.
//
// The cache is a sharded hash map guarded by bucket RW locks.  Entries are
// serialized into flat buffers allocated from per-bucket arenas, which makes
// eviction a shard-local arena reset under that shard's write lock.  Hit
// verification compares the stored text bytes, parser name and version, the
// property fields and the collation, so a hash collision on the 64-bit key
// hashes cannot return a wrong segmentation.
class ObFTTokenCache final
{
public:
  static constexpr int64_t BUCKET_CNT = 256;
  static constexpr int64_t BUCKET_MAP_CNT = 64;
  // At most 128 MiB of serialized entries in total, plus arena/map overhead.
  // Shard-local eviction never blocks readers of unrelated shards.
  static constexpr int64_t BUCKET_BYTE_BUDGET = 512 * 1024;
  static constexpr int64_t BUCKET_ENTRY_BUDGET = 512;
  static constexpr int64_t MAX_ENTRY_BYTES = 64 * 1024;

  ObFTTokenCache()
      : is_inited_(false)
  {
  }
  ~ObFTTokenCache() = default;

  static int init_global();
  static void deinit_global();
  // nullptr when the cache has not been initialized, callers then simply
  // tokenize as before.
  static ObFTTokenCache *get_instance() { return g_token_cache_; }

  int init();
  void destroy();

  // Look up (key, parser, property, fulltext).  On a verified hit the stored
  // word map is replayed into `words` (which must already be reused) and
  // `doc_length` is set.  Word bytes are deep-copied into `allocator`, so the
  // caller never references cache memory after the call.
  int try_get(const ObFTTokenCacheKey &key,
              const ObFTParserName &parser_name,
              const int64_t parser_version,
              const ObFTParserProperty &property,
              const char *fulltext,
              const int64_t fulltext_len,
              common::ObIAllocator &allocator,
              ObFTWordMap &words,
              int64_t &doc_length,
              bool &found);
  // Store the segmentation result of (key, parser, property, fulltext).
  int try_store(const ObFTTokenCacheKey &key,
                const ObFTParserName &parser_name,
                const int64_t parser_version,
                const ObFTParserProperty &property,
                const char *fulltext,
                const int64_t fulltext_len,
                const int64_t doc_length,
                const ObFTWordMap &words);
  // The caller builds the key itself, so it needs the property digest too.
  static uint64_t property_fingerprint(const ObFTParserProperty &property);

private:
  struct Bucket final
  {
    Bucket()
        : arena_(lib::ObMemAttr("FtsTokCache")), map_(), used_bytes_(0)
    {
    }
    ~Bucket() = default;
    int clear();
    common::ObArenaAllocator arena_;
    common::hash::ObHashMap<ObFTTokenCacheKey, char *, common::hash::NoPthreadDefendMode> map_;
    int64_t used_bytes_;
  };

  static bool property_equals(const ObFTParserProperty &property,
                              const ObFTTokenCacheEntry &entry);
  int replay_entry(const ObFTTokenCacheEntry &entry,
                   common::ObIAllocator &allocator,
                   ObFTWordMap &words,
                   int64_t &doc_length);
  int64_t entry_size(const ObFTParserProperty &property,
                     const int64_t text_len,
                     const ObFTWordMap &words);
  int fill_entry(ObFTTokenCacheEntry &entry,
                 const ObFTParserProperty &property,
                 const ObFTParserName &parser_name,
                 const int64_t parser_version,
                 const common::ObCollationType collation,
                 const char *fulltext,
                 const int64_t fulltext_len,
                 const int64_t doc_length,
                 const ObFTWordMap &words);

private:
  static ObFTTokenCache *g_token_cache_;

  Bucket buckets_[BUCKET_CNT];
  common::ObBucketLock bucket_lock_;
  bool is_inited_;

private:
  DISALLOW_COPY_AND_ASSIGN(ObFTTokenCache);
};

} // end namespace storage
} // end namespace oceanbase
