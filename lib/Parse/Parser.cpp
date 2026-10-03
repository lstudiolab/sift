#include "sift/Parse/Parser.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <functional>
#include <iterator>
#include <type_traits>
#include <utility>

namespace sift::parse {

namespace {

class ASTArena final {
public:
  static constexpr std::size_t blockSize = 64u * 1024u;

  void* allocate(std::size_t size) {
    const std::size_t alignment = alignof(std::max_align_t);
    const std::size_t alignedSize =
        (size + alignment - 1u) & ~(alignment - 1u);

    if (blocks_.empty() ||
        offset_ + alignedSize > blockSize) {
      const std::size_t allocationSize =
          std::max(blockSize, alignedSize);
      blocks_.push_back(
          std::make_unique<std::byte[]>(allocationSize));
      offset_ = 0;
    }

    void* result = blocks_.back().get() + offset_;
    offset_ += alignedSize;
    return result;
  }

private:
  std::vector<std::unique_ptr<std::byte[]>> blocks_;
  std::size_t offset_ = 0;
};

thread_local ASTArena* activeASTArena = nullptr;

class ASTArenaScope final {
public:
  explicit ASTArenaScope(ASTArena* arena) noexcept
      : previous_(activeASTArena) {
    activeASTArena = arena;
  }

  ~ASTArenaScope() {
    activeASTArena = previous_;
  }

  ASTArenaScope(const ASTArenaScope&) = delete;
  ASTArenaScope& operator=(const ASTArenaScope&) = delete;

private:
  ASTArena* previous_;
};

struct alignas(std::max_align_t) ASTAllocationHeader {
  ASTArena* arena = nullptr;
  std::uint64_t magic = 0;
};

constexpr std::uint64_t kASTAllocationMagic =
    0x534946544152454Eull;

enum ParserTokenFlag : std::uint8_t {
  TokenFlagDeclaration = 1u << 0,
  TokenFlagControl = 1u << 1,
  TokenFlagContextualName = 1u << 2,
  TokenFlagAccess = 1u << 3,
  TokenFlagType = 1u << 4,
  TokenFlagUnary = 1u << 5
};

constexpr auto makeTokenFlags() {
  constexpr std::size_t count =
      static_cast<std::size_t>(TokenKind::Newline) + 1u;

  std::array<std::uint8_t, count> flags{};

  const auto add = [&flags](TokenKind kind, std::uint8_t flag) constexpr {
    flags[static_cast<std::size_t>(kind)] |= flag;
  };

  for (TokenKind kind : {
           TokenKind::KeywordVar, TokenKind::KeywordConst,
           TokenKind::KeywordFunction, TokenKind::KeywordStruct,
           TokenKind::KeywordClass, TokenKind::KeywordEnum,
           TokenKind::KeywordProtocol, TokenKind::KeywordExtension,
           TokenKind::KeywordPublic, TokenKind::KeywordPrivate,
           TokenKind::KeywordProtect, TokenKind::KeywordStatic,
           TokenKind::KeywordFinal, TokenKind::KeywordOpen,
           TokenKind::KeywordRequired}) {
    add(kind, TokenFlagDeclaration);
  }

  for (TokenKind kind : {
           TokenKind::KeywordIf, TokenKind::KeywordWhile,
           TokenKind::KeywordRepeat, TokenKind::KeywordFor,
           TokenKind::KeywordReturn, TokenKind::KeywordDefer,
           TokenKind::KeywordSwitch, TokenKind::KeywordBreak,
           TokenKind::KeywordContinue, TokenKind::KeywordGuard,
           TokenKind::KeywordLoop, TokenKind::KeywordDo,
           TokenKind::KeywordThrow, TokenKind::KeywordTry}) {
    add(kind, TokenFlagControl);
  }

  for (TokenKind kind : {
           TokenKind::KeywordData, TokenKind::KeywordMin,
           TokenKind::KeywordMax, TokenKind::KeywordOutput,
           TokenKind::KeywordMessage, TokenKind::KeywordMath,
           TokenKind::KeywordAbs, TokenKind::KeywordGetData,
           TokenKind::KeywordCreateData, TokenKind::KeywordControl,
           TokenKind::KeywordConnect, TokenKind::KeywordBackup,
           TokenKind::KeywordBinary, TokenKind::KeywordKernel,
           TokenKind::KeywordOS, TokenKind::KeywordDelete,
           TokenKind::KeywordDestroy, TokenKind::KeywordError,
           TokenKind::KeywordPanic, TokenKind::KeywordFile,
           TokenKind::KeywordFileID, TokenKind::KeywordAPI,
           TokenKind::KeywordRepo, TokenKind::KeywordWebLink,
           TokenKind::KeywordDatabase, TokenKind::KeywordSection,
           TokenKind::KeywordImport}) {
    add(kind, TokenFlagContextualName);
  }

  for (TokenKind kind : {
           TokenKind::KeywordPublic, TokenKind::KeywordPrivate,
           TokenKind::KeywordProtect, TokenKind::KeywordStatic,
           TokenKind::KeywordFinal, TokenKind::KeywordOpen,
           TokenKind::KeywordRequired}) {
    add(kind, TokenFlagAccess);
  }

  for (TokenKind kind : {
           TokenKind::Identifier, TokenKind::KeywordInt,
           TokenKind::KeywordNum, TokenKind::KeywordString,
           TokenKind::KeywordBool, TokenKind::KeywordBytes,
           TokenKind::KeywordAny, TokenKind::KeywordSome}) {
    add(kind, TokenFlagType);
  }

  for (TokenKind kind : {
           TokenKind::Bang, TokenKind::Minus, TokenKind::Plus,
           TokenKind::KeywordError, TokenKind::KeywordPanic}) {
    add(kind, TokenFlagUnary);
  }

  return flags;
}

constexpr auto kTokenFlags = makeTokenFlags();

inline bool hasTokenFlag(TokenKind kind, std::uint8_t flag) noexcept {
  const std::size_t index = static_cast<std::size_t>(kind);
  return index < kTokenFlags.size() &&
         (kTokenFlags[index] & flag) != 0;
}

class ParserThreadPool final {
public:
  ParserThreadPool() {
    unsigned int count = std::thread::hardware_concurrency();
    if (count == 0) count = 1;

    workers_.reserve(count);
    for (unsigned int index = 0; index < count; ++index) {
      workers_.emplace_back([this] { workerLoop(); });
    }
  }

  ~ParserThreadPool() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
      ++generation_;
    }
    condition_.notify_all();

    for (auto& worker : workers_) {
      if (worker.joinable()) worker.join();
    }
  }

  ParserThreadPool(const ParserThreadPool&) = delete;
  ParserThreadPool& operator=(const ParserThreadPool&) = delete;

  void run(
      const Parser* parser,
      const std::vector<Parser::PieceBoundary>& boundaries,
      std::vector<Parser::PieceResult>& results) {
    std::lock_guard runLock(runMutex_);
    parserThreadPool().run(this, boundaries, results);

  for (PieceResult& result : results) {
    if (!result.program) continue;

    program->arenaOwners.insert(
        program->arenaOwners.end(),
        std::make_move_iterator(result.program->arenaOwners.begin()),
        std::make_move_iterator(result.program->arenaOwners.end()));
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
  auto arena = std::make_shared<ASTArena>();
  ASTArenaScope arenaScope(arena.get());

  auto program = std::make_unique<Program>();
  program->arenaOwners.push_back(arena);
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

  constexpr std::size_t inlineCapacity = 16;
  std::array<TokenKind, inlineCapacity> inlineOperators{};
  std::array<Token, inlineCapacity> inlineOperatorTokens{};
  std::array<std::unique_ptr<Expression>, inlineCapacity + 1> inlineValues{};
  std::size_t operatorCount = 0;
  std::size_t valueCount = 1;

  inlineValues[0] = std::move(left);

  std::vector<TokenKind> spilledOperators;
  std::vector<Token> spilledOperatorTokens;
  std::vector<std::unique_ptr<Expression>> spilledValues;
  bool spilled = false;

  auto spill = [&]() {
    if (spilled) return;

    spilledOperators.reserve(operatorCount + 16);
    spilledOperatorTokens.reserve(operatorCount + 16);
    spilledValues.reserve(valueCount + 16);

    for (std::size_t index = 0; index < operatorCount; ++index) {
      spilledOperators.push_back(inlineOperators[index]);
      spilledOperatorTokens.push_back(inlineOperatorTokens[index]);
    }

    for (std::size_t index = 0; index < valueCount; ++index) {
      spilledValues.push_back(std::move(inlineValues[index]));
    }

    spilled = true;
  };

  auto reduce = [&]() {
    TokenKind operatorKind;
    Token operatorToken;
    std::unique_ptr<Expression> right;
    std::unique_ptr<Expression> leftValue;

    if (spilled) {
      operatorKind = spilledOperators.back();
      operatorToken = spilledOperatorTokens.back();
      spilledOperators.pop_back();
      spilledOperatorTokens.pop_back();

      right = std::move(spilledValues.back());
      spilledValues.pop_back();

      leftValue = std::move(spilledValues.back());
      spilledValues.pop_back();
    } else {
      operatorKind = inlineOperators[operatorCount - 1];
      operatorToken = inlineOperatorTokens[operatorCount - 1];
      --operatorCount;

      right = std::move(inlineValues[valueCount - 1]);
      --valueCount;

      leftValue = std::move(inlineValues[valueCount - 1]);
      --valueCount;
    }

    auto node = std::make_unique<Expression>();
    node->location = SourceLocation{operatorToken.start, 0u, 0u};

    BinaryExpression binary;
    binary.op = operatorText(operatorKind);
    binary.left = std::move(leftValue);
    binary.right = std::move(right);
    node->value = std::move(binary);

    if (spilled) {
      spilledValues.push_back(std::move(node));
    } else {
      inlineValues[valueCount++] = std::move(node);
    }
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

    if (!spilled && operatorCount == inlineCapacity) {
      spill();
    }

    if (spilled) {
      spilledOperators.push_back(operatorKind);
      spilledOperatorTokens.push_back(operatorToken);
      spilledValues.push_back(std::move(right));
    } else {
      inlineOperators[operatorCount] = operatorKind;
      inlineOperatorTokens[operatorCount] = operatorToken;
      ++operatorCount;
      inlineValues[valueCount++] = std::move(right);
    }
  }

  while ((spilled && !spilledOperators.empty()) ||
         (!spilled && operatorCount != 0)) {
    reduce();
  }

  if (spilled) {
    return std::move(spilledValues.back());
  }

  return std::move(inlineValues[0]);
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