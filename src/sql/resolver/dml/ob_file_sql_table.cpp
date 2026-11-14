/*
 * Copyright (c) 2026 OceanBase.
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

#include "sql/resolver/dml/ob_file_sql_table.h"

#include "sql/resolver/cmd/ob_empty_query_stmt.h"
#include "sql/resolver/dml/ob_dml_resolver.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/resolver/ob_resolver.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/ob_sql_utils.h"
#include "share/system_variable/ob_sys_var_class_type.h"
#include "lib/ob_errno.h"
#include "lib/utility/ob_macro_utils.h"
#include "common/object/ob_object.h"
#include "common/timezone/ob_time_def.h"
#include "lib/charset/ob_dtoa.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace oceanbase
{
using namespace common;
namespace sql
{
namespace
{

constexpr int64_t FILE_SQL_MAX_BYTES = 128L * 1024L * 1024L;

// File SQL is deliberately restricted to the same canonical path policy as
// LOAD DATA/SELECT ... INTO OUTFILE.  Checking the canonical path after glob
// expansion prevents both ".." traversal and symlink escapes.
int check_file_sql_path(const ObString &secure_file_priv, const std::string &path)
{
  int ret = OB_SUCCESS;
  char canonical[MAX_PATH_SIZE] = {0};
  const char *resolved = NULL;
#ifdef _WIN32
  resolved = ::_fullpath(canonical, path.c_str(), sizeof(canonical));
#else
  resolved = ::realpath(path.c_str(), canonical);
#endif
  if (OB_ISNULL(resolved)) {
    ret = OB_FILE_NOT_EXIST;
  } else {
    ObString full_path(static_cast<int32_t>(strlen(resolved)), resolved);
    ret = ObResolverUtils::check_secure_path(secure_file_priv, full_path);
  }
  return ret;
}

// COPY creates a new file, so realpath(path) cannot be used before opening it.
// Canonicalize the existing parent directory instead and reject an existing
// destination.  This keeps the same secure_file_priv and symlink policy as
// read_csv/read_json/read_parquet.
int check_copy_destination_path(const ObString &secure_file_priv, const std::string &path)
{
  int ret = OB_SUCCESS;
#ifdef _WIN32
  struct _stat st;
  if (0 == ::_stat(path.c_str(), &st)) {
#else
  struct stat st;
  if (0 == ::stat(path.c_str(), &st)) {
#endif
    ret = OB_FILE_ALREADY_EXIST;
  } else if (errno != ENOENT) {
    ret = OB_IO_ERROR;
  } else {
    const std::size_t sep = path.find_last_of("/\\");
    std::string parent = sep == std::string::npos ? "." : path.substr(0, sep);
    const std::string base = sep == std::string::npos ? path : path.substr(sep + 1);
    if (parent.empty()) {
#ifdef _WIN32
      parent = "\\";
#else
      parent = "/";
#endif
    }
    if (base.empty()) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      char canonical[MAX_PATH_SIZE] = {0};
      const char *resolved = NULL;
#ifdef _WIN32
      resolved = ::_fullpath(canonical, parent.c_str(), sizeof(canonical));
#else
      resolved = ::realpath(parent.c_str(), canonical);
#endif
      if (OB_ISNULL(resolved)) {
        ret = OB_FILE_NOT_EXIST;
      } else {
        std::string full_path(resolved);
        if (full_path.empty() || (full_path.back() != '/' && full_path.back() != '\\')) {
#ifdef _WIN32
          full_path.push_back('\\');
#else
          full_path.push_back('/');
#endif
        }
        full_path.append(base);
        ObString full(static_cast<int32_t>(full_path.length()), full_path.c_str());
        ret = ObResolverUtils::check_secure_path(secure_file_priv, full);
      }
    }
  }
  return ret;
}

bool eq_cstr(const ObString &s, const char *lit)
{
  const int64_t n = static_cast<int64_t>(strlen(lit));
  if (s.length() != n) {
    return false;
  }
  for (int64_t i = 0; i < n; ++i) {
    unsigned char c = static_cast<unsigned char>(s.ptr()[i]);
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<unsigned char>(c - 'A' + 'a');
    }
    if (c != static_cast<unsigned char>(lit[i])) {
      return false;
    }
  }
  return true;
}

int copy_text(ObIAllocator &alloc, const char *ptr, int64_t len, ObString &out)
{
  int ret = OB_SUCCESS;
  char *buf = NULL;
  if (len < 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (len == 0) {
    out.assign_ptr("", 0);
  } else if (OB_ISNULL(buf = static_cast<char *>(alloc.alloc(len)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMCPY(buf, ptr, len);
    out.assign_ptr(buf, static_cast<int32_t>(len));
  }
  return ret;
}

int copy_text(ObIAllocator &alloc, const std::string &in, ObString &out)
{
  return copy_text(alloc, in.data(), static_cast<int64_t>(in.size()), out);
}

int read_whole_file(const std::string &path, std::string &data)
{
  int ret = OB_SUCCESS;
  FILE *fp = fopen(path.c_str(), "rb");
  if (OB_ISNULL(fp)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("failed to open file sql path", K(ret), K(path.c_str()));
  } else {
    char buf[8192];
    size_t n = 0;
    while (OB_SUCC(ret) && (n = fread(buf, 1, sizeof(buf), fp)) > 0) {
      if (static_cast<int64_t>(data.size() + n) > FILE_SQL_MAX_BYTES) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "file sql input larger than 128MB");
      } else {
        data.append(buf, n);
      }
    }
    if (OB_SUCC(ret) && ferror(fp)) {
      ret = OB_IO_ERROR;
    }
    fclose(fp);
  }
  return ret;
}

struct TextCol
{
  std::string name;
  std::vector<bool> is_null;
  std::vector<std::string> text;
};

struct TextTable
{
  std::vector<TextCol> cols;
  int64_t rows;
  TextTable() : rows(0) {}
};

int parse_csv(const std::string &data, TextTable &table)
{
  int ret = OB_SUCCESS;
  std::vector<std::vector<std::string> > rows;
  std::vector<std::string> fields;
  std::string cur;
  bool in_quotes = false;
  bool started = false;
  const int64_t n = static_cast<int64_t>(data.size());
  for (int64_t i = 0; OB_SUCC(ret) && i < n; ++i) {
    const char c = data[static_cast<std::size_t>(i)];
    if (in_quotes) {
      if (c == '"') {
        if (i + 1 < n && data[static_cast<std::size_t>(i + 1)] == '"') {
          cur.push_back('"');
          ++i;
        } else {
          in_quotes = false;
        }
      } else if (c == '\r') {
        // A quoted CRLF is one newline inside the field.
      } else if (c == '\n') {
        cur.push_back('\n');
      } else {
        cur.push_back(c);
      }
    } else if (c == '"') {
      in_quotes = true;
      started = true;
    } else if (c == ',') {
      fields.push_back(cur);
      cur.clear();
      started = true;
    } else if (c == '\r') {
      // consumed; the following '\n' ends the row
    } else if (c == '\n') {
      if (started || !cur.empty() || !fields.empty()) {
        fields.push_back(cur);
        rows.push_back(fields);
      }
      fields.clear();
      cur.clear();
      started = false;
    } else {
      cur.push_back(c);
      started = true;
    }
  }
  if (OB_SUCC(ret) && (started || !cur.empty() || !fields.empty())) {
    fields.push_back(cur);
    rows.push_back(fields);
  }
  if (OB_SUCC(ret) && rows.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("csv file has no header", K(ret));
  }
  if (OB_SUCC(ret)) {
    const std::vector<std::string> &header = rows[0];
    for (std::size_t r = 1; OB_SUCC(ret) && r < rows.size(); ++r) {
      // A short or over-wide record is a malformed CSV row.  Silently
      // truncating it would shift columns and produce plausible but wrong
      // query results.
      if (rows[r].size() != header.size()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("csv row has a different field count", K(ret), K(r),
                 K(rows[r].size()), K(header.size()));
      }
    }
  }
  if (OB_SUCC(ret)) {
    const std::vector<std::string> &header = rows[0];
    table.cols.resize(header.size());
    for (std::size_t c = 0; c < header.size(); ++c) {
      int occ = 1;
      for (std::size_t k = 0; k < c; ++k) {
        if (header[k] == header[c]) {
          ++occ;
        }
      }
      table.cols[c].name = occ == 1 ? header[c] : header[c] + "__" + std::to_string(occ);
    }
    table.rows = static_cast<int64_t>(rows.size()) - 1;
    for (std::size_t c = 0; c < table.cols.size(); ++c) {
      table.cols[c].is_null.assign(static_cast<std::size_t>(table.rows), true);
      table.cols[c].text.assign(static_cast<std::size_t>(table.rows), std::string());
    }
    for (int64_t r = 0; r < table.rows; ++r) {
      const std::vector<std::string> &row = rows[static_cast<std::size_t>(r + 1)];
      for (std::size_t c = 0; c < table.cols.size() && c < row.size(); ++c) {
        if (!row[c].empty()) {
          table.cols[c].is_null[static_cast<std::size_t>(r)] = false;
          table.cols[c].text[static_cast<std::size_t>(r)] = row[c];
        }
      }
    }
  }
  return ret;
}

void skip_ws(const std::string &s, std::size_t &i)
{
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) {
    ++i;
  }
}

int parse_json_string(const std::string &s, std::size_t &i, std::string &out)
{
  int ret = OB_SUCCESS;
  out.clear();
  bool closed = false;
  if (i >= s.size() || s[i] != '"') {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ++i;
    while (OB_SUCC(ret) && i < s.size()) {
      char c = s[i++];
      if (c == '"') {
        closed = true;
        break;
      } else if (c == '\\') {
        if (i >= s.size()) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          char e = s[i++];
          if (e == '"' || e == '\\' || e == '/') {
            out.push_back(e);
          } else if (e == 'n') {
            out.push_back('\n');
          } else if (e == 'r') {
            out.push_back('\r');
          } else if (e == 't') {
            out.push_back('\t');
          } else if (e == 'u' && i + 4 <= s.size()) {
            int cp = 0;
            for (int k = 0; k < 4; ++k) {
              char h = s[i++];
              cp <<= 4;
              if (h >= '0' && h <= '9') cp += h - '0';
              else if (h >= 'a' && h <= 'f') cp += h - 'a' + 10;
              else if (h >= 'A' && h <= 'F') cp += h - 'A' + 10;
              else ret = OB_INVALID_ARGUMENT;
            }
            if (OB_SUCC(ret)) {
              if (cp < 0x80) {
                out.push_back(static_cast<char>(cp));
              } else if (cp < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
              } else {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
              }
            }
          } else {
            ret = OB_INVALID_ARGUMENT;
          }
        }
      } else {
        out.push_back(c);
      }
    }
    if (OB_SUCC(ret) && !closed) {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

bool is_json_number(const std::string &s)
{
  if (s.empty()) {
    return false;
  }
  std::size_t i = 0;
  if (s[i] == '-') {
    if (++i == s.size()) {
      return false;
    }
  }
  if (s[i] == '0') {
    ++i;
    if (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      return false;
    }
  } else if (s[i] >= '1' && s[i] <= '9') {
    while (++i < s.size() && s[i] >= '0' && s[i] <= '9') {
    }
  } else {
    return false;
  }
  if (i < s.size() && s[i] == '.') {
    ++i;
    std::size_t begin = i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      ++i;
    }
    if (begin == i) {
      return false;
    }
  }
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    ++i;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
      ++i;
    }
    std::size_t begin = i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      ++i;
    }
    if (begin == i) {
      return false;
    }
  }
  return i == s.size();
}

int parse_jsonl(const std::string &data, TextTable &table)
{
  int ret = OB_SUCCESS;
  std::size_t pos = 0;
  while (OB_SUCC(ret) && pos < data.size()) {
    std::size_t eol = data.find('\n', pos);
    if (eol == std::string::npos) {
      eol = data.size();
    }
    std::string line = data.substr(pos, eol - pos);
    pos = eol < data.size() ? eol + 1 : data.size();
    if (!line.empty() && line[line.size() - 1] == '\r') {
      line.erase(line.size() - 1);
    }
    if (line.empty()) {
      continue;
    }
    std::size_t i = 0;
    skip_ws(line, i);
    if (i >= line.size() || line[i] != '{') {
      ret = OB_INVALID_ARGUMENT;
    } else {
      ++i;
      std::vector<std::pair<std::string, std::string> > kvs;
      std::vector<bool> present;
      bool first = true;
      while (OB_SUCC(ret)) {
        skip_ws(line, i);
        if (i < line.size() && line[i] == '}') {
          ++i;
          break;
        }
        if (!first) {
          if (i >= line.size() || line[i] != ',') {
            ret = OB_INVALID_ARGUMENT;
            break;
          }
          ++i;
          skip_ws(line, i);
        }
        first = false;
        std::string key;
        std::string val;
        bool is_null = false;
        if (OB_FAIL(parse_json_string(line, i, key))) {
        } else {
          skip_ws(line, i);
          if (i >= line.size() || line[i] != ':') {
            ret = OB_INVALID_ARGUMENT;
          } else {
            ++i;
            skip_ws(line, i);
            if (i + 4 <= line.size() && line.compare(i, 4, "null") == 0) {
              is_null = true;
              i += 4;
            } else if (i + 4 <= line.size() && line.compare(i, 4, "true") == 0) {
              val = "true";
              i += 4;
            } else if (i + 5 <= line.size() && line.compare(i, 5, "false") == 0) {
              val = "false";
              i += 5;
            } else if (i < line.size() && line[i] == '"') {
              if (OB_FAIL(parse_json_string(line, i, val))) {
              }
            } else if (i < line.size() && (line[i] == '{' || line[i] == '[')) {
              // The file SQL JSONL contract is intentionally limited to
              // flat scalar objects; nested objects and arrays are rejected
              // instead of being misread as truncated strings.
              ret = OB_NOT_SUPPORTED;
            } else {
              std::size_t start = i;
              while (i < line.size() && line[i] != ',' && line[i] != '}' && line[i] != ' ') {
                ++i;
              }
              val = line.substr(start, i - start);
              if (val.empty() || !is_json_number(val)) {
                ret = OB_INVALID_ARGUMENT;
              }
            }
          }
        }
        if (OB_SUCC(ret)) {
          skip_ws(line, i);
          if (i >= line.size() || (line[i] != ',' && line[i] != '}')) {
            ret = OB_INVALID_ARGUMENT;
          }
        }
        if (OB_SUCC(ret)) {
          int idx = -1;
          for (std::size_t c = 0; c < table.cols.size(); ++c) {
            if (table.cols[c].name == key) {
              idx = static_cast<int>(c);
              break;
            }
          }
          if (idx < 0) {
            TextCol col;
            col.name = key;
            col.is_null.assign(static_cast<std::size_t>(table.rows), true);
            col.text.assign(static_cast<std::size_t>(table.rows), std::string());
            table.cols.push_back(col);
            idx = static_cast<int>(table.cols.size()) - 1;
          }
          kvs.push_back(std::make_pair(key, is_null ? std::string() : val));
          present.push_back(!is_null);
          (void)idx;
        }
      }
      if (OB_SUCC(ret)) {
        skip_ws(line, i);
        if (i != line.size()) {
          ret = OB_INVALID_ARGUMENT;
        }
      }
      if (OB_SUCC(ret)) {
        for (std::size_t c = 0; c < table.cols.size(); ++c) {
          table.cols[c].is_null.push_back(true);
          table.cols[c].text.push_back(std::string());
        }
        for (std::size_t k = 0; k < kvs.size(); ++k) {
          for (std::size_t c = 0; c < table.cols.size(); ++c) {
            if (table.cols[c].name == kvs[k].first) {
              table.cols[c].is_null[static_cast<std::size_t>(table.rows)] = !present[k];
              if (present[k]) {
                table.cols[c].text[static_cast<std::size_t>(table.rows)] = kvs[k].second;
              }
              break;
            }
          }
        }
        ++table.rows;
      }
    }
  }
  return ret;
}

bool parse_i64(const std::string &s, int64_t &out)
{
  if (s.empty()) {
    return false;
  }
  std::size_t i = 0;
  int sign = 1;
  if (s[0] == '-') {
    sign = -1;
    i = 1;
    if (s.size() == 1) {
      return false;
    }
  }
  uint64_t v = 0;
  const uint64_t limit = sign < 0 ? static_cast<uint64_t>(INT64_MAX) + 1ULL
                                 : static_cast<uint64_t>(INT64_MAX);
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') {
      return false;
    }
    const uint64_t digit = static_cast<uint64_t>(s[i] - '0');
    if (v > (limit - digit) / 10) {
      return false;
    }
    v = v * 10 + digit;
  }
  if (sign < 0) {
    if (v == static_cast<uint64_t>(INT64_MAX) + 1ULL) {
      out = INT64_MIN;
    } else {
      out = -static_cast<int64_t>(v);
    }
  } else {
    out = static_cast<int64_t>(v);
  }
  return true;
}

bool parse_f64(const std::string &s, double &out)
{
  if (s.find('.') == std::string::npos && s.find('e') == std::string::npos
      && s.find('E') == std::string::npos) {
    return false;
  }
  char *end = NULL;
  out = strtod(s.c_str(), &end);
  return end == s.c_str() + s.size() && std::isfinite(out);
}

bool parse_ymd(const char *s, int &y, int &m, int &d)
{
  for (int i = 0; i < 10; ++i) {
    if (i == 4 || i == 7) {
      if (s[i] != '-') return false;
    } else if (s[i] < '0' || s[i] > '9') {
      return false;
    }
  }
  y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
  m = (s[5] - '0') * 10 + (s[6] - '0');
  d = (s[8] - '0') * 10 + (s[9] - '0');
  if (m < 1 || m > 12 || d < 1) {
    return false;
  }
  const bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
  static const int days_in_month[] = {0, 31, 28, 31, 30, 31, 30,
                                      31, 31, 30, 31, 30, 31};
  const int max_day = m == 2 && leap ? 29 : days_in_month[m];
  return d <= max_day;
}

bool parse_date_text(const std::string &s, int &y, int &m, int &d)
{
  return s.size() == 10 && parse_ymd(s.c_str(), y, m, d);
}

bool parse_datetime_text(const std::string &s, int &y, int &m, int &d, int &hh, int &mi, int &ss)
{
  if (s.size() != 19 || (s[10] != ' ' && s[10] != 'T') || s[13] != ':' || s[16] != ':') {
    return false;
  }
  if (!parse_ymd(s.c_str(), y, m, d)) {
    return false;
  }
  if (s[11] < '0' || s[11] > '9' || s[12] < '0' || s[12] > '9'
      || s[14] < '0' || s[14] > '9' || s[15] < '0' || s[15] > '9'
      || s[17] < '0' || s[17] > '9' || s[18] < '0' || s[18] > '9') {
    return false;
  }
  hh = (s[11] - '0') * 10 + (s[12] - '0');
  mi = (s[14] - '0') * 10 + (s[15] - '0');
  ss = (s[17] - '0') * 10 + (s[18] - '0');
  return hh <= 23 && mi <= 59 && ss <= 59;
}

enum LitKind { LIT_STR, LIT_INT, LIT_DBL, LIT_BOOL, LIT_DATE, LIT_DT };

LitKind classify_text(const std::string &s)
{
  int y = 0, m = 0, d = 0, hh = 0, mi = 0, ss = 0;
  int64_t iv = 0;
  double dv = 0;
  std::string lower = s;
  for (std::size_t i = 0; i < lower.size(); ++i) {
    if (lower[i] >= 'A' && lower[i] <= 'Z') {
      lower[i] = static_cast<char>(lower[i] - 'A' + 'a');
    }
  }
  if (lower == "true" || lower == "false") {
    return LIT_BOOL;
  } else if (parse_datetime_text(s, y, m, d, hh, mi, ss)) {
    return LIT_DT;
  } else if (parse_date_text(s, y, m, d)) {
    return LIT_DATE;
  } else if (parse_i64(s, iv)) {
    return LIT_INT;
  } else if (parse_f64(s, dv)) {
    return LIT_DBL;
  }
  return LIT_STR;
}

int infer_text_table(TextTable &table, ObIAllocator &alloc, ObFileSqlMeta &meta)
{
  int ret = OB_SUCCESS;
  meta.col_cnt_ = static_cast<int64_t>(table.cols.size());
  meta.row_cnt_ = table.rows;
  std::vector<ObFileSqlCell::Type> kinds(table.cols.size(), ObFileSqlCell::STR);
  for (std::size_t c = 0; OB_SUCC(ret) && c < table.cols.size(); ++c) {
    bool any = false;
    bool all_bool = true, all_int = true, all_num = true, all_date = true, all_dt = true;
    bool nullable = false;
    for (int64_t r = 0; r < table.rows; ++r) {
      if (table.cols[c].is_null[static_cast<std::size_t>(r)]) {
        nullable = true;
        continue;
      }
      any = true;
      LitKind k = classify_text(table.cols[c].text[static_cast<std::size_t>(r)]);
      if (k != LIT_BOOL) all_bool = false;
      if (k != LIT_INT) all_int = false;
      if (k != LIT_INT && k != LIT_DBL) all_num = false;
      if (k != LIT_DATE) all_date = false;
      if (k != LIT_DT) all_dt = false;
    }
    ObFileSqlCell::Type kind = ObFileSqlCell::STR;
    const char *type_name = "VARCHAR";
    if (!any) {
      kind = ObFileSqlCell::STR;
      type_name = "VARCHAR";
      nullable = true;
    } else if (all_bool) {
      kind = ObFileSqlCell::BOOL;
      type_name = "BOOLEAN";
    } else if (all_int) {
      kind = ObFileSqlCell::INT64;
      type_name = "BIGINT";
    } else if (all_num) {
      kind = ObFileSqlCell::DOUBLE;
      type_name = "DOUBLE";
    } else if (all_dt) {
      kind = ObFileSqlCell::DATETIME;
      type_name = "DATETIME";
    } else if (all_date) {
      kind = ObFileSqlCell::DATE;
      type_name = "DATE";
    }
    kinds[c] = kind;
    ObFileSqlColumn col;
    if (OB_FAIL(copy_text(alloc, table.cols[c].name, col.name_))) {
    } else {
      col.type_name_ = ObString::make_string(type_name);
      col.nullable_ = nullable;
      if (OB_FAIL(meta.columns_.push_back(col))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      }
    }
  }
  // ObFileSqlMeta::cell(row, col) addresses cells row by row, so cells must be
  // appended in row-major order.  The text formats (CSV/JSONL) used to append
  // them column by column, which transposed the table and made every projected
  // column read the values of another column.
  for (int64_t r = 0; OB_SUCC(ret) && r < table.rows; ++r) {
    for (std::size_t c = 0; OB_SUCC(ret) && c < table.cols.size(); ++c) {
      ObFileSqlCell cell;
      if (table.cols[c].is_null[static_cast<std::size_t>(r)]) {
        cell.type_ = ObFileSqlCell::NUL;
      } else {
        const std::string &text = table.cols[c].text[static_cast<std::size_t>(r)];
        const ObFileSqlCell::Type kind = kinds[c];
        cell.type_ = kind;
        int y = 0, m = 0, d = 0, hh = 0, mi = 0, ss = 0;
        if (kind == ObFileSqlCell::BOOL) {
          std::string lower = text;
          for (std::size_t i = 0; i < lower.size(); ++i) {
            if (lower[i] >= 'A' && lower[i] <= 'Z') lower[i] = static_cast<char>(lower[i] - 'A' + 'a');
          }
          cell.int_val_ = lower == "true" ? 1 : 0;
        } else if (kind == ObFileSqlCell::INT64) {
          parse_i64(text, cell.int_val_);
        } else if (kind == ObFileSqlCell::DOUBLE) {
          int64_t iv = 0;
          if (parse_i64(text, iv)) {
            cell.double_val_ = static_cast<double>(iv);
          } else {
            parse_f64(text, cell.double_val_);
          }
        } else if (kind == ObFileSqlCell::DATE) {
          parse_date_text(text, y, m, d);
          cell.year_ = static_cast<int16_t>(y);
          cell.month_ = static_cast<int8_t>(m);
          cell.day_ = static_cast<int8_t>(d);
        } else if (kind == ObFileSqlCell::DATETIME) {
          parse_datetime_text(text, y, m, d, hh, mi, ss);
          cell.year_ = static_cast<int16_t>(y);
          cell.month_ = static_cast<int8_t>(m);
          cell.day_ = static_cast<int8_t>(d);
          cell.hour_ = static_cast<int8_t>(hh);
          cell.minute_ = static_cast<int8_t>(mi);
          cell.second_ = static_cast<int8_t>(ss);
        } else if (OB_FAIL(copy_text(alloc, text, cell.str_))) {
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(meta.cells_.push_back(cell))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      }
    }
  }
  return ret;
}

struct PNode
{
  int64_t fid;
  int typ;
  int64_t i64;
  const unsigned char *bytes;
  int64_t len;
  PNode *child;
  PNode *next;
  PNode() : fid(0), typ(0), i64(0), bytes(NULL), len(0), child(NULL), next(NULL) {}
};

struct PBuf
{
  const unsigned char *p;
  int64_t n;
  int64_t i;
  int64_t last;
  ObArenaAllocator *arena;
  int err;
  PBuf() : p(NULL), n(0), i(0), last(0), arena(NULL), err(OB_SUCCESS) {}
};

int pb_need(PBuf &b, int64_t k)
{
  if (b.err != OB_SUCCESS) {
    return b.err;
  }
  if (b.i + k > b.n) {
    b.err = OB_INVALID_ARGUMENT;
  }
  return b.err;
}

uint64_t pb_varint(PBuf &b)
{
  uint64_t v = 0;
  int shift = 0;
  while (b.err == OB_SUCCESS) {
    if (pb_need(b, 1) != OB_SUCCESS) {
      break;
    }
    unsigned char c = b.p[b.i++];
    v |= static_cast<uint64_t>(c & 0x7F) << shift;
    if ((c & 0x80) == 0) {
      break;
    }
    shift += 7;
    if (shift > 63) {
      b.err = OB_INVALID_ARGUMENT;
    }
  }
  return v;
}

int64_t pb_zigzag(PBuf &b)
{
  uint64_t n = pb_varint(b);
  return static_cast<int64_t>((n >> 1) ^ (0 - (n & 1)));
}

PNode *pb_new(PBuf &b)
{
  void *mem = NULL;
  PNode *n = NULL;
  if (b.err == OB_SUCCESS) {
    mem = b.arena->alloc(sizeof(PNode));
    if (mem == NULL) {
      b.err = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      n = new (mem) PNode();
    }
  }
  return n;
}

PNode *pb_value(PBuf &b, int typ);

PNode *pb_value(PBuf &b, int typ)
{
  PNode *node = pb_new(b);
  if (node == NULL) {
    return NULL;
  }
  node->typ = typ;
  if (typ == 1) {
    node->i64 = 1;
  } else if (typ == 2) {
    node->i64 = 0;
  } else if (typ == 3) {
    if (pb_need(b, 1) == OB_SUCCESS) {
      node->i64 = b.p[b.i++];
    }
  } else if (typ == 4 || typ == 5 || typ == 6) {
    node->i64 = pb_zigzag(b);
  } else if (typ == 7) {
    if (pb_need(b, 8) == OB_SUCCESS) {
      node->bytes = b.p + b.i;
      node->len = 8;
      b.i += 8;
    }
  } else if (typ == 8) {
    uint64_t len = pb_varint(b);
    if (pb_need(b, static_cast<int64_t>(len)) == OB_SUCCESS) {
      node->bytes = b.p + b.i;
      node->len = static_cast<int64_t>(len);
      b.i += static_cast<int64_t>(len);
    }
  } else if (typ == 9) {
    if (pb_need(b, 1) != OB_SUCCESS) {
      return node;
    }
    unsigned char h = b.p[b.i++];
    int et = h & 0x0F;
    int64_t sz = h >> 4;
    if (sz == 15) {
      sz = static_cast<int64_t>(pb_varint(b));
    }
    PNode *tail = NULL;
    for (int64_t i = 0; i < sz && b.err == OB_SUCCESS; ++i) {
      PNode *item = pb_value(b, et);
      if (item == NULL) {
        break;
      }
      item->fid = i;
      if (tail == NULL) {
        node->child = item;
      } else {
        tail->next = item;
      }
      tail = item;
    }
  } else if (typ == 12) {
    int64_t saved = b.last;
    b.last = 0;
    PNode *tail = NULL;
    while (b.err == OB_SUCCESS) {
      if (pb_need(b, 1) != OB_SUCCESS) {
        break;
      }
      unsigned char h = b.p[b.i++];
      if (h == 0) {
        break;
      }
      int ft = h & 0x0F;
      int delta = h >> 4;
      int64_t fid = 0;
      if (delta == 0) {
        fid = pb_zigzag(b);
      } else {
        fid = b.last + delta;
      }
      b.last = fid;
      PNode *field = pb_value(b, ft);
      if (field == NULL) {
        break;
      }
      field->fid = fid;
      if (tail == NULL) {
        node->child = field;
      } else {
        tail->next = field;
      }
      tail = field;
    }
    b.last = saved;
  } else {
    b.err = OB_NOT_SUPPORTED;
  }
  return node;
}

const PNode *pfield(const PNode *s, int64_t fid)
{
  for (const PNode *n = s == NULL ? NULL : s->child; n != NULL; n = n->next) {
    if (n->fid == fid) {
      return n;
    }
  }
  return NULL;
}

int snappy_uncompress(const unsigned char *src, int64_t src_len, std::string &out)
{
  int ret = OB_SUCCESS;
  int64_t i = 0;
  auto varint = [&](uint64_t &v) -> int {
    v = 0;
    int shift = 0;
    while (true) {
      if (i >= src_len) {
        return OB_INVALID_ARGUMENT;
      }
      unsigned char c = src[i++];
      v |= static_cast<uint64_t>(c & 0x7F) << shift;
      if ((c & 0x80) == 0) {
        return OB_SUCCESS;
      }
      shift += 7;
    }
  };
  uint64_t ulen = 0;
  if (OB_FAIL(varint(ulen))) {
  } else if (ulen > static_cast<uint64_t>(FILE_SQL_MAX_BYTES)) {
    ret = OB_NOT_SUPPORTED;
  } else {
    out.clear();
    out.reserve(static_cast<std::size_t>(ulen));
    while (OB_SUCC(ret) && out.size() < ulen) {
      if (i >= src_len) {
        ret = OB_INVALID_ARGUMENT;
        break;
      }
      unsigned char tag = src[i++];
      int typ = tag & 3;
      int64_t len = 0;
      int64_t off = 0;
      if (typ == 0) {
        len = tag >> 2;
        if (len < 60) {
          len += 1;
        } else {
          int extra = static_cast<int>(len - 59);
          if (i + extra > src_len) {
            ret = OB_INVALID_ARGUMENT;
            break;
          }
          len = 0;
          for (int k = 0; k < extra; ++k) {
            len |= static_cast<int64_t>(src[i++]) << (8 * k);
          }
          len += 1;
        }
        if (i + len > src_len) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          out.append(reinterpret_cast<const char *>(src + i), static_cast<std::size_t>(len));
          i += len;
        }
      } else {
        if (typ == 1) {
          len = ((tag >> 2) & 7) + 4;
          if (i >= src_len) {
            ret = OB_INVALID_ARGUMENT;
            break;
          }
          off = ((tag & 0xE0) << 3) | src[i++];
        } else if (typ == 2) {
          len = (tag >> 2) + 1;
          if (i + 2 > src_len) {
            ret = OB_INVALID_ARGUMENT;
            break;
          }
          off = src[i] | (static_cast<int>(src[i + 1]) << 8);
          i += 2;
        } else {
          len = (tag >> 2) + 1;
          if (i + 4 > src_len) {
            ret = OB_INVALID_ARGUMENT;
            break;
          }
          off = src[i] | (static_cast<int>(src[i + 1]) << 8)
              | (static_cast<int>(src[i + 2]) << 16) | (static_cast<int>(src[i + 3]) << 24);
          i += 4;
        }
        if (OB_SUCC(ret) && (off <= 0 || off > static_cast<int64_t>(out.size()))) {
          ret = OB_INVALID_ARGUMENT;
        }
        for (int64_t k = 0; OB_SUCC(ret) && k < len; ++k) {
          out.push_back(out[out.size() - static_cast<std::size_t>(off)]);
        }
      }
    }
    if (OB_SUCC(ret) && out.size() != ulen) {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

int decode_plain(const std::string &raw, int physical, int64_t count,
                 std::vector<ObFileSqlCell> &cells, ObIAllocator &alloc)
{
  int ret = OB_SUCCESS;
  const unsigned char *p = reinterpret_cast<const unsigned char *>(raw.data());
  int64_t n = static_cast<int64_t>(raw.size());
  int64_t i = 0;
  for (int64_t r = 0; OB_SUCC(ret) && r < count; ++r) {
    ObFileSqlCell cell;
    if (physical == 1 || physical == 2) {
      int width = physical == 1 ? 4 : 8;
      if (i + width > n) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        int64_t v = 0;
        if (physical == 1) {
          uint32_t u = static_cast<uint32_t>(p[i])
              | (static_cast<uint32_t>(p[i + 1]) << 8)
              | (static_cast<uint32_t>(p[i + 2]) << 16)
              | (static_cast<uint32_t>(p[i + 3]) << 24);
          v = static_cast<int32_t>(u);
        } else {
          uint64_t u = 0;
          for (int b = 0; b < 8; ++b) {
            u |= static_cast<uint64_t>(p[i + b]) << (8 * b);
          }
          v = static_cast<int64_t>(u);
        }
        i += width;
        cell.type_ = ObFileSqlCell::INT64;
        cell.int_val_ = v;
      }
    } else if (physical == 4 || physical == 5) {
      int width = physical == 4 ? 4 : 8;
      if (i + width > n) {
        ret = OB_INVALID_ARGUMENT;
      } else if (physical == 4) {
        float f = 0;
        MEMCPY(&f, p + i, 4);
        cell.type_ = ObFileSqlCell::DOUBLE;
        cell.double_val_ = static_cast<double>(f);
        i += 4;
      } else {
        double d = 0;
        MEMCPY(&d, p + i, 8);
        cell.type_ = ObFileSqlCell::DOUBLE;
        cell.double_val_ = d;
        i += 8;
      }
    } else if (physical == 0) {
      if (i >= n) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        cell.type_ = ObFileSqlCell::BOOL;
        cell.int_val_ = p[i++] ? 1 : 0;
      }
    } else if (physical == 6) {
      if (i + 4 > n) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        uint32_t len = 0;
        MEMCPY(&len, p + i, 4);
        i += 4;
        if (i + static_cast<int64_t>(len) > n) {
          ret = OB_INVALID_ARGUMENT;
        } else if (OB_FAIL(copy_text(alloc, reinterpret_cast<const char *>(p + i), len, cell.str_))) {
        } else {
          cell.type_ = ObFileSqlCell::STR;
          i += len;
        }
      }
    } else {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "parquet physical type");
    }
    if (OB_SUCC(ret)) {
      cells.push_back(cell);
    }
  }
  return ret;
}

int read_parquet_pages(const std::string &file, int64_t page_off, int64_t chunk_bytes,
                       int codec, int physical, int64_t expect,
                       std::vector<ObFileSqlCell> &cells, ObIAllocator &alloc)
{
  int ret = OB_SUCCESS;
  int64_t off = page_off;
  const int64_t end = page_off + chunk_bytes;
  const int64_t start_count = static_cast<int64_t>(cells.size());
  while (OB_SUCC(ret) && static_cast<int64_t>(cells.size()) - start_count < expect && off < end) {
    ObArenaAllocator arena("FileSqlParquet");
    PBuf b;
    b.p = reinterpret_cast<const unsigned char *>(file.data());
    b.n = static_cast<int64_t>(file.size());
    b.i = off;
    b.arena = &arena;
    PNode *page = pb_value(b, 12);
    if (b.err != OB_SUCCESS) {
      ret = b.err;
    } else if (OB_ISNULL(page)) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      const PNode *type = pfield(page, 1);
      const PNode *comp_sz = pfield(page, 3);
      const PNode *v2 = pfield(page, 8);
      if (OB_ISNULL(type) || OB_ISNULL(comp_sz) || type->i64 != 3 || OB_ISNULL(v2)) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "parquet page type");
      } else {
        const PNode *num_values = pfield(v2, 1);
        const PNode *num_nulls = pfield(v2, 2);
        const PNode *encoding = pfield(v2, 4);
        const PNode *def_len = pfield(v2, 5);
        const PNode *rep_len = pfield(v2, 6);
        const PNode *is_comp = pfield(v2, 7);
        int64_t compressed = comp_sz->i64;
        int64_t header_end = b.i;
        if (OB_ISNULL(num_values) || OB_ISNULL(encoding) || encoding->i64 != 0) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "parquet encoding");
        } else if (num_nulls != NULL && num_nulls->i64 != 0) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "parquet nulls");
        } else if (header_end + compressed > static_cast<int64_t>(file.size())) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          int64_t levels = (rep_len == NULL ? 0 : rep_len->i64) + (def_len == NULL ? 0 : def_len->i64);
          if (levels > compressed) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            const unsigned char *payload =
                reinterpret_cast<const unsigned char *>(file.data()) + header_end + levels;
            int64_t payload_len = compressed - levels;
            std::string raw;
            bool compressed_values = codec != 0 && (is_comp == NULL || is_comp->i64 != 0);
            if (!compressed_values) {
              raw.assign(reinterpret_cast<const char *>(payload), static_cast<std::size_t>(payload_len));
            } else if (codec == 1) {
              if (OB_FAIL(snappy_uncompress(payload, payload_len, raw))) {
              }
            } else {
              ret = OB_NOT_SUPPORTED;
              LOG_USER_ERROR(OB_NOT_SUPPORTED, "parquet compression codec");
            }
            if (OB_SUCC(ret)) {
              if (OB_FAIL(decode_plain(raw, physical, num_values->i64, cells, alloc))) {
              }
            }
          }
        }
        off = header_end + compressed;
      }
    }
  }
  if (OB_SUCC(ret) && static_cast<int64_t>(cells.size()) - start_count != expect) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("parquet value count mismatch", K(ret), K(expect), K(cells.size()));
  }
  return ret;
}

int parse_parquet(const std::string &file, ObIAllocator &alloc, ObFileSqlMeta &meta)
{
  int ret = OB_SUCCESS;
  if (file.size() < 12 || file.compare(0, 4, "PAR1") != 0
      || file.compare(file.size() - 4, 4, "PAR1") != 0) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    uint32_t footer_len = 0;
    MEMCPY(&footer_len, file.data() + file.size() - 8, 4);
    if (static_cast<int64_t>(footer_len) + 8 > static_cast<int64_t>(file.size())) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      ObArenaAllocator arena("FileSqlFooter");
      PBuf b;
      b.p = reinterpret_cast<const unsigned char *>(file.data()) + file.size() - 8 - footer_len;
      b.n = footer_len;
      b.arena = &arena;
      PNode *root = pb_value(b, 12);
      if (b.err != OB_SUCCESS) {
        ret = b.err;
      } else if (OB_ISNULL(root) || OB_ISNULL(pfield(root, 2)) || OB_ISNULL(pfield(root, 4))) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        struct Leaf {
          std::string name;
          int physical;
          bool nullable;
        };
        std::vector<Leaf> leaves;
        for (const PNode *se = pfield(root, 2)->child; se != NULL && OB_SUCC(ret); se = se->next) {
          const PNode *type = pfield(se, 1);
          const PNode *name = pfield(se, 4);
          if (type == NULL || name == NULL || name->bytes == NULL) {
            continue;
          }
          Leaf leaf;
          leaf.name.assign(reinterpret_cast<const char *>(name->bytes), static_cast<std::size_t>(name->len));
          leaf.physical = static_cast<int>(type->i64);
          const PNode *rep = pfield(se, 3);
          leaf.nullable = rep != NULL && rep->i64 != 0;
          leaves.push_back(leaf);
        }
        std::vector<std::vector<ObFileSqlCell> > col_cells(leaves.size());
        for (const PNode *rg = pfield(root, 4)->child; rg != NULL && OB_SUCC(ret); rg = rg->next) {
          const PNode *cols = pfield(rg, 1);
          int64_t idx = 0;
          for (const PNode *chunk = cols == NULL ? NULL : cols->child;
               chunk != NULL && OB_SUCC(ret); chunk = chunk->next, ++idx) {
            const PNode *cm = pfield(chunk, 3);
            if (OB_ISNULL(cm) || idx >= static_cast<int64_t>(leaves.size())) {
              ret = OB_INVALID_ARGUMENT;
            } else {
              const PNode *phys = pfield(cm, 1);
              const PNode *codec = pfield(cm, 4);
              const PNode *nvals = pfield(cm, 5);
              const PNode *csz = pfield(cm, 7);
              const PNode *off = pfield(cm, 9);
              if (OB_ISNULL(phys) || OB_ISNULL(codec) || OB_ISNULL(nvals) || OB_ISNULL(csz) || OB_ISNULL(off)) {
                ret = OB_INVALID_ARGUMENT;
              } else if (OB_FAIL(read_parquet_pages(file, off->i64, csz->i64,
                                                    static_cast<int>(codec->i64),
                                                    static_cast<int>(phys->i64),
                                                    nvals->i64, col_cells[static_cast<std::size_t>(idx)], alloc))) {
              }
            }
          }
        }
        if (OB_SUCC(ret)) {
          int64_t rows = leaves.empty() ? 0 : static_cast<int64_t>(col_cells[0].size());
          meta.row_cnt_ = rows;
          meta.col_cnt_ = static_cast<int64_t>(leaves.size());
          for (std::size_t c = 0; OB_SUCC(ret) && c < leaves.size(); ++c) {
            ObFileSqlColumn col;
            const char *type_name = "VARCHAR";
            if (leaves[c].physical == 0) type_name = "BOOLEAN";
            else if (leaves[c].physical == 1 || leaves[c].physical == 2) type_name = "BIGINT";
            else if (leaves[c].physical == 4 || leaves[c].physical == 5) type_name = "DOUBLE";
            if (OB_FAIL(copy_text(alloc, leaves[c].name, col.name_))) {
            } else {
              col.type_name_ = ObString::make_string(type_name);
              col.nullable_ = leaves[c].nullable;
              if (OB_FAIL(meta.columns_.push_back(col))) {
                ret = OB_ALLOCATE_MEMORY_FAILED;
              }
            }
          }
          for (int64_t r = 0; OB_SUCC(ret) && r < rows; ++r) {
            for (std::size_t c = 0; OB_SUCC(ret) && c < leaves.size(); ++c) {
              if (OB_FAIL(meta.cells_.push_back(col_cells[c][static_cast<std::size_t>(r)]))) {
                ret = OB_ALLOCATE_MEMORY_FAILED;
              }
            }
          }
        }
      }
    }
  }
  return ret;
}

int list_glob(const std::string &pattern, std::vector<std::string> &paths)
{
  int ret = OB_SUCCESS;
  std::string dir = ".";
  std::string pat = pattern;
  std::size_t slash = pattern.find_last_of("/\\");
  if (slash != std::string::npos) {
    dir = pattern.substr(0, slash);
    pat = pattern.substr(slash + 1);
    if (dir.empty()) {
      dir = "/";
    }
  }
  std::size_t star = pat.find('*');
  std::string pre = star == std::string::npos ? pat : pat.substr(0, star);
  std::string suf = star == std::string::npos ? std::string() : pat.substr(star + 1);
  if (suf.find('*') != std::string::npos) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "glob with multiple stars");
  } else {
#if defined(_WIN32)
    std::string spec = dir;
    if (!spec.empty() && spec[spec.size() - 1] != '/' && spec[spec.size() - 1] != '\\') {
      spec.push_back('/');
    }
    spec.append("*");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(spec.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
          continue;
        }
        std::string name = fd.cFileName;
        bool ok = star == std::string::npos ? name == pat
            : name.size() >= pre.size() + suf.size()
              && name.compare(0, pre.size(), pre) == 0
              && (suf.empty() || name.compare(name.size() - suf.size(), suf.size(), suf) == 0);
        if (ok) {
          std::string full = dir;
          if (!full.empty() && full[full.size() - 1] != '/' && full[full.size() - 1] != '\\') {
            full.push_back('/');
          }
          full.append(name);
          paths.push_back(full);
        }
      } while (FindNextFileA(h, &fd));
      FindClose(h);
    }
#else
    DIR *dp = opendir(dir.c_str());
    if (dp == NULL) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      struct dirent *ent = NULL;
      while ((ent = readdir(dp)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
          continue;
        }
        std::string name = ent->d_name;
        bool ok = star == std::string::npos ? name == pat
            : name.size() >= pre.size() + suf.size()
              && name.compare(0, pre.size(), pre) == 0
              && (suf.empty() || name.compare(name.size() - suf.size(), suf.size(), suf) == 0);
        if (!ok) {
          continue;
        }
        std::string full = dir;
        if (!full.empty() && full[full.size() - 1] != '/') {
          full.push_back('/');
        }
        full.append(name);
        struct stat st;
        if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
          continue;
        }
        paths.push_back(full);
      }
      closedir(dp);
    }
#endif
  }
  return ret;
}

int fill_glob(const std::vector<std::string> &paths, ObIAllocator &alloc, ObFileSqlMeta &meta)
{
  int ret = OB_SUCCESS;
  ObFileSqlColumn col;
  col.name_ = ObString::make_string("file");
  col.type_name_ = ObString::make_string("VARCHAR");
  col.nullable_ = false;
  meta.col_cnt_ = 1;
  meta.row_cnt_ = static_cast<int64_t>(paths.size());
  if (OB_FAIL(meta.columns_.push_back(col))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  for (std::size_t i = 0; OB_SUCC(ret) && i < paths.size(); ++i) {
    ObFileSqlCell cell;
    cell.type_ = ObFileSqlCell::STR;
    if (OB_FAIL(copy_text(alloc, paths[i], cell.str_))) {
    } else if (OB_FAIL(meta.cells_.push_back(cell))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }
  }
  return ret;
}

int cell_to_obj(const ObFileSqlCell &cell, ObCollationType cs, ObObj &obj)
{
  int ret = OB_SUCCESS;
  switch (cell.type_) {
  case ObFileSqlCell::NUL:
    obj.set_null();
    break;
  case ObFileSqlCell::INT64:
  case ObFileSqlCell::BOOL:
    obj.set_int(cell.int_val_);
    break;
  case ObFileSqlCell::DOUBLE:
    obj.set_double(cell.double_val_);
    break;
  case ObFileSqlCell::DATE: {
    ObMySQLDate md;
    md.date_ = 0;
    md.year_ = cell.year_;
    md.month_ = cell.month_;
    md.day_ = cell.day_;
    obj.set_mysql_date(md);
    break;
  }
  case ObFileSqlCell::DATETIME: {
    ObMySQLDateTime dt;
    dt.datetime_ = 0;
    dt.year_month_ = ObMySQLDateTime::year_month(cell.year_, cell.month_);
    dt.day_ = cell.day_;
    dt.hour_ = cell.hour_;
    dt.minute_ = cell.minute_;
    dt.second_ = cell.second_;
    dt.microseconds_ = 0;
    obj.set_mysql_datetime(dt);
    break;
  }
  case ObFileSqlCell::STR:
    obj.set_string(ObVarcharType, cell.str_);
    obj.set_collation_type(cs);
    obj.set_collation_level(CS_LEVEL_COERCIBLE);
    break;
  default:
    ret = OB_ERR_UNEXPECTED;
    break;
  }
  return ret;
}

} // namespace

int ObDMLResolver::resolve_file_sql_table_item(const ParseNode &table_node, TableItem *&table_item)
{
  int ret = OB_SUCCESS;
  ObDMLStmt *stmt = get_stmt();
  bool enabled = false;
  ObString secure_file_priv;
  if (OB_ISNULL(stmt) || OB_ISNULL(allocator_) || OB_ISNULL(session_info_)
      || OB_ISNULL(params_.expr_factory_)
      || table_node.type_ != T_FILE_SQL_TABLE || table_node.num_child_ < 1
      || OB_ISNULL(table_node.children_) || OB_ISNULL(table_node.children_[0])) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(session_info_->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
    LOG_WARN("failed to read enable_file_sql", K(ret));
  } else if (!enabled) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "file sql");
  } else if (OB_FAIL(session_info_->get_secure_file_priv(secure_file_priv))) {
    LOG_WARN("failed to read secure_file_priv", K(ret));
  } else {
    bool header = true;
    bool saw_sample = false;
    int64_t sample_size = -1;
    ObString format;
    ParseNode *opt_list = table_node.num_child_ > 1 ? table_node.children_[1] : NULL;
    for (int64_t i = 0; OB_SUCC(ret) && opt_list != NULL && i < opt_list->num_child_; ++i) {
      ParseNode *opt = opt_list->children_[i];
      ObRawExpr *val_expr = NULL;
      if (OB_ISNULL(opt) || opt->num_child_ != 2 || OB_ISNULL(opt->children_[0]) || OB_ISNULL(opt->children_[1])) {
        ret = OB_ERR_PARSE_SQL;
      } else if (OB_FAIL(resolve_sql_expr(*opt->children_[1], val_expr))) {
      } else if (OB_ISNULL(val_expr) || OB_FAIL(val_expr->formalize(session_info_))) {
      } else if ((!val_expr->is_const_raw_expr() && !val_expr->is_static_scalar_const_expr())
                 || (T_QUESTIONMARK == val_expr->get_expr_type()
                     && params_.is_prepare_protocol_)) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "non-constant file sql option");
      } else {
        ObString name(opt->children_[0]->str_len_, opt->children_[0]->str_value_);
        ObObj obj;
        if (OB_ISNULL(params_.param_list_)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(ObSQLUtils::calc_simple_expr_without_row(
                       session_info_, val_expr, obj, params_.param_list_, *allocator_))) {
        } else if (eq_cstr(name, "header")) {
          if (!obj.is_integer_type()) {
            ret = OB_INVALID_ARGUMENT;
          } else if (obj.get_int() == 0) {
            ret = OB_NOT_SUPPORTED;
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "header = false");
          } else {
            header = true;
          }
        } else if (eq_cstr(name, "sample_size")) {
          if (!obj.is_integer_type()) {
            ret = OB_INVALID_ARGUMENT;
          } else if (obj.get_int() != -1) {
            ret = OB_NOT_SUPPORTED;
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "sample_size");
          } else {
            saw_sample = true;
            sample_size = -1;
          }
        } else if (eq_cstr(name, "format")) {
          if (!obj.is_string_type()) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            format = obj.get_string();
          }
        } else {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "file sql option");
        }
      }
    }
    ObRawExpr *path_expr = NULL;
    std::string path;
    const int64_t kind = table_node.value_;
    const ParseNode &path_node = *table_node.children_[0];
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(resolve_sql_expr(path_node, path_expr))) {
    } else if (OB_ISNULL(path_expr) || OB_FAIL(path_expr->formalize(session_info_))) {
    } else if ((!path_expr->is_const_raw_expr() && !path_expr->is_static_scalar_const_expr())
               || (T_QUESTIONMARK == path_expr->get_expr_type()
                   && params_.is_prepare_protocol_)) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "non-constant file sql path");
    } else {
      ObObj path_obj;
      if (OB_ISNULL(params_.param_list_)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(ObSQLUtils::calc_simple_expr_without_row(
                     session_info_, path_expr, path_obj, params_.param_list_, *allocator_))) {
      } else if (!path_obj.is_string_type()) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "non-constant file sql path");
      } else {
        ObString p = path_obj.get_string();
        path.assign(p.ptr(), static_cast<std::size_t>(p.length()));
      }
    }
    if (OB_SUCC(ret) && kind == 1 && !format.empty()
        && !eq_cstr(format, "newline_delimited")) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "json format");
    } else if (OB_SUCC(ret) && !format.empty() && kind == 0 && !eq_cstr(format, "csv")) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "csv format");
    } else if (OB_SUCC(ret) && !format.empty() && kind != 0 && kind != 1) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "file sql format");
    }
    (void)header;
    (void)saw_sample;
    (void)sample_size;
    void *mem = NULL;
    ObFileSqlMeta *meta = NULL;
    if (OB_FAIL(ret)) {
    } else if (OB_ISNULL(mem = allocator_->alloc(sizeof(ObFileSqlMeta)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (FALSE_IT(meta = new (mem) ObFileSqlMeta())) {
    } else if (kind == 3) {
      std::vector<std::string> paths;
      if (OB_FAIL(list_glob(path, paths))) {
      } else if (paths.empty()) {
        ret = OB_FILE_NOT_EXIST;
      } else {
        for (const std::string &matched_path : paths) {
          if (OB_FAIL(check_file_sql_path(secure_file_priv, matched_path))) {
            LOG_WARN("file sql path is outside secure_file_priv", K(ret), K(matched_path.c_str()));
            break;
          }
        }
        if (OB_SUCC(ret)) {
          ret = fill_glob(paths, *allocator_, *meta);
        }
      }
    } else {
      std::string bytes;
      if (OB_FAIL(check_file_sql_path(secure_file_priv, path))) {
        LOG_WARN("file sql path is outside secure_file_priv", K(ret), K(path.c_str()));
      } else if (OB_FAIL(read_whole_file(path, bytes))) {
      } else if (kind == 0) {
        TextTable text;
        if (OB_FAIL(parse_csv(bytes, text))) {
        } else if (OB_FAIL(infer_text_table(text, *allocator_, *meta))) {
        }
      } else if (kind == 1) {
        TextTable text;
        if (OB_FAIL(parse_jsonl(bytes, text))) {
        } else if (OB_FAIL(infer_text_table(text, *allocator_, *meta))) {
        }
      } else if (kind == 2) {
        if (OB_FAIL(parse_parquet(bytes, *allocator_, *meta))) {
        }
      } else {
        ret = OB_ERR_UNEXPECTED;
      }
    }
    TableItem *new_table_item = NULL;
    ObString table_name;
    ParseNode *alias_node = table_node.num_child_ > 2 ? table_node.children_[2] : NULL;
    if (OB_FAIL(ret)) {
    } else if (meta->col_cnt_ <= 0 || meta->row_cnt_ <= 0) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("file sql table is empty", K(ret), K(meta->col_cnt_), K(meta->row_cnt_));
    } else if (OB_ISNULL(new_table_item = stmt->create_table_item(*allocator_))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (alias_node != NULL) {
      if (OB_FAIL(copy_text(*allocator_, alias_node->str_value_, alias_node->str_len_, table_name))) {
      }
    } else if (OB_FAIL(stmt->generate_values_table_name(*allocator_, table_name))) {
    }
    if (OB_FAIL(ret)) {
    } else {
      void *def_buf = NULL;
      ObCollationType cs = CS_TYPE_UTF8MB4_GENERAL_CI;
      if (OB_ISNULL(def_buf = allocator_->alloc(sizeof(ObValuesTableDef)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_FAIL(session_info_->get_collation_connection(cs))) {
      } else {
        new_table_item->values_table_def_ = new (def_buf) ObValuesTableDef();
        ObValuesTableDef &def = *new_table_item->values_table_def_;
        def.column_cnt_ = meta->col_cnt_;
        for (int64_t c = 0; OB_SUCC(ret) && c < meta->col_cnt_; ++c) {
          const ObFileSqlCell *repr = NULL;
          int64_t max_len = 1;
          for (int64_t r = 0; r < meta->row_cnt_; ++r) {
            const ObFileSqlCell &cell = meta->cell(r, c);
            if (cell.type_ == ObFileSqlCell::STR && cell.str_.length() > max_len) {
              max_len = cell.str_.length();
            }
            if (repr == NULL && cell.type_ != ObFileSqlCell::NUL) {
              repr = &cell;
            }
          }
          ObObj proto;
          ObConstRawExpr *proto_expr = NULL;
          ObFileSqlCell dummy;
          if (repr == NULL) {
            dummy.type_ = ObFileSqlCell::STR;
            dummy.str_ = ObString::make_string("");
            repr = &dummy;
          }
          if (OB_FAIL(cell_to_obj(*repr, cs, proto))) {
          } else if (OB_FAIL(ObRawExprUtils::build_const_obj_expr(*params_.expr_factory_, proto, proto_expr))) {
          } else if (OB_FAIL(proto_expr->formalize(session_info_))) {
          } else {
            ObRawExprResType col_type = proto_expr->get_result_type();
            if (repr->type_ == ObFileSqlCell::STR || repr->type_ == ObFileSqlCell::NUL) {
              col_type.set_length(max_len);
            }
            if (OB_FAIL(def.column_types_.push_back(col_type))) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            }
          }
        }
        for (int64_t r = 0; OB_SUCC(ret) && r < meta->row_cnt_; ++r) {
          for (int64_t c = 0; OB_SUCC(ret) && c < meta->col_cnt_; ++c) {
            ObObj obj;
            ObConstRawExpr *cexpr = NULL;
            if (OB_FAIL(cell_to_obj(meta->cell(r, c), cs, obj))) {
            } else if (OB_FAIL(ObRawExprUtils::build_const_obj_expr(*params_.expr_factory_, obj, cexpr))) {
            } else if (OB_FAIL(cexpr->formalize(session_info_))) {
            } else if (OB_FAIL(def.access_exprs_.push_back(cexpr))) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            }
          }
        }
        if (OB_SUCC(ret) && OB_FAIL(try_add_cast_to_values(def.column_types_, def.access_exprs_))) {
        } else if (OB_FAIL(ret)) {
        } else if (OB_FAIL(compute_values_table_row_count(def))) {
        } else if (OB_FAIL(estimate_values_table_stats(def))) {
        } else {
          new_table_item->table_id_ = generate_table_id();
          new_table_item->table_name_ = table_name;
          new_table_item->alias_name_ = table_name;
          new_table_item->type_ = TableItem::VALUES_TABLE;
          new_table_item->is_view_table_ = false;
          new_table_item->is_file_sql_table_ = true;
          new_table_item->file_sql_meta_ = meta;
          if (OB_FAIL(stmt->add_table_item(session_info_, new_table_item))) {
          } else if (OB_FAIL(gen_values_table_column_items(def.column_cnt_, def.column_types_, *new_table_item))) {
          } else {
            for (int64_t c = 0; OB_SUCC(ret) && c < meta->col_cnt_; ++c) {
              ColumnItem *item = stmt->get_column_item(
                  new_table_item->table_id_, static_cast<uint64_t>(c) + OB_APP_MIN_COLUMN_ID);
              if (OB_ISNULL(item) || OB_ISNULL(item->expr_)) {
                ret = OB_ERR_UNEXPECTED;
              } else {
                item->column_name_ = meta->columns_.at(c).name_;
                item->expr_->set_column_name(meta->columns_.at(c).name_);
              }
            }
            if (OB_SUCC(ret)) {
              table_item = new_table_item;
            }
          }
        }
      }
    }
  }
  return ret;
}

namespace
{

int render_cell(const ObFileSqlCell &cell, const std::string &nullstr, std::string &out)
{
  int ret = OB_SUCCESS;
  out.clear();
  char buf[128];
  if (cell.type_ == ObFileSqlCell::NUL) {
    out = nullstr;
  } else if (cell.type_ == ObFileSqlCell::INT64 || cell.type_ == ObFileSqlCell::BOOL) {
    snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(cell.int_val_));
    out = buf;
  } else if (cell.type_ == ObFileSqlCell::DOUBLE) {
    bool err = false;
    size_t n = ob_gcvt(cell.double_val_, OB_GCVT_ARG_DOUBLE, static_cast<int>(sizeof(buf) - 1), buf, &err);
    out.assign(buf, n);
  } else if (cell.type_ == ObFileSqlCell::DATE) {
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", cell.year_, cell.month_, cell.day_);
    out = buf;
  } else if (cell.type_ == ObFileSqlCell::DATETIME) {
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             cell.year_, cell.month_, cell.day_, cell.hour_, cell.minute_, cell.second_);
    out = buf;
  } else {
    out.assign(cell.str_.ptr(), static_cast<std::size_t>(cell.str_.length()));
  }
  return ret;
}

void write_csv_field(FILE *fp, const std::string &text)
{
  bool quote = text.find_first_of(",\"\r\n") != std::string::npos;
  if (!quote) {
    fwrite(text.data(), 1, text.size(), fp);
  } else {
    fputc('"', fp);
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '"') {
        fputc('"', fp);
      }
      fputc(text[i], fp);
    }
    fputc('"', fp);
  }
}

int cmp_cell(const ObFileSqlCell &a, const ObFileSqlCell &b)
{
  if (a.type_ == ObFileSqlCell::NUL && b.type_ == ObFileSqlCell::NUL) return 0;
  if (a.type_ == ObFileSqlCell::NUL) return -1;
  if (b.type_ == ObFileSqlCell::NUL) return 1;
  if (a.type_ == ObFileSqlCell::INT64 || a.type_ == ObFileSqlCell::BOOL) {
    if (a.int_val_ < b.int_val_) return -1;
    if (a.int_val_ > b.int_val_) return 1;
    return 0;
  }
  if (a.type_ == ObFileSqlCell::DOUBLE) {
    if (a.double_val_ < b.double_val_) return -1;
    if (a.double_val_ > b.double_val_) return 1;
    return 0;
  }
  if (a.type_ == ObFileSqlCell::DATE || a.type_ == ObFileSqlCell::DATETIME) {
    int64_t ak = (static_cast<int64_t>(a.year_) << 32) | (a.month_ << 24) | (a.day_ << 16)
        | (a.hour_ << 8) | a.minute_;
    int64_t bk = (static_cast<int64_t>(b.year_) << 32) | (b.month_ << 24) | (b.day_ << 16)
        | (b.hour_ << 8) | b.minute_;
    if (ak < bk) return -1;
    if (ak > bk) return 1;
    if (a.second_ < b.second_) return -1;
    if (a.second_ > b.second_) return 1;
    return 0;
  }
  int n = std::min(a.str_.length(), b.str_.length());
  int c = MEMCMP(a.str_.ptr(), b.str_.ptr(), n);
  if (c != 0) return c;
  if (a.str_.length() < b.str_.length()) return -1;
  if (a.str_.length() > b.str_.length()) return 1;
  return 0;
}

int find_col(const ObFileSqlMeta &meta, const ObString &name)
{
  for (int64_t i = 0; i < meta.col_cnt_; ++i) {
    if (meta.columns_.at(i).name_.case_compare(name) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

} // namespace

int ObCopyFileSqlResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  if (parse_tree.type_ != T_COPY_FILE_SQL || parse_tree.num_child_ < 2
      || OB_ISNULL(parse_tree.children_) || OB_ISNULL(parse_tree.children_[0])
      || OB_ISNULL(parse_tree.children_[1]) || OB_ISNULL(params_.session_info_)
      || OB_ISNULL(params_.allocator_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    bool enabled = false;
    ObString secure_file_priv;
    if (OB_FAIL(params_.session_info_->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
    } else if (!enabled) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "file sql");
    } else if (OB_FAIL(params_.session_info_->get_secure_file_priv(secure_file_priv))) {
      LOG_WARN("failed to read secure_file_priv", K(ret));
    }
    bool write_header = false;
    std::string nullstr;
    bool saw_format = false;
    ParseNode *opts = parse_tree.num_child_ > 2 ? parse_tree.children_[2] : NULL;
    for (int64_t i = 0; OB_SUCC(ret) && opts != NULL && i < opts->num_child_; ++i) {
      ParseNode *opt = opts->children_[i];
      if (OB_ISNULL(opt) || opt->num_child_ < 1 || OB_ISNULL(opt->children_[0])) {
        ret = OB_ERR_PARSE_SQL;
      } else {
        ObString name(opt->children_[0]->str_len_, opt->children_[0]->str_value_);
        if (opt->value_ == 1 && opt->num_child_ == 2) {
          ObString word(opt->children_[1]->str_len_, opt->children_[1]->str_value_);
          if (eq_cstr(name, "format") && eq_cstr(word, "csv")) {
            saw_format = true;
          } else {
            ret = OB_NOT_SUPPORTED;
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "copy option");
          }
        } else if (opt->value_ == 2 && opt->num_child_ == 2) {
          if (!eq_cstr(name, "nullstr")) {
            ret = OB_NOT_SUPPORTED;
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "copy option");
          } else {
            nullstr.assign(opt->children_[1]->str_value_,
                           static_cast<std::size_t>(opt->children_[1]->str_len_));
          }
        } else if (opt->value_ == 4) {
          if (eq_cstr(name, "header")) {
            write_header = true;
          } else {
            ret = OB_NOT_SUPPORTED;
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "copy option");
          }
        } else {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "copy option");
        }
      }
    }
    (void)saw_format;
    ObResolver resolver(params_);
    ObStmt *child = NULL;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT, *parse_tree.children_[0], child))) {
    } else if (OB_ISNULL(child) || !child->is_select_stmt()) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "COPY query");
    } else {
      ObSelectStmt *sel = static_cast<ObSelectStmt *>(child);
      TableItem *table = NULL;
      if (sel->get_table_size() != 1) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "COPY from multiple tables");
      } else if (OB_ISNULL(table = sel->get_table_item(0)) || !table->is_file_sql_table_
                 || OB_ISNULL(table->file_sql_meta_)) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "COPY source");
      } else {
        const ObFileSqlMeta &meta = *table->file_sql_meta_;
        std::vector<int> proj;
        std::vector<std::string> headers;
        for (int64_t i = 0; OB_SUCC(ret) && i < sel->get_select_item_size(); ++i) {
          const SelectItem &item = sel->get_select_item(i);
          ObString name;
          if (item.expr_ != NULL && item.expr_->get_expr_type() == T_REF_COLUMN) {
            name = static_cast<ObColumnRefRawExpr *>(item.expr_)->get_column_name();
          } else if (!item.alias_name_.empty()) {
            name = item.alias_name_;
          } else {
            name = item.expr_name_;
          }
          int idx = find_col(meta, name);
          if (idx < 0) {
            ret = OB_NOT_SUPPORTED;
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "COPY expression");
          } else {
            proj.push_back(idx);
            headers.push_back(std::string(name.ptr(), static_cast<std::size_t>(name.length())));
          }
        }
        struct Ord {
          int col;
          bool asc;
        };
        std::vector<Ord> ords;
        for (int64_t i = 0; OB_SUCC(ret) && i < sel->get_order_item_size(); ++i) {
          OrderItem &ord = sel->get_order_item(i);
          if (OB_ISNULL(ord.expr_) || ord.expr_->get_expr_type() != T_REF_COLUMN) {
            ret = OB_NOT_SUPPORTED;
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "COPY ORDER BY expression");
          } else {
            int idx = find_col(meta, static_cast<ObColumnRefRawExpr *>(ord.expr_)->get_column_name());
            if (idx < 0) {
              ret = OB_ERR_BAD_FIELD_ERROR;
            } else {
              Ord o;
              o.col = idx;
              o.asc = ord.is_ascending();
              ords.push_back(o);
            }
          }
        }
        std::vector<int64_t> order;
        for (int64_t r = 0; r < meta.row_cnt_; ++r) {
          order.push_back(r);
        }
        if (OB_SUCC(ret) && !ords.empty()) {
          std::sort(order.begin(), order.end(), [&](int64_t a, int64_t b) {
            for (std::size_t k = 0; k < ords.size(); ++k) {
              int c = cmp_cell(meta.cell(a, ords[k].col), meta.cell(b, ords[k].col));
              if (c != 0) {
                return ords[k].asc ? c < 0 : c > 0;
              }
            }
            return a < b;
          });
        }
        std::string out_path(parse_tree.children_[1]->str_value_,
                             static_cast<std::size_t>(parse_tree.children_[1]->str_len_));
        FILE *fp = NULL;
        bool created = false;
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(check_copy_destination_path(secure_file_priv, out_path))) {
          LOG_WARN("COPY destination is not allowed", K(ret), K(out_path.c_str()));
        } else if (OB_ISNULL(fp = fopen(out_path.c_str(), "wb"))) {
          ret = OB_IO_ERROR;
          LOG_WARN("failed to open COPY destination", K(ret), K(out_path.c_str()));
        } else {
          created = true;
          if (write_header) {
            for (std::size_t c = 0; c < headers.size(); ++c) {
              if (c != 0) {
                fputc(',', fp);
              }
              write_csv_field(fp, headers[c]);
            }
            fputc('\n', fp);
          }
          for (std::size_t r = 0; r < order.size() && ret == OB_SUCCESS; ++r) {
            for (std::size_t c = 0; c < proj.size(); ++c) {
              if (c != 0) {
                fputc(',', fp);
              }
              std::string text;
              render_cell(meta.cell(order[r], proj[c]), nullstr, text);
              write_csv_field(fp, text);
            }
            fputc('\n', fp);
          }
          if (ferror(fp)) {
            ret = OB_IO_ERROR;
          }
          fclose(fp);
          if (OB_FAIL(ret) && created) {
            (void)remove(out_path.c_str());
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_ISNULL(create_stmt<ObEmptyQueryStmt>())) {
        ret = OB_SQL_RESOLVER_NO_MEMORY;
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
