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

#define USING_LOG_PREFIX SQL_ENG

#include <new>
#include <stdio.h>
#include <string.h>

#include "sql/engine/dml/ob_ai_column_dml.h"

#include "lib/atomic/ob_atomic.h"
#include "lib/string/ob_sql_string.h"
#include "lib/time/ob_time_utility.h"
#include "lib/utility/utility.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "share/ob_server_struct.h"
#include "share/inner_table/ob_inner_table_schema.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_column_schema.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/ob_sql_context.h"
#include "share/schema/ob_schema_struct.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/resolver/ddl/ob_ai_column_def.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase
{
namespace sql
{

using namespace common;
using namespace share::schema;

namespace
{

struct AiRtDef
{
  AiRtDef()
    : def_(),
      dep_ids_(),
      state_name_(),
      request_name_(),
      spec_name_(),
      result_column_id_(OB_INVALID_ID)
  {}
  ObAiColumnDef def_;
  common::ObSEArray<uint64_t, 4> dep_ids_;
  common::ObString state_name_;
  common::ObString request_name_;
  common::ObString spec_name_;
  uint64_t result_column_id_;
};

int build_hidden_name(common::ObIAllocator &alloc,
                      const common::ObString &column_name,
                      const char *suffix,
                      common::ObString &out)
{
  int ret = OB_SUCCESS;
  common::ObSqlString buf;
  if (OB_FAIL(buf.append_fmt("__ai_%.*s%s", column_name.length(), column_name.ptr(), suffix))) {
  } else if (OB_FAIL(common::ob_write_string(alloc, buf.string(), out))) {
  }
  return ret;
}

int collect_ai_defs(common::ObIAllocator &alloc,
                    const ObTableSchema &table_schema,
                    common::ObIArray<AiRtDef *> &defs)
{
  int ret = OB_SUCCESS;
  const int64_t column_count = table_schema.get_column_count();
  for (int64_t i = 0; OB_SUCC(ret) && i < column_count; ++i) {
    const ObColumnSchemaV2 *column = table_schema.get_column_schema_by_idx(i);
    if (OB_ISNULL(column)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (!column->is_hidden()
               || !is_encoded_ai_column_def(column->get_comment())) {
      // not an AI column bookkeeping column
    } else {
      void *rt_def_mem = alloc.alloc(sizeof(AiRtDef));
      AiRtDef *rt_def = (NULL == rt_def_mem) ? NULL : new (rt_def_mem) AiRtDef();
      const ObColumnSchemaV2 *result_column = NULL;
      if (OB_ISNULL(rt_def)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_FAIL(decode_ai_column_def(column->get_comment(), alloc, rt_def->def_))) {
      } else if (OB_ISNULL(result_column = table_schema.get_column_schema(
                             rt_def->def_.column_name_))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ai column result column is missing", K(ret), K(rt_def->def_.column_name_));
      } else if (FALSE_IT(rt_def->result_column_id_ = result_column->get_column_id())) {
      } else if (OB_FAIL(build_hidden_name(alloc, rt_def->def_.column_name_,
                                           AI_COLUMN_STATE_SUFFIX, rt_def->state_name_))
                 || OB_FAIL(build_hidden_name(alloc, rt_def->def_.column_name_,
                                              AI_COLUMN_REQUEST_SUFFIX, rt_def->request_name_))
                 || OB_FAIL(build_hidden_name(alloc, rt_def->def_.column_name_,
                                              AI_COLUMN_SPEC_SUFFIX, rt_def->spec_name_))) {
      } else {
        // dependency column names -> ids, unknown names (nested function names) are ignored
        const ObString &deps = rt_def->def_.dep_column_names_;
        int32_t start = 0;
        for (int32_t k = 0; OB_SUCC(ret) && k <= deps.length(); ++k) {
          if (k == deps.length() || ',' == deps.ptr()[k]) {
            if (k > start) {
              const ObString name(k - start, deps.ptr() + start);
              const ObColumnSchemaV2 *dep_column = table_schema.get_column_schema(name);
              if (NULL != dep_column
                  && OB_FAIL(rt_def->dep_ids_.push_back(dep_column->get_column_id()))) {
              }
            }
            start = k + 1;
          }
        }
        if (OB_SUCC(ret) && OB_FAIL(defs.push_back(rt_def))) {
        }
      }
    }
  }
  return ret;
}

int find_column_index(const UIntFixedArray &column_ids, const uint64_t column_id, int64_t &index)
{
  int ret = OB_SUCCESS;
  index = -1;
  for (int64_t i = 0; OB_SUCC(ret) && -1 == index && i < column_ids.count(); ++i) {
    if (column_ids.at(i) == column_id) {
      index = i;
    }
  }
  if (-1 == index) {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

int build_rowkey_predicate(ObTableModifyOp &op,
                           const ObDMLBaseCtDef &ctdef,
                           const ExprFixedArray &row,
                           const ObTableSchema &table_schema,
                           common::ObSqlString &predicate)
{
  int ret = OB_SUCCESS;
  ObEvalCtx &eval_ctx = op.get_eval_ctx();
  common::ObIAllocator &alloc = op.get_exec_ctx().get_allocator();
  const ObObjPrintParams &print_params = op.get_obj_print_params();
  const ObRowkeyInfo &rowkey_info = table_schema.get_rowkey_info();
  for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_info.get_size(); ++i) {
    uint64_t column_id = OB_INVALID_ID;
    int64_t index = -1;
    ObDatum *datum = NULL;
    ObObj obj;
    char *literal = static_cast<char *>(alloc.alloc(256));
    int64_t literal_len = 256;
    int64_t pos = 0;
    const ObColumnSchemaV2 *column = NULL;
    if (OB_FAIL(rowkey_info.get_column_id(i, column_id))) {
    } else if (OB_FAIL(find_column_index(ctdef.column_ids_, column_id, index))) {
      // delete ctdef may only project the rowkey columns, fall back to rowkey order
      index = i;
      ret = OB_SUCCESS;
    } else if (OB_ISNULL(column = table_schema.get_column_schema(column_id))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (index >= row.count()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("rowkey index is out of range", K(ret), K(index), K(row.count()));
    } else if (OB_FAIL(row.at(index)->eval(eval_ctx, datum))) {
    } else if (OB_ISNULL(datum) || datum->is_null()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("rowkey value is null", K(ret), K(column_id));
    } else if (OB_FAIL(datum->to_obj(obj, row.at(index)->obj_meta_))) {
    } else if (OB_ISNULL(literal)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(obj.print_sql_literal(literal, literal_len, pos, alloc, print_params))) {
    } else if (i > 0 && OB_FAIL(predicate.append(" and "))) {
    } else if (OB_FAIL(predicate.append_fmt("`%.*s` = %.*s",
                                            column->get_column_name_str().length(),
                                            column->get_column_name_str().ptr(),
                                            static_cast<int32_t>(pos), literal))) {
    }
  }
  return ret;
}

int make_request_id(char *buf, const int64_t buf_len)
{
  static uint64_t seq = 0;
  const uint64_t id = ATOMIC_AAF(&seq, 1);
  return snprintf(buf, static_cast<size_t>(buf_len), "%lx_%lx",
                  static_cast<uint64_t>(ObTimeUtility::current_time()), id);
}

} // anonymous namespace

bool table_has_ai_column(const share::schema::ObTableSchema &table_schema)
{
  bool found = false;
  const int64_t column_count = table_schema.get_column_count();
  for (int64_t i = 0; !found && i < column_count; ++i) {
    const ObColumnSchemaV2 *column = table_schema.get_column_schema_by_idx(i);
    if (NULL != column && column->is_hidden()
        && is_encoded_ai_column_def(column->get_comment())) {
      found = true;
    }
  }
  return found;
}

int ObAiColumnDml::process_insert(ObTableModifyOp &op, const ObInsCtDef &ctdef)
{
  return process(op, ctdef, AI_DML_INSERT);
}

int ObAiColumnDml::process_update(ObTableModifyOp &op, const ObUpdCtDef &ctdef)
{
  return process(op, ctdef, AI_DML_UPDATE);
}

int ObAiColumnDml::process_delete(ObTableModifyOp &op, const ObDelCtDef &ctdef)
{
  return process(op, ctdef, AI_DML_DELETE);
}

bool ObAiColumnDml::is_ai_result_column(const ObTableSchema &table_schema,
                                        const ObColumnSchemaV2 &column_schema)
{
  bool found = false;
  common::ObArenaAllocator alloc("AiColCheck");
  for (int64_t i = 0; !found && i < table_schema.get_column_count(); ++i) {
    const ObColumnSchemaV2 *hidden = table_schema.get_column_schema_by_idx(i);
    if (NULL != hidden && hidden->is_hidden()
        && is_encoded_ai_column_def(hidden->get_comment())) {
      ObAiColumnDef def;
      if (OB_SUCCESS == decode_ai_column_def(hidden->get_comment(), alloc, def)
          && 0 == def.column_name_.case_compare(column_schema.get_column_name_str())) {
        found = true;
      }
    }
  }
  return found;
}

int ObAiColumnDml::strict_read_check(ObSQLSessionInfo &session,
                                     const ObTableSchema &table_schema,
                                     const ObColumnSchemaV2 &column_schema)
{
  int ret = OB_SUCCESS;
  bool enabled = false;
  if (session.is_inner() || session.is_in_ai_column_maintenance()
      || !is_ai_result_column(table_schema, column_schema)) {
  } else if (OB_FAIL(session.get_sys_variable(share::SYS_VAR_AI_COLUMN_STRICT_READ, enabled))) {
  } else if (!enabled) {
  } else {
    common::ObArenaAllocator alloc("AiColStrict");
    ObSchemaGetterGuard schema_guard;
    const ObDatabaseSchema *db_schema = NULL;
    ObString state_name;
    ObSqlString sql;
    ObISQLClient::ReadResult read_result;
    bool unfinished = false;
    if (OB_ISNULL(GCTX.schema_service_)) {
      ret = OB_NOT_INIT;
    } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(schema_guard))) {
    } else if (OB_FAIL(schema_guard.get_database_schema(table_schema.get_database_id(), db_schema))) {
    } else if (OB_ISNULL(db_schema)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(build_hidden_name(alloc, column_schema.get_column_name_str(),
                                         AI_COLUMN_STATE_SUFFIX, state_name))) {
    } else if (OB_FAIL(sql.append_fmt(
                 "SELECT 1 FROM `%.*s`.`%.*s` WHERE `%.*s` IS NULL OR `%.*s` <> 'READY' LIMIT 1",
                 db_schema->get_database_name_str().length(),
                 db_schema->get_database_name_str().ptr(),
                 table_schema.get_table_name_str().length(),
                 table_schema.get_table_name_str().ptr(),
                 state_name.length(), state_name.ptr(),
                 state_name.length(), state_name.ptr()))) {
    } else if (OB_ISNULL(GCTX.sql_proxy_)) {
      ret = OB_NOT_INIT;
    } else if (OB_FAIL(GCTX.sql_proxy_->read(read_result, sql.ptr()))) {
    } else if (OB_ISNULL(read_result.get_result())) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      int next_ret = read_result.get_result()->next();
      if (OB_SUCCESS == next_ret) {
        unfinished = true;
      } else if (OB_ITER_END == next_ret) {
      } else {
        ret = next_ret;
      }
    }
    if (OB_SUCC(ret) && unfinished) {
      ret = OB_ERR_AI_COLUMN_UNFINISHED;
      LOG_USER_ERROR(OB_ERR_AI_COLUMN_UNFINISHED,
                     table_schema.get_table_id(), column_schema.get_column_id());
    }
  }
  return ret;
}

namespace
{
bool ai_ident_char(const char c)
{
  return ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z')
         || ('0' <= c && c <= '9') || '_' == c || '$' == c;
}

int rewrite_retry_predicate(const ObString &input,
                            const ObString &state_name,
                            ObSqlString &output)
{
  int ret = OB_SUCCESS;
  bool single_quote = false;
  bool double_quote = false;
  bool backtick = false;
  for (int64_t i = 0; OB_SUCC(ret) && i < input.length();) {
    const char c = input.ptr()[i];
    if (!double_quote && !backtick && '\'' == c) {
      single_quote = !single_quote;
      ret = output.append(&c, 1);
      ++i;
    } else if (!single_quote && !backtick && '"' == c) {
      double_quote = !double_quote;
      ret = output.append(&c, 1);
      ++i;
    } else if (!single_quote && !double_quote && '`' == c) {
      backtick = !backtick;
      ret = output.append(&c, 1);
      ++i;
    } else if (!single_quote && !double_quote && !backtick
               && i + 6 <= input.length()
               && 0 == strncasecmp(input.ptr() + i, "status", 6)
               && (0 == i || !ai_ident_char(input.ptr()[i - 1]))
               && (i + 6 == input.length() || !ai_ident_char(input.ptr()[i + 6]))) {
      ret = output.append_fmt("`%.*s`", state_name.length(), state_name.ptr());
      i += 6;
    } else {
      ret = output.append(&c, 1);
      ++i;
    }
  }
  return ret;
}

int append_rowkey_predicate_expr(const ObTableSchema &table_schema, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  const ObRowkeyInfo &rowkey = table_schema.get_rowkey_info();
  if (rowkey.get_size() <= 0) {
    ret = OB_NOT_SUPPORTED;
  } else if (OB_FAIL(sql.append("concat("))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < rowkey.get_size(); ++i) {
    uint64_t column_id = OB_INVALID_ID;
    const ObColumnSchemaV2 *column = NULL;
    if (OB_FAIL(rowkey.get_column_id(i, column_id))) {
    } else if (OB_ISNULL(column = table_schema.get_column_schema(column_id))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (i > 0 && OB_FAIL(sql.append(", ' and ', "))) {
    } else if (OB_FAIL(sql.append_fmt("'`%.*s` = ', ",
                                      column->get_column_name_str().length(),
                                      column->get_column_name_str().ptr()))) {
    } else if (ob_is_integer_type(column->get_data_type())
               || ob_is_number_tc(column->get_data_type())) {
      ret = sql.append_fmt("cast(`%.*s` as char)",
                           column->get_column_name_str().length(),
                           column->get_column_name_str().ptr());
    } else {
      ret = sql.append_fmt("quote(`%.*s`)",
                           column->get_column_name_str().length(),
                           column->get_column_name_str().ptr());
    }
  }
  if (OB_SUCC(ret)) {
    ret = sql.append(")");
  }
  return ret;
}

int append_row_identity_expr(const ObTableSchema &table_schema, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  const ObRowkeyInfo &rowkey = table_schema.get_rowkey_info();
  if (rowkey.get_size() <= 0) {
    ret = OB_NOT_SUPPORTED;
  } else if (OB_FAIL(sql.append("concat_ws('#'"))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < rowkey.get_size(); ++i) {
    uint64_t column_id = OB_INVALID_ID;
    const ObColumnSchemaV2 *column = NULL;
    if (OB_FAIL(rowkey.get_column_id(i, column_id))) {
    } else if (OB_ISNULL(column = table_schema.get_column_schema(column_id))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(sql.append_fmt(", cast(`%.*s` as char)",
                                      column->get_column_name_str().length(),
                                      column->get_column_name_str().ptr()))) {
    }
  }
  if (OB_SUCC(ret)) {
    ret = sql.append(")");
  }
  return ret;
}
} // anonymous namespace

int ObAiColumnDml::retry_column(ObSQLSessionInfo &session,
                                const ObTableSchema &table_schema,
                                const ObColumnSchemaV2 &column_schema,
                                const ObString &where_sql)
{
  int ret = OB_SUCCESS;
  UNUSED(session);
  common::ObArenaAllocator alloc("AiColRetry");
  ObSchemaGetterGuard schema_guard;
  const ObDatabaseSchema *db_schema = NULL;
  ObString state_name;
  ObString request_name;
  ObString spec_name;
  ObAiColumnDef def;
  const ObColumnSchemaV2 *state_column = NULL;
  ObSqlString predicate;
  ObSqlString request_expr;
  ObSqlString update_sql;
  ObSqlString task_sql;
  char request_id[64];
  make_request_id(request_id, sizeof(request_id));
  if (!is_ai_result_column(table_schema, column_schema)) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "RETRY AI COLUMN on non-AI column");
  } else if (OB_ISNULL(GCTX.schema_service_) || OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(schema_guard))) {
  } else if (OB_FAIL(schema_guard.get_database_schema(table_schema.get_database_id(), db_schema))) {
  } else if (OB_ISNULL(db_schema)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(build_hidden_name(alloc, column_schema.get_column_name_str(),
                                       AI_COLUMN_STATE_SUFFIX, state_name))
             || OB_FAIL(build_hidden_name(alloc, column_schema.get_column_name_str(),
                                          AI_COLUMN_REQUEST_SUFFIX, request_name))
             || OB_FAIL(build_hidden_name(alloc, column_schema.get_column_name_str(),
                                          AI_COLUMN_SPEC_SUFFIX, spec_name))) {
  } else if (OB_ISNULL(state_column = table_schema.get_column_schema(state_name))) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(decode_ai_column_def(state_column->get_comment(), alloc, def))) {
  } else if (where_sql.empty()) {
    ret = predicate.append("1 = 1");
  } else if (OB_FAIL(rewrite_retry_predicate(where_sql, state_name, predicate))) {
  }

  if (OB_SUCC(ret) && OB_FAIL(request_expr.append_fmt("concat('%s_', md5(", request_id))) {
  } else if (OB_SUCC(ret) && OB_FAIL(append_row_identity_expr(table_schema, request_expr))) {
  } else if (OB_SUCC(ret) && OB_FAIL(request_expr.append("))"))) {
  }

  if (OB_SUCC(ret)
      && OB_FAIL(update_sql.append_fmt(
           "UPDATE `%.*s`.`%.*s` SET `%.*s` = NULL, `%.*s` = 'PENDING', "
           "`%.*s` = %.*s, `%.*s` = '%lu' WHERE %.*s",
           db_schema->get_database_name_str().length(), db_schema->get_database_name_str().ptr(),
           table_schema.get_table_name_str().length(), table_schema.get_table_name_str().ptr(),
           column_schema.get_column_name_str().length(), column_schema.get_column_name_str().ptr(),
           state_name.length(), state_name.ptr(), request_name.length(), request_name.ptr(),
           request_expr.length(), request_expr.ptr(),
           spec_name.length(), spec_name.ptr(), def.definition_version_,
           predicate.length(), predicate.ptr()))) {
  }
  if (OB_SUCC(ret)
      && OB_FAIL(task_sql.append_fmt(
           "INSERT INTO oceanbase.%s (table_id, column_id, request_id, rowkey_predicate, "
           "eligible_ts, state, retry_count) SELECT %lu, %lu, `%.*s`, ",
           OB_ALL_AI_COLUMN_TASK_TNAME, table_schema.get_table_id(),
           column_schema.get_column_id(), request_name.length(), request_name.ptr()))) {
  } else if (OB_SUCC(ret) && OB_FAIL(append_rowkey_predicate_expr(table_schema, task_sql))) {
  } else if (OB_SUCC(ret)
             && OB_FAIL(task_sql.append_fmt(
                  ", now(6), 1, 0 FROM `%.*s`.`%.*s` "
                  "WHERE left(`%.*s`, %ld) = '%s_'",
                  db_schema->get_database_name_str().length(), db_schema->get_database_name_str().ptr(),
                  table_schema.get_table_name_str().length(), table_schema.get_table_name_str().ptr(),
                  request_name.length(), request_name.ptr(),
                  static_cast<int64_t>(strlen(request_id) + 1), request_id))) {
  }

  ObMySQLTransaction trans;
  bool started = false;
  int64_t affected_rows = 0;
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(trans.start(GCTX.sql_proxy_))) {
  } else if (FALSE_IT(started = true)) {
  } else if (OB_FAIL(trans.write(update_sql.ptr(), affected_rows))) {
  } else if (0 == affected_rows) {
    // A zero-row retry is a no-op and must not create a task.
  } else if (OB_FAIL(trans.write(task_sql.ptr(), affected_rows))) {
  }
  if (started) {
    if (OB_SUCC(ret)) {
      ret = trans.end(true);
    } else {
      int tmp_ret = trans.end(false);
      LOG_WARN("failed to roll back AI column retry", K(tmp_ret));
    }
  }
  return ret;
}

int ObAiColumnDml::process(ObTableModifyOp &op,
                           const ObDMLBaseCtDef &ctdef,
                           const DmlEvent event)
{
  int ret = OB_SUCCESS;
  ObExecContext &ctx = op.get_exec_ctx();
  ObSQLSessionInfo *session = ctx.get_my_session();
  ObSqlCtx *sql_ctx = ctx.get_sql_ctx();
  ObSchemaGetterGuard *schema_guard = NULL;
  const ObTableSchema *table_schema = NULL;
  const ObDatabaseSchema *db_schema = NULL;
  common::ObIAllocator &alloc = ctx.get_allocator();
  common::ObSEArray<AiRtDef *, 4> defs;
  // NOTE: das_base_ctdef_.table_id_ is the query-internal table-loc id, NOT the
  // physical table id (see ObTableModifyOp::has_ai_column). index_tid_ is the
  // physical id; try it first and fall back to table_id_.
  const uint64_t cand_ids[2] = { ctdef.das_base_ctdef_.index_tid_,
                                 ctdef.das_base_ctdef_.table_id_ };
  bool need_maintenance = true;

  if (OB_ISNULL(session) || OB_ISNULL(sql_ctx)
      || OB_ISNULL(schema_guard = sql_ctx->schema_guard_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (session->is_in_ai_column_maintenance()) {
    // nested maintenance statement issued by this helper, do not recurse.
    // NOTE: table_schema stays NULL in this branch; mark skip so the code
    // below never dereferences it.
    need_maintenance = false;
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < 2; ++i) {
      table_schema = NULL;
      if (OB_FAIL(schema_guard->get_table_schema(cand_ids[i], table_schema))) {
        LOG_WARN("failed to get table schema", K(ret), "table_id", cand_ids[i]);
      } else if (NULL != table_schema) {
        break;
      } else if (1 == i) {
        ret = OB_ERR_UNEXPECTED;
      }
    }
  }
  const uint64_t table_id = (NULL != table_schema) ? table_schema->get_table_id()
                                                   : OB_INVALID_ID;
  if (OB_FAIL(ret) || !need_maintenance) {
    // pass through, or nested maintenance: skip all schema work
  } else if (OB_FAIL(collect_ai_defs(alloc, *table_schema, defs))) {
  } else if (OB_FAIL(schema_guard->get_database_schema(table_schema->get_database_id(),
                                                       db_schema))) {
    LOG_WARN("failed to get database schema", K(ret), K(table_schema->get_database_id()));
  } else if (OB_ISNULL(db_schema)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    LOG_INFO("[AI_PROBE] collect", "table_id", table_schema->get_table_id(),
             "def_cnt", defs.count(), "col_cnt", table_schema->get_column_count());
  }

  if (OB_FAIL(ret) || !need_maintenance) {
    // pass through, or nested maintenance: nothing to compare
  } else if (AI_DML_UPDATE == event) {
    const ObUpdCtDef &upd_ctdef = static_cast<const ObUpdCtDef &>(ctdef);
    need_maintenance = false;
    if (ctdef.old_row_.count() == 0 || ctdef.old_row_.count() != ctdef.new_row_.count()) {
      need_maintenance = true;
    }
    // primary key change always regenerates the result
    const ObRowkeyInfo &rowkey_info = table_schema->get_rowkey_info();
    for (int64_t i = 0; OB_SUCC(ret) && !need_maintenance && i < rowkey_info.get_size(); ++i) {
      uint64_t column_id = OB_INVALID_ID;
      int64_t index = -1;
      ObDatum *old_datum = NULL;
      ObDatum *new_datum = NULL;
      if (OB_FAIL(rowkey_info.get_column_id(i, column_id))) {
      } else if (OB_FAIL(find_column_index(ctdef.column_ids_, column_id, index))) {
      } else if (OB_FAIL(ctdef.old_row_.at(index)->eval(op.get_eval_ctx(), old_datum))
                 || OB_FAIL(ctdef.new_row_.at(index)->eval(op.get_eval_ctx(), new_datum))) {
      } else if (OB_ISNULL(old_datum) || OB_ISNULL(new_datum)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (!ObDatum::binary_equal(*old_datum, *new_datum)) {
        need_maintenance = true;
      }
    }
    // dependency column change
    for (int64_t i = 0; OB_SUCC(ret) && !need_maintenance
                        && i < upd_ctdef.assign_columns_.count(); ++i) {
      const ColumnContent &content = upd_ctdef.assign_columns_.at(i);
      const uint64_t column_id = ctdef.column_ids_.at(content.projector_index_);
      for (int64_t k = 0; !need_maintenance && k < defs.count(); ++k) {
        for (int64_t d = 0; !need_maintenance && d < defs.at(k)->dep_ids_.count(); ++d) {
          if (defs.at(k)->dep_ids_.at(d) == column_id) {
            need_maintenance = true;
          }
        }
      }
    }
  }

  if (OB_FAIL(ret) || !need_maintenance) {
    // nothing to do
  } else {
    common::ObSqlString predicate;
    common::ObSqlString update_sql;
    common::ObSqlString task_sql;
    char request_id[64];
    LOG_INFO("[AI_PROBE] building task sql", "table_id", table_id, "event", (int)event);
    make_request_id(request_id, sizeof(request_id));
    const int32_t request_id_len = static_cast<int32_t>(strlen(request_id));
    const common::ObString db_name = db_schema->get_database_name_str();
    const common::ObString &table_name = table_schema->get_table_name_str();
    const ExprFixedArray &rowkey_row = (AI_DML_DELETE == event) ? ctdef.old_row_ : ctdef.new_row_;
    if (OB_FAIL(build_rowkey_predicate(op, ctdef, rowkey_row, *table_schema, predicate))) {
    } else if (AI_DML_DELETE == event) {
      if (OB_FAIL(task_sql.append_fmt("delete from oceanbase.%s where table_id = %lu and rowkey_predicate = ",
                                      OB_ALL_AI_COLUMN_TASK_TNAME, table_id))) {
      } else if (OB_FAIL(sql_append_hex_escape_str(predicate.string(), task_sql))) {
      }
    } else {
      // 1) invalidate the result columns and record the new request id
      if (OB_FAIL(update_sql.append_fmt("update `%.*s`.`%.*s` set ",
                                        db_name.length(), db_name.ptr(),
                                        table_name.length(), table_name.ptr()))) {
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < defs.count(); ++i) {
        const AiRtDef &rt_def = *defs.at(i);
        if (AI_DML_UPDATE == event
            && OB_FAIL(update_sql.append_fmt("`%.*s` = NULL, ",
                                             rt_def.def_.column_name_.length(),
                                             rt_def.def_.column_name_.ptr()))) {
        } else if (OB_FAIL(update_sql.append_fmt(
                     "`%.*s` = 'PENDING', `%.*s` = '%.*s', `%.*s` = '%lu', ",
                     rt_def.state_name_.length(), rt_def.state_name_.ptr(),
                     rt_def.request_name_.length(), rt_def.request_name_.ptr(),
                     request_id_len, request_id,
                     rt_def.spec_name_.length(), rt_def.spec_name_.ptr(),
                     rt_def.def_.definition_version_))) {
        }
      }
      if (OB_SUCC(ret)) {
        // drop the trailing ", "
        if (OB_FAIL(update_sql.set_length(update_sql.length() - 2))) {
          LOG_WARN("failed to trim sql", K(ret));
        }
        if (OB_FAIL(update_sql.append_fmt(" where %.*s",
                                          predicate.string().length(),
                                          predicate.string().ptr()))) {
        }
      }
      // 2) publish the tasks
      if (OB_SUCC(ret)
          && OB_FAIL(task_sql.append_fmt(
               "insert into oceanbase.%s (table_id, column_id, request_id, rowkey_predicate, "
               "eligible_ts, state, retry_count) values ",
               OB_ALL_AI_COLUMN_TASK_TNAME))) {
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < defs.count(); ++i) {
        const share::schema::ObColumnSchemaV2 *col_schema =
            table_schema->get_column_schema(defs.at(i)->def_.column_name_);
        uint64_t column_id = OB_INVALID_ID;
        if (OB_ISNULL(col_schema)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("[AI_PROBE] ai column schema not found", K(ret),
                   "column", defs.at(i)->def_.column_name_, K(table_id));
        } else if (OB_FAIL(task_sql.append_fmt("%s(%lu, %lu, ", 0 == i ? "" : ", ",
                                        table_id, col_schema->get_column_id()))) {
        } else if (OB_FAIL(sql_append_hex_escape_str(ObString(request_id_len, request_id),
                                                     task_sql))) {
        } else if (OB_FAIL(task_sql.append(", "))) {
        } else if (OB_FAIL(sql_append_hex_escape_str(predicate.string(), task_sql))) {
        } else if (OB_FAIL(task_sql.append(", now(6), 1, 0)"))) {
        }
      }
    }
    if (OB_SUCC(ret)) {
      session->set_in_ai_column_maintenance(true);
      // mark nested sql triggered by ai column maintenance, same as fk cascading,
      // so that the inner statement skips start/end stmt bookkeeping.
      op.get_exec_ctx().get_das_ctx().is_ai_column_cascading_ = true;
      LOG_INFO("[AI_PROBE] executing task sql", K(table_id));
      if (OB_FAIL(op.open_inner_conn())) {
        LOG_INFO("[AI_PROBE] open inner conn failed", K(ret));
      } else if (OB_FAIL(op.begin_nested_session(true))) {
        LOG_INFO("[AI_PROBE] begin nested session failed", K(ret));
      } else {
        if (AI_DML_DELETE != event && OB_FAIL(op.execute_write(update_sql.ptr()))) {
          LOG_INFO("[AI_PROBE] invalidate sql failed", K(ret), "sql", update_sql.ptr());
        } else if (OB_FAIL(op.execute_write(task_sql.ptr()))) {
          LOG_INFO("[AI_PROBE] task sql failed", K(ret), "sql", task_sql.ptr());
        }
        int end_ret = op.end_nested_session();
        if (OB_SUCCESS != end_ret) {
          LOG_INFO("[AI_PROBE] end nested session failed", K(end_ret));
          if (OB_SUCC(ret)) {
            ret = end_ret;
          }
        }
      }
      op.get_exec_ctx().get_das_ctx().is_ai_column_cascading_ = false;
      session->set_in_ai_column_maintenance(false);
    }
  }
  LOG_INFO("[AI_PROBE] process end", K(ret), "event", (int)event, K(table_id));
  return ret;
}

} // namespace sql
} // namespace oceanbase
