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

#include "storage/fts/ob_ft_token_cache.h"
#define USING_LOG_PREFIX STORAGE_FTS

#include "lib/hash_func/murmur_hash.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"

namespace oceanbase
{
namespace storage
{

ObFTTokenCache *ObFTTokenCache::g_token_cache_ = nullptr;

int ObFTTokenCache::init_global()
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(g_token_cache_)) {
    ret = OB_INIT_TWICE;
  } else if (OB_ISNULL(g_token_cache_ = OB_NEW(ObFTTokenCache, "FtsTokCache"))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(g_token_cache_->init())) {
    OB_DELETE(ObFTTokenCache, "FtsTokCache", g_token_cache_);
    g_token_cache_ = nullptr;
  }
  return ret;
}

void ObFTTokenCache::deinit_global()
{
  if (OB_NOT_NULL(g_token_cache_)) {
    g_token_cache_->destroy();
    OB_DELETE(ObFTTokenCache, "FtsTokCache", g_token_cache_);
    g_token_cache_ = nullptr;
  }
}

int ObFTTokenCache::init()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(bucket_lock_.init(BUCKET_CNT))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < BUCKET_CNT; ++i) {
      if (OB_FAIL(buckets_[i].map_.create(BUCKET_MAP_CNT, "FtsTokCacheBkt", "FtsTokCacheNode"))) {
        FLOG_WARN("fail to create fulltext token cache bucket map", K(ret), K(i));
      }
    }
    if (OB_FAIL(ret)) {
      for (int64_t i = 0; i < BUCKET_CNT; ++i) {
        buckets_[i].map_.destroy();
      }
      bucket_lock_.destroy();
    } else {
      is_inited_ = true;
      FLOG_INFO("succeeded to initialize fulltext token cache");
    }
  }
  return ret;
}

void ObFTTokenCache::destroy()
{
  if (is_inited_) {
    for (int64_t i = 0; i < BUCKET_CNT; ++i) {
      buckets_[i].map_.destroy();
      buckets_[i].arena_.reset();
    }
    bucket_lock_.destroy();
    is_inited_ = false;
  }
}

uint64_t ObFTTokenCache::property_fingerprint(const ObFTParserProperty &property)
{
  uint64_t fields[7];
  fields[0] = static_cast<uint64_t>(property.min_token_size_);
  fields[1] = static_cast<uint64_t>(property.max_token_size_);
  fields[2] = static_cast<uint64_t>(property.ngram_token_size_);
  fields[3] = property.ik_mode_smart_ ? 1ULL : 0ULL;
  fields[4] = static_cast<uint64_t>(property.stopword_table_.length());
  fields[5] = static_cast<uint64_t>(property.dict_table_.length());
  fields[6] = static_cast<uint64_t>(property.quantifier_table_.length());
  uint64_t hash_val = common::murmurhash(fields, sizeof(fields), 0);
  if (property.stopword_table_.length() > 0) {
    hash_val = common::murmurhash(property.stopword_table_.ptr(),
                                  property.stopword_table_.length(),
                                  hash_val);
  }
  if (property.dict_table_.length() > 0) {
    hash_val = common::murmurhash(property.dict_table_.ptr(),
                                  property.dict_table_.length(),
                                  hash_val);
  }
  if (property.quantifier_table_.length() > 0) {
    hash_val = common::murmurhash(property.quantifier_table_.ptr(),
                                  property.quantifier_table_.length(),
                                  hash_val);
  }
  return hash_val;
}

bool ObFTTokenCache::property_equals(const ObFTParserProperty &property,
                                     const ObFTTokenCacheEntry &entry)
{
  const char *stopword_ptr = entry.data();
  const char *dict_ptr = stopword_ptr + entry.stopword_len_;
  const char *quantifier_ptr = dict_ptr + entry.dict_len_;
  bool is_equal = property.min_token_size_ == entry.min_token_size_
                  && property.max_token_size_ == entry.max_token_size_
                  && property.ngram_token_size_ == entry.ngram_token_size_
                  && property.ik_mode_smart_ == (0 != entry.ik_mode_smart_)
                  && property.stopword_table_.length() == entry.stopword_len_
                  && property.dict_table_.length() == entry.dict_len_
                  && property.quantifier_table_.length() == entry.quantifier_len_;
  if (is_equal && entry.stopword_len_ > 0) {
    is_equal = 0 == MEMCMP(property.stopword_table_.ptr(), stopword_ptr, entry.stopword_len_);
  }
  if (is_equal && entry.dict_len_ > 0) {
    is_equal = 0 == MEMCMP(property.dict_table_.ptr(), dict_ptr, entry.dict_len_);
  }
  if (is_equal && entry.quantifier_len_ > 0) {
    is_equal = 0 == MEMCMP(property.quantifier_table_.ptr(), quantifier_ptr, entry.quantifier_len_);
  }
  return is_equal;
}

int ObFTTokenCache::try_get(const ObFTTokenCacheKey &key,
                            const ObFTParserName &parser_name,
                            const int64_t parser_version,
                            const ObFTParserProperty &property,
                            const char *fulltext,
                            const int64_t fulltext_len,
                            common::ObIAllocator &allocator,
                            ObFTWordMap &words,
                            int64_t &doc_length,
                            bool &found)
{
  int ret = OB_SUCCESS;
  found = false;
  uint64_t hash_val = 0;
  key.hash(hash_val);
  const uint64_t bucket_idx = hash_val & (BUCKET_CNT - 1);
  common::ObBucketRLockGuard guard(bucket_lock_, bucket_idx);
  if (OB_UNLIKELY(OB_SUCCESS != guard.get_ret())) {
    ret = guard.get_ret();
    FLOG_WARN("fail to read lock fulltext token cache bucket", K(ret), K(bucket_idx));
  } else {
    char *const *entry_ptr = buckets_[bucket_idx].map_.get(key);
    if (OB_ISNULL(entry_ptr)) {
      // miss
    } else {
      const ObFTTokenCacheEntry *entry = reinterpret_cast<const ObFTTokenCacheEntry *>(*entry_ptr);
      const char *text_ptr = entry->data()
                             + entry->stopword_len_
                             + entry->dict_len_
                             + entry->quantifier_len_;
      if (entry->text_len_ != fulltext_len
          || entry->parser_version_ != parser_version
          || !(entry->parser_name_ == parser_name)
          || entry->collation_ != static_cast<int32_t>(key.collation_)
          || !property_equals(property, *entry)
          || 0 != MEMCMP(fulltext, text_ptr, fulltext_len)) {
        // hash collision of the 64-bit key hashes, treat as a miss
        FLOG_INFO("fulltext token cache key collision", K(bucket_idx), K(fulltext_len));
      } else if (OB_FAIL(replay_entry(*entry, allocator, words, doc_length))) {
        FLOG_WARN("fail to replay cached fulltext tokens", K(ret));
      } else {
        found = true;
      }
    }
  }
  return ret;
}

int ObFTTokenCache::try_store(const ObFTTokenCacheKey &key,
                              const ObFTParserName &parser_name,
                              const int64_t parser_version,
                              const ObFTParserProperty &property,
                              const char *fulltext,
                              const int64_t fulltext_len,
                              const int64_t doc_length,
                              const ObFTWordMap &words)
{
  int ret = OB_SUCCESS;
  uint64_t hash_val = 0;
  key.hash(hash_val);
  const uint64_t bucket_idx = hash_val & (BUCKET_CNT - 1);
  // Skip large documents before sizing/copying their word map. Caching is
  // optional: the caller already has the correct segmentation result.
  if (fulltext_len > MAX_ENTRY_BYTES) {
    return OB_SUCCESS;
  }
  const int64_t size = entry_size(property, fulltext_len, words);
  if (size > MAX_ENTRY_BYTES) {
    return OB_SUCCESS;
  }
  common::ObBucketWLockGuard guard(bucket_lock_, bucket_idx);
  Bucket &bucket = buckets_[bucket_idx];
  if (OB_UNLIKELY(OB_SUCCESS != guard.get_ret())) {
    ret = guard.get_ret();
  } else if (OB_NOT_NULL(bucket.map_.get(key))) {
    // Another thread stored the same key first.
  } else {
    if (bucket.used_bytes_ + size > BUCKET_BYTE_BUDGET
        || bucket.map_.size() >= BUCKET_ENTRY_BUDGET) {
      ret = bucket.clear();
    }
    char *buf = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_ISNULL(buf = static_cast<char *>(bucket.arena_.alloc(size)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      // Account for allocations even if filling/inserting subsequently fails.
      bucket.used_bytes_ += size;
      ObFTTokenCacheEntry *entry = new (buf) ObFTTokenCacheEntry();
      if (OB_FAIL(fill_entry(*entry, property, parser_name, parser_version,
                            key.collation_, fulltext, fulltext_len, doc_length, words))) {
      } else if (OB_FAIL(bucket.map_.set_refactored(key, reinterpret_cast<char *>(entry)))) {
      }
    }
  }
  return ret;
}

int ObFTTokenCache::Bucket::clear()
{
  // Clear pointers before releasing their payloads. Caller holds the shard's
  // write lock, so no reader can still be copying data out of this arena.
  int ret = map_.reuse();
  if (OB_SUCCESS == ret) {
    arena_.reset();
    used_bytes_ = 0;
  }
  return ret;
}

int ObFTTokenCache::replay_entry(const ObFTTokenCacheEntry &entry,
                                 common::ObIAllocator &allocator,
                                 ObFTWordMap &words,
                                 int64_t &doc_length)
{
  int ret = OB_SUCCESS;
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(static_cast<common::ObCollationType>(entry.collation_));
  const char *cursor = entry.data()
                       + entry.stopword_len_
                       + entry.dict_len_
                       + entry.quantifier_len_
                       + entry.text_len_;
  for (int64_t i = 0; OB_SUCC(ret) && i < entry.word_cnt_; ++i) {
    int64_t word_len = 0;
    int64_t word_cnt = 0;
    MEMCPY(&word_len, cursor, sizeof(int64_t));
    cursor += sizeof(int64_t);
    MEMCPY(&word_cnt, cursor, sizeof(int64_t));
    cursor += sizeof(int64_t);
    char *word_buf = static_cast<char *>(allocator.alloc(word_len));
    if (OB_ISNULL(word_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(word_buf, cursor, word_len);
      cursor += word_len;
      if (OB_FAIL(words.set_refactored(ObFTWord(word_len, word_buf, meta), word_cnt))) {
        FLOG_WARN("fail to replay fulltext word", K(ret), K(word_len), K(word_cnt));
      }
    }
  }
  if (OB_SUCC(ret)) {
    doc_length = entry.doc_length_;
  }
  return ret;
}

int64_t ObFTTokenCache::entry_size(const ObFTParserProperty &property,
                                   const int64_t text_len,
                                   const ObFTWordMap &words)
{
  int64_t size = sizeof(ObFTTokenCacheEntry)
                 + property.stopword_table_.length()
                 + property.dict_table_.length()
                 + property.quantifier_table_.length()
                 + text_len;
  for (ObFTWordMap::const_iterator iter = words.begin(); iter != words.end(); ++iter) {
    size += 2 * sizeof(int64_t) + iter->first.get_word().get_string().length();
  }
  return size;
}
int ObFTTokenCache::fill_entry(ObFTTokenCacheEntry &entry,
                               const ObFTParserProperty &property,
                               const ObFTParserName &parser_name,
                               const int64_t parser_version,
                               const common::ObCollationType collation,
                               const char *fulltext,
                               const int64_t fulltext_len,
                               const int64_t doc_length,
                               const ObFTWordMap &words)
{
  int ret = OB_SUCCESS;
  entry.word_cnt_ = words.size();
  entry.doc_length_ = doc_length;
  entry.text_len_ = fulltext_len;
  entry.min_token_size_ = property.min_token_size_;
  entry.max_token_size_ = property.max_token_size_;
  entry.ngram_token_size_ = property.ngram_token_size_;
  entry.ik_mode_smart_ = property.ik_mode_smart_ ? 1 : 0;
  entry.stopword_len_ = property.stopword_table_.length();
  entry.dict_len_ = property.dict_table_.length();
  entry.quantifier_len_ = property.quantifier_table_.length();
  entry.collation_ = static_cast<int32_t>(collation);
  entry.parser_name_ = parser_name;
  entry.parser_version_ = parser_version;

  char *cursor = entry.data();
  if (entry.stopword_len_ > 0) {
    MEMCPY(cursor, property.stopword_table_.ptr(), entry.stopword_len_);
    cursor += entry.stopword_len_;
  }
  if (entry.dict_len_ > 0) {
    MEMCPY(cursor, property.dict_table_.ptr(), entry.dict_len_);
    cursor += entry.dict_len_;
  }
  if (entry.quantifier_len_ > 0) {
    MEMCPY(cursor, property.quantifier_table_.ptr(), entry.quantifier_len_);
    cursor += entry.quantifier_len_;
  }
  MEMCPY(cursor, fulltext, fulltext_len);
  cursor += fulltext_len;
  for (ObFTWordMap::const_iterator iter = words.begin(); OB_SUCC(ret) && iter != words.end(); ++iter) {
    const common::ObString word = iter->first.get_word().get_string();
    const int64_t word_len = word.length();
    const int64_t word_cnt = iter->second;
    MEMCPY(cursor, &word_len, sizeof(int64_t));
    cursor += sizeof(int64_t);
    MEMCPY(cursor, &word_cnt, sizeof(int64_t));
    cursor += sizeof(int64_t);
    MEMCPY(cursor, word.ptr(), word_len);
    cursor += word_len;
  }
  return ret;
}

} // end namespace storage
} // end namespace oceanbase
