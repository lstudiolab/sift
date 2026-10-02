#ifndef SIFT_LEXER_TOKEN_KIND_H
#define SIFT_LEXER_TOKEN_KIND_H

#include <cstdint>

namespace sift::lexer {

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

} // namespace sift::lexer

#endif
