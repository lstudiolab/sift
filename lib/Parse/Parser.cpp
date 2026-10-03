#include "sift/Parse/Parser.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <thread>
#include <functional>
#include <iterator>
#include <type_traits>
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
    case TokenKind::KeywordClass:
    case TokenKind::KeywordEnum:
    case TokenKind::KeywordProtocol:
    case TokenKind::KeywordExtension:
    case TokenKind::KeywordPublic:
    case TokenKind::KeywordPrivate:
    case TokenKind::KeywordProtect:
    case TokenKind::KeywordStatic:
    case TokenKind::KeywordFinal:
    case TokenKind::KeywordOpen:
    case TokenKind::KeywordRequired:
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
    case TokenKind::KeywordGuard:
    case TokenKind::KeywordLoop:
    case TokenKind::KeywordDo:
    case TokenKind::KeywordThrow:
    case TokenKind::KeywordTry:
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

bool isContextualNameToken(TokenKind kind) noexcept {
  switch (kind) {
    case TokenKind::KeywordData:
    case TokenKind::KeywordMin:
    case TokenKind::KeywordMax:
    case TokenKind::KeywordOutput:
    case TokenKind::KeywordMessage:
    case TokenKind::KeywordMath:
    case TokenKind::KeywordAbs:
    case TokenKind::KeywordGetData:
    case TokenKind::KeywordCreateData:
    case TokenKind::KeywordControl:
    case TokenKind::KeywordConnect:
    case TokenKind::KeywordBackup:
    case TokenKind::KeywordBinary:
    case TokenKind::KeywordKernel:
    case TokenKind::KeywordOS:
    case TokenKind::KeywordDelete:
    case TokenKind::KeywordDestroy:
    case TokenKind::KeywordError:
    case TokenKind::KeywordPanic:
    case TokenKind::KeywordFile:
    case TokenKind::KeywordFileID:
    case TokenKind::KeywordAPI:
    case TokenKind::KeywordRepo:
    case TokenKind::KeywordWebLink:
    case TokenKind::KeywordDatabase:
    case TokenKind::KeywordSection:
    case TokenKind::KeywordImport:
      return true;
    default:
      return false;
  }
}

bool isNameToken(TokenKind kind) noexcept {
  return kind == TokenKind::Identifier ||
         kind == TokenKind::CallingName ||
         isContextualNameToken(kind);
}

bool isAccessModifier(TokenKind kind) noexcept {
  switch (kind) {
    case TokenKind::KeywordPublic:
    case TokenKind::KeywordPrivate:
    case TokenKind::KeywordProtect:
    case TokenKind::KeywordStatic:
    case TokenKind::KeywordFinal:
    case TokenKind::KeywordOpen:
    case TokenKind::KeywordRequired:
      return true;
    default:
      return false;
  }
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

Parser::Parser(std::string_view source) : Parser(source, false) {}

Parser::Parser(std::string_view source, bool pieceMode)
    : lexer_(source, lexer::LexerOptions{false}),
      source_(source),
      pieceMode_(pieceMode),
      lineStarts_(std::make_shared<std::vector<std::size_t>>()) {
  lineStarts_->reserve(64);
  lineStarts_->push_back(0);
  for (std::size_t i = 0; i < source_.size(); ++i) {
    if (source_[i] == '\n') {
      lineStarts_->push_back(i + 1);
    }
  }

  diagnostics_.reserve(32);
  callingNames_.reserve(32);
  functionNames_.reserve(32);
  structNames_.reserve(16);
  advance();
}

Parser::Parser(
    std::string_view source,
    std::shared_ptr<const std::vector<Token>> tokens,
    std::shared_ptr<std::vector<std::size_t>> lineStarts,
    std::size_t tokenBegin,
    std::size_t tokenEnd)
    : lexer_(source, lexer::LexerOptions{false}),
      source_(source),
      pieceMode_(true),
      tokenMode_(true),
      tokenBuffer_(std::move(tokens)),
      tokenCursor_(tokenBegin),
      tokenEnd_(tokenEnd),
      lineStarts_(std::move(lineStarts)) {
  diagnostics_.reserve(32);
  callingNames_.reserve(32);
  functionNames_.reserve(32);
  structNames_.reserve(16);
  if (tokenBuffer_ && tokenCursor_ < tokenEnd_) {
    current_ = (*tokenBuffer_)[tokenCursor_++];
  } else if (tokenBuffer_ && !tokenBuffer_->empty()) {
    current_ = tokenBuffer_->back();
  }
}

const std::vector<Diagnostic>& Parser::diagnostics() const noexcept {
  return diagnostics_;
}

bool Parser::hasErrors() const noexcept {
  return hasParserErrors_ || lexer_.hasErrors();
}

SourceLocation Parser::locationAt(std::size_t offset) const noexcept {
  const std::size_t targetOffset =
      std::min(offset, source_.size());

  if (!lineStarts_ || lineStarts_->empty()) {
    return {targetOffset, 1, targetOffset + 1};
  }

  const auto iterator =
      std::upper_bound(
          lineStarts_->begin(),
          lineStarts_->end(),
          targetOffset);

  const std::size_t lineIndex =
      static_cast<std::size_t>(
          iterator - lineStarts_->begin() - 1);

  const std::size_t lineStart =
      (*lineStarts_)[lineIndex];

  return {
      targetOffset,
      lineIndex + 1,
      targetOffset - lineStart + 1
  };
}

void Parser::advance() {
  previous_ = current_;
  if (tokenMode_) {
    if (tokenBuffer_ && tokenCursor_ < tokenEnd_) {
      current_ = (*tokenBuffer_)[tokenCursor_++];
    } else if (tokenBuffer_ && !tokenBuffer_->empty()) {
      current_ = tokenBuffer_->back();
    }
    return;
  }
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

void Parser::addDiagnostic(SourceLocation location, std::string_view message) {
  hasParserErrors_ = true;
  if (diagnostics_.size() >= maxDiagnostics_) {
    if (!diagnosticsTruncated_) {
      diagnosticsTruncated_ = true;
      diagnostics_.push_back({DiagnosticSeverity::Error, location,
                               "too many parser errors; further diagnostics suppressed"});
    }
    return;
  }
  diagnostics_.push_back({DiagnosticSeverity::Error, location, std::string(message)});
}

void Parser::error(const Token& token, std::string_view message) {
  if (diagnostics_.size() >= maxDiagnostics_) {
    hasParserErrors_ = true;
    if (!diagnosticsTruncated_) {
      diagnosticsTruncated_ = true;
      diagnostics_.push_back({
          DiagnosticSeverity::Error,
          locationAt(token.start),
          "too many parser errors; further diagnostics suppressed"
      });
    }
    return;
  }

  addDiagnostic(locationAt(token.start), message);
}

void Parser::synchronize() {
  while (!check(TokenKind::EndOfFile)) {
    if (previous_.kind == TokenKind::Semicolon ||
        previous_.kind == TokenKind::RightBrace ||
        current_.kind == TokenKind::RightBrace ||
        current_.kind == TokenKind::EndOfFile) {
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


std::vector<Parser::PieceBoundary> Parser::findPieceBoundaries(
    const std::vector<Token>& tokens, std::size_t sourceSize) {
  std::vector<PieceBoundary> pieces;
  pieces.reserve(16);
  std::size_t pieceStart = 0;
  std::size_t pieceStartToken = 0;
  TokenKind pieceStartKind = TokenKind::Unknown;
  std::size_t braceDepth = 0, parenDepth = 0, bracketDepth = 0;

  const auto startsTopLevelPiece = [](TokenKind kind) noexcept {
    switch (kind) {
      case TokenKind::KeywordImport:
      case TokenKind::KeywordStruct:
      case TokenKind::KeywordFunction:
      case TokenKind::KeywordVar:
      case TokenKind::KeywordConst:
      case TokenKind::KeywordIf:
      case TokenKind::KeywordWhile:
      case TokenKind::KeywordRepeat:
      case TokenKind::KeywordFor:
      case TokenKind::KeywordSwitch:
      case TokenKind::KeywordClass:
      case TokenKind::KeywordEnum:
      case TokenKind::KeywordProtocol:
      case TokenKind::KeywordExtension:
      case TokenKind::KeywordEnum:
      case TokenKind::KeywordProtocol:
      case TokenKind::KeywordClass:
      case TokenKind::KeywordPublic:
      case TokenKind::KeywordPrivate:
      case TokenKind::KeywordProtect:
      case TokenKind::KeywordStatic:
      case TokenKind::KeywordFinal:
      case TokenKind::KeywordOpen:
      case TokenKind::KeywordRequired:
      case TokenKind::KeywordGuard:
      case TokenKind::KeywordLoop:
      case TokenKind::KeywordDo:
      case TokenKind::KeywordTry:
        return true;
      default:
        return false;
    }
  };

  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const Token& token = tokens[i];
    if (token.kind == TokenKind::EndOfFile) break;
    const TokenKind previousKind = i == 0 ? TokenKind::Unknown : tokens[i - 1].kind;
    const bool topLevel = braceDepth == 0 && parenDepth == 0 && bracketDepth == 0;

    if (topLevel && i > pieceStartToken && startsTopLevelPiece(token.kind) &&
        !isAccessModifier(previousKind) &&
        !(token.kind == TokenKind::KeywordIf && previousKind == TokenKind::KeywordElse) &&
        !(token.kind == TokenKind::KeywordWhile && pieceStartKind == TokenKind::KeywordRepeat)) {
      pieces.push_back({pieceStart, token.start, pieceStartToken, i});
      pieceStart = token.start;
      pieceStartToken = i;
      pieceStartKind = token.kind;
    }

    switch (token.kind) {
      case TokenKind::LeftBrace:
        ++braceDepth;
        break;
      case TokenKind::RightBrace:
        if (braceDepth != 0) --braceDepth;
        if (braceDepth == 0 && parenDepth == 0 && bracketDepth == 0) {
          const TokenKind nextKind = i + 1 < tokens.size() ? tokens[i + 1].kind : TokenKind::EndOfFile;
          if (nextKind == TokenKind::KeywordElse ||
              (pieceStartKind == TokenKind::KeywordRepeat && nextKind == TokenKind::KeywordWhile)) {
            break;
          }
          const std::size_t end = token.endOffset();
          if (end > pieceStart) {
            pieces.push_back({pieceStart, end, pieceStartToken, i + 1});
            pieceStart = end;
            pieceStartToken = i + 1;
            pieceStartKind = TokenKind::Unknown;
          }
        }
        break;
      case TokenKind::LeftParen: ++parenDepth; break;
      case TokenKind::RightParen: if (parenDepth != 0) --parenDepth; break;
      case TokenKind::LeftBracket: ++bracketDepth; break;
      case TokenKind::RightBracket: if (bracketDepth != 0) --bracketDepth; break;
      case TokenKind::Semicolon:
        if (braceDepth == 0 && parenDepth == 0 && bracketDepth == 0) {
          const std::size_t end = token.endOffset();
          if (end > pieceStart) {
            pieces.push_back({pieceStart, end, pieceStartToken, i + 1});
            pieceStart = end;
            pieceStartToken = i + 1;
            pieceStartKind = TokenKind::Unknown;
          }
        }
        break;
      default:
        if (pieceStartKind == TokenKind::Unknown && topLevel && startsTopLevelPiece(token.kind))
          pieceStartKind = token.kind;
        break;
    }
  }

  const std::size_t eofIndex = tokens.empty() ? 0 : tokens.size() - 1;
  if (pieceStartToken < eofIndex && pieceStart < sourceSize) {
    pieces.push_back({pieceStart, sourceSize, pieceStartToken, eofIndex});
  }
  return pieces;
}

Parser::PieceResult Parser::parsePiece(std::size_t sequence, PieceBoundary boundary) const {
  PieceResult result;
  result.sequence = sequence;
  result.boundary = boundary;
  Parser pieceParser(
      source_,
      tokenBuffer_,
      lineStarts_,
      boundary.tokenBegin,
      boundary.tokenEnd);
  result.program = pieceParser.parse();
  result.diagnostics = pieceParser.diagnostics();
  result.hasErrors = pieceParser.hasErrors();
  return result;
}

std::unique_ptr<Program> Parser::parse() {
  if (pieceMode_ && tokenMode_) return parseSequentialProgram();

  auto program = std::make_unique<Program>();
  program->imports.reserve(8);
  program->structs.reserve(8);
  program->classes.reserve(4);
  program->enums.reserve(4);
  program->protocols.reserve(4);
  program->extensions.reserve(4);
  program->functions.reserve(16);
  program->statements.reserve(32);
  program->location = SourceLocation{0u, 0u, 0u};

  lexer_.reset();
  auto tokens = std::make_shared<std::vector<Token>>();
  tokens->reserve(source_.size() / 6u + 16u);
  while (true) {
    Token token = lexer_.lex();
    tokens->push_back(token);
    if (token.kind == TokenKind::EndOfFile) break;
  }

  for (const auto& diagnostic : lexer_.diagnostics()) {
    if (diagnostic.severity == lexer::DiagnosticSeverity::Error) {
      hasParserErrors_ = true;
    }
    diagnostics_.push_back({
        static_cast<DiagnosticSeverity>(diagnostic.severity),
        diagnostic.location,
        std::string(diagnostic.message)
    });
  }

  tokenBuffer_ = tokens;
  tokenMode_ = true;
  tokenCursor_ = 0;
  tokenEnd_ = tokens->empty() ? 0 : tokens->size() - 1;
  if (tokens->empty()) return program;

  const auto boundaries = findPieceBoundaries(*tokens, source_.size());
  if (source_.size() < 64u * 1024u || boundaries.size() < 2) {
    current_ = (*tokens)[0];
    tokenCursor_ = 1;
    return parseSequentialProgram();
  }

  std::vector<PieceResult> results(boundaries.size());
  std::atomic<std::size_t> nextSequence{0};
  unsigned int workerCount = std::thread::hardware_concurrency();
  if (workerCount == 0) workerCount = 1;
  workerCount = std::min(workerCount, static_cast<unsigned int>(boundaries.size()));

  std::vector<std::thread> workers;
  workers.reserve(workerCount);
  for (unsigned int worker = 0; worker < workerCount; ++worker) {
    workers.emplace_back([this, &boundaries, &results, &nextSequence] {
      while (true) {
        const std::size_t sequence =
            nextSequence.fetch_add(1, std::memory_order_relaxed);
        if (sequence >= boundaries.size()) return;

        try {
          results[sequence] = parsePiece(sequence, boundaries[sequence]);
        } catch (const std::exception&) {
          PieceResult failed;
          failed.sequence = sequence;
          failed.boundary = boundaries[sequence];
          failed.hasErrors = true;
          failed.diagnostics.push_back({
              DiagnosticSeverity::Error,
              locationAt(boundaries[sequence].start),
              "parser worker failed while parsing this piece"
          });
          results[sequence] = std::move(failed);
        } catch (...) {
          PieceResult failed;
          failed.sequence = sequence;
          failed.boundary = boundaries[sequence];
          failed.hasErrors = true;
          failed.diagnostics.push_back({
              DiagnosticSeverity::Error,
              locationAt(boundaries[sequence].start),
              "parser worker failed while parsing this piece"
          });
          results[sequence] = std::move(failed);
        }
      }
    });
  }
  for (auto& worker : workers) worker.join();

  for (PieceResult& result : results) {
    if (!result.program) continue;
    for (auto& import : result.program->imports) program->imports.push_back(std::move(import));
    for (auto& structure : result.program->structs) program->structs.push_back(std::move(structure));
    for (auto& classNode : result.program->classes) program->classes.push_back(std::move(classNode));
    for (auto& enumNode : result.program->enums) program->enums.push_back(std::move(enumNode));
    for (auto& protocolNode : result.program->protocols) program->protocols.push_back(std::move(protocolNode));
    for (auto& extensionNode : result.program->extensions) program->extensions.push_back(std::move(extensionNode));
    for (auto& function : result.program->functions) program->functions.push_back(std::move(function));
    for (auto& statement : result.program->statements) program->statements.push_back(std::move(statement));
    diagnostics_.insert(diagnostics_.end(),
                        std::make_move_iterator(result.diagnostics.begin()),
                        std::make_move_iterator(result.diagnostics.end()));
    hasParserErrors_ = hasParserErrors_ || result.hasErrors;
  }

  structNames_.clear();
  functionNames_.clear();
  callingNames_.clear();

  for (const auto& structure : program->structs) {
    if (!structure->name.empty() &&
        !structNames_.insert(structure->name).second) {
      addDiagnostic(locationAt(structure->location.offset),
                    "duplicate struct name");
    }

    for (const auto& function : structure->functions) {
      if (!function->name.empty() &&
          !functionNames_.insert(function->name).second) {
        addDiagnostic(locationAt(function->location.offset),
                      "duplicate function name");
      }

      if (!function->callingName.empty() &&
          !callingNames_.insert(function->callingName).second) {
        addDiagnostic(locationAt(function->location.offset),
                      "duplicate function calling name");
      }
    }
  }

  for (const auto& classNode : program->classes) {
    if (!classNode->name.empty() &&
        !structNames_.insert(classNode->name).second) {
      addDiagnostic(locationAt(classNode->location.offset),
                    "duplicate type name");
    }
    for (const auto& function : classNode->functions) {
      if (!function->name.empty() &&
          !functionNames_.insert(function->name).second) {
        addDiagnostic(locationAt(function->location.offset),
                      "duplicate function name");
      }
      if (!function->callingName.empty() &&
          !callingNames_.insert(function->callingName).second) {
        addDiagnostic(locationAt(function->location.offset),
                      "duplicate function calling name");
      }
    }
  }

  for (const auto& enumNode : program->enums) {
    if (!enumNode->name.empty() &&
        !structNames_.insert(enumNode->name).second) {
      addDiagnostic(locationAt(enumNode->location.offset),
                    "duplicate type name");
    }
  }

  for (const auto& protocolNode : program->protocols) {
    if (!protocolNode->name.empty() &&
        !structNames_.insert(protocolNode->name).second) {
      addDiagnostic(locationAt(protocolNode->location.offset),
                    "duplicate type name");
    }
    for (const auto& function : protocolNode->functions) {
      if (!function->name.empty() &&
          !functionNames_.insert(function->name).second) {
        addDiagnostic(locationAt(function->location.offset),
                      "duplicate function name");
      }
    }
  }

  for (const auto& extensionNode : program->extensions) {
    for (const auto& function : extensionNode->functions) {
      if (!function->name.empty() &&
          !functionNames_.insert(function->name).second) {
        addDiagnostic(locationAt(function->location.offset),
                      "duplicate function name");
      }
      if (!function->callingName.empty() &&
          !callingNames_.insert(function->callingName).second) {
        addDiagnostic(locationAt(function->location.offset),
                      "duplicate function calling name");
      }
    }
  }

  for (const auto& function : program->functions) {
    if (!function->name.empty() &&
        !functionNames_.insert(function->name).second) {
      addDiagnostic(locationAt(function->location.offset),
                    "duplicate function name");
    }

    if (!function->callingName.empty() &&
        !callingNames_.insert(function->callingName).second) {
      addDiagnostic(locationAt(function->location.offset),
                    "duplicate function calling name");
    }
  }

  std::stable_sort(diagnostics_.begin(), diagnostics_.end(),
                   [](const Diagnostic& left, const Diagnostic& right) {
                     return left.location.offset < right.location.offset;
                   });
  return program;
}

std::unique_ptr<Program> Parser::parseSequentialProgram() {
  auto program = std::make_unique<Program>();
  program->imports.reserve(8);
  program->structs.reserve(8);
  program->classes.reserve(4);
  program->enums.reserve(4);
  program->protocols.reserve(4);
  program->extensions.reserve(4);
  program->functions.reserve(16);
  program->statements.reserve(32);
  program->location = SourceLocation{source_.empty() ? 0u : 0u, 0u, 0u};

  while (!check(TokenKind::EndOfFile)) {
    if (match(TokenKind::Semicolon) || match(TokenKind::Comment)) {
      continue;
    }

    const Token before = current_;

    switch (current_.kind) {
      case TokenKind::KeywordImport: {
        if (auto node = parseImport()) {
          program->imports.push_back(std::move(node));
        }
        break;
      }
      case TokenKind::KeywordStruct: {
        if (auto node = parseStruct()) {
          program->structs.push_back(std::move(node));
        }
        break;
      }
      case TokenKind::KeywordClass: {
        if (auto node = parseClass()) {
          program->classes.push_back(std::move(node));
        }
        break;
      }
      case TokenKind::KeywordEnum: {
        if (auto node = parseEnum()) {
          program->enums.push_back(std::move(node));
        }
        break;
      }
      case TokenKind::KeywordProtocol: {
        if (auto node = parseProtocol()) {
          program->protocols.push_back(std::move(node));
        }
        break;
      }
      case TokenKind::KeywordExtension: {
        if (auto node = parseExtension()) {
          program->extensions.push_back(std::move(node));
        }
        break;
      }
      case TokenKind::KeywordFunction: {
        if (auto node = parseFunction()) {
          program->functions.push_back(std::move(node));
        }
        break;
      }
      case TokenKind::KeywordAsync: {
        const Token asyncToken = current_;
        advance();
        if (!check(TokenKind::KeywordFunction)) {
          error(asyncToken, "expected 'function' after 'async'");
          break;
        }
        if (auto node = parseFunction()) {
          node->isAsync = true;
          program->functions.push_back(std::move(node));
        }
        break;
      }
      default: {
        if (isAccessModifier(current_.kind)) {
          const std::string access = parseAccessModifier();
          if (check(TokenKind::KeywordFunction)) {
            if (auto node = parseFunction()) {
              node->accessModifier = access;
              program->functions.push_back(std::move(node));
            }
          } else if (check(TokenKind::KeywordVar) || check(TokenKind::KeywordConst)) {
            if (auto node = parseVariable(check(TokenKind::KeywordConst))) {
              node->accessModifier = access;
              program->statements.push_back(std::make_unique<Statement>(
                  Statement{std::move(*node)}));
            }
          } else if (check(TokenKind::KeywordStruct)) {
            if (auto node = parseStruct()) {
              node->accessModifier = access;
              program->structs.push_back(std::move(node));
            }
          } else if (check(TokenKind::KeywordClass)) {
            if (auto node = parseClass()) {
              node->accessModifier = access;
              program->classes.push_back(std::move(node));
            }
          } else if (check(TokenKind::KeywordEnum)) {
            if (auto node = parseEnum()) {
              node->accessModifier = access;
              program->enums.push_back(std::move(node));
            }
          } else if (check(TokenKind::KeywordProtocol)) {
            if (auto node = parseProtocol()) {
              node->accessModifier = access;
              program->protocols.push_back(std::move(node));
            }
          } else if (check(TokenKind::KeywordExtension)) {
            if (auto node = parseExtension()) {
              node->accessModifier = access;
              program->extensions.push_back(std::move(node));
            }
          } else {
            error(current_, "access modifier must precede a declaration");
          }
        } else if (auto node = parseStatement()) {
          program->statements.push_back(std::move(node));
        }
        break;
      }
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
  node->location = SourceLocation{start.start, 0u, 0u};

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
  const std::string access = parseAccessModifier();
  const Token start = current_;
  advance();

  auto node = std::make_unique<StructDeclaration>();
  node->variables.reserve(8);
  node->functions.reserve(8);
  node->location = SourceLocation{start.start, 0u, 0u};
  node->accessModifier = access;
  node->name = parseIdentifier("struct name");

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
      // synchronize() intentionally stops at declaration keywords. Consume
      // this rejected declaration keyword so recovery can make progress.
      advance();
      synchronize();
      continue;
    }

    if (isAccessModifier(current_.kind)) {
      const std::string access = parseAccessModifier();

      if (check(TokenKind::KeywordVar) || check(TokenKind::KeywordConst)) {
        auto member = parseVariable(check(TokenKind::KeywordConst));
        member->accessModifier = access;
        node->variables.push_back(std::move(member));
        continue;
      }

      if (check(TokenKind::KeywordFunction)) {
        auto function = parseFunction();
        function->accessModifier = access;
        node->functions.push_back(std::move(function));
        continue;
      }

      error(current_, "access modifier must precede a struct member declaration");
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

std::string Parser::parseAccessModifier() {
  std::string value;

  while (isAccessModifier(current_.kind)) {
    if (!value.empty()) {
      value += " ";
    }
    value += tokenText(current_);
    advance();
  }

  return value;
}

std::unique_ptr<ClassDeclaration> Parser::parseClass() {
  const std::string access = {};
  const Token start = current_;
  if (!check(TokenKind::KeywordClass)) {
    error(current_, "expected 'class'");
    return nullptr;
  }
  advance();

  auto node = std::make_unique<ClassDeclaration>();
  node->location = SourceLocation{start.start, 0u, 0u};
  node->accessModifier = access;
  node->name = parseIdentifier("class name");
  if (!expect(TokenKind::LeftBrace, "expected '{' after class name")) {
    synchronizeToBlockStart();
    return node;
  }

  while (!check(TokenKind::RightBrace) && !check(TokenKind::EndOfFile)) {
    if (match(TokenKind::Semicolon) || match(TokenKind::Comment)) continue;
    if (isAccessModifier(current_.kind)) {
      const std::string memberAccess = parseAccessModifier();
      if (check(TokenKind::KeywordVar) || check(TokenKind::KeywordConst)) {
        auto member = parseVariable(check(TokenKind::KeywordConst));
        member->accessModifier = memberAccess;
        node->variables.push_back(std::move(member));
        continue;
      }
      if (check(TokenKind::KeywordFunction)) {
        auto function = parseFunction();
        function->accessModifier = memberAccess;
        node->functions.push_back(std::move(function));
        continue;
      }
      error(current_, "access modifier must precede a class member declaration");
      continue;
    }
    if (check(TokenKind::KeywordVar) || check(TokenKind::KeywordConst)) {
      node->variables.push_back(parseVariable(check(TokenKind::KeywordConst)));
      continue;
    }
    if (check(TokenKind::KeywordFunction)) {
      node->functions.push_back(parseFunction());
      continue;
    }
    error(current_, "class body only permits var, const, and function declarations");
    const Token before = current_;
    synchronize();
    if (current_.start == before.start && current_.kind == before.kind) advance();
  }

  expect(TokenKind::RightBrace, "expected '}' after class declaration");
  return node;
}

std::unique_ptr<EnumDeclaration> Parser::parseEnum() {
  const std::string access = {};
  const Token start = current_;
  if (!check(TokenKind::KeywordEnum)) {
    error(current_, "expected 'enum'");
    return nullptr;
  }
  advance();

  auto node = std::make_unique<EnumDeclaration>();
  node->location = SourceLocation{start.start, 0u, 0u};
  node->accessModifier = access;
  node->name = parseIdentifier("enum name");

  if (!expect(TokenKind::LeftBrace, "expected '{' after enum name")) {
    synchronizeToBlockStart();
    return node;
  }

  while (!check(TokenKind::RightBrace) && !check(TokenKind::EndOfFile)) {
    if (match(TokenKind::Comma) || match(TokenKind::Semicolon) || match(TokenKind::Comment)) continue;
    if (!match(TokenKind::KeywordCase)) {
      error(current_, "expected 'case' in enum");
      const Token before = current_;
      synchronize();
      if (current_.start == before.start && current_.kind == before.kind) advance();
      continue;
    }
    if (!isNameToken(current_.kind)) {
      error(current_, "expected enum case name");
      synchronize();
      continue;
    }
    node->cases.push_back(tokenText(current_));
    advance();
    if (match(TokenKind::Colon)) {
      parseTypeName();
    }
  }

  expect(TokenKind::RightBrace, "expected '}' after enum declaration");
  return node;
}

std::unique_ptr<ProtocolDeclaration> Parser::parseProtocol() {
  const std::string access = {};
  const Token start = current_;
  if (!check(TokenKind::KeywordProtocol)) {
    error(current_, "expected 'protocol'");
    return nullptr;
  }
  advance();

  auto node = std::make_unique<ProtocolDeclaration>();
  node->location = SourceLocation{start.start, 0u, 0u};
  node->accessModifier = access;
  node->name = parseIdentifier("protocol name");

  if (!expect(TokenKind::LeftBrace, "expected '{' after protocol name")) {
    synchronizeToBlockStart();
    return node;
  }

  while (!check(TokenKind::RightBrace) && !check(TokenKind::EndOfFile)) {
    if (match(TokenKind::Semicolon) || match(TokenKind::Comment)) continue;
    if (check(TokenKind::KeywordFunction)) {
      node->functions.push_back(parseFunction());
      continue;
    }
    error(current_, "protocol body only permits function declarations");
    const Token before = current_;
    synchronize();
    if (current_.start == before.start && current_.kind == before.kind) advance();
  }

  expect(TokenKind::RightBrace, "expected '}' after protocol declaration");
  return node;
}

std::unique_ptr<ExtensionDeclaration> Parser::parseExtension() {
  const std::string access = {};
  const Token start = current_;
  if (!check(TokenKind::KeywordExtension)) {
    error(current_, "expected 'extension'");
    return nullptr;
  }
  advance();

  auto node = std::make_unique<ExtensionDeclaration>();
  node->location = SourceLocation{start.start, 0u, 0u};
  node->accessModifier = access;
  node->target = parseIdentifier("extension target");

  if (!expect(TokenKind::LeftBrace, "expected '{' after extension target")) {
    synchronizeToBlockStart();
    return node;
  }

  while (!check(TokenKind::RightBrace) && !check(TokenKind::EndOfFile)) {
    if (match(TokenKind::Semicolon) || match(TokenKind::Comment)) continue;
    if (check(TokenKind::KeywordFunction)) {
      node->functions.push_back(parseFunction());
      continue;
    }
    error(current_, "extension body only permits function declarations");
    const Token before = current_;
    synchronize();
    if (current_.start == before.start && current_.kind == before.kind) advance();
  }

  expect(TokenKind::RightBrace, "expected '}' after extension declaration");
  return node;
}

std::unique_ptr<FunctionDeclaration> Parser::parseFunction() {
  const std::string access = parseAccessModifier();
  const Token start = current_;
  advance();

  DepthGuard functionScope(functionDepth_);

  auto node = std::make_unique<FunctionDeclaration>();
  node->parameters.reserve(4);
  node->location = SourceLocation{start.start, 0u, 0u};
  node->accessModifier = access;
  node->name = parseIdentifier("function name");

  if (!expect(TokenKind::LeftParen, "expected '(' after function name")) {
    synchronizeToBlockStart();
    if (!check(TokenKind::LeftBrace)) {
      return node;
    }
  }

  if (check(TokenKind::RightParen)) {
    error(current_, "Sift functions require exactly one calling name");
  } else {
    node->callingName = parseCallingName();

    while (match(TokenKind::Comma)) {
      if (check(TokenKind::RightParen) || check(TokenKind::EndOfFile)) {
        error(current_, "expected parameter after ','");
        break;
      }

      auto parameter = parseParameter();
      if (!parameter) {
        synchronizeExpression();
        break;
      }

      node->parameters.push_back(std::move(parameter));
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

  if (match(TokenKind::KeywordThrows)) {
    node->isThrows = true;
  }

  node->body = parseBlock();

  return node;
}

std::unique_ptr<Statement> Parser::parseStatement() {
  auto node = std::make_unique<Statement>();

  if (isAccessModifier(current_.kind)) {
    const std::string access = parseAccessModifier();
    switch (current_.kind) {
      case TokenKind::KeywordVar:
      case TokenKind::KeywordConst: {
        if (auto parsed = parseVariable(check(TokenKind::KeywordConst))) {
          parsed->accessModifier = access;
          node->value = std::move(*parsed);
        }
        return node;
      }
      case TokenKind::KeywordFunction: {
        if (auto parsed = parseFunction()) {
          parsed->accessModifier = access;
          node->value = std::move(*parsed);
        }
        return node;
      }
      case TokenKind::KeywordClass: {
        if (auto parsed = parseClass()) {
          parsed->accessModifier = access;
          node->value = std::move(*parsed);
        }
        return node;
      }
      case TokenKind::KeywordEnum: {
        if (auto parsed = parseEnum()) {
          parsed->accessModifier = access;
          node->value = std::move(*parsed);
        }
        return node;
      }
      case TokenKind::KeywordProtocol: {
        if (auto parsed = parseProtocol()) {
          parsed->accessModifier = access;
          node->value = std::move(*parsed);
        }
        return node;
      }
      case TokenKind::KeywordExtension: {
        if (auto parsed = parseExtension()) {
          parsed->accessModifier = access;
          node->value = std::move(*parsed);
        }
        return node;
      }
      default:
        error(current_, "access modifier must precede a declaration");
        return node;
    }
  }

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
    case TokenKind::KeywordAsync: {
      const Token asyncToken = current_;
      advance();
      if (!check(TokenKind::KeywordFunction)) {
        error(asyncToken, "expected 'function' after 'async'");
      } else if (auto parsed = parseFunction()) {
        parsed->isAsync = true;
        node->value = std::move(*parsed);
      }
      break;
    }
    case TokenKind::KeywordStruct:
      if (auto parsed = parseStruct()) {
        node->value = std::move(*parsed);
      }
      break;
    case TokenKind::KeywordClass:
      if (auto parsed = parseClass()) {
        node->value = std::move(*parsed);
      }
      break;
    case TokenKind::KeywordEnum:
      if (auto parsed = parseEnum()) {
        node->value = std::move(*parsed);
      }
      break;
    case TokenKind::KeywordProtocol:
      if (auto parsed = parseProtocol()) {
        node->value = std::move(*parsed);
      }
      break;
    case TokenKind::KeywordExtension:
      if (auto parsed = parseExtension()) {
        node->value = std::move(*parsed);
      }
      break;
    case TokenKind::KeywordGuard:
      if (auto parsed = parseGuard()) {
        node->value = std::move(*parsed);
      }
      break;
    case TokenKind::KeywordLoop:
      if (auto parsed = parseLoop()) {
        node->value = std::move(*parsed);
      }
      break;
    case TokenKind::KeywordDo:
      if (auto parsed = parseDo()) {
        node->value = std::move(*parsed);
      }
      break;
    case TokenKind::KeywordThrow:
    case TokenKind::KeywordRethrow:
      if (auto parsed = parseThrow()) {
        node->value = std::move(*parsed);
      }
      break;
    case TokenKind::KeywordTry:
      if (auto parsed = parseTry()) {
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
  node->location = SourceLocation{start.start, 0u, 0u};
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

std::unique_ptr<Parameter> Parser::parseParameter() {
  const Token start = current_;
  auto node = std::make_unique<Parameter>();
  node->location = SourceLocation{start.start, 0u, 0u};
  node->name = parseIdentifier("parameter name");

  if (match(TokenKind::Colon)) {
    node->type = parseTypeName();
  }

  if (node->name.empty()) {
    synchronizeExpression();
    return nullptr;
  }

  return node;
}

std::unique_ptr<IfStatement> Parser::parseIf() {
  if (statementDepth_ >= maxStatementDepth_) {
    error(current_, "statement nesting exceeds parser limit");
    synchronizeToBlockStart();
    return nullptr;
  }

  DepthGuard statementScope(statementDepth_);

  const Token start = current_;
  advance();

  auto node = std::make_unique<IfStatement>();
  node->location = SourceLocation{start.start, 0u, 0u};
  node->condition = parseExpression();
  node->thenBlock = parseBlock();

  if (match(TokenKind::KeywordElse)) {
    if (check(TokenKind::KeywordIf)) {
      node->elseBlock = std::make_unique<Block>();
      node->elseBlock->location = SourceLocation{current_.start, 0u, 0u};

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
  node->location = SourceLocation{start.start, 0u, 0u};
  node->condition = parseExpression();

  DepthGuard loopScope(loopDepth_);
  node->body = parseBlock();

  return node;
}

std::unique_ptr<RepeatStatement> Parser::parseRepeat() {
  const Token start = current_;
  advance();

  auto node = std::make_unique<RepeatStatement>();
  node->location = SourceLocation{start.start, 0u, 0u};

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
  node->location = SourceLocation{start.start, 0u, 0u};
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
  node->location = SourceLocation{start.start, 0u, 0u};

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
  node->location = SourceLocation{start.start, 0u, 0u};
  node->body = parseBlock();
  return node;
}

std::unique_ptr<BreakStatement> Parser::parseBreak() {
  const Token start = current_;
  advance();

  auto node = std::make_unique<BreakStatement>();
  node->location = SourceLocation{start.start, 0u, 0u};

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
  node->location = SourceLocation{start.start, 0u, 0u};

  if (!canContinue()) {
    error(start, "'continue' is only valid inside a loop");
  }
  match(TokenKind::Semicolon);
  return node;
}

std::unique_ptr<GuardStatement> Parser::parseGuard() {
  const Token start = current_;
  advance();

  auto node = std::make_unique<GuardStatement>();
  node->location = SourceLocation{start.start, 0u, 0u};
  node->condition = parseExpression();
  node->body = parseBlock();
  return node;
}

std::unique_ptr<LoopStatement> Parser::parseLoop() {
  const Token start = current_;
  advance();

  auto node = std::make_unique<LoopStatement>();
  node->location = SourceLocation{start.start, 0u, 0u};

  DepthGuard loopScope(loopDepth_);
  node->body = parseBlock();
  return node;
}

std::unique_ptr<ThrowStatement> Parser::parseThrow() {
  const Token start = current_;
  advance();

  auto node = std::make_unique<ThrowStatement>();
  node->location = SourceLocation{start.start, 0u, 0u};
  if (start.kind != TokenKind::KeywordRethrow) {
    node->value = parseExpression();
  }
  match(TokenKind::Semicolon);
  return node;
}

std::unique_ptr<TryStatement> Parser::parseTry() {
  const Token start = current_;
  advance();

  auto node = std::make_unique<TryStatement>();
  node->location = SourceLocation{start.start, 0u, 0u};

  if (check(TokenKind::LeftBrace)) {
    node->body = parseBlock();
  } else {
    node->expression = parseExpression();
  }

  while (match(TokenKind::KeywordCatch)) {
    auto clause = std::make_unique<CatchClause>();
    clause->location = SourceLocation{previous_.start, 0u, 0u};

    if (!check(TokenKind::LeftBrace)) {
      clause->condition = parseExpression();
    }

    clause->body = parseBlock();
    node->catches.push_back(std::move(clause));
  }

  if (node->catches.empty() && !node->expression) {
    error(current_, "try statement requires a block or expression");
  }

  return node;
}

std::unique_ptr<DoStatement> Parser::parseDo() {
  const Token start = current_;
  advance();

  auto node = std::make_unique<DoStatement>();
  node->location = SourceLocation{start.start, 0u, 0u};
  node->body = parseBlock();

  while (match(TokenKind::KeywordCatch)) {
    auto clause = std::make_unique<CatchClause>();
    clause->location = SourceLocation{previous_.start, 0u, 0u};
    if (!check(TokenKind::LeftBrace)) {
      clause->condition = parseExpression();
    }
    clause->body = parseBlock();
    node->catches.push_back(std::move(clause));
  }

  return node;
}

std::unique_ptr<SwitchStatement> Parser::parseSwitch() {
  const Token start = current_;
  advance();

  auto node = std::make_unique<SwitchStatement>();
  node->location = SourceLocation{start.start, 0u, 0u};
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
    caseNode->location = SourceLocation{caseStart.start, 0u, 0u};
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
  node->location = SourceLocation{current_.start, 0u, 0u};
  node->expression = parseExpression();

  if (!node->expression) {
    synchronizeExpression();
  }

  match(TokenKind::Semicolon);
  return node;
}

std::unique_ptr<Block> Parser::parseBlock() {
  if (statementDepth_ >= maxStatementDepth_) {
    error(current_, "statement nesting exceeds parser limit");
    synchronizeToBlockStart();
    return nullptr;
  }

  DepthGuard statementScope(statementDepth_);

  auto node = std::make_unique<Block>();
  node->statements.reserve(8);
  node->location = SourceLocation{current_.start, 0u, 0u};

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
  node->location = SourceLocation{operatorToken.start, 0u, 0u};

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

  if (minimumPrecedence != 1) {
    return left;
  }

  // The overwhelmingly common expression is a single primary/postfix.
  // Do not allocate temporary operator/value storage unless an operator
  // is actually present.
  if (binaryPrecedence(current_.kind) == 0) {
    return left;
  }

  std::vector<TokenKind> operators;
  std::vector<Token> operatorTokens;
  std::vector<std::unique_ptr<Expression>> values;

  operators.reserve(8);
  operatorTokens.reserve(8);
  values.reserve(8);
  values.push_back(std::move(left));

  auto reduce = [&]() {
    const TokenKind operatorKind = operators.back();
    const Token operatorToken = operatorTokens.back();
    operators.pop_back();
    operatorTokens.pop_back();

    auto right = std::move(values.back());
    values.pop_back();

    auto leftValue = std::move(values.back());
    values.pop_back();

    auto node = std::make_unique<Expression>();
    node->location = SourceLocation{operatorToken.start, 0u, 0u};

    BinaryExpression binary;
    binary.op = operatorText(operatorKind);
    binary.left = std::move(leftValue);
    binary.right = std::move(right);
    node->value = std::move(binary);
    values.push_back(std::move(node));
  };

  while (true) {
    const int precedence = binaryPrecedence(current_.kind);
    if (precedence == 0) {
      break;
    }

    while (!operators.empty() &&
           binaryPrecedence(operators.back()) >= precedence) {
      reduce();
    }

    const Token operatorToken = current_;
    const TokenKind operatorKind = current_.kind;
    advance();

    auto right = parseUnary();
    if (!right) {
      error(current_, "expected expression after binary operator");
      break;
    }

    operators.push_back(operatorKind);
    operatorTokens.push_back(operatorToken);
    values.push_back(std::move(right));
  }

  while (!operators.empty()) {
    reduce();
  }

  return std::move(values.back());
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
    node->location = SourceLocation{operatorToken.start, 0u, 0u};

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
    if (check(TokenKind::Dot)) {
      const Token firstDot = current_;
      advance();

      if (check(TokenKind::Dot)) {
        advance();
        bool inclusive = true;
        if (check(TokenKind::Dot)) {
          advance();
          inclusive = true;
        } else if (match(TokenKind::Less)) {
          inclusive = false;
        }

        auto range = std::make_unique<Expression>();
        range->location = SourceLocation{firstDot.start, 0u, 0u};

        RangeExpression rangeValue;
        rangeValue.start = std::move(expression);
        rangeValue.inclusive = inclusive;
        rangeValue.end = parseUnary();

        if (!rangeValue.end) {
          error(current_, "expected range end expression");
          synchronizeExpression();
          return range;
        }

        range->value = std::move(rangeValue);
        expression = std::move(range);
        continue;
      }

      const Token memberToken = current_;
      if (!isNameToken(current_.kind)) {
        error(current_, "expected a name after '.'");
        synchronizeExpression();
        return expression;
      }

      advance();

      auto node = std::make_unique<Expression>();
      node->location = SourceLocation{memberToken.start, 0u, 0u};

      MemberExpression member;
      member.base = std::move(expression);
      member.member = tokenText(memberToken);
      node->value = std::move(member);
      expression = std::move(node);
      continue;
    }

    if (match(TokenKind::LeftBracket)) {
      auto node = std::make_unique<Expression>();
      node->location = SourceLocation{previous_.start, 0u, 0u};

      IndexExpression index;
      index.base = std::move(expression);
      index.index = parseExpression();

      expect(TokenKind::RightBracket, "expected ']' after index expression");
      node->value = std::move(index);
      expression = std::move(node);
      continue;
    }

    if (match(TokenKind::LeftParen)) {
      auto node = std::make_unique<Expression>();
      node->location = SourceLocation{previous_.start, 0u, 0u};

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

  if (isNameToken(token.kind) || token.kind == TokenKind::KeywordSelf) {
    advance();

    auto node = std::make_unique<Expression>();
    node->location = SourceLocation{token.start, 0u, 0u};

    IdentifierExpression identifier;
    identifier.name = tokenText(token);
    node->value = std::move(identifier);
    return node;
  }

  if (token.kind == TokenKind::KeywordAsync ||
      token.kind == TokenKind::KeywordAwait ||
      token.kind == TokenKind::KeywordWait ||
      token.kind == TokenKind::KeywordTry ||
      token.kind == TokenKind::KeywordCall) {
    advance();

    auto operand = parseUnary();
    if (!operand) {
      error(current_, "expected expression after prefix keyword");
      return nullptr;
    }

    auto node = std::make_unique<Expression>();
    node->location = SourceLocation{token.start, 0u, 0u};

    UnaryExpression unary;
    unary.op = tokenText(token);
    unary.operand = std::move(operand);
    node->value = std::move(unary);
    return node;
  }

  if (token.isLiteral()) {
    advance();

    auto node = std::make_unique<Expression>();
    node->location = SourceLocation{token.start, 0u, 0u};

    LiteralExpression literal;
    literal.literalKind = token.kind;
    literal.value = tokenText(token);
    node->value = std::move(literal);
    return node;
  }

  if (match(TokenKind::LeftBracket)) {
    auto node = std::make_unique<Expression>();
    node->location = SourceLocation{token.start, 0u, 0u};

    if (match(TokenKind::RightBracket)) {
      ArrayLiteralExpression array;
      node->value = std::move(array);
      return node;
    }

    auto first = parseExpression();
    if (!first) {
      synchronizeExpression();
      expect(TokenKind::RightBracket, "expected ']' after collection literal");
      return nullptr;
    }

    if (match(TokenKind::Colon)) {
      DictionaryLiteralExpression dictionary;
      dictionary.entries.reserve(4);

      auto value = parseExpression();
      if (!value) {
        error(current_, "expected dictionary value");
      } else {
        dictionary.entries.emplace_back(std::move(first), std::move(value));
      }

      while (match(TokenKind::Comma)) {
        if (check(TokenKind::RightBracket)) break;
        auto key = parseExpression();
        expect(TokenKind::Colon, "expected ':' between dictionary key and value");
        auto mapped = parseExpression();
        if (key && mapped) {
          dictionary.entries.emplace_back(std::move(key), std::move(mapped));
        }
      }

      expect(TokenKind::RightBracket, "expected ']' after dictionary literal");
      node->value = std::move(dictionary);
      return node;
    }

    ArrayLiteralExpression array;
    array.elements.reserve(4);
    array.elements.push_back(std::move(first));

    while (match(TokenKind::Comma)) {
      if (check(TokenKind::RightBracket)) break;
      auto element = parseExpression();
      if (!element) break;
      array.elements.push_back(std::move(element));
    }

    expect(TokenKind::RightBracket, "expected ']' after array literal");
    node->value = std::move(array);
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
  if (!isTypeToken(current_.kind) && !isNameToken(current_.kind)) {
    error(current_, "expected a type name");
    return {};
  }

  std::string type = tokenText(current_);
  advance();

  while (match(TokenKind::Dot)) {
    if (!isNameToken(current_.kind)) {
      error(current_, "expected type name after '.'");
      break;
    }

    type += ".";
    type += tokenText(current_);
    advance();
  }

  if (match(TokenKind::Question)) {
    type += "?";
  }

  return type;
}

std::string Parser::parseIdentifier(std::string_view context) {
  if (!isNameToken(current_.kind)) {
    error(current_, std::string("expected ") + std::string(context));
    return {};
  }

  const std::string value = tokenText(current_);
  advance();
  return value;
}

std::string Parser::parseCallingName() {
  if (!isNameToken(current_.kind)) {
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