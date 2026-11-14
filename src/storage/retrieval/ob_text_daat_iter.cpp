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

#define USING_LOG_PREFIX STORAGE

#include "ob_text_daat_iter.h"

namespace oceanbase
{
namespace storage
{
int ObTextDaaTIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRDaaTIterImpl::init(*param.base_param_, *param.dim_iters_,
                                            *param.allocator_, *param.relevance_collector_))) {
  } else if (nullptr != param.base_param_->relevance_expr_
      && OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  } else {
    mode_flag_ = param.mode_flag_;
    function_lookup_mode_ = param.function_lookup_mode_;
    minimum_should_match_ = param.minimum_should_match_;
    reuse_cursors();
  }
  return ret;
}

void ObTextDaaTIter::reset()
{
  reuse_cursors();
  bm25_param_estimator_.reset();
  // this class can know the type of dim_iters_, but ObSRDaaTIterImpl maybe not
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRDaaTIterImpl::reset();
}

void ObTextDaaTIter::reuse(const bool switch_tablet)
{
  reuse_cursors();
  if ((OB_NOT_NULL(dim_iters_) && 0 == dim_iters_->count())) {
    // do nothing
  } else {
    bm25_param_estimator_.reuse(switch_tablet);
  }
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRDaaTIterImpl::reuse(switch_tablet);
}

int ObTextDaaTIter::pre_process()
{
  int ret = OB_SUCCESS;
  if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (nullptr != iter_param_->relevance_expr_) {
    if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
    }
  }
  return ret;
}

int ObTextDaaTIter::do_one_merge_round(int64_t &count)
{
  int ret = OB_SUCCESS;
  // For a small natural-language membership union, the ordered document IDs
  // can be merged directly. Retain the collector for minimum_should_match,
  // and the common cache/projection path for LIMIT and lookup ownership.
  // Scores, boolean expressions, function lookups and larger unions retain
  // the general heap implementation.
  if (nullptr != iter_param_->relevance_expr_ || iter_param_->need_filter()
      || iter_param_->need_pushdown_topk() || function_lookup_mode_
      || mode_flag_ != sql::ObMatchAgainstMode::NATURAL_LANGUAGE_MODE
      || dim_iters_->count() > 8) {
    ret = ObSRDaaTIterImpl::do_one_merge_round(count);
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < next_round_cnt_; ++i) {
      const int64_t idx = next_round_iter_idxes_[i];
      ObISRDaaTDimIter *dim = dim_iters_->at(idx);
      if (OB_ISNULL(dim)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(dim->get_next_row())) {
        if (OB_ITER_END == ret) {
          iter_domain_ids_[idx] = nullptr;
          ret = OB_SUCCESS;
        }
      } else if (OB_FAIL(dim->get_curr_id(iter_domain_ids_[idx]))) {
      } else if (OB_ISNULL(iter_domain_ids_[idx]) || iter_domain_ids_[idx]->is_null()) {
        ret = OB_ERR_UNEXPECTED;
      }
    }
    next_round_cnt_ = 0;
    const ObDatum *first = nullptr;
    ObSRMergeItem first_item;
    const bool integer_ids = iter_param_->id_proj_expr_->datum_meta_.type_ == common::ObUInt64Type;
    for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
      const ObDatum *id = iter_domain_ids_[i];
      if (nullptr != id) {
        int64_t cmp = 1;
        if (nullptr == first) {
        } else if (integer_ids) {
          cmp = first->get_uint() > id->get_uint() ? 1
              : (first->get_uint() < id->get_uint() ? -1 : 0);
        } else {
          ObSRMergeItem item;
          item.iter_idx_ = i;
          if (OB_FAIL(merge_cmp_.cmp(first_item, item, cmp))) {
          }
        }
        if (OB_SUCC(ret) && cmp >= 0) {
          if (cmp > 0) {
            first = id;
            first_item.iter_idx_ = i;
            next_round_cnt_ = 0;
          }
          next_round_iter_idxes_[next_round_cnt_++] = i;
        }
      }
    }
    if (OB_SUCC(ret) && nullptr == first) {
      ret = OB_ITER_END;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < next_round_cnt_; ++i) {
      if (OB_FAIL(relevance_collector_->collect_one_dim(next_round_iter_idxes_[i], 1.0))) {
      }
    }
    if (OB_SUCC(ret)) {
      double relevance = 0.0;
      bool valid = false;
      if (OB_FAIL(relevance_collector_->get_result(relevance, valid))) {
      } else if (valid && OB_FAIL(cache_result(count, *first, relevance))) {
      }
    }
  }
  return ret;
}

void ObTextDaaTIter::reuse_cursors()
{
  for (PostingCursor &cursor : cursors_) {
    cursor = PostingCursor();
  }
}

bool ObTextDaaTIter::use_buffered_union() const
{
  return is_inited_ && nullptr == iter_param_->relevance_expr_ && !iter_param_->need_filter()
      && !iter_param_->need_pushdown_topk() && !function_lookup_mode_
      && mode_flag_ == sql::ObMatchAgainstMode::NATURAL_LANGUAGE_MODE
      && minimum_should_match_ >= 0 && dim_iters_->count() > 0 && dim_iters_->count() <= 8
      && iter_param_->eval_ctx_->is_vectorized() && !iter_param_->limit_param_->is_valid();
}

int ObTextDaaTIter::refill_postings()
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    PostingCursor &cursor = cursors_[i];
    if (!cursor.ended_ && cursor.pos_ >= cursor.count_) {
      auto *dim = static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i));
      dim->consume_doc_id_batch(cursor.count_);
      cursor.pos_ = 0;
      if (OB_FAIL(dim->get_doc_id_batch(cursor.ids_, cursor.count_))) {
        if (OB_ITER_END == ret) {
          cursor.ended_ = true;
          cursor.ids_ = nullptr;
          cursor.count_ = 0;
          ret = OB_SUCCESS;
        }
      }
    }
    if (OB_SUCC(ret)) {
      iter_domain_ids_[i] = cursor.ended_ ? nullptr : &cursor.ids_[cursor.pos_].get_datum();
    }
  }
  return ret;
}

int ObTextDaaTIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  if (!use_buffered_union()) {
    return ObSRDaaTIterImpl::get_next_rows(capacity, count);
  }
  int ret = OB_SUCCESS;
  count = 0;
  const int64_t real_capacity = MIN(capacity, iter_param_->max_batch_size_);
  if (OB_UNLIKELY(real_capacity > buffered_domain_ids_.count()
      || real_capacity > buffered_relevances_.count())) {
    return OB_INVALID_ARGUMENT;
  }
  const bool integer_ids = iter_param_->id_proj_expr_->datum_meta_.type_ == common::ObUInt64Type;
  const bool binary_ids = iter_param_->id_proj_expr_->datum_meta_.type_ == common::ObVarcharType
      && iter_param_->id_proj_expr_->datum_meta_.cs_type_ == common::CS_TYPE_BINARY;
  // Heads that did not win the previous merge are still valid. Revisit all
  // posting buffers only at output-batch entry or when one buffer is exhausted.
  bool need_refill = true;
  while (OB_SUCC(ret) && count < real_capacity) {
    const ObDatum *first = nullptr;
    ObSRMergeItem first_item;
    uint32_t matching_dims = 0;
    if (need_refill) {
      ret = refill_postings();
      need_refill = false;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
      const ObDatum *id = iter_domain_ids_[i];
      if (nullptr != id) {
        int64_t cmp = 1;
        if (nullptr == first) {
        } else if (integer_ids) {
          cmp = first->get_uint() > id->get_uint() ? 1
              : (first->get_uint() < id->get_uint() ? -1 : 0);
        } else if (binary_ids && first->len_ == 16 && id->len_ == 16) {
          // Native seekdb document IDs are opaque 16-byte binary values; their
          // storage order is byte order, not the host-endian sequence number.
          cmp = MEMCMP(first->ptr_, id->ptr_, 16);
        } else {
          ObSRMergeItem item;
          item.iter_idx_ = i;
          if (OB_FAIL(merge_cmp_.cmp(first_item, item, cmp))) {
          }
        }
        if (OB_SUCC(ret) && cmp >= 0) {
          if (cmp > 0) {
            first = id;
            first_item.iter_idx_ = i;
            matching_dims = 0;
          }
          matching_dims |= uint32_t(1) << i;
        }
      }
    }
    if (OB_SUCC(ret) && nullptr == first) {
      ret = OB_ITER_END;
    } else if (OB_SUCC(ret)) {
      const int matched = __builtin_popcount(matching_dims);
      if (matched >= minimum_should_match_) {
        // This path has no LIMIT/OFFSET. Posting IDs already own their bytes,
        // so copy the value object rather than deep-copying its generic datum
        // through the row-wise cache API for every result.
        const PostingCursor &winner = cursors_[first_item.iter_idx_];
        buffered_domain_ids_[count] = winner.ids_[winner.pos_];
        buffered_relevances_[count] = static_cast<double>(matched);
        ++count;
        ++input_row_cnt_;
        ++output_row_cnt_;
      }
      // Refill on the next merge round, after output IDs have been copied.
      for (int64_t i = 0; i < dim_iters_->count(); ++i) {
        if (matching_dims & (uint32_t(1) << i)) {
          PostingCursor &cursor = cursors_[i];
          ++cursor.pos_;
          if (cursor.pos_ < cursor.count_) {
            iter_domain_ids_[i] = &cursor.ids_[cursor.pos_].get_datum();
          } else {
            need_refill = true;
          }
        }
      }
    }
  }
  if ((OB_SUCC(ret) || OB_ITER_END == ret) && count > 0) {
    int project_ret = project_results(count);
    if (OB_SUCCESS != project_ret) {
      ret = project_ret;
    }
  }
  return ret;
}

ObTextBMWIter::ObTextBMWIter()
  : ObSRBMWIterImpl(),
    bm25_param_estimator_() {}

void ObTextBMWIter::reuse(const bool switch_tablet)
{
  bm25_param_estimator_.reuse(switch_tablet);
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRBMWIterImpl::reuse(switch_tablet);
}

void ObTextBMWIter::reset()
{
  bm25_param_estimator_.reset();
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRBMWIterImpl::reset();
}

int ObTextBMWIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRBMWIterImpl::init(*param.base_param_, *param.dim_iters_,
                                           *param.allocator_, *param.relevance_collector_))) {
  } else if (OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  }
  return ret;
}

int ObTextBMWIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
  }

  if (FAILEDx(ObSRBMWIterImpl::get_next_rows(capacity, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  }
  return ret;
}

int ObTextBMWIter::init_before_wand_process()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!bm25_param_estimator_.is_estimated())) {
    ret = OB_ERR_UNEXPECTED;
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    ObTextRetrievalBlockMaxIter *block_max_iter = static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i));
    if (OB_FAIL(block_max_iter->init_block_max_iter(
        bm25_param_estimator_.get_total_doc_cnt(), bm25_param_estimator_.get_avg_doc_token_cnt()))) {
    }
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
