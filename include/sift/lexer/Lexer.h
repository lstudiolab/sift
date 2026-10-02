#ifndef SIFT_LEXER_LEXER_H
#define SIFT_LEXER_LEXER_H

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace sift::lexer {

enum class TokenKind : std::uint16_t {
  EndOfFile, Unknown,
  Identifier, CallingName, IntegerLiteral, FloatingLiteral, StringLiteral, CharacterLiteral,
  KeywordVar, KeywordConst, KeywordFunction, KeywordInit, KeywordDeinit, KeywordType,
  KeywordTypealias, KeywordStruct, KeywordClass, KeywordEnum, KeywordProtocol, KeywordExtension,
  KeywordIf, KeywordElse, KeywordEnd, KeywordGuard, KeywordSwitch, KeywordCase, KeywordDefat,
  KeywordWhile, KeywordRepeat, KeywordFor, KeywordLoop, KeywordIn, KeywordDo, KeywordBreak,
  KeywordContinue, KeywordReturn, KeywordDefer,
  KeywordCall, KeywordAsync, KeywordAwait, KeywordWait, KeywordThrows, KeywordThrow, KeywordTry,
  KeywordCatch, KeywordRethrow, KeywordTask,
  KeywordTrue, KeywordFalse, KeywordSelf, KeywordSome, KeywordAny, KeywordInt, KeywordNum,
  KeywordString, KeywordBool, KeywordBytes,
  KeywordPublic, KeywordPrivate, KeywordProtect, KeywordStatic, KeywordFinal, KeywordOpen,
  KeywordOverRide, KeywordRequired, KeywordImport, KeywordExport, KeywordModule, KeywordPackage,
  KeywordFile, KeywordFileID, KeywordAPI, KeywordRepo, KeywordWebLink, KeywordDatabase,
  KeywordMessage, KeywordError, KeywordMath, KeywordAbs, KeywordMin, KeywordMax, KeywordDecrease,
  KeywordIncrease, KeywordSection, KeywordData, KeywordGetData, KeywordCreateData, KeywordControl,
  KeywordConnect, KeywordBackup, KeywordBinary, KeywordKernel, KeywordOS, KeywordOutput,
  KeywordDelete, KeywordDestroy, KeywordPanic,
  LeftParen, RightParen, LeftBrace, RightBrace, LeftBracket, RightBracket, Comma, Dot, Colon,
  Semicolon, Question, At, Hash,
  Equal, EqualEqual, Bang, BangEqual, Plus, PlusEqual, Minus, MinusEqual, Star, StarEqual,
  Slash, SlashEqual, Percent, PercentEqual, Less, LessEqual, Greater, GreaterEqual, AndAnd,
  OrOr, Arrow, Tilde, TildeEqual, PlusPlus, MinusMinus,
  Comment, Newline
};

enum class DiagnosticSeverity : std::uint8_t { Note, Warning, Error };

struct SourceLocation {
  std::size_t offset = 0;
  std::size_t line = 1;
  std::size_t column = 1;
};

struct Diagnostic {
  DiagnosticSeverity severity = DiagnosticSeverity::Error;
  SourceLocation location{};
  std::string_view message{};
};

struct Token {
  TokenKind kind = TokenKind::Unknown;
  std::string_view text{};
  SourceLocation location{};
  std::size_t endOffset = 0;

  constexpr bool is(TokenKind expected) const noexcept { return kind == expected; }
  constexpr bool isIdentifier() const noexcept {
    return kind == TokenKind::Identifier || kind == TokenKind::CallingName;
  }
  constexpr bool isLiteral() const noexcept {
    switch (kind) {
      case TokenKind::IntegerLiteral: case TokenKind::FloatingLiteral:
      case TokenKind::StringLiteral: case TokenKind::CharacterLiteral:
      case TokenKind::KeywordTrue: case TokenKind::KeywordFalse: return true;
      default: return false;
    }
  }
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
  struct Cursor {
    const char* current = nullptr;
    const char* end = nullptr;
    std::size_t offset = 0;
    std::size_t line = 1;
    std::size_t column = 1;
  };

  std::string_view source_;
  LexerOptions options_{};
  Cursor cursor_{};
  Cursor tokenStart_{};
  Token lookahead_{};
  bool hasLookahead_ = false;
  bool expectingCallingName_ = false;
  bool afterFunctionKeyword_ = false;
  bool sawFunctionName_ = false;
  bool lastWasDot_ = false;
  std::vector<Diagnostic> diagnostics_;

  char peekChar(std::size_t distance = 0) const noexcept;
  char consumeChar() noexcept;
  bool consumeIf(char value) noexcept;
  bool consumeIf(std::string_view value) noexcept;
  void beginToken() noexcept;
  Token finish(TokenKind kind) noexcept;
  Token makeToken(TokenKind kind, const char* begin, const char* end,
                  SourceLocation location) const noexcept;
  void addDiagnostic(DiagnosticSeverity severity, SourceLocation location,
                     std::string_view message);

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
  bool validateUTF8(const char* begin, const char* end) const noexcept;

  bool isIdentifierStart(char c) const noexcept;
  bool isIdentifierContinue(char c) const noexcept;
  bool isAsciiIdentifierStart(char c) const noexcept;
  bool isAsciiIdentifierContinue(char c) const noexcept;

  static bool isAsciiSpace(char c) noexcept;
  static bool isDecimalDigit(char c) noexcept;
  static bool isHexDigit(char c) noexcept;
  static bool isBinaryDigit(char c) noexcept;
  static bool isOctalDigit(char c) noexcept;
  static unsigned hexValue(char c) noexcept;
  static std::uint64_t keywordHash(std::string_view text) noexcept;
  static TokenKind classifyKeyword(std::string_view text) noexcept;

  Token handleIdentifierContext(Token token) noexcept;
  void updateContext(TokenKind kind, std::string_view text) noexcept;
};

} // namespace sift::lexer

#endif
