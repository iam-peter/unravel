#pragma once

#include <QJsonObject>

namespace clang {
class ASTContext;
class CFG;
class FunctionDecl;
class LangOptions;
class SourceManager;
class SourceRange;
} // namespace clang

namespace Unravel::Flow {
QJsonObject location(const clang::SourceManager &sm,
                     const clang::LangOptions &lang, clang::SourceRange range);
QJsonObject experimentalFacts(clang::FunctionDecl *function,
                              clang::ASTContext &context,
                              const clang::CFG &cfg);
} // namespace Unravel::Flow
