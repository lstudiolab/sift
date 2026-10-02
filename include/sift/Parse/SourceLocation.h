#ifndef SIFT_LEXER_SOURCE_LOCATION_H
#define SIFT_LEXER_SOURCE_LOCATION_H

#include <cstddef>

namespace sift::lexer {

struct SourceLocation {
  std::size_t offset = 0;
  std::size_t line = 1;
  std::size_t column = 1;
};

} // namespace sift::lexer

#endif
