#include "keel/ast.hpp"

#include "keel/error.hpp"

#include <cctype>
#include <unordered_map>

namespace keel {
namespace {

struct Token {
  enum class Kind {
    End, Ident, Number, String, Comma, Semi, LParen, RParen, Dot, Star,
    Eq, Ne, Lt, Gt, Le, Ge, Plus, Minus, Slash
  };
  Kind kind{Kind::End};
  std::string text;
  int line{1};
  int col{1};
};

const std::unordered_map<std::string, std::string>& Keywords() {
  static const std::unordered_map<std::string, std::string> k = {
      {"create", "create"},   {"table", "table"},     {"index", "index"},
      {"unique", "unique"},   {"insert", "insert"},   {"into", "into"},
      {"values", "values"},   {"select", "select"},   {"from", "from"},
      {"where", "where"},     {"join", "join"},       {"inner", "inner"},
      {"cross", "cross"},     {"on", "on"},           {"and", "and"},
      {"or", "or"},           {"not", "not"},         {"order", "order"},
      {"by", "by"},           {"group", "group"},     {"asc", "asc"},
      {"desc", "desc"},       {"limit", "limit"},     {"as", "as"},
      {"primary", "primary"}, {"key", "key"},         {"null", "null"},
      {"is", "is"},           {"begin", "begin"},     {"commit", "commit"},
      {"rollback", "rollback"}, {"explain", "explain"}, {"analyze", "analyze"},
      {"update", "update"},   {"set", "set"},         {"delete", "delete"},
      {"int", "int"},         {"integer", "integer"}, {"double", "double"},
      {"bool", "bool"},       {"boolean", "boolean"}, {"varchar", "varchar"},
      {"text", "text"},       {"true", "true"},       {"false", "false"},
      {"count", "count"},     {"sum", "sum"},         {"avg", "avg"},
      {"min", "min"},         {"max", "max"},
  };
  return k;
}

class Parser {
 public:
  explicit Parser(std::string sql) : sql_(std::move(sql)) { Next(); }

  std::vector<Statement> ParseScript() {
    std::vector<Statement> out;
    while (!Is(Token::Kind::End)) {
      if (Is(Token::Kind::Semi)) {
        Next();
        continue;
      }
      out.push_back(ParseStatement());
      if (Is(Token::Kind::Semi)) {
        Next();
      }
    }
    return out;
  }

 private:
  std::string sql_;
  std::size_t i_{0};
  int line_{1};
  int col_{1};
  Token tok_;

  [[noreturn]] void Fail(const std::string& message) const {
    throw Error(message + " at line " + std::to_string(tok_.line) + " column " +
                std::to_string(tok_.col));
  }

  bool Is(Token::Kind kind) const { return tok_.kind == kind; }
  bool IsWord(const char* word) const {
    return tok_.kind == Token::Kind::Ident && tok_.text == word;
  }

  void Next() {
    while (true) {
      if (i_ >= sql_.size()) {
        tok_ = {Token::Kind::End, "", line_, col_};
        return;
      }
      const char c = sql_[i_];
      if (c == ' ' || c == '\t' || c == '\r') {
        Advance();
        continue;
      }
      if (c == '\n') {
        ++i_;
        ++line_;
        col_ = 1;
        continue;
      }
      if (c == '-' && i_ + 1 < sql_.size() && sql_[i_ + 1] == '-') {
        while (i_ < sql_.size() && sql_[i_] != '\n') {
          Advance();
        }
        continue;
      }
      if (c == '/' && i_ + 1 < sql_.size() && sql_[i_ + 1] == '*') {
        Advance();
        Advance();
        while (i_ + 1 < sql_.size() && !(sql_[i_] == '*' && sql_[i_ + 1] == '/')) {
          if (sql_[i_] == '\n') {
            ++i_;
            ++line_;
            col_ = 1;
          } else {
            Advance();
          }
        }
        if (i_ + 1 >= sql_.size()) {
          Fail("unterminated block comment");
        }
        Advance();
        Advance();
        continue;
      }
      break;
    }
    tok_.line = line_;
    tok_.col = col_;
    if (i_ >= sql_.size()) {
      tok_.kind = Token::Kind::End;
      tok_.text.clear();
      return;
    }
    const char c = sql_[i_];
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      std::string word;
      while (i_ < sql_.size() && (std::isalnum(static_cast<unsigned char>(sql_[i_])) || sql_[i_] == '_')) {
        word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(sql_[i_]))));
        Advance();
      }
      tok_.kind = Token::Kind::Ident;
      tok_.text = word;
      return;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) ||
        (c == '.' && i_ + 1 < sql_.size() && std::isdigit(static_cast<unsigned char>(sql_[i_ + 1])))) {
      std::string word;
      while (i_ < sql_.size() && std::isdigit(static_cast<unsigned char>(sql_[i_]))) {
        word.push_back(sql_[i_]);
        Advance();
      }
      if (i_ < sql_.size() && sql_[i_] == '.') {
        word.push_back('.');
        Advance();
        while (i_ < sql_.size() && std::isdigit(static_cast<unsigned char>(sql_[i_]))) {
          word.push_back(sql_[i_]);
          Advance();
        }
      }
      tok_.kind = Token::Kind::Number;
      tok_.text = word;
      return;
    }
    if (c == '\'') {
      Advance();
      std::string word;
      while (i_ < sql_.size()) {
        if (sql_[i_] == '\'') {
          Advance();
          if (i_ < sql_.size() && sql_[i_] == '\'') {
            word.push_back('\'');
            Advance();
            continue;
          }
          tok_.kind = Token::Kind::String;
          tok_.text = word;
          return;
        }
        if (sql_[i_] == '\n') {
          word.push_back('\n');
          ++i_;
          ++line_;
          col_ = 1;
          continue;
        }
        word.push_back(sql_[i_]);
        Advance();
      }
      Fail("unterminated string");
    }
    Advance();
    tok_.text = std::string(1, c);
    switch (c) {
      case ',': tok_.kind = Token::Kind::Comma; return;
      case ';': tok_.kind = Token::Kind::Semi; return;
      case '(': tok_.kind = Token::Kind::LParen; return;
      case ')': tok_.kind = Token::Kind::RParen; return;
      case '.': tok_.kind = Token::Kind::Dot; return;
      case '*': tok_.kind = Token::Kind::Star; return;
      case '+': tok_.kind = Token::Kind::Plus; return;
      case '-': tok_.kind = Token::Kind::Minus; return;
      case '/': tok_.kind = Token::Kind::Slash; return;
      case '=': tok_.kind = Token::Kind::Eq; return;
      case '<':
        if (i_ < sql_.size() && sql_[i_] == '=') {
          Advance();
          tok_.kind = Token::Kind::Le;
          tok_.text = "<=";
          return;
        }
        if (i_ < sql_.size() && sql_[i_] == '>') {
          Advance();
          tok_.kind = Token::Kind::Ne;
          tok_.text = "<>";
          return;
        }
        tok_.kind = Token::Kind::Lt;
        return;
      case '>':
        if (i_ < sql_.size() && sql_[i_] == '=') {
          Advance();
          tok_.kind = Token::Kind::Ge;
          tok_.text = ">=";
          return;
        }
        tok_.kind = Token::Kind::Gt;
        return;
      case '!':
        if (i_ < sql_.size() && sql_[i_] == '=') {
          Advance();
          tok_.kind = Token::Kind::Ne;
          tok_.text = "!=";
          return;
        }
        break;
      default:
        break;
    }
    Fail("unexpected character");
  }

  void Advance() {
    ++i_;
    ++col_;
  }

  void Expect(Token::Kind kind, const char* what) {
    if (!Is(kind)) {
      Fail(std::string("expected ") + what);
    }
    Next();
  }

  void ExpectWord(const char* word) {
    if (!IsWord(word)) {
      Fail(std::string("expected ") + word);
    }
    Next();
  }

  std::string Ident() {
    if (!Is(Token::Kind::Ident)) {
      Fail("expected identifier");
    }
    if (Keywords().count(tok_.text) && tok_.text != "true" && tok_.text != "false" &&
        tok_.text != "null") {
      // Keywords can still be used as identifiers only when the caller asked
      // for a general ident. Reject reserved words here.
    }
    std::string name = tok_.text;
    Next();
    return name;
  }

  Statement ParseStatement() {
    bool explain = false;
    bool analyze = false;
    if (IsWord("explain")) {
      explain = true;
      Next();
      if (IsWord("analyze")) {
        analyze = true;
        Next();
      }
    }
    Statement stmt;
    if (IsWord("create")) {
      Next();
      if (IsWord("unique")) {
        Next();
        ExpectWord("index");
        stmt = ParseCreateIndex(true);
      } else if (IsWord("index")) {
        Next();
        stmt = ParseCreateIndex(false);
      } else if (IsWord("table")) {
        Next();
        stmt = ParseCreateTable();
      } else {
        Fail("expected TABLE or INDEX");
      }
    } else if (IsWord("insert")) {
      stmt = ParseInsert();
    } else if (IsWord("select")) {
      stmt = ParseSelect();
    } else if (IsWord("update")) {
      stmt = ParseUpdate();
    } else if (IsWord("delete")) {
      stmt = ParseDelete();
    } else if (IsWord("begin")) {
      Next();
      stmt.kind = Statement::Kind::Begin;
    } else if (IsWord("commit")) {
      Next();
      stmt.kind = Statement::Kind::Commit;
    } else if (IsWord("rollback")) {
      Next();
      stmt.kind = Statement::Kind::Rollback;
    } else {
      Fail("expected a statement");
    }
    stmt.explain = explain;
    stmt.analyze = analyze;
    return stmt;
  }

  Statement ParseCreateTable() {
    Statement stmt;
    stmt.kind = Statement::Kind::CreateTable;
    stmt.create_table.name = Ident();
    Expect(Token::Kind::LParen, "(");
    if (Is(Token::Kind::RParen)) {
      Fail("table needs at least one column");
    }
    while (!Is(Token::Kind::RParen) && !Is(Token::Kind::End)) {
      ColumnDef column;
      column.name = Ident();
      ParseType(column);
      while (IsWord("primary") || IsWord("not")) {
        if (IsWord("primary")) {
          Next();
          ExpectWord("key");
          column.primary_key = true;
          column.not_null = true;
        } else {
          Next();
          ExpectWord("null");
          column.not_null = true;
        }
      }
      stmt.create_table.columns.push_back(std::move(column));
      if (Is(Token::Kind::Comma)) {
        Next();
        continue;
      }
      break;
    }
    Expect(Token::Kind::RParen, ")");
    return stmt;
  }

  void ParseType(ColumnDef& column) {
    if (IsWord("int") || IsWord("integer")) {
      column.type = Type::Integer;
      Next();
    } else if (IsWord("double")) {
      column.type = Type::Double;
      Next();
    } else if (IsWord("bool") || IsWord("boolean")) {
      column.type = Type::Boolean;
      Next();
    } else if (IsWord("varchar") || IsWord("text")) {
      column.type = Type::Varchar;
      Next();
      if (Is(Token::Kind::LParen)) {
        Next();
        if (!Is(Token::Kind::Number)) {
          Fail("expected varchar length");
        }
        column.varchar_len = static_cast<uint32_t>(std::stoll(tok_.text));
        Next();
        Expect(Token::Kind::RParen, ")");
      }
    } else {
      Fail("expected a type");
    }
  }

  Statement ParseCreateIndex(bool unique) {
    Statement stmt;
    stmt.kind = Statement::Kind::CreateIndex;
    stmt.create_index.unique = unique;
    stmt.create_index.name = Ident();
    ExpectWord("on");
    stmt.create_index.table = Ident();
    Expect(Token::Kind::LParen, "(");
    stmt.create_index.columns.push_back(Ident());
    while (Is(Token::Kind::Comma)) {
      Next();
      stmt.create_index.columns.push_back(Ident());
    }
    Expect(Token::Kind::RParen, ")");
    return stmt;
  }

  Statement ParseInsert() {
    Statement stmt;
    stmt.kind = Statement::Kind::Insert;
    ExpectWord("insert");
    ExpectWord("into");
    stmt.insert.table = Ident();
    if (Is(Token::Kind::LParen)) {
      Next();
      stmt.insert.columns.push_back(Ident());
      while (Is(Token::Kind::Comma)) {
        Next();
        stmt.insert.columns.push_back(Ident());
      }
      Expect(Token::Kind::RParen, ")");
    }
    ExpectWord("values");
    do {
      Expect(Token::Kind::LParen, "(");
      std::vector<Expr> row;
      if (!Is(Token::Kind::RParen)) {
        row.push_back(ParseExpr());
        while (Is(Token::Kind::Comma)) {
          Next();
          row.push_back(ParseExpr());
        }
      }
      Expect(Token::Kind::RParen, ")");
      stmt.insert.rows.push_back(std::move(row));
      if (!Is(Token::Kind::Comma)) {
        break;
      }
      Next();
    } while (true);
    return stmt;
  }

  Statement ParseUpdate() {
    Statement stmt;
    stmt.kind = Statement::Kind::Update;
    ExpectWord("update");
    stmt.update.table = Ident();
    ExpectWord("set");
    do {
      Assign assign;
      assign.column = Ident();
      Expect(Token::Kind::Eq, "=");
      assign.value = ParseExpr();
      stmt.update.sets.push_back(std::move(assign));
      if (!Is(Token::Kind::Comma)) {
        break;
      }
      Next();
    } while (true);
    if (IsWord("where")) {
      Next();
      stmt.update.where = std::make_shared<Expr>(ParseExpr());
    }
    return stmt;
  }

  Statement ParseDelete() {
    Statement stmt;
    stmt.kind = Statement::Kind::Delete;
    ExpectWord("delete");
    ExpectWord("from");
    stmt.del.table = Ident();
    if (IsWord("where")) {
      Next();
      stmt.del.where = std::make_shared<Expr>(ParseExpr());
    }
    return stmt;
  }

  Statement ParseSelect() {
    Statement stmt;
    stmt.kind = Statement::Kind::Select;
    ExpectWord("select");
    if (Is(Token::Kind::Star)) {
      Next();
      SelectItem item;
      item.star = true;
      stmt.select.items.push_back(std::move(item));
    } else {
      stmt.select.items.push_back(ParseSelectItem());
      while (Is(Token::Kind::Comma)) {
        Next();
        if (Is(Token::Kind::Star)) {
          Next();
          SelectItem item;
          item.star = true;
          stmt.select.items.push_back(std::move(item));
        } else {
          stmt.select.items.push_back(ParseSelectItem());
        }
      }
    }
    ExpectWord("from");
    stmt.select.from = ParseTable();
    while (IsWord("join") || IsWord("inner") || IsWord("cross") || Is(Token::Kind::Comma)) {
      JoinClause join;
      if (Is(Token::Kind::Comma)) {
        Next();
        join.table = ParseTable();
      } else if (IsWord("cross")) {
        Next();
        ExpectWord("join");
        join.table = ParseTable();
      } else {
        if (IsWord("inner")) {
          Next();
        }
        ExpectWord("join");
        join.table = ParseTable();
        ExpectWord("on");
        join.on = std::make_shared<Expr>(ParseExpr());
      }
      stmt.select.joins.push_back(std::move(join));
    }
    if (IsWord("where")) {
      Next();
      stmt.select.where = std::make_shared<Expr>(ParseExpr());
    }
    if (IsWord("group")) {
      Next();
      ExpectWord("by");
      stmt.select.group_by.push_back(ParseExpr());
      while (Is(Token::Kind::Comma)) {
        Next();
        stmt.select.group_by.push_back(ParseExpr());
      }
    }
    if (IsWord("order")) {
      Next();
      ExpectWord("by");
      do {
        OrderItem item;
        item.expr = ParseExpr();
        if (IsWord("asc")) {
          Next();
          item.asc = true;
        } else if (IsWord("desc")) {
          Next();
          item.asc = false;
        }
        stmt.select.order_by.push_back(std::move(item));
        if (!Is(Token::Kind::Comma)) {
          break;
        }
        Next();
      } while (true);
    }
    if (IsWord("limit")) {
      Next();
      if (!Is(Token::Kind::Number)) {
        Fail("expected limit");
      }
      stmt.select.limit = std::stoll(tok_.text);
      Next();
    }
    return stmt;
  }

  SelectItem ParseSelectItem() {
    SelectItem item;
    if (Is(Token::Kind::Ident) && PeekStarQual()) {
      item.star = true;
      item.star_table = tok_.text;
      Next();
      Expect(Token::Kind::Dot, ".");
      Expect(Token::Kind::Star, "*");
      return item;
    }
    item.expr = ParseExpr();
    if (IsWord("as")) {
      Next();
      item.expr.alias = Ident();
    }
    return item;
  }

  bool PeekStarQual() const {
    // ident '.' '*'  — look ahead in the raw buffer only when the next tokens
    // were already consumed? We only know the current token. Check the upcoming
    // characters are '.' and '*', skipping whitespace.
    std::size_t j = i_;
    while (j < sql_.size() && std::isspace(static_cast<unsigned char>(sql_[j]))) {
      ++j;
    }
    if (j >= sql_.size() || sql_[j] != '.') {
      return false;
    }
    ++j;
    while (j < sql_.size() && std::isspace(static_cast<unsigned char>(sql_[j]))) {
      ++j;
    }
    return j < sql_.size() && sql_[j] == '*';
  }

  TableRef ParseTable() {
    TableRef table;
    table.name = Ident();
    table.alias = table.name;
    if (IsWord("as")) {
      Next();
      table.alias = Ident();
    } else if (Is(Token::Kind::Ident) && !IsWord("where") && !IsWord("join") && !IsWord("inner") &&
               !IsWord("cross") && !IsWord("group") && !IsWord("order") && !IsWord("limit") &&
               !IsWord("on") && !IsWord("left")) {
      table.alias = Ident();
    }
    return table;
  }

  Expr ParseExpr() { return ParseOr(); }

  Expr ParseOr() {
    Expr expr = ParseAnd();
    while (IsWord("or")) {
      Next();
      expr = Expr::Bin("OR", std::move(expr), ParseAnd());
    }
    return expr;
  }

  Expr ParseAnd() {
    Expr expr = ParseNot();
    while (IsWord("and")) {
      Next();
      expr = Expr::Bin("AND", std::move(expr), ParseNot());
    }
    return expr;
  }

  Expr ParseNot() {
    if (IsWord("not")) {
      Next();
      return Expr::Una("NOT", ParseNot());
    }
    return ParseCmp();
  }

  Expr ParseCmp() {
    Expr expr = ParseSum();
    if (IsWord("is")) {
      Next();
      bool neg = false;
      if (IsWord("not")) {
        neg = true;
        Next();
      }
      ExpectWord("null");
      return Expr::Una(neg ? "IS NOT NULL" : "IS NULL", std::move(expr));
    }
    std::string op;
    if (Is(Token::Kind::Eq)) op = "=";
    else if (Is(Token::Kind::Ne)) op = "!=";
    else if (Is(Token::Kind::Lt)) op = "<";
    else if (Is(Token::Kind::Gt)) op = ">";
    else if (Is(Token::Kind::Le)) op = "<=";
    else if (Is(Token::Kind::Ge)) op = ">=";
    else return expr;
    Next();
    return Expr::Bin(op, std::move(expr), ParseSum());
  }

  Expr ParseSum() {
    Expr expr = ParseProd();
    while (Is(Token::Kind::Plus) || Is(Token::Kind::Minus)) {
      const std::string op = Is(Token::Kind::Plus) ? "+" : "-";
      Next();
      expr = Expr::Bin(op, std::move(expr), ParseProd());
    }
    return expr;
  }

  Expr ParseProd() {
    Expr expr = ParseUnary();
    while (Is(Token::Kind::Star) || Is(Token::Kind::Slash)) {
      const std::string op = Is(Token::Kind::Star) ? "*" : "/";
      Next();
      expr = Expr::Bin(op, std::move(expr), ParseUnary());
    }
    return expr;
  }

  Expr ParseUnary() {
    if (Is(Token::Kind::Minus)) {
      Next();
      return Expr::Una("NEG", ParseUnary());
    }
    if (Is(Token::Kind::Plus)) {
      Next();
      return ParseUnary();
    }
    return ParsePrimary();
  }

  Expr ParsePrimary() {
    if (Is(Token::Kind::Number)) {
      Value value;
      if (tok_.text.find('.') != std::string::npos) {
        value = Value::Double(std::stod(tok_.text));
      } else {
        value = Value::Int(std::stoll(tok_.text));
      }
      Next();
      return Expr::Lit(std::move(value));
    }
    if (Is(Token::Kind::String)) {
      Expr expr = Expr::Lit(Value::Varchar(tok_.text));
      Next();
      return expr;
    }
    if (IsWord("null")) {
      Next();
      return Expr::Lit(Value::Null(Type::Integer));
    }
    if (IsWord("true")) {
      Next();
      return Expr::Lit(Value::Bool(true));
    }
    if (IsWord("false")) {
      Next();
      return Expr::Lit(Value::Bool(false));
    }
    if (Is(Token::Kind::LParen)) {
      Next();
      Expr expr = ParseExpr();
      Expect(Token::Kind::RParen, ")");
      return expr;
    }
    if (Is(Token::Kind::Star)) {
      Next();
      return Expr::Star("");
    }
    if (Is(Token::Kind::Ident)) {
      const std::string name = tok_.text;
      Next();
      if (Is(Token::Kind::LParen) &&
          (name == "count" || name == "sum" || name == "avg" || name == "min" || name == "max")) {
        Next();
        Expr arg;
        bool star = false;
        if (Is(Token::Kind::Star)) {
          star = true;
          arg = Expr::Star("");
          Next();
        } else {
          arg = ParseExpr();
        }
        Expect(Token::Kind::RParen, ")");
        std::string fn = name;
        for (char& c : fn) {
          c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return Expr::Agg(fn, std::move(arg), star);
      }
      if (Is(Token::Kind::Dot)) {
        Next();
        if (Is(Token::Kind::Star)) {
          Next();
          return Expr::Star(name);
        }
        return Expr::Col(name, Ident());
      }
      return Expr::Col("", name);
    }
    Fail("expected an expression");
  }
};

void WriteExpr(std::string& out, const Expr& expr) {
  switch (expr.kind) {
    case Expr::Kind::Literal:
      if (expr.literal.is_null) {
        out += "NULL";
      } else if (expr.literal.type == Type::Varchar) {
        out.push_back('\'');
        out += expr.literal.s;
        out.push_back('\'');
      } else {
        out += FormatValue(expr.literal);
      }
      break;
    case Expr::Kind::Column:
      if (!expr.table.empty()) {
        out += expr.table;
        out.push_back('.');
      }
      out += expr.name;
      break;
    case Expr::Kind::Star:
      if (!expr.table.empty()) {
        out += expr.table;
        out.push_back('.');
      }
      out.push_back('*');
      break;
    case Expr::Kind::Unary:
      if (expr.op == "NEG") {
        out.push_back('-');
        WriteExpr(out, *expr.left);
      } else if (expr.op == "NOT") {
        out += "NOT ";
        WriteExpr(out, *expr.left);
      } else {
        WriteExpr(out, *expr.left);
        out.push_back(' ');
        out += expr.op;
      }
      break;
    case Expr::Kind::Binary:
      out.push_back('(');
      WriteExpr(out, *expr.left);
      out.push_back(' ');
      out += expr.op;
      out.push_back(' ');
      WriteExpr(out, *expr.right);
      out.push_back(')');
      break;
    case Expr::Kind::Aggregate:
      out += expr.op;
      out.push_back('(');
      if (expr.name == "*") {
        out.push_back('*');
      } else if (expr.left) {
        WriteExpr(out, *expr.left);
      }
      out.push_back(')');
      break;
  }
}

}  // namespace

Expr Expr::Lit(Value value) {
  Expr expr;
  expr.kind = Kind::Literal;
  expr.literal = std::move(value);
  return expr;
}

Expr Expr::Col(std::string table, std::string name) {
  Expr expr;
  expr.kind = Kind::Column;
  expr.table = std::move(table);
  expr.name = std::move(name);
  return expr;
}

Expr Expr::Bin(std::string op, Expr lhs, Expr rhs) {
  Expr expr;
  expr.kind = Kind::Binary;
  expr.op = std::move(op);
  expr.left = std::make_shared<Expr>(std::move(lhs));
  expr.right = std::make_shared<Expr>(std::move(rhs));
  return expr;
}

Expr Expr::Una(std::string op, Expr inner) {
  Expr expr;
  expr.kind = Kind::Unary;
  expr.op = std::move(op);
  expr.left = std::make_shared<Expr>(std::move(inner));
  return expr;
}

Expr Expr::Agg(std::string fn, Expr arg, bool star) {
  Expr expr;
  expr.kind = Kind::Aggregate;
  expr.op = std::move(fn);
  if (star) {
    expr.name = "*";
  } else {
    expr.left = std::make_shared<Expr>(std::move(arg));
  }
  return expr;
}

Expr Expr::Star(std::string table) {
  Expr expr;
  expr.kind = Kind::Star;
  expr.table = std::move(table);
  return expr;
}

std::vector<Statement> ParseSql(const std::string& sql) {
  Parser parser(sql);
  return parser.ParseScript();
}

std::string ExprToString(const Expr& expr) {
  std::string out;
  WriteExpr(out, expr);
  return out;
}

}  // namespace keel
