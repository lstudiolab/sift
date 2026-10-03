#ifndef SIFT_PARSE_PARSER_H
#define SIFT_PARSE_PARSER_H

#include "sift/Parse/Lexer.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sift::parse {

using lexer::SourceLocation;
using lexer::Token;
using lexer::TokenKind;

enum class DiagnosticSeverity {
  Note,
  Warning,
  Error
};

struct Diagnostic {
  DiagnosticSeverity severity = DiagnosticSeverity::Error;
  SourceLocation location{};
  std::string message;
};

enum class NodeKind {
  Program,
  ImportDeclaration,
  FunctionDeclaration,
  StructDeclaration,
  ClassDeclaration,
  EnumDeclaration,
  ProtocolDeclaration,
  ExtensionDeclaration,
  VariableDeclaration,
  ConstDeclaration,
  Parameter,
  Block,
  IfStatement,
  ElseStatement,
  WhileStatement,
  RepeatStatement,
  ForStatement,
  ReturnStatement,
  DeferStatement,
  BreakStatement,
  ContinueStatement,
  GuardStatement,
  LoopStatement,
  DoStatement,
  ThrowStatement,
  TryStatement,
  SwitchStatement,
  SwitchCase,
  CatchClause,
  ExpressionStatement,
  IdentifierExpression,
  LiteralExpression,
  BinaryExpression,
  UnaryExpression,
  AssignmentExpression,
  MemberExpression,
  IndexExpression,
  RangeExpression,
  ArrayLiteralExpression,
  DictionaryLiteralExpression,
  CallExpression
};

struct ASTNode {
  virtual ~ASTNode() = default;
  virtual NodeKind kind() const noexcept = 0;
  SourceLocation location{};
};

struct Expression;
struct Statement;

struct IdentifierExpression final : ASTNode {
  std::string name;

  NodeKind kind() const noexcept override {
    return NodeKind::IdentifierExpression;
  }
};

struct LiteralExpression final : ASTNode {
  TokenKind literalKind = TokenKind::Unknown;
  std::string value;

  NodeKind kind() const noexcept override {
    return NodeKind::LiteralExpression;
  }
};

struct BinaryExpression final : ASTNode {
  std::string op;
  std::unique_ptr<Expression> left;
  std::unique_ptr<Expression> right;

  NodeKind kind() const noexcept override {
    return NodeKind::BinaryExpression;
  }
};

struct UnaryExpression final : ASTNode {
  std::string op;
  std::unique_ptr<Expression> operand;

  NodeKind kind() const noexcept override {
    return NodeKind::UnaryExpression;
  }
};

struct AssignmentExpression final : ASTNode {
  std::string op;
  std::unique_ptr<Expression> target;
  std::unique_ptr<Expression> value;

  NodeKind kind() const noexcept override {
    return NodeKind::AssignmentExpression;
  }
};

struct MemberExpression final : ASTNode {
  std::unique_ptr<Expression> base;
  std::string member;

  NodeKind kind() const noexcept override {
    return NodeKind::MemberExpression;
  }
};

struct IndexExpression final : ASTNode {
  std::unique_ptr<Expression> base;
  std::unique_ptr<Expression> index;

  NodeKind kind() const noexcept override {
    return NodeKind::IndexExpression;
  }
};

struct RangeExpression final : ASTNode {
  std::unique_ptr<Expression> start;
  std::unique_ptr<Expression> end;
  bool inclusive = true;

  NodeKind kind() const noexcept override {
    return NodeKind::RangeExpression;
  }
};

struct ArrayLiteralExpression final : ASTNode {
  std::vector<std::unique_ptr<Expression>> elements;

  NodeKind kind() const noexcept override {
    return NodeKind::ArrayLiteralExpression;
  }
};

struct DictionaryLiteralExpression final : ASTNode {
  std::vector<std::pair<std::unique_ptr<Expression>, std::unique_ptr<Expression>>> entries;

  NodeKind kind() const noexcept override {
    return NodeKind::DictionaryLiteralExpression;
  }
};

struct CallExpression final : ASTNode {
  std::unique_ptr<Expression> callee;
  std::vector<std::unique_ptr<Expression>> arguments;

  NodeKind kind() const noexcept override {
    return NodeKind::CallExpression;
  }
};

struct Expression final : ASTNode {
  std::variant<
      IdentifierExpression,
      LiteralExpression,
      BinaryExpression,
      UnaryExpression,
      AssignmentExpression,
      MemberExpression,
      IndexExpression,
      RangeExpression,
      ArrayLiteralExpression,
      DictionaryLiteralExpression,
      CallExpression> value;

  NodeKind kind() const noexcept override {
    return std::visit(
        [](const auto& node) { return node.kind(); },
        value);
  }
};

struct Parameter final : ASTNode {
  std::string name;
  std::string type;

  NodeKind kind() const noexcept override {
    return NodeKind::Parameter;
  }
};

struct Block final : ASTNode {
  std::vector<std::unique_ptr<Statement>> statements;

  NodeKind kind() const noexcept override {
    return NodeKind::Block;
  }
};

struct VariableDeclaration final : ASTNode {
  std::string accessModifier;
  bool isConst = false;
  std::string name;
  std::string type;
  std::unique_ptr<Expression> initializer;

  NodeKind kind() const noexcept override {
    return isConst ? NodeKind::ConstDeclaration
                   : NodeKind::VariableDeclaration;
  }
};

struct FunctionDeclaration final : ASTNode {
  std::string accessModifier;
  bool isAsync = false;
  bool isThrows = false;
  std::string name;
  std::string callingName;
  std::vector<std::unique_ptr<Parameter>> parameters;
  std::string returnType;
  std::unique_ptr<Block> body;

  NodeKind kind() const noexcept override {
    return NodeKind::FunctionDeclaration;
  }
};

struct ImportDeclaration final : ASTNode {
  std::string module;

  NodeKind kind() const noexcept override {
    return NodeKind::ImportDeclaration;
  }
};

struct StructDeclaration final : ASTNode {
  std::string accessModifier;
  std::string name;
  std::vector<std::unique_ptr<VariableDeclaration>> variables;
  std::vector<std::unique_ptr<FunctionDeclaration>> functions;

  NodeKind kind() const noexcept override {
    return NodeKind::StructDeclaration;
  }
};

struct ClassDeclaration final : ASTNode {
  std::string accessModifier;
  std::string name;
  std::vector<std::unique_ptr<VariableDeclaration>> variables;
  std::vector<std::unique_ptr<FunctionDeclaration>> functions;

  NodeKind kind() const noexcept override {
    return NodeKind::ClassDeclaration;
  }
};

struct EnumDeclaration final : ASTNode {
  std::string accessModifier;
  std::string name;
  std::vector<std::string> cases;

  NodeKind kind() const noexcept override {
    return NodeKind::EnumDeclaration;
  }
};

struct ProtocolDeclaration final : ASTNode {
  std::string accessModifier;
  std::string name;
  std::vector<std::unique_ptr<FunctionDeclaration>> functions;

  NodeKind kind() const noexcept override {
    return NodeKind::ProtocolDeclaration;
  }
};

struct ExtensionDeclaration final : ASTNode {
  std::string accessModifier;
  std::string target;
  std::vector<std::unique_ptr<FunctionDeclaration>> functions;

  NodeKind kind() const noexcept override {
    return NodeKind::ExtensionDeclaration;
  }
};

struct GuardStatement final : ASTNode {
  std::unique_ptr<Expression> condition;
  std::unique_ptr<Block> body;

  NodeKind kind() const noexcept override {
    return NodeKind::GuardStatement;
  }
};

struct LoopStatement final : ASTNode {
  std::unique_ptr<Block> body;

  NodeKind kind() const noexcept override {
    return NodeKind::LoopStatement;
  }
};

struct DoStatement final : ASTNode {
  std::unique_ptr<Block> body;
  std::vector<std::unique_ptr<ASTNode>> catches;

  NodeKind kind() const noexcept override {
    return NodeKind::DoStatement;
  }
};

struct ThrowStatement final : ASTNode {
  std::unique_ptr<Expression> value;

  NodeKind kind() const noexcept override {
    return NodeKind::ThrowStatement;
  }
};

struct CatchClause final : ASTNode {
  std::unique_ptr<Expression> condition;
  std::unique_ptr<Block> body;

  NodeKind kind() const noexcept override {
    return NodeKind::CatchClause;
  }
};

struct TryStatement final : ASTNode {
  std::unique_ptr<Block> body;
  std::vector<std::unique_ptr<CatchClause>> catches;

  NodeKind kind() const noexcept override {
    return NodeKind::TryStatement;
  }
};

struct IfStatement final : ASTNode {
  std::unique_ptr<Expression> condition;
  std::unique_ptr<Block> thenBlock;
  std::unique_ptr<Block> elseBlock;

  NodeKind kind() const noexcept override {
    return NodeKind::IfStatement;
  }
};

struct WhileStatement final : ASTNode {
  std::unique_ptr<Expression> condition;
  std::unique_ptr<Block> body;

  NodeKind kind() const noexcept override {
    return NodeKind::WhileStatement;
  }
};

struct RepeatStatement final : ASTNode {
  std::unique_ptr<Block> body;
  std::unique_ptr<Expression> condition;

  NodeKind kind() const noexcept override {
    return NodeKind::RepeatStatement;
  }
};

struct ForStatement final : ASTNode {
  std::string variable;
  std::unique_ptr<Expression> sequence;
  std::unique_ptr<Block> body;

  NodeKind kind() const noexcept override {
    return NodeKind::ForStatement;
  }
};

struct ReturnStatement final : ASTNode {
  std::unique_ptr<Expression> value;

  NodeKind kind() const noexcept override {
    return NodeKind::ReturnStatement;
  }
};

struct DeferStatement final : ASTNode {
  std::unique_ptr<Block> body;

  NodeKind kind() const noexcept override {
    return NodeKind::DeferStatement;
  }
};

struct BreakStatement final : ASTNode {
  NodeKind kind() const noexcept override {
    return NodeKind::BreakStatement;
  }
};

struct ContinueStatement final : ASTNode {
  NodeKind kind() const noexcept override {
    return NodeKind::ContinueStatement;
  }
};

struct SwitchCase final : ASTNode {
  std::unique_ptr<Expression> condition;
  std::unique_ptr<Block> body;
  bool isDefault = false;

  NodeKind kind() const noexcept override {
    return NodeKind::SwitchCase;
  }
};

struct SwitchStatement final : ASTNode {
  std::unique_ptr<Expression> subject;
  std::vector<std::unique_ptr<SwitchCase>> cases;

  NodeKind kind() const noexcept override {
    return NodeKind::SwitchStatement;
  }
};

struct ExpressionStatement final : ASTNode {
  std::unique_ptr<Expression> expression;

  NodeKind kind() const noexcept override {
    return NodeKind::ExpressionStatement;
  }
};

struct Statement final {
  std::variant<
      VariableDeclaration,
      FunctionDeclaration,
      StructDeclaration,
      ClassDeclaration,
      EnumDeclaration,
      ProtocolDeclaration,
      ExtensionDeclaration,
      IfStatement,
      WhileStatement,
      RepeatStatement,
      ForStatement,
      ReturnStatement,
      DeferStatement,
      BreakStatement,
      ContinueStatement,
      GuardStatement,
      LoopStatement,
      DoStatement,
      ThrowStatement,
      TryStatement,
      SwitchStatement,
      ExpressionStatement> value;
};

struct Program final : ASTNode {
  std::vector<std::unique_ptr<ImportDeclaration>> imports;
  std::vector<std::unique_ptr<StructDeclaration>> structs;
  std::vector<std::unique_ptr<ClassDeclaration>> classes;
  std::vector<std::unique_ptr<EnumDeclaration>> enums;
  std::vector<std::unique_ptr<ProtocolDeclaration>> protocols;
  std::vector<std::unique_ptr<ExtensionDeclaration>> extensions;
  std::vector<std::unique_ptr<FunctionDeclaration>> functions;
  std::vector<std::unique_ptr<Statement>> statements;

  NodeKind kind() const noexcept override {
    return NodeKind::Program;
  }
};

class Parser final {
public:
  explicit Parser(std::string_view source);

  std::unique_ptr<Program> parse();

  const std::vector<Diagnostic>& diagnostics() const noexcept;
  bool hasErrors() const noexcept;
  SourceLocation locationAt(std::size_t offset) const noexcept;

private:
  lexer::Lexer lexer_;
  std::string_view source_;
  bool pieceMode_ = false;
  bool tokenMode_ = false;
  std::shared_ptr<const std::vector<Token>> tokenBuffer_;
  std::size_t tokenCursor_ = 0;
  std::size_t tokenEnd_ = 0;
  std::shared_ptr<std::vector<std::size_t>> lineStarts_;
  std::vector<Diagnostic> diagnostics_;
  bool diagnosticsTruncated_ = false;
  bool hasParserErrors_ = false;
  static constexpr std::size_t maxDiagnostics_ = 256;

  std::unordered_set<std::string_view> callingNames_;
  std::unordered_set<std::string_view> functionNames_;
  std::unordered_set<std::string_view> structNames_;

  Token current_{};
  Token previous_{};

  // Parser context used for structural control-flow validation.
  std::size_t functionDepth_ = 0;
  std::size_t loopDepth_ = 0;
  std::size_t switchDepth_ = 0;
  std::size_t statementDepth_ = 0;
  std::size_t expressionDepth_ = 0;
  std::size_t assignmentDepth_ = 0;
  static constexpr std::size_t maxStatementDepth_ = 4096;
  static constexpr std::size_t maxExpressionDepth_ = 1024;
  static constexpr std::size_t maxAssignmentDepth_ = 256;

  void advance();
  bool check(TokenKind kind) const;
  bool match(TokenKind kind);
  bool expect(TokenKind kind, std::string_view message);

  void error(const Token& token, std::string_view message);
  void addDiagnostic(SourceLocation location, std::string_view message);
  void synchronize();
  void synchronizeExpression();
  void synchronizeToBlockStart();

  Parser(std::string_view source, bool pieceMode);
  Parser(std::string_view source,
         std::shared_ptr<const std::vector<Token>> tokens,
         std::shared_ptr<std::vector<std::size_t>> lineStarts,
         std::size_t tokenBegin,
         std::size_t tokenEnd);

  struct PieceBoundary final {
    std::size_t start = 0;
    std::size_t end = 0;
    std::size_t tokenBegin = 0;
    std::size_t tokenEnd = 0;
  };

  struct PieceResult final {
    std::size_t sequence = 0;
    PieceBoundary boundary{};
    std::unique_ptr<Program> program;
    std::vector<Diagnostic> diagnostics;
    bool hasErrors = false;
  };

  static std::vector<PieceBoundary> findPieceBoundaries(const std::vector<Token>& tokens, std::size_t sourceSize);
  PieceResult parsePiece(std::size_t sequence, PieceBoundary boundary) const;
  std::unique_ptr<Program> parseSequentialProgram();

  std::unique_ptr<ImportDeclaration> parseImport();
  std::unique_ptr<StructDeclaration> parseStruct();
  std::unique_ptr<ClassDeclaration> parseClass();
  std::unique_ptr<EnumDeclaration> parseEnum();
  std::unique_ptr<ProtocolDeclaration> parseProtocol();
  std::unique_ptr<ExtensionDeclaration> parseExtension();
  std::unique_ptr<FunctionDeclaration> parseFunction();
  std::unique_ptr<Statement> parseStatement();

  std::unique_ptr<VariableDeclaration> parseVariable(bool isConst);
  std::unique_ptr<IfStatement> parseIf();
  std::unique_ptr<WhileStatement> parseWhile();
  std::unique_ptr<RepeatStatement> parseRepeat();
  std::unique_ptr<ForStatement> parseFor();
  std::unique_ptr<ReturnStatement> parseReturn();
  std::unique_ptr<DeferStatement> parseDefer();
  std::unique_ptr<SwitchStatement> parseSwitch();
  std::unique_ptr<BreakStatement> parseBreak();
  std::unique_ptr<ContinueStatement> parseContinue();
  std::unique_ptr<GuardStatement> parseGuard();
  std::unique_ptr<LoopStatement> parseLoop();
  std::unique_ptr<DoStatement> parseDo();
  std::unique_ptr<ThrowStatement> parseThrow();
  std::unique_ptr<TryStatement> parseTry();
  std::unique_ptr<ExpressionStatement> parseExpressionStatement();

  bool canBreak() const noexcept;
  bool canContinue() const noexcept;
  bool canReturn() const noexcept;
  bool enterExpression();
  void leaveExpression() noexcept;
  bool enterAssignment();
  void leaveAssignment() noexcept;

  std::unique_ptr<Block> parseBlock();
  std::unique_ptr<Expression> parseExpression();
  std::unique_ptr<Expression> parseAssignment();
  std::unique_ptr<Expression> parseBinaryExpression(int minimumPrecedence);
  static int binaryPrecedence(TokenKind kind) noexcept;
  std::unique_ptr<Expression> parseUnary();
  std::unique_ptr<Expression> parsePostfix();
  std::unique_ptr<Expression> parsePrimary();

  std::string parseTypeName();
  std::string parseIdentifier(std::string_view context);
  std::string parseAccessModifier();
  std::unique_ptr<Parameter> parseParameter();
  std::string parseCallingName();
  std::string tokenText(const Token& token) const;

  bool isAssignmentOperator(TokenKind kind) const noexcept;

  static std::string_view operatorText(TokenKind kind) noexcept;
};

} // namespace sift::parse

#endif
