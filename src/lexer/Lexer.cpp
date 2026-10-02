#include "sift/lexer/Lexer.h"

#include <cstdint>

namespace sift::lexer {
namespace {

struct KeywordEntry { std::string_view spelling; TokenKind kind; };

#define SIFT_KEYWORDS(X) \
  X("var", KeywordVar) X("const", KeywordConst) X("function", KeywordFunction) \
  X("init", KeywordInit) X("deinit", KeywordDeinit) X("type", KeywordType) \
  X("typealias", KeywordTypealias) X("struct", KeywordStruct) X("class", KeywordClass) \
  X("enum", KeywordEnum) X("protocol", KeywordProtocol) X("extension", KeywordExtension) \
  X("if", KeywordIf) X("else", KeywordElse) X("end", KeywordEnd) X("guard", KeywordGuard) \
  X("switch", KeywordSwitch) X("case", KeywordCase) X("defat", KeywordDefat) \
  X("while", KeywordWhile) X("repeat", KeywordRepeat) X("for", KeywordFor) \
  X("loop", KeywordLoop) X("in", KeywordIn) X("do", KeywordDo) X("break", KeywordBreak) \
  X("continue", KeywordContinue) X("return", KeywordReturn) X("defer", KeywordDefer) \
  X("call", KeywordCall) X("async", KeywordAsync) X("await", KeywordAwait) \
  X("wait", KeywordWait) X("throws", KeywordThrows) X("throw", KeywordThrow) \
  X("try", KeywordTry) X("catch", KeywordCatch) X("rethrow", KeywordRethrow) \
  X("task", KeywordTask) X("true", KeywordTrue) X("false", KeywordFalse) \
  X("self", KeywordSelf) X("some", KeywordSome) X("any", KeywordAny) X("int", KeywordInt) \
  X("num", KeywordNum) X("string", KeywordString) X("bool", KeywordBool) X("bytes", KeywordBytes) \
  X("public", KeywordPublic) X("private", KeywordPrivate) X("protect", KeywordProtect) \
  X("static", KeywordStatic) X("final", KeywordFinal) X("open", KeywordOpen) \
  X("overRide", KeywordOverRide) X("required", KeywordRequired) X("import", KeywordImport) \
  X("export", KeywordExport) X("module", KeywordModule) X("package", KeywordPackage) \
  X("file", KeywordFile) X("@file", KeywordFile) X("@fileID", KeywordFileID) \
  X("@api", KeywordAPI) X("@repo", KeywordRepo) X("@webLink", KeywordWebLink) \
  X("@database", KeywordDatabase) X("message", KeywordMessage) X("error", KeywordError) \
  X("math", KeywordMath) X("abs", KeywordAbs) X("min", KeywordMin) X("max", KeywordMax) \
  X("decrease", KeywordDecrease) X("increase", KeywordIncrease) X("section", KeywordSection) \
  X("data", KeywordData) X("getData", KeywordGetData) X("createData", KeywordCreateData) \
  X("control", KeywordControl) X("connect", KeywordConnect) X("backup", KeywordBackup) \
  X("binary", KeywordBinary) X("kernel", KeywordKernel) X("os", KeywordOS) \
  X("output", KeywordOutput) X("delete", KeywordDelete) X("destroy", KeywordDestroy) \
  X("panic", KeywordPanic)

constexpr std::uint64_t hash(std::string_view s) noexcept {
  std::uint64_t h = 14695981039346656037ull;
  for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
  return h;
}

constexpr KeywordEntry Keywords[] = {
#define K(s, k) {s, TokenKind::k},
  SIFT_KEYWORDS(K)
#undef K
};

constexpr std::uint64_t KeywordHashes[] = {
#define K(s, k) hash(s),
  SIFT_KEYWORDS(K)
#undef K
};

constexpr std::size_t KeywordCount = sizeof(Keywords) / sizeof(Keywords[0]);

constexpr bool continuation(unsigned char c) noexcept {
  return (c & 0xC0u) == 0x80u;
}

constexpr bool letter(char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

} // namespace

Lexer::Lexer(std::string_view source, LexerOptions options) noexcept
    : source_(source), options_(options) {
  cursor_.current = source.data();
  cursor_.end = source.data() + source.size();
  tokenStart_ = cursor_;
}

void Lexer::reset() noexcept {
  cursor_.current = source_.data();
  cursor_.end = source_.data() + source_.size();
  cursor_.offset = cursor_.line = 0;
  cursor_.line = 1;
  cursor_.column = 1;
  tokenStart_ = cursor_;
  lookahead_ = {};
  hasLookahead_ = false;
  expectingCallingName_ = false;
  afterFunctionKeyword_ = false;
  sawFunctionName_ = false;
  lastWasDot_ = false;
  diagnostics_.clear();
}

bool Lexer::atEnd() const noexcept { return cursor_.current >= cursor_.end; }
std::size_t Lexer::offset() const noexcept { return cursor_.offset; }
SourceLocation Lexer::location() const noexcept {
  return {cursor_.offset, cursor_.line, cursor_.column};
}

const std::vector<Diagnostic>& Lexer::diagnostics() const noexcept {
  return diagnostics_;
}

bool Lexer::hasErrors() const noexcept {
  for (const auto& d : diagnostics_)
    if (d.severity == DiagnosticSeverity::Error) return true;
  return false;
}

char Lexer::peekChar(std::size_t distance) const noexcept {
  const char* p = cursor_.current + distance;
  return p < cursor_.end ? *p : '\0';
}

char Lexer::consumeChar() noexcept {
  if (atEnd()) return '\0';
  const char c = *cursor_.current++;
  ++cursor_.offset;
  if (c == '\n') { ++cursor_.line; cursor_.column = 1; }
  else ++cursor_.column;
  return c;
}

bool Lexer::consumeIf(char value) noexcept {
  if (peekChar() != value) return false;
  consumeChar();
  return true;
}

bool Lexer::consumeIf(std::string_view value) noexcept {
  if (static_cast<std::size_t>(cursor_.end - cursor_.current) < value.size())
    return false;
  for (std::size_t i = 0; i < value.size(); ++i)
    if (cursor_.current[i] != value[i]) return false;
  for (std::size_t i = 0; i < value.size(); ++i) consumeChar();
  return true;
}

void Lexer::beginToken() noexcept { tokenStart_ = cursor_; }

Token Lexer::makeToken(TokenKind kind, const char* begin, const char* end,
                       SourceLocation location) const noexcept {
  return {kind, std::string_view(begin, static_cast<std::size_t>(end - begin)),
          location, static_cast<std::size_t>(end - source_.data())};
}

Token Lexer::finish(TokenKind kind) noexcept {
  return makeToken(kind, tokenStart_.current, cursor_.current,
                   {tokenStart_.offset, tokenStart_.line, tokenStart_.column});
}

void Lexer::addDiagnostic(DiagnosticSeverity severity, SourceLocation where,
                          std::string_view message) {
  diagnostics_.push_back({severity, where, message});
}

bool Lexer::isAsciiSpace(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\v' || c == '\f' ||
         c == '\r' || c == '\n';
}

bool Lexer::isDecimalDigit(char c) noexcept { return c >= '0' && c <= '9'; }

bool Lexer::isHexDigit(char c) noexcept {
  return isDecimalDigit(c) || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

bool Lexer::isBinaryDigit(char c) noexcept { return c == '0' || c == '1'; }
bool Lexer::isOctalDigit(char c) noexcept { return c >= '0' && c <= '7'; }

unsigned Lexer::hexValue(char c) noexcept {
  if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
  return static_cast<unsigned>(c - 'A' + 10);
}

std::uint64_t Lexer::keywordHash(std::string_view text) noexcept {
  return hash(text);
}

TokenKind Lexer::classifyKeyword(std::string_view text) noexcept {
  const auto h = keywordHash(text);
  for (std::size_t i = 0; i < KeywordCount; ++i) {
    if (KeywordHashes[i] == h && Keywords[i].spelling == text)
      return Keywords[i].kind;
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
#define N(k) case TokenKind::k: return #k;
    N(EndOfFile) N(Unknown) N(Identifier) N(CallingName)
    N(IntegerLiteral) N(FloatingLiteral) N(StringLiteral) N(CharacterLiteral)
    N(KeywordVar) N(KeywordConst) N(KeywordFunction) N(KeywordInit) N(KeywordDeinit)
    N(KeywordType) N(KeywordTypealias) N(KeywordStruct) N(KeywordClass) N(KeywordEnum)
    N(KeywordProtocol) N(KeywordExtension) N(KeywordIf) N(KeywordElse) N(KeywordEnd)
    N(KeywordGuard) N(KeywordSwitch) N(KeywordCase) N(KeywordDefat) N(KeywordWhile)
    N(KeywordRepeat) N(KeywordFor) N(KeywordLoop) N(KeywordIn) N(KeywordDo)
    N(KeywordBreak) N(KeywordContinue) N(KeywordReturn) N(KeywordDefer)
    N(KeywordCall) N(KeywordAsync) N(KeywordAwait) N(KeywordWait) N(KeywordThrows)
    N(KeywordThrow) N(KeywordTry) N(KeywordCatch) N(KeywordRethrow) N(KeywordTask)
    N(KeywordTrue) N(KeywordFalse) N(KeywordSelf) N(KeywordSome) N(KeywordAny)
    N(KeywordInt) N(KeywordNum) N(KeywordString) N(KeywordBool) N(KeywordBytes)
    N(KeywordPublic) N(KeywordPrivate) N(KeywordProtect) N(KeywordStatic) N(KeywordFinal)
    N(KeywordOpen) N(KeywordOverRide) N(KeywordRequired) N(KeywordImport) N(KeywordExport)
    N(KeywordModule) N(KeywordPackage) N(KeywordFile) N(KeywordFileID) N(KeywordAPI)
    N(KeywordRepo) N(KeywordWebLink) N(KeywordDatabase) N(KeywordMessage) N(KeywordError)
    N(KeywordMath) N(KeywordAbs) N(KeywordMin) N(KeywordMax) N(KeywordDecrease)
    N(KeywordIncrease) N(KeywordSection) N(KeywordData) N(KeywordGetData)
    N(KeywordCreateData) N(KeywordControl) N(KeywordConnect) N(KeywordBackup)
    N(KeywordBinary) N(KeywordKernel) N(KeywordOS) N(KeywordOutput) N(KeywordDelete)
    N(KeywordDestroy) N(KeywordPanic)
    N(LeftParen) N(RightParen) N(LeftBrace) N(RightBrace) N(LeftBracket) N(RightBracket)
    N(Comma) N(Dot) N(Colon) N(Semicolon) N(Question) N(At) N(Hash)
    N(Equal) N(EqualEqual) N(Bang) N(BangEqual) N(Plus) N(PlusEqual) N(Minus)
    N(MinusEqual) N(Star) N(StarEqual) N(Slash) N(SlashEqual) N(Percent)
    N(PercentEqual) N(Less) N(LessEqual) N(Greater) N(GreaterEqual) N(AndAnd)
    N(OrOr) N(Arrow) N(Tilde) N(TildeEqual) N(PlusPlus) N(MinusMinus)
    N(Comment) N(Newline)
#undef N
  }
  return "Unknown";
}

bool Lexer::isAsciiIdentifierStart(char c) const noexcept {
  return letter(c) || c == '_' || c == '$';
}

bool Lexer::isAsciiIdentifierContinue(char c) const noexcept {
  return isAsciiIdentifierStart(c) || isDecimalDigit(c);
}

bool Lexer::isIdentifierStart(char c) const noexcept {
  return isAsciiIdentifierStart(c) ||
         (options_.allowUnicodeIdentifiers &&
          static_cast<unsigned char>(c) >= 0x80u);
}

bool Lexer::isIdentifierContinue(char c) const noexcept {
  return isAsciiIdentifierContinue(c) ||
         (options_.allowUnicodeIdentifiers &&
          static_cast<unsigned char>(c) >= 0x80u);
}

void Lexer::consumeIdentifier() {
  while (!atEnd() && isIdentifierContinue(peekChar())) consumeChar();
}

void Lexer::consumeUnicodeIdentifier() {
  while (!atEnd() && static_cast<unsigned char>(peekChar()) >= 0x80u)
    consumeChar();
}

void Lexer::skipWhitespace() {
  while (!atEnd()) {
    const char c = peekChar();
    if (c == '\n') {
      if (options_.emitNewlines) return;
      consumeChar();
      continue;
    }
    if (!isAsciiSpace(c)) {
      if (static_cast<unsigned char>(c) == 0xEF &&
          static_cast<unsigned char>(peekChar(1)) == 0xBB &&
          static_cast<unsigned char>(peekChar(2)) == 0xBF) {
        consumeChar(); consumeChar(); consumeChar(); continue;
      }
      return;
    }
    consumeChar();
  }
}

void Lexer::skipLineComment() {
  while (!atEnd() && peekChar() != '\n') consumeChar();
}

void Lexer::skipBlockComment() {
  const SourceLocation start = location();
  consumeChar(); consumeChar();
  unsigned depth = 1;

  while (!atEnd()) {
    if (peekChar() == '/' && peekChar(1) == '*') {
      consumeChar(); consumeChar(); ++depth; continue;
    }
    if (peekChar() == '*' && peekChar(1) == '/') {
      consumeChar(); consumeChar();
      if (--depth == 0) return;
      continue;
    }
    consumeChar();
  }

  addDiagnostic(DiagnosticSeverity::Error, start, "unterminated block comment");
}

void Lexer::consumeDigits(unsigned base) {
  bool digit = false;
  while (!atEnd()) {
    const char c = peekChar();
    bool valid = base == 2 ? isBinaryDigit(c) :
                 base == 8 ? isOctalDigit(c) :
                 base == 10 ? isDecimalDigit(c) : isHexDigit(c);

    if (valid) { consumeChar(); digit = true; continue; }

    if (c == '_') {
      if (!digit || !isDecimalDigit(peekChar(1)))
        addDiagnostic(DiagnosticSeverity::Error, location(),
                      "invalid numeric separator");
      consumeChar();
      digit = false;
      continue;
    }
    break;
  }
}

bool Lexer::consumeEscapeSequence() {
  if (!consumeIf('\\')) return false;
  if (atEnd()) {
    addDiagnostic(DiagnosticSeverity::Error, location(),
                  "unterminated escape sequence");
    return false;
  }

  switch (consumeChar()) {
    case 'n': case 'r': case 't': case '0': case '\\':
    case '"': case '\'': case 'b': case 'f': case 'v': return true;
    case 'x':
      if (!isHexDigit(peekChar()) || !isHexDigit(peekChar(1))) {
        addDiagnostic(DiagnosticSeverity::Error, location(),
                      "hex escape requires two hexadecimal digits");
        return false;
      }
      consumeChar(); consumeChar(); return true;
    case 'u': return consumeUnicodeEscape();
    default:
      addDiagnostic(DiagnosticSeverity::Warning, location(),
                    "unknown escape sequence");
      return true;
  }
}

bool Lexer::consumeUnicodeEscape() {
  if (consumeIf('{')) {
    unsigned digits = 0;
    std::uint32_t value = 0;
    while (!atEnd() && peekChar() != '}') {
      if (!isHexDigit(peekChar()) || digits == 6) {
        addDiagnostic(DiagnosticSeverity::Error, location(),
                      "invalid Unicode escape");
        while (!atEnd() && peekChar() != '}') consumeChar();
        break;
      }
      value = value * 16u + hexValue(consumeChar());
      ++digits;
    }
    if (!consumeIf('}')) {
      addDiagnostic(DiagnosticSeverity::Error, location(),
                    "unterminated Unicode escape");
      return false;
    }
    if (!digits || value > 0x10FFFFu ||
        (value >= 0xD800u && value <= 0xDFFFu)) {
      addDiagnostic(DiagnosticSeverity::Error, location(),
                    "Unicode escape is outside the valid scalar range");
      return false;
    }
    return true;
  }

  for (unsigned i = 0; i < 4; ++i) {
    if (!isHexDigit(peekChar())) {
      addDiagnostic(DiagnosticSeverity::Error, location(),
                    "Unicode escape requires four hexadecimal digits");
      return false;
    }
    consumeChar();
  }
  return true;
}

bool Lexer::validateUTF8(const char* begin, const char* end) const noexcept {
  const auto* p = reinterpret_cast<const unsigned char*>(begin);
  const auto* e = reinterpret_cast<const unsigned char*>(end);

  while (p < e) {
    const unsigned char lead = *p++;
    if (lead < 0x80u) continue;

    unsigned count = 0;
    std::uint32_t value = 0;
    std::uint32_t minimum = 0;

    if ((lead & 0xE0u) == 0xC0u) {
      count = 1; value = lead & 0x1Fu; minimum = 0x80u;
    } else if ((lead & 0xF0u) == 0xE0u) {
      count = 2; value = lead & 0x0Fu; minimum = 0x800u;
    } else if ((lead & 0xF8u) == 0xF0u) {
      count = 3; value = lead & 0x07u; minimum = 0x10000u;
    } else {
      return false;
    }

    for (unsigned i = 0; i < count; ++i) {
      if (p >= e || !continuation(*p)) return false;
      value = (value << 6) | (*p++ & 0x3Fu);
    }

    if (value < minimum || value > 0x10FFFFu ||
        (value >= 0xD800u && value <= 0xDFFFu))
      return false;
  }

  return true;
}

Token Lexer::lexIdentifierOrKeyword() {
  beginToken();
  consumeIdentifier();

  const std::string_view spelling(
      tokenStart_.current,
      static_cast<std::size_t>(cursor_.current - tokenStart_.current));

  if (!validateUTF8(tokenStart_.current, cursor_.current)) {
    addDiagnostic(DiagnosticSeverity::Error,
                  {tokenStart_.offset, tokenStart_.line, tokenStart_.column},
                  "invalid UTF-8 sequence in identifier");
    return finish(TokenKind::Unknown);
  }

  // Function names are identifiers even when they overlap a contextual
  // Sift word. This permits declarations such as function math(calc).
  if (afterFunctionKeyword_ && !sawFunctionName_)
    return finish(TokenKind::Identifier);

  const TokenKind keyword = classifyKeyword(spelling);
  if (keyword != TokenKind::Identifier)
    return finish(keyword);

  return handleIdentifierContext(finish(TokenKind::Identifier));
}

Token Lexer::lexNumber() {
  beginToken();
  bool floating = false;

  if (peekChar() == '.') {
    floating = true;
    consumeChar();
    consumeDigits(10);
  } else if (peekChar() == '0') {
    const char prefix = peekChar(1);
    if ((prefix == 'x' || prefix == 'X') && options_.allowHexInteger) {
      consumeChar(); consumeChar();
      if (!isHexDigit(peekChar())) {
        addDiagnostic(DiagnosticSeverity::Error, location(),
                      "hexadecimal literal requires hexadecimal digits");
        return finish(TokenKind::Unknown);
      }
      consumeDigits(16);
      return finish(TokenKind::IntegerLiteral);
    }
    if ((prefix == 'b' || prefix == 'B') && options_.allowBinaryInteger) {
      consumeChar(); consumeChar();
      if (!isBinaryDigit(peekChar())) {
        addDiagnostic(DiagnosticSeverity::Error, location(),
                      "binary literal requires binary digits");
        return finish(TokenKind::Unknown);
      }
      consumeDigits(2);
      return finish(TokenKind::IntegerLiteral);
    }
    if ((prefix == 'o' || prefix == 'O') && options_.allowOctalInteger) {
      consumeChar(); consumeChar();
      if (!isOctalDigit(peekChar())) {
        addDiagnostic(DiagnosticSeverity::Error, location(),
                      "octal literal requires octal digits");
        return finish(TokenKind::Unknown);
      }
      consumeDigits(8);
      return finish(TokenKind::IntegerLiteral);
    }
    consumeDigits(10);
  } else {
    consumeDigits(10);
  }

  if (peekChar() == '.' && isDecimalDigit(peekChar(1))) {
    floating = true;
    consumeChar();
    consumeDigits(10);
  }

  if (peekChar() == 'e' || peekChar() == 'E') {
    floating = true;
    consumeChar();
    if (peekChar() == '+' || peekChar() == '-') consumeChar();
    if (!isDecimalDigit(peekChar())) {
      addDiagnostic(DiagnosticSeverity::Error, location(),
                    "exponent requires decimal digits");
    } else {
      consumeDigits(10);
    }
  }

  return finish(floating ? TokenKind::FloatingLiteral
                         : TokenKind::IntegerLiteral);
}

Token Lexer::lexString() {
  beginToken();
  const SourceLocation start{tokenStart_.offset, tokenStart_.line,
                             tokenStart_.column};
  consumeChar();

  while (!atEnd()) {
    if (peekChar() == '"') {
      consumeChar();
      return finish(TokenKind::StringLiteral);
    }
    if (peekChar() == '\\') {
      consumeEscapeSequence();
      continue;
    }
    if (peekChar() == '\n') {
      addDiagnostic(DiagnosticSeverity::Error, start,
                    "newline is not allowed in a Sift string literal");
      return finish(TokenKind::Unknown);
    }
    consumeChar();
  }

  addDiagnostic(DiagnosticSeverity::Error, start, "unterminated string literal");
  return finish(TokenKind::Unknown);
}

Token Lexer::lexCharacter() {
  beginToken();
  const SourceLocation start{tokenStart_.offset, tokenStart_.line,
                             tokenStart_.column};
  consumeChar();

  if (atEnd()) {
    addDiagnostic(DiagnosticSeverity::Error, start,
                  "unterminated character literal");
    return finish(TokenKind::Unknown);
  }

  if (peekChar() == '\\') consumeEscapeSequence();
  else {
    if (peekChar() == '\n') {
      addDiagnostic(DiagnosticSeverity::Error, start,
                    "newline is not allowed in a character literal");
      return finish(TokenKind::Unknown);
    }
    consumeChar();
  }

  if (!consumeIf('\'')) {
    addDiagnostic(DiagnosticSeverity::Error, start,
                  "character literal must contain exactly one character");
    while (!atEnd() && peekChar() != '\'' && peekChar() != '\n') consumeChar();
    consumeIf('\'');
    return finish(TokenKind::Unknown);
  }

  return finish(TokenKind::CharacterLiteral);
}

Token Lexer::lexHashOrDirective() {
  beginToken();
  consumeChar();
  if (!isIdentifierStart(peekChar())) return finish(TokenKind::Hash);
  consumeIdentifier();

  const auto spelling = std::string_view(
      tokenStart_.current,
      static_cast<std::size_t>(cursor_.current - tokenStart_.current));

  if (spelling == "#if") return finish(TokenKind::KeywordIf);
  if (spelling == "#else") return finish(TokenKind::KeywordElse);
  if (spelling == "#end") return finish(TokenKind::KeywordEnd);
  return finish(TokenKind::Hash);
}

Token Lexer::lexAtOrDirective() {
  beginToken();
  consumeChar();
  if (!isIdentifierStart(peekChar())) return finish(TokenKind::At);
  consumeIdentifier();

  const auto spelling = std::string_view(
      tokenStart_.current,
      static_cast<std::size_t>(cursor_.current - tokenStart_.current));

  if (spelling == "@file") return finish(TokenKind::KeywordFile);
  if (spelling == "@fileID") return finish(TokenKind::KeywordFileID);
  if (spelling == "@api") return finish(TokenKind::KeywordAPI);
  if (spelling == "@repo") return finish(TokenKind::KeywordRepo);
  if (spelling == "@webLink") return finish(TokenKind::KeywordWebLink);
  if (spelling == "@database") return finish(TokenKind::KeywordDatabase);
  return finish(TokenKind::At);
}

Token Lexer::lexCommentOrSlash() {
  beginToken();

  if (peekChar() == '/' && peekChar(1) == '/') {
    consumeChar(); consumeChar(); skipLineComment();
    if (!options_.retainComments) return lexImpl();
    return finish(TokenKind::Comment);
  }

  if (peekChar() == '/' && peekChar(1) == '*') {
    consumeChar(); consumeChar(); skipBlockComment();
    if (!options_.retainComments) return lexImpl();
    return finish(TokenKind::Comment);
  }

  consumeChar();
  if (consumeIf('=')) return finish(TokenKind::SlashEqual);
  return finish(TokenKind::Slash);
}

Token Lexer::lexOperatorOrPunctuation() {
  beginToken();

  switch (peekChar()) {
    case '(': consumeChar(); return finish(TokenKind::LeftParen);
    case ')': consumeChar(); return finish(TokenKind::RightParen);
    case '{': consumeChar(); return finish(TokenKind::LeftBrace);
    case '}': consumeChar(); return finish(TokenKind::RightBrace);
    case '[': consumeChar(); return finish(TokenKind::LeftBracket);
    case ']': consumeChar(); return finish(TokenKind::RightBracket);
    case ',': consumeChar(); return finish(TokenKind::Comma);
    case '.': consumeChar(); return finish(TokenKind::Dot);
    case ':': consumeChar(); return finish(TokenKind::Colon);
    case ';': consumeChar(); return finish(TokenKind::Semicolon);
    case '?': consumeChar(); return finish(TokenKind::Question);
    case '=':
      consumeChar(); return finish(consumeIf('=') ? TokenKind::EqualEqual : TokenKind::Equal);
    case '!':
      consumeChar(); return finish(consumeIf('=') ? TokenKind::BangEqual : TokenKind::Bang);
    case '+':
      consumeChar();
      if (consumeIf('=')) return finish(TokenKind::PlusEqual);
      if (consumeIf('+')) return finish(TokenKind::PlusPlus);
      return finish(TokenKind::Plus);
    case '-':
      consumeChar();
      if (consumeIf('=')) return finish(TokenKind::MinusEqual);
      if (consumeIf('-')) return finish(TokenKind::MinusMinus);
      if (consumeIf('>')) return finish(TokenKind::Arrow);
      return finish(TokenKind::Minus);
    case '*':
      consumeChar(); return finish(consumeIf('=') ? TokenKind::StarEqual : TokenKind::Star);
    case '%':
      consumeChar(); return finish(consumeIf('=') ? TokenKind::PercentEqual : TokenKind::Percent);
    case '<':
      consumeChar(); return finish(consumeIf('=') ? TokenKind::LessEqual : TokenKind::Less);
    case '>':
      consumeChar(); return finish(consumeIf('=') ? TokenKind::GreaterEqual : TokenKind::Greater);
    case '&':
      consumeChar();
      if (consumeIf('&')) return finish(TokenKind::AndAnd);
      break;
    case '|':
      consumeChar();
      if (consumeIf('|')) return finish(TokenKind::OrOr);
      break;
    case '~':
      consumeChar(); return finish(consumeIf('=') ? TokenKind::TildeEqual : TokenKind::Tilde);
    default:
      break;
  }

  addDiagnostic(DiagnosticSeverity::Error,
                {tokenStart_.offset, tokenStart_.line, tokenStart_.column},
                "unrecognized Sift character");
  consumeChar();
  return finish(TokenKind::Unknown);
}

Token Lexer::handleIdentifierContext(Token token) noexcept {
  if (expectingCallingName_ && token.kind == TokenKind::Identifier) {
    token.kind = TokenKind::CallingName;
    expectingCallingName_ = false;
  }
  return token;
}

void Lexer::updateContext(TokenKind kind, std::string_view text) noexcept {
  if (kind == TokenKind::KeywordFunction) {
    afterFunctionKeyword_ = true;
    sawFunctionName_ = false;
    expectingCallingName_ = false;
    lastWasDot_ = false;
    return;
  }

  if (afterFunctionKeyword_ && !sawFunctionName_ &&
      kind == TokenKind::Identifier) {
    sawFunctionName_ = true;
    return;
  }

  if (afterFunctionKeyword_ && sawFunctionName_ &&
      kind == TokenKind::LeftParen) {
    expectingCallingName_ = true;
    afterFunctionKeyword_ = false;
    return;
  }

  if (afterFunctionKeyword_ && kind != TokenKind::Comment &&
      kind != TokenKind::Newline) {
    afterFunctionKeyword_ = false;
    sawFunctionName_ = false;
  }

  if (kind == TokenKind::Dot) {
    lastWasDot_ = true;
    return;
  }

  if (lastWasDot_ && kind == TokenKind::Identifier) {
    lastWasDot_ = false;
    return;
  }

  if (kind != TokenKind::Comment && kind != TokenKind::Newline)
    lastWasDot_ = false;

  (void)text;
}

Token Lexer::lexImpl() {
  for (;;) {
    skipWhitespace();

    if (atEnd())
      return makeToken(TokenKind::EndOfFile, cursor_.current, cursor_.current,
                       location());

    beginToken();
    const char c = peekChar();

    if (c == '\n') {
      consumeChar();
      if (options_.emitNewlines) return finish(TokenKind::Newline);
      continue;
    }

    if (c == '/' && (peekChar(1) == '/' || peekChar(1) == '*')) {
      Token token = lexCommentOrSlash();
      if (!options_.retainComments) continue;
      return token;
    }

    if (isIdentifierStart(c)) {
      Token token = lexIdentifierOrKeyword();
      updateContext(token.kind, token.text);
      return token;
    }

    if (isDecimalDigit(c) ||
        (c == '.' && options_.allowLeadingDotFloat &&
         isDecimalDigit(peekChar(1)))) {
      Token token = lexNumber();
      updateContext(token.kind, token.text);
      return token;
    }

    if (c == '"') {
      Token token = lexString();
      updateContext(token.kind, token.text);
      return token;
    }

    if (c == '\'') {
      Token token = lexCharacter();
      updateContext(token.kind, token.text);
      return token;
    }

    if (c == '#') {
      Token token = lexHashOrDirective();
      updateContext(token.kind, token.text);
      return token;
    }

    if (c == '@') {
      Token token = lexAtOrDirective();
      updateContext(token.kind, token.text);
      return token;
    }

    Token token = lexOperatorOrPunctuation();
    updateContext(token.kind, token.text);
    return token;
  }
}

Token Lexer::lex() {
  if (hasLookahead_) {
    hasLookahead_ = false;
    Token result = lookahead_;
    lookahead_ = {};
    return result;
  }
  return lexImpl();
}

Token Lexer::peek() {
  if (!hasLookahead_) {
    lookahead_ = lexImpl();
    hasLookahead_ = true;
  }
  return lookahead_;
}

Token Lexer::lexCallingName() {
  Token token = lex();
  if (token.kind == TokenKind::Identifier)
    token.kind = TokenKind::CallingName;
  return token;
}

} // namespace sift::lexer
