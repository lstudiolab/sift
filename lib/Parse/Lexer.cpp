// Sift production lexer implementation.
//
// This scanner follows the same broad engineering principles used by
// production compiler lexers: contiguous-buffer pointer scanning, zero-copy
// token text, bounded lookahead, deterministic keyword classification,
// contextual token formation, precise source locations, UTF-8 validation,
// structured diagnostics, and recovery without recursive rescanning.
//
// The language rules in this file are Sift-specific. The implementation
// techniques are intentionally suitable for a high-level language compiler
// that may ultimately target low-level and bare-metal backends.

#include "sift/Parse/Lexer.h"
#include "sift/Parse/Diagnostic.h"
#include "sift/Parse/LexerCursor.h"
#include "sift/Parse/LexerOptions.h"
#include "sift/Parse/SourceLocation.h"
#include "sift/Parse/Token.h"
#include "sift/Parse/TokenKind.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace sift::lexer {

namespace {

struct KeywordEntry {
  std::string_view spelling;
  TokenKind kind;
  std::uint64_t hash;
};

constexpr std::uint64_t fnv1a(std::string_view text) noexcept {
  std::uint64_t value = 14695981039346656037ull;

  for (unsigned char character : text) {
    value ^= static_cast<std::uint64_t>(character);
    value *= 1099511628211ull;
  }

  return value;
}

constexpr std::array<KeywordEntry, 90> Keywords = {{
  {"var", TokenKind::KeywordVar, fnv1a("var")},
  {"const", TokenKind::KeywordConst, fnv1a("const")},
  {"function", TokenKind::KeywordFunction, fnv1a("function")},
  {"init", TokenKind::KeywordInit, fnv1a("init")},
  {"deinit", TokenKind::KeywordDeinit, fnv1a("deinit")},
  {"type", TokenKind::KeywordType, fnv1a("type")},
  {"typealias", TokenKind::KeywordTypealias, fnv1a("typealias")},
  {"struct", TokenKind::KeywordStruct, fnv1a("struct")},
  {"class", TokenKind::KeywordClass, fnv1a("class")},
  {"enum", TokenKind::KeywordEnum, fnv1a("enum")},
  {"protocol", TokenKind::KeywordProtocol, fnv1a("protocol")},
  {"extension", TokenKind::KeywordExtension, fnv1a("extension")},
  {"if", TokenKind::KeywordIf, fnv1a("if")},
  {"else", TokenKind::KeywordElse, fnv1a("else")},
  {"end", TokenKind::KeywordEnd, fnv1a("end")},
  {"guard", TokenKind::KeywordGuard, fnv1a("guard")},
  {"switch", TokenKind::KeywordSwitch, fnv1a("switch")},
  {"case", TokenKind::KeywordCase, fnv1a("case")},
  {"defat", TokenKind::KeywordDefat, fnv1a("defat")},
  {"while", TokenKind::KeywordWhile, fnv1a("while")},
  {"repeat", TokenKind::KeywordRepeat, fnv1a("repeat")},
  {"for", TokenKind::KeywordFor, fnv1a("for")},
  {"loop", TokenKind::KeywordLoop, fnv1a("loop")},
  {"in", TokenKind::KeywordIn, fnv1a("in")},
  {"do", TokenKind::KeywordDo, fnv1a("do")},
  {"break", TokenKind::KeywordBreak, fnv1a("break")},
  {"continue", TokenKind::KeywordContinue, fnv1a("continue")},
  {"return", TokenKind::KeywordReturn, fnv1a("return")},
  {"defer", TokenKind::KeywordDefer, fnv1a("defer")},
  {"call", TokenKind::KeywordCall, fnv1a("call")},
  {"async", TokenKind::KeywordAsync, fnv1a("async")},
  {"await", TokenKind::KeywordAwait, fnv1a("await")},
  {"wait", TokenKind::KeywordWait, fnv1a("wait")},
  {"throws", TokenKind::KeywordThrows, fnv1a("throws")},
  {"throw", TokenKind::KeywordThrow, fnv1a("throw")},
  {"try", TokenKind::KeywordTry, fnv1a("try")},
  {"catch", TokenKind::KeywordCatch, fnv1a("catch")},
  {"rethrow", TokenKind::KeywordRethrow, fnv1a("rethrow")},
  {"task", TokenKind::KeywordTask, fnv1a("task")},
  {"true", TokenKind::KeywordTrue, fnv1a("true")},
  {"false", TokenKind::KeywordFalse, fnv1a("false")},
  {"self", TokenKind::KeywordSelf, fnv1a("self")},
  {"some", TokenKind::KeywordSome, fnv1a("some")},
  {"any", TokenKind::KeywordAny, fnv1a("any")},
  {"int", TokenKind::KeywordInt, fnv1a("int")},
  {"num", TokenKind::KeywordNum, fnv1a("num")},
  {"string", TokenKind::KeywordString, fnv1a("string")},
  {"bool", TokenKind::KeywordBool, fnv1a("bool")},
  {"bytes", TokenKind::KeywordBytes, fnv1a("bytes")},
  {"public", TokenKind::KeywordPublic, fnv1a("public")},
  {"private", TokenKind::KeywordPrivate, fnv1a("private")},
  {"protect", TokenKind::KeywordProtect, fnv1a("protect")},
  {"static", TokenKind::KeywordStatic, fnv1a("static")},
  {"final", TokenKind::KeywordFinal, fnv1a("final")},
  {"open", TokenKind::KeywordOpen, fnv1a("open")},
  {"overRide", TokenKind::KeywordOverRide, fnv1a("overRide")},
  {"required", TokenKind::KeywordRequired, fnv1a("required")},
  {"import", TokenKind::KeywordImport, fnv1a("import")},
  {"export", TokenKind::KeywordExport, fnv1a("export")},
  {"module", TokenKind::KeywordModule, fnv1a("module")},
  {"package", TokenKind::KeywordPackage, fnv1a("package")},
  {"file", TokenKind::KeywordFile, fnv1a("file")},
  {"@file", TokenKind::KeywordFile, fnv1a("@file")},
  {"@fileID", TokenKind::KeywordFileID, fnv1a("@fileID")},
  {"@api", TokenKind::KeywordAPI, fnv1a("@api")},
  {"@repo", TokenKind::KeywordRepo, fnv1a("@repo")},
  {"@webLink", TokenKind::KeywordWebLink, fnv1a("@webLink")},
  {"@database", TokenKind::KeywordDatabase, fnv1a("@database")},
  {"message", TokenKind::KeywordMessage, fnv1a("message")},
  {"error", TokenKind::KeywordError, fnv1a("error")},
  {"math", TokenKind::KeywordMath, fnv1a("math")},
  {"abs", TokenKind::KeywordAbs, fnv1a("abs")},
  {"min", TokenKind::KeywordMin, fnv1a("min")},
  {"max", TokenKind::KeywordMax, fnv1a("max")},
  {"decrease", TokenKind::KeywordDecrease, fnv1a("decrease")},
  {"increase", TokenKind::KeywordIncrease, fnv1a("increase")},
  {"section", TokenKind::KeywordSection, fnv1a("section")},
  {"data", TokenKind::KeywordData, fnv1a("data")},
  {"getData", TokenKind::KeywordGetData, fnv1a("getData")},
  {"createData", TokenKind::KeywordCreateData, fnv1a("createData")},
  {"control", TokenKind::KeywordControl, fnv1a("control")},
  {"connect", TokenKind::KeywordConnect, fnv1a("connect")},
  {"backup", TokenKind::KeywordBackup, fnv1a("backup")},
  {"binary", TokenKind::KeywordBinary, fnv1a("binary")},
  {"kernel", TokenKind::KeywordKernel, fnv1a("kernel")},
  {"os", TokenKind::KeywordOS, fnv1a("os")},
  {"output", TokenKind::KeywordOutput, fnv1a("output")},
  {"delete", TokenKind::KeywordDelete, fnv1a("delete")},
  {"destroy", TokenKind::KeywordDestroy, fnv1a("destroy")},
  {"panic", TokenKind::KeywordPanic, fnv1a("panic")}
}};

struct KeywordBuckets {
  std::array<int, 256> heads{};
  std::array<int, Keywords.size()> next{};

  constexpr KeywordBuckets() {
    heads.fill(-1);
    next.fill(-1);

    for (std::size_t index = 0; index < Keywords.size(); ++index) {
      const std::size_t bucket =
          static_cast<std::size_t>(Keywords[index].hash & 0xffu);

      next[index] = heads[bucket];
      heads[bucket] = static_cast<int>(index);
    }
  }
};

constexpr KeywordBuckets KeywordIndex{};

static_assert(Keywords.size() == 90, "Sift keyword table changed without updating its declared size.");

constexpr bool isContinuationByte(unsigned char value) noexcept {
  return (value & 0xc0u) == 0x80u;
}

constexpr bool isAsciiLetter(char value) noexcept {
  return (value >= 'a' && value <= 'z') ||
         (value >= 'A' && value <= 'Z');
}

constexpr bool isAsciiDigitValue(char value) noexcept {
  return value >= '0' && value <= '9';
}


namespace detail {

constexpr unsigned char byteOf(char value) noexcept {
  return static_cast<unsigned char>(value);
}

constexpr bool isAsciiLetter(char value) noexcept {
  return (value >= 'a' && value <= 'z') ||
         (value >= 'A' && value <= 'Z');
}

constexpr bool isAsciiDigit(char value) noexcept {
  return value >= '0' && value <= '9';
}

constexpr bool isAsciiIdentifierStart(char value) noexcept {
  return isAsciiLetter(value) ||
         value == '_';
}

inline bool hasBytes(
    const char* current,
    const char* end,
    std::size_t count) noexcept {
  if (current > end) {
    return false;
  }

  return static_cast<std::size_t>(end - current) >= count;
}

inline bool startsBOM(
    const char* current,
    const char* end) noexcept {
  return hasBytes(current, end, 3u) &&
         byteOf(current[0]) == 0xefu &&
         byteOf(current[1]) == 0xbbu &&
         byteOf(current[2]) == 0xbfu;
}

inline bool isPotentialComment(
    const char* current,
    const char* end) noexcept {
  return hasBytes(current, end, 2u) &&
         current[0] == '/' &&
         (current[1] == '/' || current[1] == '*');
}

inline bool isPotentialIdentifier(
    char current) noexcept {
  return isAsciiIdentifierStart(current) ||
         byteOf(current) >= 0x80u;
}

inline bool keywordCandidateCanMatch(
    std::string_view text) noexcept {
  if (text.empty()) {
    return false;
  }

  if (text.size() > 16u) {
    return false;
  }

  for (char value : text) {
    if (byteOf(value) >= 0x80u) {
      return false;
    }
  }

  return true;
}

inline bool isHorizontalWhitespace(char value) noexcept {
  return value == ' ' ||
         value == '\t' ||
         value == '\v' ||
         value == '\f';
}


inline bool isOperatorLeadByte(char value) noexcept {
  return value == '=' ||
         value == '!' ||
         value == '+' ||
         value == '-' ||
         value == '*' ||
         value == '%' ||
         value == '<' ||
         value == '>' ||
         value == '&' ||
         value == '|' ||
         value == '~';
}

inline bool isPunctuationLeadByte(char value) noexcept {
  return value == '(' ||
         value == ')' ||
         value == '{' ||
         value == '}' ||
         value == '[' ||
         value == ']' ||
         value == ',' ||
         value == '.' ||
         value == ':' ||
         value == ';' ||
         value == '?';
}

inline bool isDoubleQuote(char value) noexcept {
  return value == '"';
}

inline bool isSingleQuote(char value) noexcept {
  return value == '\'';
}

inline bool isHashLead(char value) noexcept {
  return value == '#';
}

inline bool isAtLead(char value) noexcept {
  return value == '@';
}

inline const char* scanAsciiIdentifier(
    const char* current,
    const char* end) noexcept {
  while (current < end) {
    const unsigned char value =
        static_cast<unsigned char>(*current);

    if (value >= 0x80u) {
      break;
    }

    if (!isAsciiIdentifierStart(
            static_cast<char>(value)) &&
        static_cast<char>(value) != '$' &&
        !isAsciiDigit(
            static_cast<char>(value))) {
      break;
    }

    ++current;
  }

  return current;
}

inline const char* scanHorizontalWhitespace(
    const char* current,
    const char* end) noexcept {
  while (current < end &&
         isHorizontalWhitespace(*current)) {
    ++current;
  }

  return current;
}

inline const char* scanUntilLineBreak(
    const char* current,
    const char* end) noexcept {
  while (current < end) {
    const char value = *current;

    if (value == '\n' ||
        value == '\r') {
      break;
    }

    ++current;
  }

  return current;
}
}

} // namespace detail

// Initialize the lexer over the source buffer.
Lexer::Lexer(std::string_view source, LexerOptions options) noexcept
    : source_(source),
      options_(options) {
  cursor_.current = source_.data();
  cursor_.end = source_.data() + source_.size();
  cursor_.offset = 0;
  cursor_.line = 1;
  cursor_.column = 1;

  tokenStart_ = cursor_;
}

// Reset the lexer state to the start of the source.
void Lexer::reset() noexcept {
  cursor_.current = source_.data();
  cursor_.end = source_.data() + source_.size();
  cursor_.offset = 0;
  cursor_.line = 1;
  cursor_.column = 1;

  tokenStart_ = cursor_;

  lookahead_ = {};
  lookaheadDiagnostics_.clear();
  hasLookahead_ = false;

  expectingCallingName_ = false;
  afterFunctionKeyword_ = false;
  sawFunctionName_ = false;
  lastWasDot_ = false;

  diagnostics_.clear();
}

bool Lexer::atEnd() const noexcept {
  return cursor_.current >= cursor_.end;
}

std::size_t Lexer::offset() const noexcept {
  return cursor_.offset;
}

SourceLocation Lexer::location() const noexcept {
  return {
      cursor_.offset,
      cursor_.line,
      cursor_.column
  };
}

const std::vector<Diagnostic>& Lexer::diagnostics() const noexcept {
  return diagnostics_;
}

bool Lexer::hasErrors() const noexcept {
  for (const Diagnostic& diagnostic : diagnostics_) {
    if (diagnostic.severity == DiagnosticSeverity::Error) {
      return true;
    }
  }

  return false;
}

// Read ahead without advancing the cursor.
char Lexer::peekChar(std::size_t distance) const noexcept {
  const std::size_t remaining =
      static_cast<std::size_t>(cursor_.end - cursor_.current);

  if (distance >= remaining) {
    return '\0';
  }

  return cursor_.current[distance];
}

// Consume one character and update its source location.
char Lexer::consumeChar() noexcept {
  if (atEnd()) {
    return '\0';
  }

  const char value = *cursor_.current;

  if (value == '\r') {
    ++cursor_.current;
    ++cursor_.offset;

    if (!atEnd() &&
        *cursor_.current == '\n') {
      ++cursor_.current;
      ++cursor_.offset;
    }

    ++cursor_.line;
    cursor_.column = 1;

    return '\n';
  }

  ++cursor_.current;
  ++cursor_.offset;

  if (value == '\n') {
    ++cursor_.line;
    cursor_.column = 1;
  } else {
    ++cursor_.column;
  }

  return value;
}

bool Lexer::consumeIf(char value) noexcept {
  if (peekChar() != value) {
    return false;
  }

  consumeChar();
  return true;
}

bool Lexer::consumeIf(std::string_view value) noexcept {
  const std::size_t length =
      value.size();

  const std::size_t remaining =
      static_cast<std::size_t>(
          cursor_.end - cursor_.current);

  if (length > remaining) {
    return false;
  }

  const char* current =
      cursor_.current;

  for (std::size_t index = 0;
       index < length;
       ++index) {
    if (current[index] != value[index]) {
      return false;
    }
  }

  cursor_.current =
      current + length;
  cursor_.offset += length;
  cursor_.column += length;

  return true;
}

// Record the start of the next token.
void Lexer::beginToken() noexcept {
  tokenStart_ = cursor_;
}

Token Lexer::makeToken(
    TokenKind kind,
    const char* begin,
    const char* end,
    SourceLocation tokenLocation) const noexcept {
  const std::size_t length =
      static_cast<std::size_t>(end - begin);

  return {
      kind,
      std::string_view(begin, length),
      tokenLocation,
      static_cast<std::size_t>(end - source_.data())
  };
}

// Form a zero-copy token from the current source range.
Token Lexer::finish(TokenKind kind) noexcept {
  return makeToken(
      kind,
      tokenStart_.current,
      cursor_.current,
      {
          tokenStart_.offset,
          tokenStart_.line,
          tokenStart_.column
      });
}

void Lexer::addDiagnostic(
    DiagnosticSeverity severity,
    SourceLocation diagnosticLocation,
    std::string_view message) {
  diagnostics_.push_back({
      severity,
      diagnosticLocation,
      message
  });
}

// Recover from malformed input at a safe boundary.
void Lexer::recoverAfterLexicalError(
    bool stopAtLineBreak) noexcept {
  const char* recoveryStart = cursor_.current;

  while (!atEnd()) {
    const char value = peekChar();

    if (value == '\n' ||
        value == '\r') {
      if (stopAtLineBreak) {
        break;
      }

      consumeChar();
      continue;
    }

    if (isAsciiSpace(value)) {
      break;
    }

    if (detail::isPunctuationLeadByte(value)) {
      break;
    }

    if (value == '/' &&
        (peekChar(1) == '/' ||
         peekChar(1) == '*')) {
      break;
    }

    if (detail::isOperatorLeadByte(value)) {
      break;
    }

    consumeChar();
  }

  if (cursor_.current == recoveryStart &&
      !atEnd() &&
      peekChar() != '\n' &&
      peekChar() != '\r') {
    consumeChar();
  }
}

// Recover the current malformed token and continue lexing.
void Lexer::recoverMalformedToken() noexcept {
  recoverAfterLexicalError(true);
}

// Recover a malformed string up to its delimiter.
void Lexer::recoverStringLiteral() noexcept {
  while (!atEnd()) {
    const char value = peekChar();

    if (value == '"' ||
        value == '\n' ||
        value == '\r') {
      return;
    }

    consumeChar();
  }
}

// Recover a malformed character up to its delimiter.
void Lexer::recoverCharacterLiteral() noexcept {
  while (!atEnd()) {
    const char value = peekChar();

    if (value == '\'' ||
        value == '\n' ||
        value == '\r') {
      return;
    }

    consumeChar();
  }
}

bool Lexer::isAsciiSpace(char value) noexcept {
  return value == ' ' ||
         value == '\t' ||
         value == '\v' ||
         value == '\f' ||
         value == '\r' ||
         value == '\n';
}

bool Lexer::isDecimalDigit(char value) noexcept {
  return value >= '0' && value <= '9';
}

bool Lexer::isHexDigit(char value) noexcept {
  return isDecimalDigit(value) ||
         (value >= 'a' && value <= 'f') ||
         (value >= 'A' && value <= 'F');
}

bool Lexer::isBinaryDigit(char value) noexcept {
  return value == '0' || value == '1';
}

bool Lexer::isOctalDigit(char value) noexcept {
  return value >= '0' && value <= '7';
}

unsigned Lexer::hexValue(char value) noexcept {
  if (value >= '0' && value <= '9') {
    return static_cast<unsigned>(value - '0');
  }

  if (value >= 'a' && value <= 'f') {
    return static_cast<unsigned>(value - 'a' + 10);
  }

  return static_cast<unsigned>(value - 'A' + 10);
}

std::uint64_t Lexer::keywordHash(std::string_view text) noexcept {
  return fnv1a(text);
}

TokenKind Lexer::classifyKeyword(std::string_view text) noexcept {
  // Sift keywords are a closed part of the grammar. Recognition therefore
  // stays entirely inside the lexer and never allocates or consults runtime
  // state.
  //
  // The operation intentionally proceeds in several explicit stages:
  //   1. reject impossible spellings;
  //   2. reject spellings that cannot be Sift keywords because they contain
  //      non-ASCII bytes;
  //   3. calculate the stable compile-time-compatible hash;
  //   4. select one of 256 buckets;
  //   5. compare candidate lengths;
  //   6. compare candidate hashes;
  //   7. perform the final exact spelling comparison.
  //
  // The lexer never constructs a temporary string, never inserts into a
  // runtime map, and never scans the entire keyword table for an identifier.
  // This keeps keyword recognition deterministic while leaving the common
  // identifier path allocation-free.
  //
  // Keeping these stages explicit makes the hot path predictable and makes
  // the token-formation contract easy to audit.

  if (!detail::keywordCandidateCanMatch(text)) {
    return TokenKind::Identifier;
  }

  const std::uint64_t textHash =
      keywordHash(text);

  const std::size_t bucket =
      static_cast<std::size_t>(
          textHash & 0xffu);

  int keywordIndex =
      KeywordIndex.heads[bucket];

  while (keywordIndex >= 0) {
    const KeywordEntry& keyword =
        Keywords[static_cast<std::size_t>(keywordIndex)];

    if (keyword.spelling.size() != text.size()) {
      keywordIndex =
          KeywordIndex.next[
              static_cast<std::size_t>(keywordIndex)];

      continue;
    }

    if (keyword.hash != textHash) {
      keywordIndex =
          KeywordIndex.next[
              static_cast<std::size_t>(keywordIndex)];

      continue;
    }

    if (keyword.spelling == text) {
      return keyword.kind;
    }

    keywordIndex =
        KeywordIndex.next[
            static_cast<std::size_t>(keywordIndex)];
  }

  return TokenKind::Identifier;
}

TokenKind Lexer::keywordKind(std::string_view text) noexcept {
  return classifyKeyword(text);
}

bool Lexer::isKeyword(std::string_view text) noexcept {
  return classifyKeyword(text) != TokenKind::Identifier;
}

std::string_view Lexer::tokenName(TokenKind kind) noexcept {
  switch (kind) {
    case TokenKind::EndOfFile:
      return "EndOfFile";
    case TokenKind::Unknown:
      return "Unknown";
    case TokenKind::Identifier:
      return "Identifier";
    case TokenKind::CallingName:
      return "CallingName";
    case TokenKind::IntegerLiteral:
      return "IntegerLiteral";
    case TokenKind::FloatingLiteral:
      return "FloatingLiteral";
    case TokenKind::StringLiteral:
      return "StringLiteral";
    case TokenKind::CharacterLiteral:
      return "CharacterLiteral";

    case TokenKind::KeywordVar:
      return "KeywordVar";
    case TokenKind::KeywordConst:
      return "KeywordConst";
    case TokenKind::KeywordFunction:
      return "KeywordFunction";
    case TokenKind::KeywordInit:
      return "KeywordInit";
    case TokenKind::KeywordDeinit:
      return "KeywordDeinit";
    case TokenKind::KeywordType:
      return "KeywordType";
    case TokenKind::KeywordTypealias:
      return "KeywordTypealias";
    case TokenKind::KeywordStruct:
      return "KeywordStruct";
    case TokenKind::KeywordClass:
      return "KeywordClass";
    case TokenKind::KeywordEnum:
      return "KeywordEnum";
    case TokenKind::KeywordProtocol:
      return "KeywordProtocol";
    case TokenKind::KeywordExtension:
      return "KeywordExtension";
    case TokenKind::KeywordIf:
      return "KeywordIf";
    case TokenKind::KeywordElse:
      return "KeywordElse";
    case TokenKind::KeywordEnd:
      return "KeywordEnd";
    case TokenKind::KeywordGuard:
      return "KeywordGuard";
    case TokenKind::KeywordSwitch:
      return "KeywordSwitch";
    case TokenKind::KeywordCase:
      return "KeywordCase";
    case TokenKind::KeywordDefat:
      return "KeywordDefat";
    case TokenKind::KeywordWhile:
      return "KeywordWhile";
    case TokenKind::KeywordRepeat:
      return "KeywordRepeat";
    case TokenKind::KeywordFor:
      return "KeywordFor";
    case TokenKind::KeywordLoop:
      return "KeywordLoop";
    case TokenKind::KeywordIn:
      return "KeywordIn";
    case TokenKind::KeywordDo:
      return "KeywordDo";
    case TokenKind::KeywordBreak:
      return "KeywordBreak";
    case TokenKind::KeywordContinue:
      return "KeywordContinue";
    case TokenKind::KeywordReturn:
      return "KeywordReturn";
    case TokenKind::KeywordDefer:
      return "KeywordDefer";
    case TokenKind::KeywordCall:
      return "KeywordCall";
    case TokenKind::KeywordAsync:
      return "KeywordAsync";
    case TokenKind::KeywordAwait:
      return "KeywordAwait";
    case TokenKind::KeywordWait:
      return "KeywordWait";
    case TokenKind::KeywordThrows:
      return "KeywordThrows";
    case TokenKind::KeywordThrow:
      return "KeywordThrow";
    case TokenKind::KeywordTry:
      return "KeywordTry";
    case TokenKind::KeywordCatch:
      return "KeywordCatch";
    case TokenKind::KeywordRethrow:
      return "KeywordRethrow";
    case TokenKind::KeywordTask:
      return "KeywordTask";
    case TokenKind::KeywordTrue:
      return "KeywordTrue";
    case TokenKind::KeywordFalse:
      return "KeywordFalse";
    case TokenKind::KeywordSelf:
      return "KeywordSelf";
    case TokenKind::KeywordSome:
      return "KeywordSome";
    case TokenKind::KeywordAny:
      return "KeywordAny";
    case TokenKind::KeywordInt:
      return "KeywordInt";
    case TokenKind::KeywordNum:
      return "KeywordNum";
    case TokenKind::KeywordString:
      return "KeywordString";
    case TokenKind::KeywordBool:
      return "KeywordBool";
    case TokenKind::KeywordBytes:
      return "KeywordBytes";
    case TokenKind::KeywordPublic:
      return "KeywordPublic";
    case TokenKind::KeywordPrivate:
      return "KeywordPrivate";
    case TokenKind::KeywordProtect:
      return "KeywordProtect";
    case TokenKind::KeywordStatic:
      return "KeywordStatic";
    case TokenKind::KeywordFinal:
      return "KeywordFinal";
    case TokenKind::KeywordOpen:
      return "KeywordOpen";
    case TokenKind::KeywordOverRide:
      return "KeywordOverRide";
    case TokenKind::KeywordRequired:
      return "KeywordRequired";
    case TokenKind::KeywordImport:
      return "KeywordImport";
    case TokenKind::KeywordExport:
      return "KeywordExport";
    case TokenKind::KeywordModule:
      return "KeywordModule";
    case TokenKind::KeywordPackage:
      return "KeywordPackage";
    case TokenKind::KeywordFile:
      return "KeywordFile";
    case TokenKind::KeywordFileID:
      return "KeywordFileID";
    case TokenKind::KeywordAPI:
      return "KeywordAPI";
    case TokenKind::KeywordRepo:
      return "KeywordRepo";
    case TokenKind::KeywordWebLink:
      return "KeywordWebLink";
    case TokenKind::KeywordDatabase:
      return "KeywordDatabase";
    case TokenKind::KeywordMessage:
      return "KeywordMessage";
    case TokenKind::KeywordError:
      return "KeywordError";
    case TokenKind::KeywordMath:
      return "KeywordMath";
    case TokenKind::KeywordAbs:
      return "KeywordAbs";
    case TokenKind::KeywordMin:
      return "KeywordMin";
    case TokenKind::KeywordMax:
      return "KeywordMax";
    case TokenKind::KeywordDecrease:
      return "KeywordDecrease";
    case TokenKind::KeywordIncrease:
      return "KeywordIncrease";
    case TokenKind::KeywordSection:
      return "KeywordSection";
    case TokenKind::KeywordData:
      return "KeywordData";    case TokenKind::KeywordGetData:
      return "KeywordGetData";
    case TokenKind::KeywordCreateData:
      return "KeywordCreateData";
    case TokenKind::KeywordControl:
      return "KeywordControl";
    case TokenKind::KeywordConnect:
      return "KeywordConnect";
    case TokenKind::KeywordBackup:
      return "KeywordBackup";
    case TokenKind::KeywordBinary:
      return "KeywordBinary";
    case TokenKind::KeywordKernel:
      return "KeywordKernel";
    case TokenKind::KeywordOS:
      return "KeywordOS";
    case TokenKind::KeywordOutput:
      return "KeywordOutput";
    case TokenKind::KeywordDelete:
      return "KeywordDelete";
    case TokenKind::KeywordDestroy:
      return "KeywordDestroy";
    case TokenKind::KeywordPanic:
      return "KeywordPanic";

    case TokenKind::LeftParen:
      return "LeftParen";
    case TokenKind::RightParen:
      return "RightParen";
    case TokenKind::LeftBrace:
      return "LeftBrace";
    case TokenKind::RightBrace:
      return "RightBrace";
    case TokenKind::LeftBracket:
      return "LeftBracket";
    case TokenKind::RightBracket:
      return "RightBracket";
    case TokenKind::Comma:
      return "Comma";
    case TokenKind::Dot:
      return "Dot";
    case TokenKind::Colon:
      return "Colon";
    case TokenKind::Semicolon:
      return "Semicolon";
    case TokenKind::Question:
      return "Question";
    case TokenKind::At:
      return "At";
    case TokenKind::Hash:
      return "Hash";

    case TokenKind::Equal:
      return "Equal";
    case TokenKind::EqualEqual:
      return "EqualEqual";
    case TokenKind::Bang:
      return "Bang";
    case TokenKind::BangEqual:
      return "BangEqual";
    case TokenKind::Plus:
      return "Plus";
    case TokenKind::PlusEqual:
      return "PlusEqual";
    case TokenKind::Minus:
      return "Minus";
    case TokenKind::MinusEqual:
      return "MinusEqual";
    case TokenKind::Star:
      return "Star";
    case TokenKind::StarEqual:
      return "StarEqual";
    case TokenKind::Slash:
      return "Slash";
    case TokenKind::SlashEqual:
      return "SlashEqual";
    case TokenKind::Percent:
      return "Percent";
    case TokenKind::PercentEqual:
      return "PercentEqual";
    case TokenKind::Less:
      return "Less";
    case TokenKind::LessEqual:
      return "LessEqual";
    case TokenKind::Greater:
      return "Greater";
    case TokenKind::GreaterEqual:
      return "GreaterEqual";
    case TokenKind::AndAnd:
      return "AndAnd";
    case TokenKind::OrOr:
      return "OrOr";
    case TokenKind::Arrow:
      return "Arrow";
    case TokenKind::Tilde:
      return "Tilde";
    case TokenKind::TildeEqual:
      return "TildeEqual";
    case TokenKind::PlusPlus:
      return "PlusPlus";
    case TokenKind::MinusMinus:
      return "MinusMinus";
    case TokenKind::Comment:
      return "Comment";
    case TokenKind::Newline:
      return "Newline";
  }

  return "Unknown";
}

bool Lexer::isAsciiIdentifierStart(char value) const noexcept {
  return isAsciiLetter(value) ||
         value == '_' ||
         value == '$';
}

bool Lexer::isAsciiIdentifierContinue(char value) const noexcept {
  return isAsciiIdentifierStart(value) ||
         isAsciiDigitValue(value);
}

bool Lexer::isIdentifierStart(char value) const noexcept {
  if (isAsciiIdentifierStart(value)) {
    return true;
  }

  if (!options_.allowUnicodeIdentifiers) {
    return false;
  }

  return static_cast<unsigned char>(value) >= 0x80u;
}

bool Lexer::isIdentifierContinue(char value) const noexcept {
  if (isAsciiIdentifierContinue(value)) {
    return true;
  }

  if (!options_.allowUnicodeIdentifiers) {
    return false;
  }

  return static_cast<unsigned char>(value) >= 0x80u;
}

void Lexer::consumeIdentifier() {
  const char* begin = cursor_.current;

  // The overwhelmingly common identifier path is ASCII. Scan it with raw
  // pointers so the hot loop does not repeatedly call peekChar(), perform
  // bounds calculations, or update source-location state one byte at a time.
  const char* current =
      scanAsciiIdentifier(
          begin,
          cursor_.end);

  const std::size_t asciiBytes =
      static_cast<std::size_t>(
          current - begin);

  if (asciiBytes != 0) {
    cursor_.current = current;
    cursor_.offset += asciiBytes;
    cursor_.column += asciiBytes;
  }

  // Unicode is deliberately kept as a separate slow path. This preserves
  // the fast ASCII path while retaining the language option for Unicode
  // identifiers and the existing UTF-8 validation performed by lexing.
  if (current < cursor_.end &&
      static_cast<unsigned char>(*current) >= 0x80u &&
      options_.allowUnicodeIdentifiers) {
    consumeUnicodeIdentifier();
  }
}

void Lexer::consumeUnicodeIdentifier() {
  while (!atEnd()) {
    const unsigned char value =
        static_cast<unsigned char>(peekChar());

    if (value < 0x80u) {
      break;
    }

    consumeChar();
  }
}

void Lexer::skipWhitespace() {
  for (;;) {
    if (atEnd()) {
      return;
    }

    // A BOM is valid only at the beginning of the source buffer. Handle it
    // before the generic whitespace scan so it never becomes part of a token.
    if (cursor_.offset == 0 &&
        detail::startsBOM(
            cursor_.current,
            cursor_.end)) {
      cursor_.current += 3;
      cursor_.offset += 3;
      cursor_.column += 3;
      continue;
    }

    const char* begin =
        cursor_.current;

    const char* afterHorizontal =
        detail::scanHorizontalWhitespace(
            begin,
            cursor_.end);

    if (afterHorizontal != begin) {
      const std::size_t consumed =
          static_cast<std::size_t>(
              afterHorizontal - begin);

      cursor_.current = afterHorizontal;
      cursor_.offset += consumed;
      cursor_.column += consumed;
      continue;
    }

    const char value = *cursor_.current;

    if (value == '\n' ||
        value == '\r') {
      if (options_.emitNewlines) {
        return;
      }

      consumeChar();
      continue;
    }

    return;
  }
}

void Lexer::skipLineComment() {
  const char* begin =
      cursor_.current;

  const char* end =
      scanUntilLineBreak(
          begin,
          cursor_.end);

  const std::size_t consumed =
      static_cast<std::size_t>(
          end - begin);

  cursor_.current = end;
  cursor_.offset += consumed;
  cursor_.column += consumed;
}

void Lexer::skipBlockComment() {
  const SourceLocation start = location();

  consumeChar();
  consumeChar();

  unsigned depth = 1;

  while (!atEnd()) {
    if (peekChar() == '/' &&
        peekChar(1) == '*') {
      consumeChar();
      consumeChar();
      ++depth;
      continue;
    }

    if (peekChar() == '*' &&
        peekChar(1) == '/') {
      consumeChar();
      consumeChar();

      --depth;

      if (depth == 0) {
        return;
      }

      continue;
    }

    consumeChar();
  }

  addDiagnostic(
      DiagnosticSeverity::Error,
      start,
      "unterminated block comment");

  // EOF is already the safest synchronization point for an unterminated
  // block comment. The important recovery property is that the lexer returns
  // a comment token instead of throwing or entering another scan loop.
}

void Lexer::consumeDigits(unsigned base) {
  bool sawDigit = false;
  bool previousWasSeparator = false;
  bool separatorErrorReported = false;

  const auto isDigitForBase = [base](char value) noexcept {
    switch (base) {
      case 2:
        return isBinaryDigit(value);
      case 8:
        return isOctalDigit(value);
      case 10:
        return isDecimalDigit(value);
      case 16:
        return isHexDigit(value);
      default:
        return false;
    }
  };

  while (!atEnd()) {
    const char value = peekChar();

    if (isDigitForBase(value)) {
      consumeChar();

      sawDigit = true;
      previousWasSeparator = false;
      separatorErrorReported = false;

      continue;
    }

    if (value == '_') {
      const char next = peekChar(1);

      const bool separatorIsInvalid =
          !sawDigit ||
          previousWasSeparator ||
          !isDigitForBase(next);

      if (separatorIsInvalid &&
          !separatorErrorReported) {
        addDiagnostic(
            DiagnosticSeverity::Error,
            location(),
            "invalid numeric separator");

        separatorErrorReported = true;
      }

      consumeChar();

      previousWasSeparator = true;

      continue;
    }

    break;
  }

  if (previousWasSeparator &&
      !separatorErrorReported) {
    addDiagnostic(
        DiagnosticSeverity::Error,
        location(),
        "numeric literal cannot end with a separator");
  }
}

bool Lexer::consumeEscapeSequence() {
  if (!consumeIf('\\')) {
    return false;
  }

  if (atEnd()) {
    addDiagnostic(
        DiagnosticSeverity::Error,
        location(),
        "unterminated escape sequence");
    return false;
  }

  const char escape = consumeChar();

  switch (escape) {
    case 'n':
    case 'r':
    case 't':
    case '0':
    case '\\':
    case '"':
    case '\'':
    case 'b':
    case 'f':
    case 'v':
      return true;

    case 'x':
      if (!isHexDigit(peekChar()) ||
          !isHexDigit(peekChar(1))) {
        addDiagnostic(
            DiagnosticSeverity::Error,
            location(),
            "hex escape requires two hexadecimal digits");
        return false;
      }

      consumeChar();
      consumeChar();
      return true;

    case 'u':
      return consumeUnicodeEscape();

    default:
      addDiagnostic(
          DiagnosticSeverity::Warning,
          location(),
          "unknown escape sequence");
      return true;
  }
}

bool Lexer::consumeUnicodeEscape() {
  if (consumeIf('{')) {
    std::uint32_t value = 0;
    unsigned digits = 0;

    while (!atEnd() && peekChar() != '}') {
      if (!isHexDigit(peekChar()) || digits == 6) {
        addDiagnostic(
            DiagnosticSeverity::Error,
            location(),
            "invalid Unicode escape");

        while (!atEnd() && peekChar() != '}') {
          consumeChar();
        }

        break;
      }

      value = value * 16u +
              static_cast<std::uint32_t>(hexValue(consumeChar()));

      ++digits;
    }

    if (!consumeIf('}')) {
      addDiagnostic(
          DiagnosticSeverity::Error,
          location(),
          "unterminated Unicode escape");
      return false;
    }

    if (digits == 0 ||
        !isValidUnicodeScalar(value)) {
      addDiagnostic(
          DiagnosticSeverity::Error,
          location(),
          "Unicode escape is outside the valid scalar range");
      return false;
    }

    return true;
  }

  for (unsigned index = 0; index < 4; ++index) {
    if (!isHexDigit(peekChar())) {
      addDiagnostic(
          DiagnosticSeverity::Error,
          location(),
          "Unicode escape requires four hexadecimal digits");
      return false;
    }

    consumeChar();
  }

  return true;
}

bool Lexer::validateUTF8(
    const char* begin,
    const char* end) const noexcept {
  const auto* current =
      reinterpret_cast<const unsigned char*>(begin);

  const auto* limit =
      reinterpret_cast<const unsigned char*>(end);

  while (current < limit) {
    const unsigned char leading = *current;
    ++current;

    if (leading < 0x80u) {
      continue;
    }

    unsigned continuationCount = 0;
    std::uint32_t value = 0;
    std::uint32_t minimum = 0;

    if ((leading & 0xe0u) == 0xc0u) {
      continuationCount = 1;
      value = leading & 0x1fu;
      minimum = 0x80u;
    } else if ((leading & 0xf0u) == 0xe0u) {
      continuationCount = 2;
      value = leading & 0x0fu;
      minimum = 0x800u;
    } else if ((leading & 0xf8u) == 0xf0u) {
      continuationCount = 3;
      value = leading & 0x07u;
      minimum = 0x10000u;
    } else {
      return false;
    }

    for (unsigned index = 0;
         index < continuationCount;
         ++index) {
      if (current >= limit) {
        return false;
      }

      if (!isContinuationByte(*current)) {
        return false;
      }

      value = (value << 6) |
              (*current & 0x3fu);

      ++current;
    }

    if (value < minimum ||
        value > 0x10ffffu ||
        (value >= 0xd800u && value <= 0xdfffu)) {
      return false;
    }
  }

  return true;
}

// Lex an identifier and classify its keyword spelling.
Token Lexer::lexIdentifierOrKeyword() {
  beginToken();
  consumeIdentifier();

  const std::string_view spelling(
      tokenStart_.current,
      static_cast<std::size_t>(
          cursor_.current - tokenStart_.current));

  if (!validateUTF8(
          tokenStart_.current,
          cursor_.current)) {
    addDiagnostic(
        DiagnosticSeverity::Error,
        {
            tokenStart_.offset,
            tokenStart_.line,
            tokenStart_.column
        },
        "invalid UTF-8 sequence in identifier");

    recoverMalformedToken();

    return finish(TokenKind::Unknown);
  }

  if (afterFunctionKeyword_ &&
      !sawFunctionName_) {
    return finish(TokenKind::Identifier);
  }

  const TokenKind keyword =
      classifyKeyword(spelling);

  if (keyword != TokenKind::Identifier) {
    return finish(keyword);
  }

  return handleIdentifierContext(
      finish(TokenKind::Identifier));
}

// Lex an integer or floating-point literal.
Token Lexer::lexNumber() {
  beginToken();

  bool floating = false;

  if (peekChar() == '.') {
    floating = true;

    consumeChar();
    consumeDigits(10);
  } else if (peekChar() == '0') {
    const char prefix = peekChar(1);

    if ((prefix == 'x' || prefix == 'X') &&
        options_.allowHexInteger) {
      consumeChar();
      consumeChar();

      if (!isHexDigit(peekChar())) {
        addDiagnostic(
            DiagnosticSeverity::Error,
            location(),
            "hexadecimal literal requires hexadecimal digits");

        recoverMalformedToken();
        return finish(TokenKind::Unknown);
      }

      consumeDigits(16);
      return finish(TokenKind::IntegerLiteral);
    }

    if ((prefix == 'b' || prefix == 'B') &&
        options_.allowBinaryInteger) {
      consumeChar();
      consumeChar();

      if (!isBinaryDigit(peekChar())) {
        addDiagnostic(
            DiagnosticSeverity::Error,
            location(),
            "binary literal requires binary digits");

        recoverMalformedToken();
        return finish(TokenKind::Unknown);
      }

      consumeDigits(2);
      return finish(TokenKind::IntegerLiteral);
    }

    if ((prefix == 'o' || prefix == 'O') &&
        options_.allowOctalInteger) {
      consumeChar();
      consumeChar();

      if (!isOctalDigit(peekChar())) {
        addDiagnostic(
            DiagnosticSeverity::Error,
            location(),
            "octal literal requires octal digits");

        recoverMalformedToken();
        return finish(TokenKind::Unknown);
      }

      consumeDigits(8);
      return finish(TokenKind::IntegerLiteral);
    }

    consumeDigits(10);
  } else {
    consumeDigits(10);
  }

  if (peekChar() == '.' &&
      detail::isAsciiDigit(peekChar(1))) {
    floating = true;

    consumeChar();
    consumeDigits(10);
  }

  if (peekChar() == 'e' ||
      peekChar() == 'E') {
    floating = true;

    consumeChar();

    if (peekChar() == '+' ||
        peekChar() == '-') {
      consumeChar();
    }

    if (!isDecimalDigit(peekChar())) {
      addDiagnostic(
          DiagnosticSeverity::Error,
          location(),
          "exponent requires decimal digits");

      recoverMalformedToken();
      return finish(TokenKind::Unknown);
    }

    consumeDigits(10);
  }

  if (floating) {
    return finish(TokenKind::FloatingLiteral);
  }

  return finish(TokenKind::IntegerLiteral);
}

// Lex a string literal with escape recovery.
Token Lexer::lexString() {
  beginToken();

  const SourceLocation start{
      tokenStart_.offset,
      tokenStart_.line,
      tokenStart_.column
  };

  consumeChar();

  bool valid = true;

  while (!atEnd()) {
    if (peekChar() == '"') {
      consumeChar();

      if (valid) {
        return finish(TokenKind::StringLiteral);
      }

      return finish(TokenKind::Unknown);
    }

    if (peekChar() == '\\') {
      if (!consumeEscapeSequence()) {
        valid = false;
        recoverStringLiteral();

        if (peekChar() == '"') {
          consumeChar();
          return finish(TokenKind::Unknown);
        }

        if (peekChar() == '\n' ||
            peekChar() == '\r') {
          return finish(TokenKind::Unknown);
        }
      }

      continue;
    }

    if (peekChar() == '\n' ||
        peekChar() == '\r') {
      addDiagnostic(
          DiagnosticSeverity::Error,
          start,
          "newline is not allowed in a Sift string literal");

      return finish(TokenKind::Unknown);
    }

    consumeChar();
  }

  addDiagnostic(
      DiagnosticSeverity::Error,
      start,
      "unterminated string literal");

  return finish(TokenKind::Unknown);
}

// Lex a character literal with boundary checking.
Token Lexer::lexCharacter() {
  beginToken();

  const SourceLocation start{
      tokenStart_.offset,
      tokenStart_.line,
      tokenStart_.column
  };

  consumeChar();

  if (atEnd()) {
    addDiagnostic(
        DiagnosticSeverity::Error,
        start,
        "unterminated character literal");

    return finish(TokenKind::Unknown);
  }

  if (peekChar() == '\\') {
    if (!consumeEscapeSequence()) {
      recoverCharacterLiteral();

      if (peekChar() == '\'') {
        consumeChar();
      }

      return finish(TokenKind::Unknown);
    }
  } else {
    if (peekChar() == '\n') {
      addDiagnostic(
          DiagnosticSeverity::Error,
          start,
          "newline is not allowed in a character literal");

      return finish(TokenKind::Unknown);
    }

    consumeChar();
  }

  if (!consumeIf('\'')) {
    addDiagnostic(
        DiagnosticSeverity::Error,
        start,
        "character literal must contain exactly one character");

    while (!atEnd() &&
           peekChar() != '\'' &&
           peekChar() != '\n') {
      consumeChar();
    }

    consumeIf('\'');

    return finish(TokenKind::Unknown);
  }

  return finish(TokenKind::CharacterLiteral);
}

// Lex a hash-prefixed directive.
Token Lexer::lexHashOrDirective() {
  beginToken();

  consumeChar();

  if (!isIdentifierStart(peekChar())) {
    return finish(TokenKind::Hash);
  }

  consumeIdentifier();

  const std::string_view spelling(
      tokenStart_.current,
      static_cast<std::size_t>(
          cursor_.current - tokenStart_.current));

  if (spelling == "#if") {
    return finish(TokenKind::KeywordIf);
  }

  if (spelling == "#else") {
    return finish(TokenKind::KeywordElse);
  }

  if (spelling == "#end") {
    return finish(TokenKind::KeywordEnd);
  }

  return finish(TokenKind::Hash);
}

// Lex an at-prefixed directive.
Token Lexer::lexAtOrDirective() {
  beginToken();

  consumeChar();

  if (!isIdentifierStart(peekChar())) {
    return finish(TokenKind::At);
  }

  consumeIdentifier();

  const std::string_view spelling(
      tokenStart_.current,
      static_cast<std::size_t>(
          cursor_.current - tokenStart_.current));

  if (spelling == "@file") {
    return finish(TokenKind::KeywordFile);
  }

  if (spelling == "@fileID") {
    return finish(TokenKind::KeywordFileID);
  }

  if (spelling == "@api") {
    return finish(TokenKind::KeywordAPI);
  }

  if (spelling == "@repo") {
    return finish(TokenKind::KeywordRepo);
  }

  if (spelling == "@webLink") {    return finish(TokenKind::KeywordWebLink);
  }

  if (spelling == "@database") {
    return finish(TokenKind::KeywordDatabase);
  }

  return finish(TokenKind::At);
}

// Distinguish a comment from the slash operator.
Token Lexer::lexCommentOrSlash() {
  beginToken();

  if (detail::isPotentialComment(
          cursor_.current,
          cursor_.end) &&
      peekChar() == '/' &&
      peekChar(1) == '/') {
    consumeChar();
    consumeChar();

    skipLineComment();

    return finish(TokenKind::Comment);
  }

  if (peekChar() == '/' &&
      peekChar(1) == '*') {
    consumeChar();
    consumeChar();

    skipBlockComment();

    return finish(TokenKind::Comment);
  }

  consumeChar();

  if (consumeIf('=')) {
    return finish(TokenKind::SlashEqual);
  }

  return finish(TokenKind::Slash);
}

// Lex punctuation and operators.
Token Lexer::lexOperatorOrPunctuation() {
  beginToken();

  switch (peekChar()) {
    case '(':
      consumeChar();
      return finish(TokenKind::LeftParen);

    case ')':
      consumeChar();
      return finish(TokenKind::RightParen);

    case '{':
      consumeChar();
      return finish(TokenKind::LeftBrace);

    case '}':
      consumeChar();
      return finish(TokenKind::RightBrace);

    case '[':
      consumeChar();
      return finish(TokenKind::LeftBracket);

    case ']':
      consumeChar();
      return finish(TokenKind::RightBracket);

    case ',':
      consumeChar();
      return finish(TokenKind::Comma);

    case '.':
      consumeChar();
      return finish(TokenKind::Dot);

    case ':':
      consumeChar();
      return finish(TokenKind::Colon);

    case ';':
      consumeChar();
      return finish(TokenKind::Semicolon);

    case '?':
      consumeChar();
      return finish(TokenKind::Question);

    // Lex assignment and equality.
    case '=':
      consumeChar();

      if (peekChar() == '=') {
        consumeChar();
        return finish(TokenKind::EqualEqual);
      }

      return finish(TokenKind::Equal);

    // Lex negation and inequality.
    case '!':
      consumeChar();

      if (peekChar() == '=') {
        consumeChar();
        return finish(TokenKind::BangEqual);
      }

      return finish(TokenKind::Bang);

    // Lex addition and compound assignment.
    case '+':
      consumeChar();

      if (peekChar() == '=') {
        consumeChar();
        return finish(TokenKind::PlusEqual);
      }

      if (peekChar() == '+') {
        consumeChar();
        return finish(TokenKind::PlusPlus);
      }

      return finish(TokenKind::Plus);

    // Lex subtraction, decrement, and arrows.
    case '-':
      consumeChar();

      if (peekChar() == '=') {
        consumeChar();
        return finish(TokenKind::MinusEqual);
      }

      if (peekChar() == '-') {
        consumeChar();
        return finish(TokenKind::MinusMinus);
      }

      if (peekChar() == '>') {
        consumeChar();
        return finish(TokenKind::Arrow);
      }

      return finish(TokenKind::Minus);

    // Lex multiplication and compound assignment.
    case '*':
      consumeChar();

      if (consumeIf('=')) {
        return finish(TokenKind::StarEqual);
      }

      return finish(TokenKind::Star);

    // Lex remainder and compound assignment.
    case '%':
      consumeChar();

      if (consumeIf('=')) {
        return finish(TokenKind::PercentEqual);
      }

      return finish(TokenKind::Percent);

    // Lex less-than comparisons.
    case '<':
      consumeChar();

      if (consumeIf('=')) {
        return finish(TokenKind::LessEqual);
      }

      return finish(TokenKind::Less);

    // Lex greater-than comparisons.
    case '>':
      consumeChar();

      if (consumeIf('=')) {
        return finish(TokenKind::GreaterEqual);
      }

      return finish(TokenKind::Greater);

    // Lex logical conjunction.
    case '&':
      consumeChar();

      if (consumeIf('&')) {
        return finish(TokenKind::AndAnd);
      }

      break;

    // Lex logical disjunction.
    case '|':
      consumeChar();

      if (consumeIf('|')) {
        return finish(TokenKind::OrOr);
      }

      break;

    // Lex pattern matching.
    case '~':
      consumeChar();

      if (consumeIf('=')) {
        return finish(TokenKind::TildeEqual);
      }

      return finish(TokenKind::Tilde);

    default:
      break;
  }

  addDiagnostic(
      DiagnosticSeverity::Error,
      {
          tokenStart_.offset,
          tokenStart_.line,
          tokenStart_.column
      },
      "unrecognized Sift character");

  recoverMalformedToken();

  return finish(TokenKind::Unknown);
}

// Apply identifier context to the completed token.
Token Lexer::handleIdentifierContext(Token token) noexcept {
  if (expectingCallingName_ &&
      token.kind == TokenKind::Identifier) {
    token.kind = TokenKind::CallingName;
    expectingCallingName_ = false;
  }

  return token;
}

void Lexer::commitLookaheadDiagnostics() {
  if (lookaheadDiagnostics_.empty()) {
    return;
  }

  diagnostics_.insert(
      diagnostics_.end(),
      lookaheadDiagnostics_.begin(),
      lookaheadDiagnostics_.end());
}

// Update contextual lexer state after token formation.
void Lexer::updateContext(
    TokenKind kind,
    std::string_view text) noexcept {
  (void)text;

  if (kind == TokenKind::KeywordFunction) {
    afterFunctionKeyword_ = true;
    sawFunctionName_ = false;
    expectingCallingName_ = false;
    lastWasDot_ = false;
    return;
  }

  if (afterFunctionKeyword_ &&
      !sawFunctionName_ &&
      kind == TokenKind::Identifier) {
    sawFunctionName_ = true;
    return;
  }

  if (afterFunctionKeyword_ &&
      sawFunctionName_ &&
      kind == TokenKind::LeftParen) {
    expectingCallingName_ = true;
    afterFunctionKeyword_ = false;
    return;
  }

  if (afterFunctionKeyword_ &&
      kind != TokenKind::Comment &&
      kind != TokenKind::Newline) {
    afterFunctionKeyword_ = false;
    sawFunctionName_ = false;
  }

  if (kind == TokenKind::Dot) {
    lastWasDot_ = true;
    return;
  }

  if (lastWasDot_ &&
      kind == TokenKind::Identifier) {
    lastWasDot_ = false;
    return;
  }

  if (kind != TokenKind::Comment &&
      kind != TokenKind::Newline) {
    lastWasDot_ = false;
  }
}

// Save state before speculative lexing.
Lexer::LexState Lexer::saveState() const noexcept {
  return {
      cursor_,
      tokenStart_,
      expectingCallingName_,
      afterFunctionKeyword_,
      sawFunctionName_,
      lastWasDot_
  };
}

// Restore state after speculative lexing.
void Lexer::restoreState(
    const LexState& state) noexcept {
  cursor_ = state.cursor;
  tokenStart_ = state.tokenStart;
  expectingCallingName_ = state.expectingCallingName;
  afterFunctionKeyword_ = state.afterFunctionKeyword;
  sawFunctionName_ = state.sawFunctionName;
  lastWasDot_ = state.lastWasDot;
}

// Dispatch the next source character to its lexer path.
Token Lexer::lexImpl() {
  for (;;) {
    skipWhitespace();

    if (atEnd()) {
      return makeToken(
          TokenKind::EndOfFile,
          cursor_.current,
          cursor_.current,
          location());
    }

    beginToken();

    const char current = peekChar();

    if (current == '\n' ||
        current == '\r') {
      consumeChar();

      if (options_.emitNewlines) {
        return finish(TokenKind::Newline);
      }

      continue;
    }

    // Slash is checked before the general operator path because it can
    // begin either a comment or a slash token. Keeping this branch early
    // avoids entering the larger punctuation/operator switch for comments.
    if (current == '/' &&
        detail::isPotentialComment(
            cursor_.current,
            cursor_.end)) {
      Token token =
          lexCommentOrSlash();

      if (!options_.retainComments) {
        continue;
      }

      return token;
    }

    if (detail::isPotentialIdentifier(current) &&
        isIdentifierStart(current)) {
      Token token = lexIdentifierOrKeyword();
      updateContext(token.kind, token.text);
      return token;
    }

    if (detail::isAsciiDigit(current) ||
        (current == '.' &&
         options_.allowLeadingDotFloat &&
         isDecimalDigit(peekChar(1)))) {
      Token token = lexNumber();
      updateContext(token.kind, token.text);
      return token;
    }

    if (detail::isDoubleQuote(current)) {
      Token token = lexString();
      updateContext(token.kind, token.text);
      return token;
    }

    if (detail::isSingleQuote(current)) {
      Token token = lexCharacter();
      updateContext(token.kind, token.text);
      return token;
    }

    if (detail::isHashLead(current)) {
      Token token = lexHashOrDirective();
      updateContext(token.kind, token.text);
      return token;
    }

    if (detail::isAtLead(current)) {
      Token token = lexAtOrDirective();
      updateContext(token.kind, token.text);
      return token;
    }

    Token token = lexOperatorOrPunctuation();
    updateContext(token.kind, token.text);
    return token;
  }
}

// Consume the next token.
Token Lexer::lex() {
  if (hasLookahead_) {
    Token result = lookahead_;

    commitLookaheadDiagnostics();

    hasLookahead_ = false;
    lookahead_ = {};
    lookaheadDiagnostics_.clear();

    updateContext(
        result.kind,
        result.text);

    return result;
  }

  return lexImpl();
}

// Speculatively lex the next token without committing state.
Token Lexer::peek() {
  if (hasLookahead_) {
    return lookahead_;
  }

  const LexState savedState =
      saveState();

  const std::size_t savedDiagnosticCount =
      diagnostics_.size();

  Token speculativeToken =
      lexImpl();

  if (diagnostics_.size() > savedDiagnosticCount) {
    lookaheadDiagnostics_.assign(
        diagnostics_.begin() +
            static_cast<std::ptrdiff_t>(
                savedDiagnosticCount),
        diagnostics_.end());

    diagnostics_.resize(
        savedDiagnosticCount);
  } else {
    lookaheadDiagnostics_.clear();
  }

  restoreState(savedState);

  lookahead_ = speculativeToken;
  hasLookahead_ = true;

  return lookahead_;
}

// Lex the next identifier as a calling name.
Token Lexer::lexCallingName() {
  Token token = lex();

  if (token.kind == TokenKind::Identifier) {
    token.kind = TokenKind::CallingName;
  }

  return token;
}

} // namespace sift::lexer
