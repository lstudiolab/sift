#ifndef SIFT_LEXER_TOKEN_H
#define SIFT_LEXER_TOKEN_H

#include <cstddef>
#include <string_view>

#include "sift/lexer/TokenKind.h"
#include "sift/lexer/SourceLocation.h"

namespace sift::lexer {

struct Token {
  TokenKind kind = TokenKind::Unknown;
  std::string_view text{};
  SourceLocation location{};
  std::size_t endOffset = 0;

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
};

} // namespace sift::lexer

#endif
