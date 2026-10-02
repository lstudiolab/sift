#include "sift/Parse/Parser.h"

#include <utility>

namespace sift::parse {

namespace {

// Fast token-category predicates keep grammar dispatch branch-light.

class DepthGuard final {
public:
  explicit DepthGuard(std::size_t& depth) noexcept : depth_(depth) {
    ++depth_;
  }

  ~DepthGuard() {
    --depth_;
  }

  DepthGuard(const DepthGuard&) = delete;
  DepthGuard& operator=(const DepthGuard&) = delete;

private:
  std::size_t& depth_;
};

bool isDeclarationKeyword(TokenKind kind) noexcept {
  switch (kind) {
    case TokenKind::KeywordVar:
    case TokenKind::KeywordConst:
    case TokenKind::KeywordFunction:
    case TokenKind::KeywordStruct:
      return true;
    default:
      return false;
  }
}

bool isControlKeyword(TokenKind kind) noexcept {
  switch (kind) {
    case TokenKind::KeywordIf:
    case TokenKind::KeywordWhile:
    case TokenKind::KeywordRepeat:
    case TokenKind::KeywordFor:
    case TokenKind::KeywordReturn:
    case TokenKind::KeywordDefer:
    case TokenKind::KeywordSwitch:
    case TokenKind::KeywordBreak:
    case TokenKind::KeywordContinue:
      return true;
    default:
      return false;
  }
}

bool isAssignableExpression(const Expression* expression) noexcept {
  if (expression == nullptr) return false;
  return expression->kind() == NodeKind::IdentifierExpression ||
         expression->kind() == NodeKind::MemberExpression;
}

bool isTypeToken(TokenKind kind) noexcept {
  switch (kind) {
    case TokenKind::Identifier:
    case TokenKind::KeywordInt:
    case TokenKind::KeywordNum:
    case TokenKind::KeywordString:
    case TokenKind::KeywordBool:
    case TokenKind::KeywordBytes:
    case TokenKind::KeywordAny:
    case TokenKind::KeywordSome:
      return true;
    default:
      return false;
  }
}

bool isUnaryOperator(TokenKind kind) noexcept {
  switch (kind) {
    case TokenKind::Bang:
    case TokenKind::Minus:
    case TokenKind::Plus:
    case TokenKind::KeywordError:
    case TokenKind::KeywordPanic:
      return true;
    default:
      return false;
  }
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
  return hasParserErrors_ || lexer_.hasErrors();
}

void Parser::advance() {
  previous_ = current_;
  current_ = lexer_.lex();
}

bool Parser::canBreak() const noexcept {
  return loopDepth_ != 0 || switchDepth_ != 0;
}

bool Parser::canContinue() const noexcept {
  return loopDepth_ != 0;
}

bool Parser::canReturn() const noexcept {
  return functionDepth_ != 0;
}

bool Parser::enterExpression() {
  if (expressionDepth_ >= maxExpressionDepth_) {
    return false;
  }

  ++expressionDepth_;
  return true;
}

void Parser::leaveExpression() noexcept {
  if (expressionDepth_ != 0) {
    --expressionDepth_;
  }
}

bool Parser::enterAssignment() {
  if (assignmentDepth_ >= maxAssignmentDepth_) {
    return false;
  }

  ++assignmentDepth_;
  return true;
}

void Parser::leaveAssignment() noexcept {
  if (assignmentDepth_ != 0) {
    --assignmentDepth_;
  }
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
  hasParserErrors_ = true;

  if (diagnostics_.size() >= maxDiagnostics_) {
    if (!diagnosticsTruncated_) {
      diagnosticsTruncated_ = true;
      diagnostics_.push_back({
          DiagnosticSeverity::Error,
          lexer_.locationAt(token.start),
          "too many parser errors; further diagnostics suppressed"
      });
    }
    return;
  }

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
        isControlKeyword(current_.kind) ||
        current_.kind == TokenKind::KeywordCase ||
        current_.kind == TokenKind::KeywordDefat) {
      return;
    }

    advance();
  }
}

void Parser::synchronizeExpression() {
  while (!check(TokenKind::EndOfFile)) {
    switch (current_.kind) {
      case TokenKind::Semicolon:
      case TokenKind::RightBrace:
      case TokenKind::RightParen:
      case TokenKind::Comma:
      case TokenKind::KeywordCase:
      case TokenKind::KeywordDefat:
        return;
      default:
        advance();
        break;
    }
  }
}

void Parser::synchronizeToBlockStart() {
  while (!check(TokenKind::EndOfFile)) {
    switch (current_.kind) {
      case TokenKind::LeftBrace:
      case TokenKind::RightBrace:
      case TokenKind::Semicolon:
        return;
      default:
        if (isDeclarationKeyword(current_.kind) ||
            isControlKeyword(current_.kind)) {
          return;
        }
        advance();
        break;
    }
  }
}

// Parse the translation unit without recursive top-level descent.
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

    const Token before = current_;

    switch (current_.kind) {
      case TokenKind::KeywordImport:
        program->imports.push_back(parseImport());
        break;

      case TokenKind::KeywordStruct:
        program->structs.push_back(parseStruct());
        break;

      case TokenKind::KeywordFunction:
        program->functions.push_back(parseFunction());
        break;

      default:
        if (auto statement = parseStatement()) {
          program->statements.push_back(std::move(statement));
        }
        break;
    }

    if (current_.start == before.start &&
        current_.kind == before.kind) {
      error(current_, "parser made no progress while parsing the program");
      advance();
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
    const Token before = current_;
    synchronize();

    if (current_.start == before.start &&
        current_.kind == before.kind) {
      advance();
    }
  }

  expect(TokenKind::RightBrace, "expected '}' after struct declaration");
  return node;
}

std::unique_ptr<FunctionDeclaration> Parser::parseFunction() {
  const Token start = current_;
  advance();

  DepthGuard functionScope(functionDepth_);

  auto node = std::make_unique<FunctionDeclaration>();
  node->parameters.reserve(4);
  node->location = lexer_.locationAt(start.start);
  node->name = parseIdentifier("function name");

  if (!functionNames_.insert(node->name).second) {
    error(previous_, "duplicate function name in this file");
  }

  if (!expect(TokenKind::LeftParen, "expected '(' after function name")) {
    synchronize();
    if (!check(TokenKind::LeftBrace)) {
      synchronizeToBlockStart();
    }
    if (!check(TokenKind::LeftBrace)) {
      return node;
    }
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

  if (!expect(TokenKind::RightParen, "expected ')' after function calling name")) {
    synchronizeToBlockStart();
  }

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
      if (auto parsed = parseVariable(false)) {
        node->value = std::move(*parsed);
      }
      break;
    case TokenKind::KeywordConst:
      if (auto parsed = parseVariable(true)) {
        node->value = std::move(*parsed);
      }
      break;
    case TokenKind::KeywordIf:
      if (auto parsed = parseIf()) {
        node->value = std::move(*parsed);
      }
      break;
    case TokenKind::KeywordWhile:
      if (auto parsed = parseWhile()) {
        node->value = std::move(*parsed);
      }
      break;
    case TokenKind::KeywordRepeat:
      if (auto parsed = parseRepeat()) {
        node->value = std::move(*parsed);
      }
      break;
    case TokenKind::KeywordFor:
      if (auto parsed = parseFor()) {
        node->value = std::move(*parsed);
      }
      break;
    case TokenKind::KeywordReturn:
      if (auto parsed = parseReturn()) {
        node->value = std::move(*parsed);
      }
      break;
    case TokenKind::KeywordDefer:
      if (auto parsed = parseDefer()) {
        node->value = std::move(*parsed);
      }
      break;
    case TokenKind::KeywordBreak:
      if (auto parsed = parseBreak()) {
        node->value = std::move(*parsed);
      }
      break;
    case TokenKind::KeywordContinue:
      if (auto parsed = parseContinue()) {
        node->value = std::move(*parsed);
      }
      break;
    case TokenKind::KeywordSwitch:
      if (auto parsed = parseSwitch()) {
        node->value = std::move(*parsed);
      }
      break;
    case TokenKind::KeywordFunction:
      if (auto parsed = parseFunction()) {
        node->value = std::move(*parsed);
      }
      break;
    case TokenKind::KeywordStruct:
      if (auto parsed = parseStruct()) {
        node->value = std::move(*parsed);
      }
      break;
    default:
      if (auto parsed = parseExpressionStatement()) {
        node->value = std::move(*parsed);
      }
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

      if (auto nested = parseIf()) {
        auto statement = std::make_unique<Statement>();
        statement->value = std::move(*nested);
        node->elseBlock->statements.push_back(
            std::move(statement));
      }
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

  DepthGuard loopScope(loopDepth_);
  node->body = parseBlock();

  return node;
}

std::unique_ptr<RepeatStatement> Parser::parseRepeat() {
  const Token start = current_;
  advance();

  auto node = std::make_unique<RepeatStatement>();
  node->location = lexer_.locationAt(start.start);

  DepthGuard loopScope(loopDepth_);
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

  DepthGuard loopScope(loopDepth_);
  node->body = parseBlock();

  return node;
}

std::unique_ptr<ReturnStatement> Parser::parseReturn() {
  const Token start = current_;
  advance();

  auto node = std::make_unique<ReturnStatement>();
  node->location = lexer_.locationAt(start.start);

  if (!canReturn()) {
    error(start, "'return' is only valid inside a function");
  }

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

  if (!canBreak()) {
    error(start, "'break' is only valid inside a loop or switch");
  }
  match(TokenKind::Semicolon);
  return node;
}

std::unique_ptr<ContinueStatement> Parser::parseContinue() {
  const Token start = current_;
  advance();

  auto node = std::make_unique<ContinueStatement>();
  node->location = lexer_.locationAt(start.start);

  if (!canContinue()) {
    error(start, "'continue' is only valid inside a loop");
  }
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
    synchronizeToBlockStart();
    if (!check(TokenKind::LeftBrace)) {
      return node;
    }
  }

  bool sawDefault = false;
  bool hasCase = false;
  DepthGuard switchScope(switchDepth_);

  while (!check(TokenKind::RightBrace) && !check(TokenKind::EndOfFile)) {
    if (match(TokenKind::Semicolon) || match(TokenKind::Comment)) {
      continue;
    }

    const bool isCase = check(TokenKind::KeywordCase);
    const bool isDefault = check(TokenKind::KeywordDefat);
    if (!isCase && !isDefault) {
      error(current_, "expected 'case' or 'defat' in switch");
      const Token before = current_;
      synchronize();

      if (current_.start == before.start &&
          current_.kind == before.kind) {
        advance();
      }
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
      hasCase = true;
      caseNode->condition = parseExpression();

      if (!caseNode->condition) {
        error(caseStart, "expected a case expression");
      }
    }

    caseNode->body = parseBlock();
    node->cases.push_back(std::move(caseNode));
  }

  expect(TokenKind::RightBrace, "expected '}' after switch statement");

  if (!hasCase && !sawDefault) {
    error(start, "switch statement requires at least one case");
  }

  return node;
}

std::unique_ptr<ExpressionStatement> Parser::parseExpressionStatement() {
  auto node = std::make_unique<ExpressionStatement>();
  node->location = lexer_.locationAt(current_.start);
  node->expression = parseExpression();

  if (!node->expression) {
    synchronizeExpression();
  }

  match(TokenKind::Semicolon);
  return node;
}

std::unique_ptr<Block> Parser::parseBlock() {
  auto node = std::make_unique<Block>();
  node->statements.reserve(8);
  node->location = lexer_.locationAt(current_.start);

  if (!expect(TokenKind::LeftBrace, "expected '{' to begin block")) {
    synchronizeToBlockStart();
    if (!match(TokenKind::LeftBrace)) {
      return node;
    }
  }

  while (!check(TokenKind::RightBrace) &&
         !check(TokenKind::EndOfFile)) {
    if (match(TokenKind::Semicolon) ||
        match(TokenKind::Comment)) {
      continue;
    }

    const Token before = current_;
    auto statement = parseStatement();

    if (statement) {
      node->statements.push_back(std::move(statement));
    }

    if (current_.start == before.start &&
        current_.kind == before.kind) {
      error(current_, "parser made no progress while parsing a block");
      advance();
    }
  }

  expect(TokenKind::RightBrace, "expected '}' to close block");
  return node;
}

std::unique_ptr<Expression> Parser::parseExpression() {
  return parseAssignment();
}

// Assignment is right-associative and is the only expression level that recurses.
std::unique_ptr<Expression> Parser::parseAssignment() {
  if (!enterAssignment()) {
    error(current_, "assignment nesting exceeds parser limit");
    synchronizeExpression();
    return nullptr;
  }

  auto left = parseBinaryExpression(1);

  if (!left || !isAssignmentOperator(current_.kind)) {
    leaveAssignment();
    return left;
  }

  const Token operatorToken = current_;
  const std::string_view op = operatorText(current_.kind);
  advance();

  auto right = parseAssignment();
  if (!right) {
    error(current_, "expected expression after assignment operator");
    synchronizeExpression();
    leaveAssignment();
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
  leaveAssignment();
  return node;
}

int Parser::binaryPrecedence(TokenKind kind) noexcept {
  switch (kind) {
    case TokenKind::OrOr: return 1;
    case TokenKind::AndAnd: return 2;
    case TokenKind::EqualEqual:
    case TokenKind::BangEqual:
    case TokenKind::TildeEqual: return 3;
    case TokenKind::Less:
    case TokenKind::LessEqual:
    case TokenKind::Greater:
    case TokenKind::GreaterEqual: return 4;
    case TokenKind::Plus:
    case TokenKind::Minus: return 5;
    case TokenKind::Star:
    case TokenKind::Slash:
    case TokenKind::Percent: return 6;
    default: return 0;
  }
}

std::unique_ptr<Expression> Parser::parseBinaryExpression(int minimumPrecedence) {
  auto left = parseUnary();
  if (!left) {
    return nullptr;
  }

  while (true) {
    const int precedence = binaryPrecedence(current_.kind);
    if (precedence < minimumPrecedence) {
      break;
    }

    const Token operatorToken = current_;
    const TokenKind operatorKind = current_.kind;
    advance();

    auto right = parseBinaryExpression(precedence + 1);
    if (!right) {
      error(current_, "expected expression after binary operator");
      return left;
    }

    auto node = std::make_unique<Expression>();
    node->location = lexer_.locationAt(operatorToken.start);

    BinaryExpression binary;
    binary.op = operatorText(operatorKind);
    binary.left = std::move(left);
    binary.right = std::move(right);
    node->value = std::move(binary);
    left = std::move(node);
  }

  return left;
}

std::unique_ptr<Expression> Parser::parseUnary() {
  if (!enterExpression()) {
    error(current_, "expression nesting exceeds parser limit");
    return nullptr;
  }

  if (isUnaryOperator(current_.kind)) {
    const Token operatorToken = current_;
    advance();

    auto operand = parseUnary();
    if (!operand) {
      error(current_, "expected expression after unary operator");
      leaveExpression();
      return nullptr;
    }

    auto node = std::make_unique<Expression>();
    node->location = lexer_.locationAt(operatorToken.start);

    UnaryExpression unary;
    unary.op = operatorText(operatorToken.kind);
    unary.operand = std::move(operand);
    node->value = std::move(unary);
    leaveExpression();
    return node;
  }

  auto expression = parsePostfix();
  leaveExpression();
  return expression;
}

// Postfix parsing is iterative so long member/call chains stay stack-safe.
std::unique_ptr<Expression> Parser::parsePostfix() {
  auto expression = parsePrimary();

  while (expression) {
    if (match(TokenKind::Dot)) {
      const Token memberToken = current_;

      if (!current_.isIdentifier()) {
        error(current_, "expected a name after '.'");
        synchronizeExpression();
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
            synchronizeExpression();
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
  synchronizeExpression();
  return nullptr;
}

std::string Parser::parseTypeName() {
  if (isTypeToken(current_.kind)) {
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

std::string Parser::parseIdentifier(std::string_view context) {
  if (!current_.isIdentifier()) {
    error(current_, std::string("expected ") + std::string(context));
    return {};
  }

  const std::string value = tokenText(current_);
  advance();
  return value;
}

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

// Token spelling is copied only when it becomes AST-owned state.
std::string Parser::tokenText(const Token& token) const {
  return std::string(token.text());
}

bool Parser::isAssignmentOperator(TokenKind kind) const noexcept {
  switch (kind) {
    case TokenKind::Equal:
    case TokenKind::PlusEqual:
    case TokenKind::MinusEqual:
    case TokenKind::StarEqual:
    case TokenKind::SlashEqual:
    case TokenKind::PercentEqual:
      return true;
    default:
      return false;
  }
}

std::string_view Parser::operatorText(TokenKind kind) noexcept {
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
