#ifndef SIFT_LEXER_LEXER_OPTIONS_H
#define SIFT_LEXER_LEXER_OPTIONS_H

namespace sift::lexer {

struct LexerOptions {
  bool retainComments = true;
  bool emitNewlines = false;
  bool allowUnicodeIdentifiers = true;
  bool allowLeadingDotFloat = true;
  bool allowBinaryInteger = true;
  bool allowOctalInteger = true;
  bool allowHexInteger = true;
};

} // namespace sift::lexer

#endif
