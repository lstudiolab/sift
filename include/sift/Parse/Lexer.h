#ifndef SIFT_LEXER_LEXER_H
#define SIFT_LEXER_LEXER_H

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "sift/Parse/Diagnostic.h"
#include "sift/Parse/LexerCursor.h"
#include "sift/Parse/LexerOptions.h"
#include "sift/Parse/SourceLocation.h"
#include "sift/Parse/Token.h"
#include "sift/Parse/TokenKind.h"

namespace sift::lexer {

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
  bool consumeIf(char value) noexcept;
  bool consumeIf(std::string_view value) noexcept;

  void beginToken() noexcept;

  Token finish(TokenKind kind) noexcept;

  Token makeToken(
      TokenKind kind,
      const char* begin,
      const char* end,
      SourceLocation location) const noexcept;

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
