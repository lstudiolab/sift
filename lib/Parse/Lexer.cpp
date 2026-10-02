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

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace sift::lexer {

namespace {

struct KeywordEntry {
  std::string_view spelling;
  TokenKind kind;
};

constexpr std::array<KeywordEntry, 90> Keywords = {{
  {"var", TokenKind::KeywordVar},
  {"const", TokenKind::KeywordConst},
  {"function", TokenKind::KeywordFunction},
  {"init", TokenKind::KeywordInit},
  {"deinit", TokenKind::KeywordDeinit},
  {"type", TokenKind::KeywordType},
  {"typealias", TokenKind::KeywordTypealias},
  {"struct", TokenKind::KeywordStruct},
  {"class", TokenKind::KeywordClass},
  {"enum", TokenKind::KeywordEnum},
  {"protocol", TokenKind::KeywordProtocol},
  {"extension", TokenKind::KeywordExtension},
  {"if", TokenKind::KeywordIf},
  {"else", TokenKind::KeywordElse},
  {"end", TokenKind::KeywordEnd},
  {"guard", TokenKind::KeywordGuard},
  {"switch", TokenKind::KeywordSwitch},
  {"case", TokenKind::KeywordCase},
  {"defat", TokenKind::KeywordDefat},
  {"while", TokenKind::KeywordWhile},
  {"repeat", TokenKind::KeywordRepeat},
  {"for", TokenKind::KeywordFor},
  {"loop", TokenKind::KeywordLoop},
  {"in", TokenKind::KeywordIn},
  {"do", TokenKind::KeywordDo},
  {"break", TokenKind::KeywordBreak},
  {"continue", TokenKind::KeywordContinue},
  {"return", TokenKind::KeywordReturn},
  {"defer", TokenKind::KeywordDefer},
  {"call", TokenKind::KeywordCall},
  {"async", TokenKind::KeywordAsync},
  {"await", TokenKind::KeywordAwait},
  {"wait", TokenKind::KeywordWait},
  {"throws", TokenKind::KeywordThrows},
  {"throw", TokenKind::KeywordThrow},
  {"try", TokenKind::KeywordTry},
  {"catch", TokenKind::KeywordCatch},
  {"rethrow", TokenKind::KeywordRethrow},
  {"task", TokenKind::KeywordTask},
  {"true", TokenKind::KeywordTrue},
  {"false", TokenKind::KeywordFalse},
  {"self", TokenKind::KeywordSelf},
  {"some", TokenKind::KeywordSome},
  {"any", TokenKind::KeywordAny},
  {"int", TokenKind::KeywordInt},
  {"num", TokenKind::KeywordNum},
  {"string", TokenKind::KeywordString},
  {"bool", TokenKind::KeywordBool},
  {"bytes", TokenKind::KeywordBytes},
  {"public", TokenKind::KeywordPublic},
  {"private", TokenKind::KeywordPrivate},
  {"protect", TokenKind::KeywordProtect},
  {"static", TokenKind::KeywordStatic},
  {"final", TokenKind::KeywordFinal},
  {"open", TokenKind::KeywordOpen},
  {"overRide", TokenKind::KeywordOverRide},
  {"required", TokenKind::KeywordRequired},
  {"import", TokenKind::KeywordImport},
  {"export", TokenKind::KeywordExport},
  {"module", TokenKind::KeywordModule},
  {"package", TokenKind::KeywordPackage},
  {"file", TokenKind::KeywordFile},
  {"@file", TokenKind::KeywordFile},
  {"@fileID", TokenKind::KeywordFileID},
  {"@api", TokenKind::KeywordAPI},
  {"@repo", TokenKind::KeywordRepo},
  {"@webLink", TokenKind::KeywordWebLink},
  {"@database", TokenKind::KeywordDatabase},
  {"message", TokenKind::KeywordMessage},
  {"error", TokenKind::KeywordError},
  {"math", TokenKind::KeywordMath},
  {"abs", TokenKind::KeywordAbs},
  {"min", TokenKind::KeywordMin},
  {"max", TokenKind::KeywordMax},
  {"decrease", TokenKind::KeywordDecrease},
  {"increase", TokenKind::KeywordIncrease},
  {"section", TokenKind::KeywordSection},
  {"data", TokenKind::KeywordData},
  {"getData", TokenKind::KeywordGetData},
  {"createData", TokenKind::KeywordCreateData},
  {"control", TokenKind::KeywordControl},
  {"connect", TokenKind::KeywordConnect},
  {"backup", TokenKind::KeywordBackup},
  {"binary", TokenKind::KeywordBinary},
  {"kernel", TokenKind::KeywordKernel},
  {"os", TokenKind::KeywordOS},
  {"output", TokenKind::KeywordOutput},
  {"delete", TokenKind::KeywordDelete},
  {"destroy", TokenKind::KeywordDestroy},
  {"panic", TokenKind::KeywordPanic}
}};

struct KeywordBuckets {
  std::array<int, 256> heads{};
  std::array<int, Keywords.size()> next{};

  KeywordBuckets() noexcept {
    heads.fill(-1);
    next.fill(-1);

    for (std::size_t index = 0; index < Keywords.size(); ++index) {
      const unsigned char first =
          static_cast<unsigned char>(Keywords[index].spelling.front());

      next[index] = heads[first];
      heads[first] = static_cast<int>(index);
    }
  }
};

const KeywordBuckets KeywordIndex{};

static_assert(Keywords.size() == 90, "Sift keyword table changed without updating its declared size.");

// Check whether a byte continues a UTF-8 scalar.
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

constexpr std::array<unsigned char, 256> CharacterClassTable = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 16, 16, 16, 16, 16, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    16, 32, 0, 0, 12, 32, 32, 0, 64, 64, 32, 32, 64, 32, 64, 32,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 64, 64, 32, 32, 32, 64,
    0, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13,
    13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 64, 0, 64, 0, 12,
    0, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13,
    13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 64, 32, 64, 32, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

constexpr bool hasCharacterClass(char value, unsigned char characterClass) noexcept {
  return (CharacterClassTable[static_cast<unsigned char>(value)] & characterClass) != 0;
}

constexpr unsigned char byteOf(char value) noexcept {
  return static_cast<unsigned char>(value);
}

constexpr bool isAsciiLetter(char value) noexcept {
  return (value >= 'a' && value <= 'z') ||
         (value >= 'A' && value <= 'Z');
}

constexpr bool isAsciiDigit(char value) noexcept {
  return hasCharacterClass(value, 2u);
}

constexpr bool isAsciiIdentifierStart(char value) noexcept {
  return hasCharacterClass(value, 4u);
}

// Check that enough source bytes remain before reading ahead.
inline bool hasBytes(
    const char* current,
    const char* end,
    std::size_t count) noexcept {
  if (current > end) {
    return false;
  }

  return static_cast<std::size_t>(end - current) >= count;
}

// Detect a UTF-8 byte-order mark at the source start.
inline bool startsBOM(
    const char* current,
    const char* end) noexcept {
  return hasBytes(current, end, 3u) &&
         byteOf(current[0]) == 0xefu &&
         byteOf(current[1]) == 0xbbu &&
         byteOf(current[2]) == 0xbfu;
}

// Check whether the current slash can begin a comment.
inline bool isPotentialComment(
    const char* current,
    const char* end) noexcept {
  return hasBytes(current, end, 2u) &&
         current[0] == '/' &&
         (current[1] == '/' || current[1] == '*');
}

// Check whether a character can begin an identifier.
inline bool isPotentialIdentifier(
    char current) noexcept {
  return isAsciiIdentifierStart(current) ||
         byteOf(current) >= 0x80u;
}

// Check for horizontal whitespace handled by the fast path.
inline bool isHorizontalWhitespace(char value) noexcept {
  return hasCharacterClass(value, 16u) && value != '\r' && value != '\n';
}


// Check whether a byte can begin an operator.
inline bool isOperatorLeadByte(char value) noexcept {
  return hasCharacterClass(value, 32u);
}

// Check whether a byte can begin punctuation.
inline bool isPunctuationLeadByte(char value) noexcept {
  return hasCharacterClass(value, 64u);
}

// Recognize a string literal delimiter.
inline bool isDoubleQuote(char value) noexcept {
  return value == '"';
}

// Recognize a character literal delimiter.
inline bool isSingleQuote(char value) noexcept {
  return value == '\'';
}

// Recognize a hash-prefixed directive.
inline bool isHashLead(char value) noexcept {
  return value == '#';
}

// Recognize an at-prefixed directive.
inline bool isAtLead(char value) noexcept {
  return value == '@';
}

// Scan an ASCII identifier using the contiguous source buffer.
inline const char* scanAsciiIdentifier(
    const char* current,
    const char* end) noexcept {
  while (current < end) {
    const unsigned char value =
        static_cast<unsigned char>(*current);

    if (value >= 0x80u) {
      break;
    }

    if (!hasCharacterClass(
            static_cast<char>(value),
            8u)) {
      break;
    }

    ++current;
  }

  return current;
}

// Scan horizontal whitespace without forming tokens.
inline const char* scanHorizontalWhitespace(
    const char* current,
    const char* end) noexcept {
  while (current < end &&
         isHorizontalWhitespace(*current)) {
    ++current;
  }

  return current;
}

// Scan forward to the next line boundary.
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

// Recovery mode selects a safe restart strategy after malformed input.
enum class RecoveryMode : std::uint8_t {
  Line,
  Statement,
  Delimiter,
  Block,
  String,
  Character,
  Comment,
  Directive,
  Number,
  Operator,
  Identifier,
  UTF8
};

// Find a newline synchronization point.
inline const char* recoveryLine(
    const char* current,
    const char* end) noexcept {
  while (current < end) {
    if (*current == '\n' || *current == '\r') {
      return current;
    }
    ++current;
  }
  return end;
}

// Find a statement synchronization point while respecting braces.
inline const char* recoveryStatement(
    const char* current,
    const char* end) noexcept {
  unsigned depth = 0;

  while (current < end) {
    const char value = *current;

    if (value == '{') {
      ++depth;
      ++current;
      continue;
    }

    if (value == '}') {
      if (depth == 0) {
        return current;
      }
      --depth;
      ++current;
      continue;
    }

    if (value == ';' && depth == 0) {
      return current;
    }

    if (value == '\n' || value == '\r') {
      return current;
    }

    ++current;
  }

  return end;
}

// Find a delimiter synchronization point while tracking nesting.
inline const char* recoveryDelimiter(
    const char* current,
    const char* end) noexcept {
  unsigned parentheses = 0;
  unsigned brackets = 0;
  unsigned braces = 0;

  while (current < end) {
    const char value = *current;

    if (value == '(') ++parentheses;
    if (value == '[') ++brackets;
    if (value == '{') ++braces;

    if (value == ')' && parentheses != 0) --parentheses;
    if (value == ']' && brackets != 0) --brackets;
    if (value == '}' && braces != 0) --braces;

    if ((value == ')' || value == ']' || value == '}') &&
        parentheses == 0 &&
        brackets == 0 &&
        braces == 0) {
      return current;
    }

    if (value == ',' &&
        parentheses == 0 &&
        brackets == 0 &&
        braces == 0) {
      return current;
    }

    if (value == '\n' || value == '\r') {
      return current;
    }

    ++current;
  }

  return end;
}

// Find the end of a damaged balanced block.
inline const char* recoveryBlock(
    const char* current,
    const char* end) noexcept {
  unsigned braces = 0;
  unsigned parentheses = 0;
  unsigned brackets = 0;

  while (current < end) {
    const char value = *current;

    if (value == '{') ++braces;
    if (value == '(') ++parentheses;
    if (value == '[') ++brackets;

    if (value == ')' && parentheses != 0) --parentheses;
    if (value == ']' && brackets != 0) --brackets;

    if (value == '}') {
      if (braces == 0) {
        return current;
      }

      --braces;

      if (braces == 0 &&
          parentheses == 0 &&
          brackets == 0) {
        return current + 1;
      }
    }

    ++current;
  }

  return end;
}

// Find a string recovery point without stopping on escaped quotes.
inline const char* recoveryString(
    const char* current,
    const char* end) noexcept {
  bool escaped = false;

  while (current < end) {
    const char value = *current;

    if (escaped) {
      escaped = false;
      ++current;
      continue;
    }

    if (value == '\\') {
      escaped = true;
      ++current;
      continue;
    }

    if (value == '"' ||
        value == '\n' ||
        value == '\r') {
      return current;
    }

    ++current;
  }

  return end;
}

// Find a character recovery point without stopping on escaped quotes.
inline const char* recoveryCharacter(
    const char* current,
    const char* end) noexcept {
  bool escaped = false;

  while (current < end) {
    const char value = *current;

    if (escaped) {
      escaped = false;
      ++current;
      continue;
    }

    if (value == '\\') {
      escaped = true;
      ++current;
      continue;
    }

    if (value == '\'' ||
        value == '\n' ||
        value == '\r') {
      return current;
    }

    ++current;
  }

  return end;
}

// Find the end of a nested block comment.
inline const char* recoveryBlockComment(
    const char* current,
    const char* end) noexcept {
  unsigned depth = 1;

  while (current < end) {
    if (*current == '/' &&
        current + 1 < end &&
        current[1] == '*') {
      ++depth;
      current += 2;
      continue;
    }

    if (*current == '*' &&
        current + 1 < end &&
        current[1] == '/') {
      --depth;
      current += 2;

      if (depth == 0) {
        return current;
      }

      continue;
    }

    ++current;
  }

  return end;
}

// Find the end of a line comment.
inline const char* recoveryLineComment(
    const char* current,
    const char* end) noexcept {
  while (current < end) {
    if (*current == '\n' ||
        *current == '\r') {
      return current;
    }

    ++current;
  }

  return end;
}

// Find a safe restart point after a malformed directive.
inline const char* recoveryDirective(
    const char* current,
    const char* end) noexcept {
  while (current < end) {
    const char value = *current;

    if (value == '@' ||
        value == '#') {
      return current;
    }

    if (value == ';' ||
        value == '{' ||
        value == '}' ||
        value == '\n' ||
        value == '\r') {
      return current;
    }

    if (value == ' ' ||
        value == '\t') {
      return current;
    }

    if (value == '/' &&
        current + 1 < end &&
        (current[1] == '/' ||
         current[1] == '*')) {
      return current;
    }

    ++current;
  }

  return end;
}

// Find the end of a malformed numeric literal.
inline const char* recoveryNumber(
    const char* current,
    const char* end) noexcept {
  bool point = false;
  bool exponent = false;
  bool separator = false;

  while (current < end) {
    const char value = *current;

    if (isAsciiDigitValue(value)) {
      separator = false;
      ++current;
      continue;
    }

    if (value == '_' &&
        !separator) {
      separator = true;
      ++current;
      continue;
    }

    if (value == '.' &&
        !point) {
      point = true;
      ++current;
      continue;
    }

    if ((value == 'e' ||
         value == 'E') &&
        !exponent) {
      exponent = true;
      ++current;

      if (current < end &&
          (*current == '+' ||
           *current == '-')) {
        ++current;
      }

      continue;
    }

    break;
  }

  return current;
}

// Find the end of a malformed operator sequence.
inline const char* recoveryOperator(
    const char* current,
    const char* end) noexcept {
  while (current < end) {
    const char value = *current;

    if (value == '=' ||
        value == '!' ||
        value == '+' ||
        value == '-' ||
        value == '*' ||
        value == '%' ||
        value == '<' ||
        value == '>' ||
        value == '&' ||
        value == '|' ||
        value == '~') {
      ++current;
      continue;
    }

    if (value == '/') {
      if (current + 1 < end &&
          (current[1] == '/' ||
           current[1] == '*')) {
        return current;
      }

      ++current;
      continue;
    }

    return current;
  }

  return end;
}

// Find the end of a malformed identifier sequence.
inline const char* recoveryIdentifier(
    const char* current,
    const char* end) noexcept {
  while (current < end) {
    const char value = *current;

    if (value == ' ' ||
        value == '\t' ||
        value == '\n' ||
        value == '\r' ||
        value == ';' ||
        value == ',' ||
        value == ':' ||
        value == '.' ||
        value == '?' ||
        value == '(' ||
        value == ')' ||
        value == '{' ||
        value == '}' ||
        value == '[' ||
        value == ']' ||
        value == '=' ||
        value == '+' ||
        value == '-') {
      return current;
    }

    ++current;
  }

  return end;
}

// Consume one invalid UTF-8 sequence without stalling.
inline const char* recoveryUTF8(
    const char* current,
    const char* end) noexcept {
  if (current >= end) {
    return end;
  }

  const unsigned char first =
      static_cast<unsigned char>(*current);

  if (first < 0x80u) {
    return current + 1;
  }

  unsigned width = 0;

  if (first >= 0xc2u &&
      first <= 0xdfu) {
    width = 2;
  } else if (first >= 0xe0u &&
             first <= 0xefu) {
    width = 3;
  } else if (first >= 0xf0u &&
             first <= 0xf4u) {
    width = 4;
  } else {
    return current + 1;
  }

  ++current;

  for (unsigned index = 1;
       index < width;
       ++index) {
    if (current >= end) {
      return current;
    }

    if (!isContinuationByte(
            static_cast<unsigned char>(*current))) {
      return current;
    }

    ++current;
  }

  return current;
}

// Choose the recovery family from the malformed token's first byte.
inline RecoveryMode chooseRecoveryMode(
    char value) noexcept {
  if (value == '"') {
    return RecoveryMode::String;
  }

  if (value == '\'') {
    return RecoveryMode::Character;
  }

  if (value == '/') {
    return RecoveryMode::Comment;
  }

  if (value == '@' ||
      value == '#') {
    return RecoveryMode::Directive;
  }

  if (isAsciiDigitValue(value)) {
    return RecoveryMode::Number;
  }

  if (value == '=' ||
      value == '!' ||
      value == '+' ||
      value == '-' ||
      value == '*' ||
      value == '%' ||
      value == '<' ||
      value == '>' ||
      value == '&' ||
      value == '|' ||
      value == '~') {
    return RecoveryMode::Operator;
  }

  if (isAsciiLetter(value) ||
      value == '_' ||
      value == '$') {
    return RecoveryMode::Identifier;
  }

  if (value == '(' ||
      value == ')' ||
      value == '[' ||
      value == ']' ||
      value == '{' ||
      value == '}') {
    return RecoveryMode::Delimiter;
  }

  if (static_cast<unsigned char>(value) >= 0x80u) {
    return RecoveryMode::UTF8;
  }

  return RecoveryMode::Line;
}

// Select the correct recovery scanner.
inline const char* chooseRecoveryTarget(
    const char* current,
    const char* end,
    RecoveryMode mode) noexcept {
  switch (mode) {
    case RecoveryMode::Line:
      return recoveryLine(current, end);
    case RecoveryMode::Statement:
      return recoveryStatement(current, end);
    case RecoveryMode::Delimiter:
      return recoveryDelimiter(current, end);
    case RecoveryMode::Block:
      return recoveryBlock(current, end);
    case RecoveryMode::String:
      return recoveryString(current, end);
    case RecoveryMode::Character:
      return recoveryCharacter(current, end);
    case RecoveryMode::Comment:
      if (current + 1 < end &&
          current[1] == '*') {
        return recoveryBlockComment(
            current + 2,
            end);
      }
      return recoveryLineComment(current, end);
    case RecoveryMode::Directive:
      return recoveryDirective(current, end);
    case RecoveryMode::Number:
      return recoveryNumber(current, end);
    case RecoveryMode::Operator:
      return recoveryOperator(current, end);
    case RecoveryMode::Identifier:
      return recoveryIdentifier(current, end);
    case RecoveryMode::UTF8:
      return recoveryUTF8(current, end);
  }

  return current;
}

// Consume a recovery range while preserving line and column accounting.
inline bool consumeRecoveryRange(
    LexerCursor& cursor,
    const char* target) noexcept {
  if (cursor.current >= target) {
    return false;
  }

  while (cursor.current < target) {
    const char value = *cursor.current;

    if (value == '\r') {
      ++cursor.current;
      ++cursor.offset;

      if (cursor.current < target &&
          *cursor.current == '\n') {
        ++cursor.current;
        ++cursor.offset;
      }

      ++cursor.line;
      cursor.column = 1;
      continue;
    }

    ++cursor.current;
    ++cursor.offset;

    if (value == '\n') {
      ++cursor.line;
      cursor.column = 1;
    } else {
      ++cursor.column;
    }
  }

  return true;
}

// Force one-byte progress when a structured recovery pass cannot advance.
inline bool forceRecoveryProgress(
    LexerCursor& cursor) noexcept {
  if (cursor.current >= cursor.end) {
    return false;
  }

  const char value = *cursor.current;

  if (value == '\r') {
    ++cursor.current;
    ++cursor.offset;

    if (cursor.current < cursor.end &&
        *cursor.current == '\n') {
      ++cursor.current;
      ++cursor.offset;
    }

    ++cursor.line;
    cursor.column = 1;
    return true;
  }

  ++cursor.current;
  ++cursor.offset;

  if (value == '\n') {
    ++cursor.line;
    cursor.column = 1;
  } else {
    ++cursor.column;
  }

  return true;
}

// Broaden recovery from a token-local boundary to a statement boundary.
inline RecoveryMode broadenRecovery(
    RecoveryMode mode) noexcept {
  switch (mode) {
    case RecoveryMode::String:
    case RecoveryMode::Character:
      return RecoveryMode::Line;
    case RecoveryMode::Comment:
    case RecoveryMode::Directive:
    case RecoveryMode::Number:
    case RecoveryMode::Identifier:
      return RecoveryMode::Statement;
    case RecoveryMode::Operator:
    case RecoveryMode::Delimiter:
      return RecoveryMode::Block;
    case RecoveryMode::UTF8:
      return RecoveryMode::Line;
    case RecoveryMode::Line:
      return RecoveryMode::Statement;
    case RecoveryMode::Statement:
      return RecoveryMode::Block;
    case RecoveryMode::Block:
      return RecoveryMode::Line;
  }

  return RecoveryMode::Line;
}

// Run bounded recovery attempts before using the final escape hatch.
inline bool runRecoveryBackDoors(
    LexerCursor& cursor,
    RecoveryMode mode) noexcept {
  RecoveryMode current = mode;

  for (unsigned attempt = 0;
       attempt < 6;
       ++attempt) {
    const char* start = cursor.current;
    const char* target =
        chooseRecoveryTarget(
            start,
            cursor.end,
            current);

    if (consumeRecoveryRange(
            cursor,
            target)) {
      return true;
    }

    current =
        broadenRecovery(
            current);
  }

  return forceRecoveryProgress(cursor);
}

// Recover malformed source while guaranteeing forward progress.
inline bool executeRecoveryBackDoor(
    LexerCursor& cursor,
    RecoveryMode mode) noexcept {
  const char* start = cursor.current;

  if (runRecoveryBackDoors(
          cursor,
          mode)) {
    return true;
  }

  if (cursor.current != start) {
    return true;
  }

  return forceRecoveryProgress(cursor);
}

} // namespace detail

// Initialize the lexer over the source buffer.
// Initialize the lexer with source storage and scanning options.
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
// Reset all lexer state so scanning can start from the beginning.
void Lexer::reset() noexcept {
  cursor_.current = source_.data();
  cursor_.end = source_.data() + source_.size();
  cursor_.offset = 0;
  cursor_.line = 1;
  cursor_.column = 1;

  tokenStart_ = cursor_;

  lookahead_ = {};
  lookaheadState_ = saveState();
  lookaheadDiagnosticCount_ = 0;
  hasLookahead_ = false;

  expectingCallingName_ = false;
  afterFunctionKeyword_ = false;
  sawFunctionName_ = false;
  lastWasDot_ = false;

  diagnostics_.clear();
}

// Check whether the source cursor reached the end.
bool Lexer::atEnd() const noexcept {
  return cursor_.current >= cursor_.end;
}

std::size_t Lexer::offset() const noexcept {
  return cursor_.offset;
}

// Return the current source location.
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

// Check whether lexing has produced an error diagnostic.
bool Lexer::hasErrors() const noexcept {
  for (const Diagnostic& diagnostic : diagnostics_) {
    if (diagnostic.severity == DiagnosticSeverity::Error) {
      return true;
    }
  }

  return false;
}

// Consume one character only when it matches the expected value.
bool Lexer::consumeIf(char value) noexcept {
  if (peekChar() != value) {
    return false;
  }

  consumeChar();
  return true;
}

// Consume a complete spelling only when it matches the source.
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

  if (length != 0u &&
      std::memcmp(
          current,
          value.data(),
          length) != 0) {
    return false;
  }

  cursor_.current =
      current + length;
  cursor_.offset += length;
  cursor_.column += length;

  return true;
}

// Record the start of the next token.
// Record the source position where the next token begins.
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
// Finish the current token using its recorded source range.
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

// Record a diagnostic while allowing lexing to continue.
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
// Recover from malformed input at a safe scanning boundary.
void Lexer::recoverAfterLexicalError(
    bool stopAtLineBreak) noexcept {
  const char* start = cursor_.current;

  if (atEnd()) {
    return;
  }

  if (stopAtLineBreak &&
      (peekChar() == '\n' ||
       peekChar() == '\r')) {
    return;
  }

  RecoveryMode mode =
      chooseRecoveryMode(
          peekChar());

  if (!executeRecoveryBackDoor(
          cursor_,
          mode) &&
      cursor_.current == start &&
      !atEnd()) {
    forceRecoveryProgress(cursor_);
  }
}

// Recover the current malformed token and continue lexing.
// Skip an invalid token while guaranteeing forward progress.
void Lexer::recoverMalformedToken() noexcept {
  recoverAfterLexicalError(true);
}

// Recover a malformed string up to its delimiter.
// Recover a malformed string up to its delimiter or line end.
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
// Recover a malformed character up to its delimiter or line end.
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

constexpr bool isValidUnicodeScalar(std::uint32_t value) noexcept {
  return value <= 0x10ffffu &&
         !(value >= 0xd800u && value <= 0xdfffu);
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

TokenKind Lexer::classifyKeyword(std::string_view text) noexcept {
  // Sift keywords are a closed part of the grammar.
  // First-byte dispatch narrows the candidate set before exact comparison.
  // No runtime map, hashing pass, or temporary string is used.

  if (text.empty() ||
      text.size() > 16u) {
    return TokenKind::Identifier;
  }

  const unsigned char first =
      static_cast<unsigned char>(text.front());

  int keywordIndex =
      KeywordIndex.heads[first];

  while (keywordIndex >= 0) {
    const KeywordEntry& keyword =
        Keywords[static_cast<std::size_t>(keywordIndex)];

    if (keyword.spelling.size() == text.size() &&
        keyword.spelling == text) {
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

// Determine whether a spelling is a reserved Sift keyword.
bool Lexer::isKeyword(std::string_view text) noexcept {
  return classifyKeyword(text) != TokenKind::Identifier;
}

// Return the stable diagnostic name for a token kind.
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

// Consume an identifier with the ASCII fast path.
void Lexer::consumeIdentifier() {
  const char* begin = cursor_.current;

  // The overwhelmingly common identifier path is ASCII. Scan it with raw
  // pointers so the hot loop does not repeatedly call peekChar(), perform
  // bounds calculations, or update source-location state one byte at a time.
  const char* current =
      detail::scanAsciiIdentifier(
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

// Consume UTF-8 identifier scalars while continuing through following ASCII.
void Lexer::consumeUnicodeIdentifier() {
  while (!atEnd()) {
    const unsigned char first =
        static_cast<unsigned char>(*cursor_.current);

    if (first < 0x80u) {
      if (!isAsciiIdentifierContinue(
              static_cast<char>(first))) {
        break;
      }

      ++cursor_.current;
      ++cursor_.offset;
      ++cursor_.column;
      continue;
    }

    unsigned width = 0;

    if (first >= 0xc2u && first <= 0xdfu) {
      width = 2;
    } else if (first >= 0xe0u && first <= 0xefu) {
      width = 3;
    } else if (first >= 0xf0u && first <= 0xf4u) {
      width = 4;
    } else {
      ++cursor_.current;
      ++cursor_.offset;
      ++cursor_.column;
      continue;
    }

    const std::size_t remaining =
        static_cast<std::size_t>(
            cursor_.end - cursor_.current);

    if (remaining < width) {
      while (!atEnd() &&
             static_cast<unsigned char>(
                 *cursor_.current) >= 0x80u) {
        ++cursor_.current;
        ++cursor_.offset;
        ++cursor_.column;
      }
      break;
    }

    bool valid = true;

    for (unsigned index = 1; index < width; ++index) {
      if (!isContinuationByte(
              static_cast<unsigned char>(
                  cursor_.current[index]))) {
        valid = false;
        break;
      }
    }

    if (!valid) {
      ++cursor_.current;
      ++cursor_.offset;
      ++cursor_.column;
      continue;
    }

    cursor_.current += width;
    cursor_.offset += width;
    cursor_.column += width;
  }
}

// Skip whitespace and normalize supported line endings.
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

// Skip a semicolon line comment.
void Lexer::skipLineComment() {
  const char* begin =
      cursor_.current;

  const char* end =
      detail::scanUntilLineBreak(
          begin,
          cursor_.end);

  const std::size_t consumed =
      static_cast<std::size_t>(
          end - begin);

  cursor_.current = end;
  cursor_.offset += consumed;
  cursor_.column += consumed;
}

// Skip a nested block comment and diagnose unterminated input.
void Lexer::skipBlockComment() {
  const SourceLocation start = location();
  const char* current = cursor_.current + 2;
  const char* end = cursor_.end;
  unsigned depth = 1;

  cursor_.offset += 2;
  cursor_.column += 2;

  while (current < end) {
    const char value = current[0];

    if (value == '\n') {
      ++current;
      ++cursor_.offset;
      ++cursor_.line;
      cursor_.column = 1;
      continue;
    }

    if (value == '\r') {
      ++current;
      ++cursor_.offset;
      if (current < end && current[0] == '\n') {
        ++current;
        ++cursor_.offset;
      }
      ++cursor_.line;
      cursor_.column = 1;
      continue;
    }

    if (value == '/' && current + 1 < end && current[1] == '*') {
      current += 2;
      cursor_.offset += 2;
      cursor_.column += 2;
      ++depth;
      continue;
    }

    if (value == '*' && current + 1 < end && current[1] == '/') {
      current += 2;
      cursor_.offset += 2;
      cursor_.column += 2;
      --depth;
      if (depth == 0) {
        cursor_.current = current;
        return;
      }
      continue;
    }

    const char* run = current;
    do {
      ++current;
    } while (current < end && current[0] != '/' && current[0] != '*' && current[0] != '\n' && current[0] != '\r');

    const std::size_t consumed = static_cast<std::size_t>(current - run);
    cursor_.offset += consumed;
    cursor_.column += consumed;
  }

  cursor_.current = current;
  addDiagnostic(DiagnosticSeverity::Error, start, "unterminated block comment");
}

// Consume digits for an integer or floating-point component.
void Lexer::consumeDigits(unsigned base) {
  const char* begin = cursor_.current;
  const char* current = begin;

  auto isDigitForBase = [base](char value) noexcept {
    switch (base) {
      case 2: return isBinaryDigit(value);
      case 8: return isOctalDigit(value);
      case 10: return isDecimalDigit(value);
      case 16: return isHexDigit(value);
      default: return false;
    }
  };

  bool sawDigit = false;
  bool sawSeparator = false;
  bool invalidSeparator = false;
  std::size_t separatorOffset = 0;

  while (current < cursor_.end) {
    const char value = *current;
    if (isDigitForBase(value)) {
      sawDigit = true;
      ++current;
      continue;
    }
    if (value != '_') {
      break;
    }

    sawSeparator = true;
    const bool previousValid = sawDigit && current > begin && current[-1] != '_';
    const bool nextValid = current + 1 < cursor_.end && isDigitForBase(current[1]);

    if ((!previousValid || !nextValid) && !invalidSeparator) {
      invalidSeparator = true;
      separatorOffset = static_cast<std::size_t>(current - begin);
    }
    ++current;
  }

  const std::size_t consumed = static_cast<std::size_t>(current - begin);
  cursor_.current = current;
  cursor_.offset += consumed;
  cursor_.column += consumed;

  if (invalidSeparator) {
    addDiagnostic(
        DiagnosticSeverity::Error,
        {tokenStart_.offset + separatorOffset,
         tokenStart_.line,
         tokenStart_.column + separatorOffset},
        "invalid numeric separator");
  } else if (sawSeparator && current > begin && current[-1] == '_') {
    addDiagnostic(
        DiagnosticSeverity::Error,
        {cursor_.offset - 1,
         cursor_.line,
         cursor_.column - 1},
        "numeric literal cannot end with a separator");
  }
}

// Consume and validate one string or character escape.
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

// Consume and validate a Unicode escape sequence.
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

// Validate UTF-8 in a source range.
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
        !isValidUnicodeScalar(value)) {
      return false;
    }
  }

  return true;
}

// Lex an identifier and classify its keyword spelling.
// Lex an identifier and classify reserved keyword spellings.
Token Lexer::lexIdentifierOrKeyword() {
  beginToken();
  consumeIdentifier();

  const std::string_view spelling(
      tokenStart_.current,
      static_cast<std::size_t>(
          cursor_.current - tokenStart_.current));

  bool containsNonASCII = false;

  if (options_.allowUnicodeIdentifiers) {
    containsNonASCII = std::any_of(
        tokenStart_.current,
        cursor_.current,
        [](char value) noexcept {
          return static_cast<unsigned char>(value) >= 0x80u;
        });
  }

  if (containsNonASCII &&
      !validateUTF8(
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
// Lex decimal, binary, octal, hexadecimal, and floating literals.
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

      if (isAsciiIdentifierContinue(peekChar())) {
        addDiagnostic(
            DiagnosticSeverity::Error,
            location(),
            "invalid character in hexadecimal literal");

        recoverMalformedToken();
        return finish(TokenKind::Unknown);
      }

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

      if (isAsciiIdentifierContinue(peekChar())) {
        addDiagnostic(
            DiagnosticSeverity::Error,
            location(),
            "invalid character in binary literal");

        recoverMalformedToken();
        return finish(TokenKind::Unknown);
      }

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

      if (isAsciiIdentifierContinue(peekChar())) {
        addDiagnostic(
            DiagnosticSeverity::Error,
            location(),
            "invalid character in octal literal");

        recoverMalformedToken();
        return finish(TokenKind::Unknown);
      }

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
// Lex a string literal with escape and UTF-8 validation.
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

    const char* runStart = cursor_.current;
    const char* runEnd = runStart;
    bool containsNonASCII = false;

    while (runEnd < cursor_.end &&
           *runEnd != '"' &&
           *runEnd != '\\' &&
           *runEnd != '\n' &&
           *runEnd != '\r') {
      if (static_cast<unsigned char>(*runEnd) >= 0x80u) {
        containsNonASCII = true;
      }
      ++runEnd;
    }

    if (runEnd != runStart) {
      if (containsNonASCII && !validateUTF8(runStart, runEnd)) {
        addDiagnostic(
            DiagnosticSeverity::Error,
            {
                cursor_.offset,
                cursor_.line,
                cursor_.column
            },
            "invalid UTF-8 sequence in string literal");

        valid = false;
      }

      const std::size_t consumed =
          static_cast<std::size_t>(
              runEnd - runStart);

      cursor_.current = runEnd;
      cursor_.offset += consumed;
      cursor_.column += consumed;
      continue;
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
// Lex a character literal with delimiter-aware recovery.
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
  } else if (peekChar() == '\n' ||
             peekChar() == '\r') {
    addDiagnostic(
        DiagnosticSeverity::Error,
        start,
        "newline is not allowed in a character literal");

    return finish(TokenKind::Unknown);
  } else if (static_cast<unsigned char>(peekChar()) >= 0x80u) {
    const char* scalarStart = cursor_.current;
    const unsigned char first =
        static_cast<unsigned char>(*scalarStart);

    unsigned width = 0;

    if (first >= 0xc2u && first <= 0xdfu) {
      width = 2;
    } else if (first >= 0xe0u && first <= 0xefu) {
      width = 3;
    } else if (first >= 0xf0u && first <= 0xf4u) {
      width = 4;
    }

    const std::size_t remaining =
        static_cast<std::size_t>(
            cursor_.end - scalarStart);

    if (width == 0 || remaining < width) {
      addDiagnostic(
          DiagnosticSeverity::Error,
          start,
          "invalid UTF-8 sequence in character literal");

      recoverCharacterLiteral();

      if (peekChar() == '\'') {
        consumeChar();
      }

      return finish(TokenKind::Unknown);
    }

    for (unsigned index = 1; index < width; ++index) {
      if (!isContinuationByte(
              static_cast<unsigned char>(
                  scalarStart[index]))) {
        addDiagnostic(
            DiagnosticSeverity::Error,
            start,
            "invalid UTF-8 sequence in character literal");

        recoverCharacterLiteral();

        if (peekChar() == '\'') {
          consumeChar();
        }

        return finish(TokenKind::Unknown);
      }
    }

    cursor_.current += width;
    cursor_.offset += width;
    cursor_.column += width;

    if (!validateUTF8(scalarStart, cursor_.current)) {
      addDiagnostic(
          DiagnosticSeverity::Error,
          start,
          "invalid UTF-8 sequence in character literal");

      return finish(TokenKind::Unknown);
    }
  } else {
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
// Lex hash-prefixed directives.
Token Lexer::lexHashOrDirective() {
  beginToken();

  consumeChar();

  if (!isIdentifierStart(peekChar())) {
    return finish(TokenKind::Hash);
  }

  const LexerCursor afterHash = cursor_;
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

  cursor_ = afterHash;
  return finish(TokenKind::Hash);
}

// Lex an at-prefixed directive.
// Lex @ directives through the shared keyword table.
Token Lexer::lexAtOrDirective() {
  beginToken();

  consumeChar();

  if (!isIdentifierStart(peekChar())) {
    return finish(TokenKind::At);
  }

  const LexerCursor afterAt = cursor_;
  consumeIdentifier();

  const std::string_view spelling(
      tokenStart_.current,
      static_cast<std::size_t>(
          cursor_.current - tokenStart_.current));

  const TokenKind keyword =
      classifyKeyword(spelling);

  if (keyword != TokenKind::Identifier) {
    return finish(keyword);
  }

  cursor_ = afterAt;
  return finish(TokenKind::At);
}

// Distinguish a comment from the slash operator.
// Distinguish comments from slash-based operators.
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
    // skipBlockComment() consumes the opening delimiter itself.
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
// Lex operators and punctuation using direct character dispatch.
Token Lexer::lexOperatorOrPunctuation() {
  beginToken();

  const char* current = cursor_.current;
  const char* end = cursor_.end;
  const char first = current[0];
  const char second = current + 1 < end ? current[1] : '\0';

  auto advance = [this, current](std::size_t count) noexcept {
    cursor_.current = current + count;
    cursor_.offset += count;
    cursor_.column += count;
  };

  switch (first) {
    case '(': advance(1); return finish(TokenKind::LeftParen);
    case ')': advance(1); return finish(TokenKind::RightParen);
    case '{': advance(1); return finish(TokenKind::LeftBrace);
    case '}': advance(1); return finish(TokenKind::RightBrace);
    case '[': advance(1); return finish(TokenKind::LeftBracket);
    case ']': advance(1); return finish(TokenKind::RightBracket);
    case ',': advance(1); return finish(TokenKind::Comma);
    case '.': advance(1); return finish(TokenKind::Dot);
    case ':': advance(1); return finish(TokenKind::Colon);
    case ';': advance(1); return finish(TokenKind::Semicolon);
    case '?': advance(1); return finish(TokenKind::Question);
    case '=': advance(second == '=' ? 2 : 1); return finish(second == '=' ? TokenKind::EqualEqual : TokenKind::Equal);
    case '!': advance(second == '=' ? 2 : 1); return finish(second == '=' ? TokenKind::BangEqual : TokenKind::Bang);
    case '+':
      if (second == '=') { advance(2); return finish(TokenKind::PlusEqual); }
      if (second == '+') { advance(2); return finish(TokenKind::PlusPlus); }
      advance(1); return finish(TokenKind::Plus);
    case '-':
      if (second == '=') { advance(2); return finish(TokenKind::MinusEqual); }
      if (second == '-') { advance(2); return finish(TokenKind::MinusMinus); }
      if (second == '>') { advance(2); return finish(TokenKind::Arrow); }
      advance(1); return finish(TokenKind::Minus);
    case '*': advance(second == '=' ? 2 : 1); return finish(second == '=' ? TokenKind::StarEqual : TokenKind::Star);
    case '%': advance(second == '=' ? 2 : 1); return finish(second == '=' ? TokenKind::PercentEqual : TokenKind::Percent);
    case '<': advance(second == '=' ? 2 : 1); return finish(second == '=' ? TokenKind::LessEqual : TokenKind::Less);
    case '>': advance(second == '=' ? 2 : 1); return finish(second == '=' ? TokenKind::GreaterEqual : TokenKind::Greater);
    case '&':
      if (second == '&') { advance(2); return finish(TokenKind::AndAnd); }
      break;
    case '|':
      if (second == '|') { advance(2); return finish(TokenKind::OrOr); }
      break;
    case '~':
      if (second == '=') { advance(2); return finish(TokenKind::TildeEqual); }
      advance(1); return finish(TokenKind::Tilde);
    default:
      break;
  }

  consumeChar();
  addDiagnostic(
      DiagnosticSeverity::Error,
      {tokenStart_.offset, tokenStart_.line, tokenStart_.column},
      "unrecognized Sift character");
  return finish(TokenKind::Unknown);
}

// Apply identifier context to the completed token.
// Apply function-name and calling-name context to identifiers.
Token Lexer::handleIdentifierContext(Token token) noexcept {
  if (expectingCallingName_ &&
      token.kind == TokenKind::Identifier) {
    token.kind = TokenKind::CallingName;
    expectingCallingName_ = false;
  }

  return token;
}

// Commit diagnostics produced during successful lookahead.
void Lexer::commitLookaheadDiagnostics() noexcept {
  lookaheadDiagnosticCount_ = 0;
}

// Update contextual lexer state after token formation.
// Update contextual state after forming a token.
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
// Save lexer state before speculative scanning.
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
// Restore lexer state after speculative scanning.
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
    if (current == '/') {
      Token token = lexCommentOrSlash();

      if (token.kind == TokenKind::Comment &&
          !options_.retainComments) {
        continue;
      }

      updateContext(token.kind, token.text);
      return token;
    }

    if (isIdentifierStart(current)) {
      Token token = lexIdentifierOrKeyword();
      updateContext(token.kind, token.text);
      return token;
    }

    if (detail::isAsciiDigit(current) ||
        (current == '.' &&
         options_.allowLeadingDotFloat &&
         detail::isAsciiDigit(peekChar(1)))) {
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
// Consume and return the next token.
Token Lexer::lex() {
  if (hasLookahead_) {
    Token result = lookahead_;

    restoreState(lookaheadState_);
    commitLookaheadDiagnostics();

    hasLookahead_ = false;
    lookahead_ = {};
    lookaheadState_ = saveState();
    lookaheadDiagnosticCount_ = 0;

    return result;
  }

  return lexImpl();
}

// Speculatively lex the next token without committing state.
// Return the next token without committing cursor state.
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

  const LexState advancedState =
      saveState();

  lookaheadDiagnosticCount_ = diagnostics_.size() - savedDiagnosticCount;

  restoreState(savedState);

  lookahead_ = speculativeToken;
  lookaheadState_ = advancedState;
  hasLookahead_ = true;

  return lookahead_;
}

// Lex the next identifier as a calling name.
// Lex the next identifier as an explicit calling name.
Token Lexer::lexCallingName() {
  Token token = lex();

  if (token.kind == TokenKind::Identifier) {
    token.kind = TokenKind::CallingName;
  }

  return token;
}
} // namespace sift::lexer
