#include "sift/Parse/Parser.h"

#include <algorithm>
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
         kind == TokenKind::KeywordDefer;
}

} // namespace

Parser::Parser(std::string_view source)
    : lexer_(source) {
  advance();
}

const std::vector<Diagnostic>& Parser::diagnostics() const noexcept {
  return diagnostics_;
}

bool Parser::hasErrors() const noexcept {
  if (!diagnostics_.empty()) {
    return std::any_of(
        diagnostics_.begin(),
        diagnostics_.end(),
        [](const Diagnostic& diagnostic) {
          return diagnostic.severity == DiagnosticSeverity::Error;
        });
  }

  return lexer_.hasErrors();
}

void Parser::advance() {
  previous_ = current_;
  current_ = lexer_.lex();
  hasCurrent_ = true;
}

Token Parser::peek() const {
  return current_;
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

    try {
      if (check(TokenKind::KeywordImport)) {
        program->imports.push_back(parseImport());
        continue;
      }

      if (check(TokenKind::KeywordStruct)) {
        program->structs.push_back(parseStruct());
        continue;
      }

      if (check(TokenKind::KeywordFunction)) {
        program->functions.push_back(parseFunction());
        continue;
      }

      if (check(TokenKind::KeywordVar)) {
        {
        auto declaration = parseVariable(false);
        auto statement = std::make_unique<Statement>();
        statement->value = std::move(*declaration);
        program->statements.push_back(std::move(statement));
      }
        continue;
      }

      if (check(TokenKind::KeywordConst)) {
        {
        auto declaration = parseVariable(true);
        auto statement = std::make_unique<Statement>();
        statement->value = std::move(*declaration);
        program->statements.push_back(std::move(statement));
      }
        continue;
      }

      program->statements.push_back(parseStatement());
    } catch (...) {
      error(current_, "parser aborted while processing this declaration");
      synchronize();
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
  node->location = lexer_.locationAt(start.start);
  node->name = parseIdentifier("struct name");

  if (containsName(structNames_, node->name)) {
    error(previous_, "duplicate struct name");
  } else {
    structNames_.push_back(node->name);
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
  node->location = lexer_.locationAt(start.start);
  node->name = parseIdentifier("function name");

  if (containsName(functionNames_, node->name)) {
    error(previous_, "duplicate function name in this file");
  } else {
    functionNames_.push_back(node->name);
  }

  if (!expect(TokenKind::LeftParen, "expected '(' after function name")) {
    synchronize();
    return node;
  }

  if (check(TokenKind::RightParen)) {
    error(current_, "Sift functions require exactly one calling name");
  } else {
    node->callingName = parseCallingName();

    if (containsName(callingNames_, node->callingName)) {
      error(previous_, "calling name is already used by another function in this file");
    } else {
      callingNames_.push_back(node->callingName);
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
  if (check(TokenKind::KeywordVar)) {
    auto node = std::make_unique<Statement>();
    node->value = std::move(*parseVariable(false));
    return node;
  }

  if (check(TokenKind::KeywordConst)) {
    auto node = std::make_unique<Statement>();
    node->value = std::move(*parseVariable(true));
    return node;
  }

  if (check(TokenKind::KeywordIf)) {
    auto node = std::make_unique<Statement>();
    node->value = std::move(*parseIf());
    return node;
  }

  if (check(TokenKind::KeywordWhile)) {
    auto node = std::make_unique<Statement>();
    node->value = std::move(*parseWhile());
    return node;
  }

  if (check(TokenKind::KeywordRepeat)) {
    auto node = std::make_unique<Statement>();
    node->value = std::move(*parseRepeat());
    return node;
  }

  if (check(TokenKind::KeywordFor)) {
    auto node = std::make_unique<Statement>();
    node->value = std::move(*parseFor());
    return node;
  }

  if (check(TokenKind::KeywordReturn)) {
    auto node = std::make_unique<Statement>();
    node->value = std::move(*parseReturn());
    return node;
  }

  if (check(TokenKind::KeywordDefer)) {
    auto node = std::make_unique<Statement>();
    node->value = std::move(*parseDefer());
    return node;
  }

  if (check(TokenKind::KeywordFunction)) {
    auto node = std::make_unique<Statement>();
    node->value = std::move(*parseFunction());
    return node;
  }

  if (check(TokenKind::KeywordStruct)) {
    auto node = std::make_unique<Statement>();
    node->value = std::move(*parseStruct());
    return node;
  }

  {
    auto expression = parseExpressionStatement();
    auto statement = std::make_unique<Statement>();
    statement->value = std::move(*expression);
    return statement;
  }
}

std::unique_ptr<VariableDeclaration> Parser::parseVariable(bool isConst) {
  const Token start = current_;
  advance();

  auto node = std::make_unique<VariableDeclaration>();
  node->location = lexer_.locationAt(start.start);
  node->isConst = isConst;
  node->name = parseIdentifier(isConst ? "const name" : "var name");

  if (match(TokenKind::Colon)) {
    node->type = parseTypeName();
  }

  if (match(TokenKind::Equal)) {
    node->initializer = parseExpression();
  }

  if (!node->initializer && node->type.empty()) {
    error(current_, "variable declaration requires a type or initializer");
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

std::unique_ptr<ExpressionStatement> Parser::parseExpressionStatement() {
  auto node = std::make_unique<ExpressionStatement>();
  node->location = lexer_.locationAt(current_.start);
  node->expression = parseExpression();
  match(TokenKind::Semicolon);
  return node;
}

std::unique_ptr<Block> Parser::parseBlock() {
  auto node = std::make_unique<Block>();
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

  auto node = std::make_unique<Expression>();
  node->location = lexer_.locationAt(operatorToken.start);

  AssignmentExpression assignment;
  assignment.op = op;
  assignment.target = std::move(left);
  assignment.value = std::move(right);
  node->value = std::move(assignment);
  return node;
}

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

std::string Parser::tokenText(const Token& token) const {
  return std::string(token.text());
}

bool Parser::isExpressionStart(TokenKind kind) const noexcept {
  return kind == TokenKind::Identifier ||
         kind == TokenKind::CallingName ||
         kind == TokenKind::IntegerLiteral ||
         kind == TokenKind::FloatingLiteral ||
         kind == TokenKind::StringLiteral ||
         kind == TokenKind::CharacterLiteral ||
         kind == TokenKind::KeywordTrue ||
         kind == TokenKind::KeywordFalse ||
         kind == TokenKind::LeftParen ||
         kind == TokenKind::Bang ||
         kind == TokenKind::Minus ||
         kind == TokenKind::Plus;
}

bool Parser::isStatementStart(TokenKind kind) const noexcept {
  return isDeclarationKeyword(kind) ||
         isControlKeyword(kind) ||
         isExpressionStart(kind);
}

bool Parser::isTypeToken(TokenKind kind) const noexcept {
  return kind == TokenKind::Identifier ||
         kind == TokenKind::KeywordInt ||
         kind == TokenKind::KeywordNum ||
         kind == TokenKind::KeywordString ||
         kind == TokenKind::KeywordBool ||
         kind == TokenKind::KeywordBytes ||
         kind == TokenKind::KeywordAny ||
         kind == TokenKind::KeywordSome;
}

bool Parser::isAssignmentOperator(TokenKind kind) const noexcept {
  return kind == TokenKind::Equal ||
         kind == TokenKind::PlusEqual ||
         kind == TokenKind::MinusEqual ||
         kind == TokenKind::StarEqual ||
         kind == TokenKind::SlashEqual ||
         kind == TokenKind::PercentEqual;
}

int Parser::precedence(TokenKind kind) noexcept {
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
    default: return -1;
  }
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

bool Parser::containsName(
    const std::vector<std::string>& names,
    std::string_view name) noexcept {
  return std::find(
      names.begin(),
      names.end(),
      name) != names.end();
}

} // namespace sift::parse
