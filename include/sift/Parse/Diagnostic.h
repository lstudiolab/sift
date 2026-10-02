#ifndef SIFT_LEXER_DIAGNOSTIC_H
#define SIFT_LEXER_DIAGNOSTIC_H

#include <cstdint>
#include <string_view>

#include "sift/Parse/SourceLocation.h"

namespace sift::lexer {

enum class DiagnosticSeverity : std::uint8_t {
  Note,
  Warning,
  Error
};

struct Diagnostic {
  DiagnosticSeverity severity = DiagnosticSeverity::Error;
  SourceLocation location{};
  std::string_view message{};
};

} // namespace sift::lexer

#endif
