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

#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/ddl/ob_explain_resolver.h"
#include "sql/resolver/dml/ob_file_sql_table.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/parser/ob_parser.h"
#include "sql/resolver/ob_resolver.h"
#include "lib/string/ob_sql_string.h"

using namespace oceanbase;
using namespace sql;
using namespace common;
const static int64_t EXPLAIN_FORMAT = 0;
const static int64_t EXPLAIN_DISPLAY_OPTION = 1;
const static int64_t EXPLAIN_INTO_TABLE = 2;
const static int64_t EXPLAIN_STMT_ID = 3;
const static int64_t EXPLAIN_CHILD_STMT = 4;
int ObExplainResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObExplainStmt *explain_stmt = NULL;
  bool replaced = false;
  if (!(T_EXPLAIN == parse_tree.type_  && 5 == parse_tree.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid EXPLAIN syntax",
             "type", parse_tree.type_,
             "# of child", parse_tree.num_child_);
  } else if (NULL == (explain_stmt = create_stmt<ObExplainStmt>())) {
    ret = OB_SQL_RESOLVER_NO_MEMORY;
    LOG_WARN("failed to create explain stmt");
  } else {
    ParseNode *child_node = parse_tree.children_[EXPLAIN_CHILD_STMT];
    ObStmt *child_stmt = NULL;
    ObExplainDisplayOpt opt;
    opt.with_tree_line_ = true;
    if (OB_ISNULL(child_node)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("empty parse tree for stmt");
    } else if (!(T_SELECT == child_node->type_
                 || T_DELETE == child_node->type_
                 || T_INSERT == child_node->type_
                 || T_UPDATE == child_node->type_
                 || T_REPLACE == child_node->type_)) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("invalid stmt type for EXPLAIN", "type", child_node->type_);
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "stmt type for EXPLAIN");
    } else {
      ObResolver resolver(params_);
      if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                   *child_node,
                                   child_stmt))) {
      } else if (OB_ISNULL(child_stmt)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to generate stmt for child stmt");
      } else if (NULL == parse_tree.children_[EXPLAIN_FORMAT]
                 && OB_FAIL(try_replace_file_sql_describe(
                        static_cast<ObDMLStmt *>(child_stmt), replaced))) {
      } else if (replaced) {
      } else {
        explain_stmt->set_explain_query_stmt(static_cast<ObDMLStmt *>(child_stmt));
        ParseNode *format = parse_tree.children_[EXPLAIN_FORMAT];
        if (NULL != format) {
          switch (format->type_) {
          case T_OUTLINE:
          {
            explain_stmt->set_explain_format(EXPLAIN_OUTLINE);
            opt.with_tree_line_ = true;
          } break;
          case T_EXTENDED:
          {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED);
            opt.with_tree_line_ = true;
          } break;
          case T_PARTITIONS:
          {
            explain_stmt->set_explain_format(EXPLAIN_PARTITIONS);
            opt.with_tree_line_ = true;
          } break;
          case T_TRADITIONAL:
          {
            explain_stmt->set_explain_format(EXPLAIN_TRADITIONAL);
            opt.with_tree_line_ = true;
          } break;
          case T_FORMAT_JSON:
          {
            explain_stmt->set_explain_format(EXPLAIN_FORMAT_JSON);
            opt.with_tree_line_ = false;
          } break;
          case T_BASIC: {
            explain_stmt->set_explain_format(EXPLAIN_BASIC);
            opt.with_tree_line_ = true;
            break;
          }
          case T_EXTENDED_NOADDR: {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED_NOADDR);
            opt.with_tree_line_ = true;
            break;
          }
          case T_PLANREGRESS: {
            explain_stmt->set_explain_format(EXPLAIN_PLANREGRESS);
            opt.with_tree_line_ = true;
            break;
          }
          default:
            ret = OB_ERROR;
            LOG_WARN("unknown explain format", "format", format->type_);
            break;
          }
        }
      }
    }
    ParseNode *opt_node = parse_tree.children_[EXPLAIN_DISPLAY_OPTION];
    if (OB_SUCC(ret) && !replaced && NULL != opt_node) {
      switch (opt_node->type_) {
      case T_PRETTY_COLOR:
        opt.with_color_ = true;
        // pass
      case T_PRETTY:
        opt.with_tree_line_ = true;
        break;
      default:
        ret = OB_ERR_UNEXPECTED;
      }
    }
    if (OB_SUCC(ret) && !replaced) {
      explain_stmt->set_display_opt(opt);
    }
    ParseNode *into_table_node = parse_tree.children_[EXPLAIN_INTO_TABLE];
    if (OB_SUCC(ret) && !replaced && NULL != into_table_node) {
      ObString into_table(into_table_node->str_len_, into_table_node->str_value_);
      explain_stmt->set_into_table(into_table);
    }
    ParseNode *statement_id_node = parse_tree.children_[EXPLAIN_STMT_ID];
    if (OB_SUCC(ret) && !replaced && NULL != statement_id_node) {
      ObString statement_id(statement_id_node->str_len_, statement_id_node->str_value_);
      explain_stmt->set_statement_id(statement_id);
    }
  }

  return ret;
}

int ObExplainResolver::try_replace_file_sql_describe(ObDMLStmt *child, bool &replaced)
{
  int ret = OB_SUCCESS;
  replaced = false;
  ObSelectStmt *sel = NULL;
  TableItem *table = NULL;
  if (OB_ISNULL(child) || !child->is_select_stmt()) {
  } else if (FALSE_IT(sel = static_cast<ObSelectStmt *>(child))) {
  } else if (sel->get_table_size() != 1 || sel->is_distinct() || sel->has_order_by()
             || sel->has_limit() || !sel->get_condition_exprs().empty()) {
  } else if (OB_ISNULL(table = sel->get_table_item(0)) || !table->is_file_sql_table_
             || OB_ISNULL(table->file_sql_meta_)) {
  } else if (sel->get_select_item_size() != table->file_sql_meta_->col_cnt_) {
  } else if (OB_ISNULL(params_.allocator_) || OB_ISNULL(session_info_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    const ObFileSqlMeta &meta = *table->file_sql_meta_;
    ObSqlString sql;
    for (int64_t i = 0; OB_SUCC(ret) && i < meta.col_cnt_; ++i) {
      const ObFileSqlColumn &col = meta.columns_.at(i);
      if (OB_FAIL(sql.append(i == 0 ? "SELECT '" : " UNION ALL SELECT '"))) {
      }
      for (int64_t k = 0; OB_SUCC(ret) && k < col.name_.length(); ++k) {
        char c = col.name_.ptr()[k];
        if (c == '\'') {
          ret = sql.append("''");
        } else {
          ret = sql.append(&c, 1);
        }
      }
      if (OB_FAIL(ret)) {
      } else if (i == 0) {
        ret = sql.append_fmt("' AS column_name, '%.*s' AS column_type, '%s' AS `null`, NULL AS `key`, NULL AS `default`, NULL AS extra",
                             static_cast<int>(col.type_name_.length()), col.type_name_.ptr(),
                             col.nullable_ ? "YES" : "NO");
      } else {
        ret = sql.append_fmt("', '%.*s', '%s', NULL, NULL, NULL",
                             static_cast<int>(col.type_name_.length()), col.type_name_.ptr(),
                             col.nullable_ ? "YES" : "NO");
      }
    }
    if (OB_SUCC(ret)) {
      ObParser parser(*params_.allocator_, session_info_->get_sql_mode());
      ParseResult parse_result;
      ObResolver resolver(params_);
      ObStmt *desc = NULL;
      if (OB_FAIL(parser.parse(sql.string(), parse_result))) {
        LOG_WARN("failed to parse file sql describe", K(ret), K(sql.string()));
      } else if (OB_ISNULL(parse_result.result_tree_) || parse_result.result_tree_->num_child_ < 1
                 || OB_ISNULL(parse_result.result_tree_->children_[0])) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                          *parse_result.result_tree_->children_[0], desc))) {
      } else if (OB_ISNULL(desc)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        stmt_ = desc;
        replaced = true;
      }
    }
  }
  return ret;
}
