#ifndef SIFT_LEXER_LEXER_CURSOR_H
#define SIFT_LEXER_LEXER_CURSOR_H

#include <cstddef>

namespace sift::lexer {

struct LexerCursor {
  const char* current = nullptr;
  const char* end = nullptr;
  std::size_t offset = 0;
  std::size_t line = 1;
  std::size_t column = 1;
};

} // namespace sift::lexer

#endif
