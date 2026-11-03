#pragma once

#include "lib/container/ob_se_array.h"
#include "lib/string/ob_string.h"
#include "sql/resolver/cmd/ob_cmd_resolver.h"

namespace oceanbase
{
namespace sql
{

struct ObFileSqlCell
{
  enum Type
  {
    NUL = 0,
    INT64 = 1,
    DOUBLE = 2,
    BOOL = 3,
    DATE = 4,
    DATETIME = 5,
    STR = 6
  };
  Type type_;
  int64_t int_val_;
  double double_val_;
  int16_t year_;
  int8_t month_;
  int8_t day_;
  int8_t hour_;
  int8_t minute_;
  int8_t second_;
  common::ObString str_;

  ObFileSqlCell()
    : type_(NUL), int_val_(0), double_val_(0.0),
      year_(0), month_(0), day_(0), hour_(0), minute_(0), second_(0)
  {}
};

struct ObFileSqlColumn
{
  common::ObString name_;
  common::ObString type_name_;
  bool nullable_;
  ObFileSqlColumn() : nullable_(true) {}
};

struct ObFileSqlMeta
{
  common::ObSEArray<ObFileSqlColumn, 4> columns_;
  common::ObSEArray<ObFileSqlCell, 16> cells_;
  int64_t row_cnt_;
  int64_t col_cnt_;

  ObFileSqlMeta() : row_cnt_(0), col_cnt_(0) {}

  const ObFileSqlCell &cell(int64_t row, int64_t col) const
  {
    return cells_.at(row * col_cnt_ + col);
  }
};

class ObCopyFileSqlResolver : public ObCMDResolver
{
public:
  explicit ObCopyFileSqlResolver(ObResolverParams &params) : ObCMDResolver(params) {}
  virtual ~ObCopyFileSqlResolver() {}
  virtual int resolve(const ParseNode &parse_tree) override;

private:
  DISALLOW_COPY_AND_ASSIGN(ObCopyFileSqlResolver);
};

} // namespace sql
} // namespace oceanbase
