#include "sift/Parse/Parser.h"

#include <utility>

namespace sift::parse {

namespace {

bool isDeclarationKeyword(TokenKind kind) noexcept {
  return kind == TokenKind::KeywordVar ||
         kind == TokenKind::KeywordConst ||
         kind == TokenKind::KeywordFunction ||
         kind == TokenKind::KeywordStruct;
}

bool isControlKeyword(TokenKind kind) noexcept {
  return kind == TokenKind::KeywordIf ||
         kind == TokenKind::KeywordWhile ||
         kind == TokenKind::KeywordRepeat ||
         kind == TokenKind::KeywordFor ||
         kind == TokenKind::KeywordReturn ||
         kind == TokenKind::KeywordDefer ||
         kind == TokenKind::KeywordSwitch ||
         kind == TokenKind::KeywordBreak ||
         kind == TokenKind::KeywordContinue;
}

// Expression-property helpers used by syntax validation.  Keeping these
// independent of allocation details makes them cheap and easy to inline.
bool isAssignableExpression(const Expression* expression) noexcept {
  if (expression == nullptr) return false;
  return expression->kind() == NodeKind::IdentifierExpression ||
         expression->kind() == NodeKind::MemberExpression;
}

bool isLiteralExpression(const Expression* expression) noexcept {
  return expression != nullptr &&
         expression->kind() == NodeKind::LiteralExpression;
}

bool isMemberExpression(const Expression* expression) noexcept {
  return expression != nullptr &&
         expression->kind() == NodeKind::MemberExpression;
}

bool isIdentifierExpression(const Expression* expression) noexcept {
  return expression != nullptr &&
         expression->kind() == NodeKind::IdentifierExpression;
}

} // namespace

Parser::Parser(std::string_view source)
    : lexer_(source) {
  diagnostics_.reserve(32);
  callingNames_.reserve(32);
  functionNames_.reserve(32);
  structNames_.reserve(16);
  advance();
}

const std::vector<Diagnostic>& Parser::diagnostics() const noexcept {
  return diagnostics_;
}

bool Parser::hasErrors() const noexcept {
  for (const Diagnostic& diagnostic : diagnostics_) {
    if (diagnostic.severity == DiagnosticSeverity::Error) {
      return true;
    }
  }
  return lexer_.hasErrors();
}

void Parser::advance() {
  previous_ = current_;
  current_ = lexer_.lex();
}

bool Parser::check(TokenKind kind) const {
  return current_.kind == kind;
}

bool Parser::match(TokenKind kind) {
  if (!check(kind)) {
    return false;
  }

  advance();
  return true;
}

bool Parser::expect(TokenKind kind, std::string_view message) {
  if (check(kind)) {
    advance();
    return true;
  }

  error(current_, message);
  return false;
}

void Parser::error(const Token& token, std::string_view message) {
  diagnostics_.push_back({
      DiagnosticSeverity::Error,
      lexer_.locationAt(token.start),
      std::string(message)
  });
}

void Parser::synchronize() {
  while (!check(TokenKind::EndOfFile)) {
    if (previous_.kind == TokenKind::Semicolon ||
        previous_.kind == TokenKind::RightBrace) {
      return;
    }

    if (isDeclarationKeyword(current_.kind) ||
        isControlKeyword(current_.kind)) {
      return;
    }

    advance();
  }
}

std::unique_ptr<Program> Parser::parse() {
  auto program = std::make_unique<Program>();
  program->imports.reserve(8);
  program->structs.reserve(8);
  program->functions.reserve(16);
  program->statements.reserve(32);
  program->location = current_.kind == TokenKind::EndOfFile
      ? SourceLocation{}
      : lexer_.locationAt(current_.start);

  while (!check(TokenKind::EndOfFile)) {
    if (match(TokenKind::Semicolon)) {
      continue;
    }

    if (check(TokenKind::Comment)) {
      advance();
      continue;
    }

    switch (current_.kind) {
      case TokenKind::KeywordImport:
        program->imports.push_back(parseImport());
        continue;

      case TokenKind::KeywordStruct:
        program->structs.push_back(parseStruct());
        continue;

      case TokenKind::KeywordFunction:
        program->functions.push_back(parseFunction());
        continue;

      default:
        program->statements.push_back(parseStatement());
        continue;
    }
  }

  return program;
}

std::unique_ptr<ImportDeclaration> Parser::parseImport() {
  const Token start = current_;
  advance();

  auto node = std::make_unique<ImportDeclaration>();
  node->location = lexer_.locationAt(start.start);

  if (check(TokenKind::StringLiteral)) {
    node->module = tokenText(current_);
    advance();
  } else {
    node->module = parseIdentifier("import module");
  }

  match(TokenKind::Semicolon);
  return node;
}

std::unique_ptr<StructDeclaration> Parser::parseStruct() {
  const Token start = current_;
  advance();

  auto node = std::make_unique<StructDeclaration>();
  node->variables.reserve(8);
  node->functions.reserve(8);
  node->location = lexer_.locationAt(start.start);
  node->name = parseIdentifier("struct name");

  if (!structNames_.insert(node->name).second) {
    error(previous_, "duplicate struct name");
  }

  if (!expect(TokenKind::LeftBrace, "expected '{' after struct name")) {
    synchronize();
    return node;
  }

  while (!check(TokenKind::RightBrace) &&
         !check(TokenKind::EndOfFile)) {
    if (match(TokenKind::Semicolon) ||
        match(TokenKind::Comment)) {
      continue;
    }

    if (check(TokenKind::KeywordStruct)) {
      error(current_, "Sift does not allow a struct inside another struct");
      synchronize();
      continue;
    }

    if (check(TokenKind::KeywordVar) ||
        check(TokenKind::KeywordConst)) {
      auto member = parseVariable(check(TokenKind::KeywordConst));
      node->variables.push_back(std::move(member));
      continue;
    }

    if (check(TokenKind::KeywordFunction)) {
      node->functions.push_back(parseFunction());
      continue;
    }

    error(current_, "only var, const, and function declarations are allowed in a Sift struct");
    synchronize();
  }

  expect(TokenKind::RightBrace, "expected '}' after struct declaration");
  return node;
}

std::unique_ptr<FunctionDeclaration> Parser::parseFunction() {
  const Token start = current_;
  advance();

  auto node = std::make_unique<FunctionDeclaration>();
  node->parameters.reserve(4);
  node->location = lexer_.locationAt(start.start);
  node->name = parseIdentifier("function name");

  if (!functionNames_.insert(node->name).second) {
    error(previous_, "duplicate function name in this file");
  }

  if (!expect(TokenKind::LeftParen, "expected '(' after function name")) {
    synchronize();
    return node;
  }

  if (check(TokenKind::RightParen)) {
    error(current_, "Sift functions require exactly one calling name");
  } else {
    node->callingName = parseCallingName();

    if (!callingNames_.insert(node->callingName).second) {
      error(previous_, "calling name is already used by another function in this file");
    }

    if (match(TokenKind::Comma)) {
      error(previous_, "Sift functions allow only one calling name");
      while (!check(TokenKind::RightParen) &&
             !check(TokenKind::EndOfFile)) {
        advance();
      }
    }
  }

  expect(TokenKind::RightParen, "expected ')' after function calling name");

  if (match(TokenKind::Colon)) {
    node->returnType = parseTypeName();
  } else if (match(TokenKind::Arrow)) {
    node->returnType = parseTypeName();
  }

  node->body = parseBlock();
  return node;
}

std::unique_ptr<Statement> Parser::parseStatement() {
  auto node = std::make_unique<Statement>();

  switch (current_.kind) {
    case TokenKind::KeywordVar:
      node->value = std::move(*parseVariable(false));
      break;
    case TokenKind::KeywordConst:
      node->value = std::move(*parseVariable(true));
      break;
    case TokenKind::KeywordIf:
      node->value = std::move(*parseIf());
      break;
    case TokenKind::KeywordWhile:
      node->value = std::move(*parseWhile());
      break;
    case TokenKind::KeywordRepeat:
      node->value = std::move(*parseRepeat());
      break;
    case TokenKind::KeywordFor:
      node->value = std::move(*parseFor());
      break;
    case TokenKind::KeywordReturn:
      node->value = std::move(*parseReturn());
      break;
    case TokenKind::KeywordDefer:
      node->value = std::move(*parseDefer());
      break;
    case TokenKind::KeywordBreak:
      node->value = std::move(*parseBreak());
      break;
    case TokenKind::KeywordContinue:
      node->value = std::move(*parseContinue());
      break;
    case TokenKind::KeywordSwitch:
      node->value = std::move(*parseSwitch());
      break;
    case TokenKind::KeywordFunction:
      node->value = std::move(*parseFunction());
      break;
    case TokenKind::KeywordStruct:
      node->value = std::move(*parseStruct());
      break;
    default:
      node->value = std::move(*parseExpressionStatement());
      break;
  }

  return node;
}

std::unique_ptr<VariableDeclaration> Parser::parseVariable(bool isConst) {
  const Token start = current_;
  advance();

  auto node = std::make_unique<VariableDeclaration>();
  node->location = lexer_.locationAt(start.start);
  node->isConst = isConst;
  node->name = parseIdentifier(isConst ? "const name" : "var name");

  if (node->name.empty()) {
    synchronize();
    return node;
  }

  if (match(TokenKind::Colon)) {
    node->type = parseTypeName();
  }

  if (match(TokenKind::Equal)) {
    node->initializer = parseExpression();
  }

  if (!node->initializer && node->type.empty()) {
    error(current_, "variable declaration requires a type or initializer");
  }

  if (isConst && !node->initializer) {
    error(current_, "const declaration requires an initializer");
  }

  match(TokenKind::Semicolon);
  return node;
}

std::unique_ptr<IfStatement> Parser::parseIf() {
  const Token start = current_;
  advance();

  auto node = std::make_unique<IfStatement>();
  node->location = lexer_.locationAt(start.start);
  node->condition = parseExpression();
  node->thenBlock = parseBlock();

  if (match(TokenKind::KeywordElse)) {
    if (check(TokenKind::KeywordIf)) {
      node->elseBlock = std::make_unique<Block>();
      node->elseBlock->location = lexer_.locationAt(current_.start);

      auto nested = parseIf();
      auto statement = std::make_unique<Statement>();
      statement->value = std::move(*nested);
      node->elseBlock->statements.push_back(
          std::move(statement));
    } else {
      node->elseBlock = parseBlock();
    }
  }

  return node;
}

std::unique_ptr<WhileStatement> Parser::parseWhile() {
  const Token start = current_;
  advance();

  auto node = std::make_unique<WhileStatement>();
  node->location = lexer_.locationAt(start.start);
  node->condition = parseExpression();
  node->body = parseBlock();
  return node;
}

std::unique_ptr<RepeatStatement> Parser::parseRepeat() {
  const Token start = current_;
  advance();

  auto node = std::make_unique<RepeatStatement>();
  node->location = lexer_.locationAt(start.start);
  node->body = parseBlock();

  if (match(TokenKind::KeywordWhile)) {
    node->condition = parseExpression();
  } else {
    error(current_, "expected 'while' after repeat block");
  }

  return node;
}

std::unique_ptr<ForStatement> Parser::parseFor() {
  const Token start = current_;
  advance();

  auto node = std::make_unique<ForStatement>();
  node->location = lexer_.locationAt(start.start);
  node->variable = parseIdentifier("for variable");

  if (!expect(TokenKind::KeywordIn, "expected 'in' in for statement")) {
    synchronize();
    return node;
  }

  node->sequence = parseExpression();
  node->body = parseBlock();
  return node;
}

std::unique_ptr<ReturnStatement> Parser::parseReturn() {
  const Token start = current_;
  advance();

  auto node = std::make_unique<ReturnStatement>();
  node->location = lexer_.locationAt(start.start);

  if (!check(TokenKind::RightBrace) &&
      !check(TokenKind::Semicolon) &&
      !check(TokenKind::EndOfFile)) {
    node->value = parseExpression();
  }

  match(TokenKind::Semicolon);
  return node;
}

std::unique_ptr<DeferStatement> Parser::parseDefer() {
  const Token start = current_;
  advance();

  auto node = std::make_unique<DeferStatement>();
  node->location = lexer_.locationAt(start.start);
  node->body = parseBlock();
  return node;
}

std::unique_ptr<BreakStatement> Parser::parseBreak() {
  const Token start = current_;
  advance();

  auto node = std::make_unique<BreakStatement>();
  node->location = lexer_.locationAt(start.start);
  match(TokenKind::Semicolon);
  return node;
}

std::unique_ptr<ContinueStatement> Parser::parseContinue() {
  const Token start = current_;
  advance();

  auto node = std::make_unique<ContinueStatement>();
  node->location = lexer_.locationAt(start.start);
  match(TokenKind::Semicolon);
  return node;
}

std::unique_ptr<SwitchStatement> Parser::parseSwitch() {
  const Token start = current_;
  advance();

  auto node = std::make_unique<SwitchStatement>();
  node->location = lexer_.locationAt(start.start);
  node->cases.reserve(4);
  node->subject = parseExpression();

  if (!expect(TokenKind::LeftBrace, "expected '{' after switch expression")) {
    synchronize();
    return node;
  }

  bool sawDefault = false;
  while (!check(TokenKind::RightBrace) && !check(TokenKind::EndOfFile)) {
    if (match(TokenKind::Semicolon) || match(TokenKind::Comment)) {
      continue;
    }

    const bool isCase = check(TokenKind::KeywordCase);
    const bool isDefault = check(TokenKind::KeywordDefat);
    if (!isCase && !isDefault) {
      error(current_, "expected 'case' or 'defat' in switch");
      synchronize();
      continue;
    }

    const Token caseStart = current_;
    advance();

    auto caseNode = std::make_unique<SwitchCase>();
    caseNode->location = lexer_.locationAt(caseStart.start);
    caseNode->isDefault = isDefault;

    if (isDefault) {
      if (sawDefault) {
        error(caseStart, "duplicate default switch case");
      }
      sawDefault = true;
    } else {
      caseNode->condition = parseExpression();
    }

    caseNode->body = parseBlock();
    node->cases.push_back(std::move(caseNode));
  }

  expect(TokenKind::RightBrace, "expected '}' after switch statement");
  return node;
}

std::unique_ptr<ExpressionStatement> Parser::parseExpressionStatement() {
  auto node = std::make_unique<ExpressionStatement>();
  node->location = lexer_.locationAt(current_.start);
  node->expression = parseExpression();
  match(TokenKind::Semicolon);
  return node;
}

std::unique_ptr<Block> Parser::parseBlock() {
  auto node = std::make_unique<Block>();
  node->statements.reserve(8);
  node->location = lexer_.locationAt(current_.start);

  if (!expect(TokenKind::LeftBrace, "expected '{' to begin block")) {
    return node;
  }

  while (!check(TokenKind::RightBrace) &&
         !check(TokenKind::EndOfFile)) {
    if (match(TokenKind::Semicolon) ||
        match(TokenKind::Comment)) {
      continue;
    }

    auto statement = parseStatement();
    if (statement) {
      node->statements.push_back(std::move(statement));
    }
  }

  expect(TokenKind::RightBrace, "expected '}' to close block");
  return node;
}

std::unique_ptr<Expression> Parser::parseExpression() {
  return parseAssignment();
}

std::unique_ptr<Expression> Parser::parseAssignment() {
  auto left = parseLogicalOr();

  if (!left || !isAssignmentOperator(current_.kind)) {
    return left;
  }

  const Token operatorToken = current_;
  const std::string op = operatorText(current_.kind);
  advance();

  auto right = parseAssignment();
  if (!right) {
    error(current_, "expected expression after assignment operator");
    return left;
  }

  if (!isAssignableExpression(left.get())) {
    error(operatorToken,
          "left side of assignment must be an identifier or member expression");
  }

  auto node = std::make_unique<Expression>();
  node->location = lexer_.locationAt(operatorToken.start);

  AssignmentExpression assignment;
  assignment.op = op;
  assignment.target = std::move(left);
  assignment.value = std::move(right);
  node->value = std::move(assignment);
  return node;
}

// Logical OR is the lowest binary precedence level in the current grammar.
// Parsing it separately preserves short-circuit grouping for later lowering.
std::unique_ptr<Expression> Parser::parseLogicalOr() {
  auto left = parseLogicalAnd();

  while (match(TokenKind::OrOr)) {
    const Token operatorToken = previous_;
    auto right = parseLogicalAnd();

    if (!right) {
      error(current_, "expected expression after '||'");
      break;
    }

    auto node = std::make_unique<Expression>();
    node->location = lexer_.locationAt(operatorToken.start);

    BinaryExpression binary;
    binary.op = operatorText(operatorToken.kind);
    binary.left = std::move(left);
    binary.right = std::move(right);
    node->value = std::move(binary);
    left = std::move(node);
  }

  return left;
}

// Logical AND binds more tightly than OR but less tightly than equality.
// The loop keeps long boolean expressions iterative instead of recursive.
std::unique_ptr<Expression> Parser::parseLogicalAnd() {
  auto left = parseEquality();

  while (match(TokenKind::AndAnd)) {
    const Token operatorToken = previous_;
    auto right = parseEquality();

    if (!right) {
      error(current_, "expected expression after '&&'");
      break;
    }

    auto node = std::make_unique<Expression>();
    node->location = lexer_.locationAt(operatorToken.start);

    BinaryExpression binary;
    binary.op = operatorText(operatorToken.kind);
    binary.left = std::move(left);
    binary.right = std::move(right);
    node->value = std::move(binary);
    left = std::move(node);
  }

  return left;
}

// Equality operators share one precedence level.  The parser keeps their
// original spelling in the AST so later stages can apply Sift semantics.
std::unique_ptr<Expression> Parser::parseEquality() {
  auto left = parseComparison();

  while (check(TokenKind::EqualEqual) ||
         check(TokenKind::BangEqual) ||
         check(TokenKind::TildeEqual)) {
    const Token operatorToken = current_;
    advance();

    auto right = parseComparison();
    if (!right) {
      error(current_, "expected expression after equality operator");
      break;
    }

    auto node = std::make_unique<Expression>();
    node->location = lexer_.locationAt(operatorToken.start);

    BinaryExpression binary;
    binary.op = operatorText(operatorToken.kind);
    binary.left = std::move(left);
    binary.right = std::move(right);
    node->value = std::move(binary);
    left = std::move(node);
  }

  return left;
}

// Relational operators are folded from left to right at comparison level.
// Semantic analysis can later validate any chained comparison restrictions.
std::unique_ptr<Expression> Parser::parseComparison() {
  auto left = parseTerm();

  while (check(TokenKind::Less) ||
         check(TokenKind::LessEqual) ||
         check(TokenKind::Greater) ||
         check(TokenKind::GreaterEqual)) {
    const Token operatorToken = current_;
    advance();

    auto right = parseTerm();
    if (!right) {
      error(current_, "expected expression after comparison operator");
      break;
    }

    auto node = std::make_unique<Expression>();
    node->location = lexer_.locationAt(operatorToken.start);

    BinaryExpression binary;
    binary.op = operatorText(operatorToken.kind);
    binary.left = std::move(left);
    binary.right = std::move(right);
    node->value = std::move(binary);
    left = std::move(node);
  }

  return left;
}

// Addition and subtraction are parsed iteratively.  This avoids recursive
// stack growth for generated expressions containing many terms.
std::unique_ptr<Expression> Parser::parseTerm() {
  auto left = parseFactor();

  while (check(TokenKind::Plus) ||
         check(TokenKind::Minus)) {
    const Token operatorToken = current_;
    advance();

    auto right = parseFactor();
    if (!right) {
      error(current_, "expected expression after arithmetic operator");
      break;
    }

    auto node = std::make_unique<Expression>();
    node->location = lexer_.locationAt(operatorToken.start);

    BinaryExpression binary;
    binary.op = operatorText(operatorToken.kind);
    binary.left = std::move(left);
    binary.right = std::move(right);
    node->value = std::move(binary);
    left = std::move(node);
  }

  return left;
}

// Multiplicative operators bind tighter than addition and subtraction.
// Keeping this loop iterative is important for long arithmetic expressions.
std::unique_ptr<Expression> Parser::parseFactor() {
  auto left = parseUnary();

  while (check(TokenKind::Star) ||
         check(TokenKind::Slash) ||
         check(TokenKind::Percent)) {
    const Token operatorToken = current_;
    advance();

    auto right = parseUnary();
    if (!right) {
      error(current_, "expected expression after multiplicative operator");
      break;
    }

    auto node = std::make_unique<Expression>();
    node->location = lexer_.locationAt(operatorToken.start);

    BinaryExpression binary;
    binary.op = operatorText(operatorToken.kind);
    binary.left = std::move(left);
    binary.right = std::move(right);
    node->value = std::move(binary);
    left = std::move(node);
  }

  return left;
}

// Unary operators associate from the outside inward, so this level is
// naturally recursive while postfix parsing remains iterative.
std::unique_ptr<Expression> Parser::parseUnary() {
  if (check(TokenKind::Bang) ||
      check(TokenKind::Minus) ||
      check(TokenKind::Plus) ||
      check(TokenKind::KeywordError) ||
      check(TokenKind::KeywordPanic)) {
    const Token operatorToken = current_;
    advance();

    auto operand = parseUnary();
    if (!operand) {
      error(current_, "expected expression after unary operator");
      return nullptr;
    }

    auto node = std::make_unique<Expression>();
    node->location = lexer_.locationAt(operatorToken.start);

    UnaryExpression unary;
    unary.op = operatorText(operatorToken.kind);
    unary.operand = std::move(operand);
    node->value = std::move(unary);
    return node;
  }

  return parsePostfix();
}

std::unique_ptr<Expression> Parser::parsePostfix() {
  auto expression = parsePrimary();

  while (expression) {
    if (match(TokenKind::Dot)) {
      const Token memberToken = current_;

      if (!current_.isIdentifier()) {
        error(current_, "expected a name after '.'");
        return expression;
      }

      advance();

      auto node = std::make_unique<Expression>();
      node->location = lexer_.locationAt(memberToken.start);

      MemberExpression member;
      member.base = std::move(expression);
      member.member = tokenText(memberToken);
      node->value = std::move(member);
      expression = std::move(node);
      continue;
    }

    if (match(TokenKind::LeftParen)) {
      auto node = std::make_unique<Expression>();
      node->location = lexer_.locationAt(previous_.start);

      CallExpression call;
      call.arguments.reserve(4);
      call.callee = std::move(expression);

      if (!check(TokenKind::RightParen)) {
        do {
          auto argument = parseExpression();
          if (!argument) {
            error(current_, "expected call argument");
            break;
          }
          call.arguments.push_back(std::move(argument));
        } while (match(TokenKind::Comma));
      }

      expect(TokenKind::RightParen, "expected ')' after arguments");
      node->value = std::move(call);
      expression = std::move(node);
      continue;
    }

    break;
  }

  return expression;
}

std::unique_ptr<Expression> Parser::parsePrimary() {
  const Token token = current_;

  if (token.kind == TokenKind::Identifier ||
      token.kind == TokenKind::CallingName) {
    advance();

    auto node = std::make_unique<Expression>();
    node->location = lexer_.locationAt(token.start);

    IdentifierExpression identifier;
    identifier.name = tokenText(token);
    node->value = std::move(identifier);
    return node;
  }

  if (token.isLiteral()) {
    advance();

    auto node = std::make_unique<Expression>();
    node->location = lexer_.locationAt(token.start);

    LiteralExpression literal;
    literal.literalKind = token.kind;
    literal.value = tokenText(token);
    node->value = std::move(literal);
    return node;
  }

  if (match(TokenKind::LeftParen)) {
    auto expression = parseExpression();
    expect(TokenKind::RightParen, "expected ')' after expression");
    return expression;
  }

  error(token, "expected an expression");
  return nullptr;
}

// Type names are stored as complete spellings, including qualified names.
// Semantic analysis can resolve that spelling without reparsing tokens.
std::string Parser::parseTypeName() {
  if (current_.kind == TokenKind::Identifier ||
      current_.kind == TokenKind::KeywordInt ||
      current_.kind == TokenKind::KeywordNum ||
      current_.kind == TokenKind::KeywordString ||
      current_.kind == TokenKind::KeywordBool ||
      current_.kind == TokenKind::KeywordBytes ||
      current_.kind == TokenKind::KeywordAny ||
      current_.kind == TokenKind::KeywordSome) {
    std::string type = tokenText(current_);
    advance();

    while (match(TokenKind::Dot)) {
      if (!current_.isIdentifier()) {
        error(current_, "expected type name after '.'");
        break;
      }

      type += ".";
      type += tokenText(current_);
      advance();
    }

    return type;
  }

  error(current_, "expected a type name");
  return {};
}

// Declaration names are parsed through one routine so diagnostics and token
// consumption stay consistent across all declaration kinds.
std::string Parser::parseIdentifier(std::string_view context) {
  if (!current_.isIdentifier()) {
    error(current_, std::string("expected ") + std::string(context));
    return {};
  }

  const std::string value = tokenText(current_);
  advance();
  return value;
}

// A Sift function has exactly one calling name inside its parentheses.
// Duplicate calling-name detection is handled by the declaration parser.
std::string Parser::parseCallingName() {
  if (current_.kind != TokenKind::CallingName &&
      current_.kind != TokenKind::Identifier) {
    error(current_, "expected the single calling name inside function parentheses");
    return {};
  }

  const std::string value = tokenText(current_);
  advance();
  return value;
}

// Parser recovery is token-based rather than character-based.  Once a
// production fails, synchronize() advances to a declaration, control-flow
// boundary, semicolon, closing brace, or EOF.  This keeps later diagnostics
// useful without rescanning the source from the beginning.
//
// Returning partial AST nodes after recoverable errors is intentional.  Tools
// such as syntax inspection can still consume the tree while the compiler
// checks hasErrors() before attempting code generation.
//
// Token text is copied into AST-owned strings because lexer token views refer
// to the parser source buffer.  The AST must own its names independently.
std::string Parser::tokenText(const Token& token) const {
  return std::string(token.text());
}

bool Parser::isAssignmentOperator(TokenKind kind) const noexcept {
  return kind == TokenKind::Equal ||
         kind == TokenKind::PlusEqual ||
         kind == TokenKind::MinusEqual ||
         kind == TokenKind::StarEqual ||
         kind == TokenKind::SlashEqual ||
         kind == TokenKind::PercentEqual;
}

std::string Parser::operatorText(TokenKind kind) {
  switch (kind) {
    case TokenKind::Equal: return "=";
    case TokenKind::EqualEqual: return "==";
    case TokenKind::Bang: return "!";
    case TokenKind::BangEqual: return "!=";
    case TokenKind::Plus: return "+";
    case TokenKind::PlusEqual: return "+=";
    case TokenKind::Minus: return "-";
    case TokenKind::MinusEqual: return "-=";
    case TokenKind::Star: return "*";
    case TokenKind::StarEqual: return "*=";
    case TokenKind::Slash: return "/";
    case TokenKind::SlashEqual: return "/=";
    case TokenKind::Percent: return "%";
    case TokenKind::PercentEqual: return "%=";
    case TokenKind::Less: return "<";
    case TokenKind::LessEqual: return "<=";
    case TokenKind::Greater: return ">";
    case TokenKind::GreaterEqual: return ">=";
    case TokenKind::AndAnd: return "&&";
    case TokenKind::OrOr: return "||";
    case TokenKind::Tilde: return "~";
    case TokenKind::TildeEqual: return "~=";
    case TokenKind::Arrow: return "->";
    case TokenKind::KeywordError: return "error";
    case TokenKind::KeywordPanic: return "panic";
    default: return {};
  }
}

} // namespace sift::parse
