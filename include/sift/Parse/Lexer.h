#ifndef SIFT_LEXER_LEXER_H
#define SIFT_LEXER_LEXER_H

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>


namespace sift::lexer {

enum class DiagnosticSeverity : std::uint8_t {
  Note,
  Warning,
  Error
};

struct SourceLocation {
  std::size_t offset = 0;
  std::size_t line = 1;
  std::size_t column = 1;
};

struct LexerCursor {
  const char* current = nullptr;
  const char* end = nullptr;
  std::size_t offset = 0;
};

struct LexerOptions {
  bool retainComments = true;
  bool emitNewlines = false;
  bool allowUnicodeIdentifiers = true;
  bool allowLeadingDotFloat = true;
  bool allowBinaryInteger = true;
  bool allowOctalInteger = true;
  bool allowHexInteger = true;
};

struct Diagnostic {
  DiagnosticSeverity severity = DiagnosticSeverity::Error;
  SourceLocation location{};
  std::string_view message{};
};

enum class TokenKind : std::uint16_t {
  EndOfFile,
  Unknown,
  Identifier,
  CallingName,
  IntegerLiteral,
  FloatingLiteral,
  StringLiteral,
  CharacterLiteral,

  KeywordVar, KeywordConst, KeywordFunction, KeywordInit, KeywordDeinit,
  KeywordType, KeywordTypealias, KeywordStruct, KeywordClass, KeywordEnum,
  KeywordProtocol, KeywordExtension, KeywordIf, KeywordElse, KeywordEnd,
  KeywordGuard, KeywordSwitch, KeywordCase, KeywordDefat, KeywordWhile,
  KeywordRepeat, KeywordFor, KeywordLoop, KeywordIn, KeywordDo, KeywordBreak,
  KeywordContinue, KeywordReturn, KeywordDefer, KeywordCall, KeywordAsync,
  KeywordAwait, KeywordWait, KeywordThrows, KeywordThrow, KeywordTry,
  KeywordCatch, KeywordRethrow, KeywordTask, KeywordTrue, KeywordFalse,
  KeywordSelf, KeywordSome, KeywordAny, KeywordInt, KeywordNum, KeywordString,
  KeywordBool, KeywordBytes, KeywordPublic, KeywordPrivate, KeywordProtect,
  KeywordStatic, KeywordFinal, KeywordOpen, KeywordOverRide, KeywordRequired,
  KeywordImport, KeywordExport, KeywordModule, KeywordPackage, KeywordFile,
  KeywordFileID, KeywordAPI, KeywordRepo, KeywordWebLink, KeywordDatabase,
  KeywordMessage, KeywordError, KeywordMath, KeywordAbs, KeywordMin,
  KeywordMax, KeywordDecrease, KeywordIncrease, KeywordSection, KeywordData,
  KeywordGetData, KeywordCreateData, KeywordControl, KeywordConnect,
  KeywordBackup, KeywordBinary, KeywordKernel, KeywordOS, KeywordOutput,
  KeywordDelete, KeywordDestroy, KeywordPanic,

  LeftParen, RightParen, LeftBrace, RightBrace, LeftBracket, RightBracket,
  Comma, Dot, Colon, Semicolon, Question, At, Hash,

  Equal, EqualEqual, Bang, BangEqual, Plus, PlusEqual, Minus, MinusEqual,
  Star, StarEqual, Slash, SlashEqual, Percent, PercentEqual, Less, LessEqual,
  Greater, GreaterEqual, AndAnd, OrOr, Arrow, Tilde, TildeEqual,
  PlusPlus, MinusMinus,

  Comment,
  Newline
};

struct Token {
  TokenKind kind = TokenKind::Unknown;
  const char* source = nullptr;
  std::uint32_t start = 0;
  std::uint32_t length = 0;

  constexpr bool is(TokenKind expected) const noexcept {
    return kind == expected;
  }

  constexpr bool isIdentifier() const noexcept {
    return kind == TokenKind::Identifier ||
           kind == TokenKind::CallingName;
  }

  constexpr bool isLiteral() const noexcept {
    switch (kind) {
      case TokenKind::IntegerLiteral:
      case TokenKind::FloatingLiteral:
      case TokenKind::StringLiteral:
      case TokenKind::CharacterLiteral:
      case TokenKind::KeywordTrue:
      case TokenKind::KeywordFalse:
        return true;
      default:
        return false;
    }
  }

  std::string_view text() const noexcept {
    if (source == nullptr) {
      return {};
    }

    return {
        source + start,
        length
    };
  }

  std::uint32_t endOffset() const noexcept {
    return start + length;
  }
};


class Lexer final {
public:
  explicit Lexer(std::string_view source, LexerOptions options = {}) noexcept;

  Lexer(const Lexer&) = delete;
  Lexer& operator=(const Lexer&) = delete;
  Lexer(Lexer&&) = delete;
  Lexer& operator=(Lexer&&) = delete;

  Token lex();
  Token peek();
  Token lexCallingName();
  void reset() noexcept;

  bool atEnd() const noexcept;
  std::size_t offset() const noexcept;
  SourceLocation location() const noexcept;

  const std::vector<Diagnostic>& diagnostics() const noexcept;
  bool hasErrors() const noexcept;

  static std::string_view tokenName(TokenKind kind) noexcept;
  static bool isKeyword(std::string_view text) noexcept;
  static TokenKind keywordKind(std::string_view text) noexcept;

private:
  std::string_view source_;
  LexerOptions options_{};
  LexerCursor cursor_{};
  LexerCursor tokenStart_{};

  Token lookahead_{};
  std::size_t lookaheadDiagnosticCount_ = 0;

  bool hasLookahead_ = false;
  bool expectingCallingName_ = false;
  bool afterFunctionKeyword_ = false;
  bool sawFunctionName_ = false;
  bool lastWasDot_ = false;

  struct LexState {
    LexerCursor cursor;
    LexerCursor tokenStart;
    bool expectingCallingName = false;
    bool afterFunctionKeyword = false;
    bool sawFunctionName = false;
    bool lastWasDot = false;
  };

  LexState lookaheadState_{};

  LexState saveState() const noexcept;
  void restoreState(const LexState& state) noexcept;

  std::vector<Diagnostic> diagnostics_;

  inline char peekChar(std::size_t distance = 0) const noexcept {
    const std::size_t remaining =
        static_cast<std::size_t>(cursor_.end - cursor_.current);

    if (distance >= remaining) {
      return '\0';
    }

    return cursor_.current[distance];
  }

  inline char consumeChar() noexcept {
    if (cursor_.current >= cursor_.end) {
      return '\0';
    }

    const char value = *cursor_.current;

    if (value == '\r') {
      ++cursor_.current;
      ++cursor_.offset;

      if (cursor_.current < cursor_.end &&
          *cursor_.current == '\n') {
        ++cursor_.current;
        ++cursor_.offset;
      }

      return '\n';
    }

    ++cursor_.current;
    ++cursor_.offset;

    return value;
  }
  bool consumeIf(char value) noexcept;
  bool consumeIf(std::string_view value) noexcept;

  void beginToken() noexcept;

  Token finish(TokenKind kind) noexcept;

  Token makeToken(
      TokenKind kind,
      const char* begin,
      const char* end) const noexcept;

  void addDiagnostic(
      DiagnosticSeverity severity,
      SourceLocation location,
      std::string_view message);

  void recoverAfterLexicalError(
      bool stopAtLineBreak = true) noexcept;

  void recoverMalformedToken() noexcept;

  void recoverStringLiteral() noexcept;
  void recoverCharacterLiteral() noexcept;

  Token lexImpl();
  Token lexIdentifierOrKeyword();
  Token lexNumber();
  Token lexString();
  Token lexCharacter();
  Token lexHashOrDirective();
  Token lexAtOrDirective();
  Token lexCommentOrSlash();
  Token lexOperatorOrPunctuation();

  void skipWhitespace();
  void skipLineComment();
  void skipBlockComment();

  void consumeIdentifier();
  void consumeUnicodeIdentifier();
  void consumeDigits(unsigned base);

  bool consumeEscapeSequence();
  bool consumeUnicodeEscape();

  bool validateUTF8(
      const char* begin,
      const char* end) const noexcept;

  bool isIdentifierStart(char value) const noexcept;
  bool isIdentifierContinue(char value) const noexcept;
  bool isAsciiIdentifierStart(char value) const noexcept;
  bool isAsciiIdentifierContinue(char value) const noexcept;

  static bool isAsciiSpace(char value) noexcept;
  static bool isDecimalDigit(char value) noexcept;
  static bool isHexDigit(char value) noexcept;
  static bool isBinaryDigit(char value) noexcept;
  static bool isOctalDigit(char value) noexcept;
  static unsigned hexValue(char value) noexcept;

  static TokenKind classifyKeyword(std::string_view text) noexcept;

  Token handleIdentifierContext(Token token) noexcept;

  void updateContext(
      TokenKind kind,
      std::string_view text) noexcept;

  void commitLookaheadDiagnostics() noexcept;
};

} // namespace sift::lexer

#endif
