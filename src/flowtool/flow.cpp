#include "flow.h"
#include "facts.h"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/StmtCXX.h>
#include <clang/AST/Type.h>
#include <clang/Analysis/Analyses/Dominators.h>
#include <clang/Analysis/CFG.h>
#include <clang/Basic/Version.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Lex/Lexer.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Tooling.h>

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QVector>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <llvm/ADT/StringExtras.h>

#include "project.h"

namespace Unravel::Flow {
namespace {

constexpr int kMaxBlocks = 10000;
constexpr qsizetype kMaxOutputBytes = 4 * 1024 * 1024;

QString canonical(const QString &path) {
  const QFileInfo info(path);
  const QString real = info.canonicalFilePath();
  return QDir::cleanPath(real.isEmpty() ? info.absoluteFilePath() : real);
}

QString edgeKind(const clang::CFGBlock &from, const clang::CFGBlock &to) {
  const clang::Stmt *targetLabel = to.getLabel();
  if (llvm::isa_and_nonnull<clang::SwitchStmt>(from.getTerminatorStmt()) &&
      (llvm::isa_and_nonnull<clang::CaseStmt>(targetLabel) ||
       llvm::isa_and_nonnull<clang::DefaultStmt>(targetLabel)))
    return QStringLiteral("branch");
  if (llvm::isa_and_nonnull<clang::CaseStmt>(targetLabel) ||
      llvm::isa_and_nonnull<clang::DefaultStmt>(targetLabel)) {
    if (!llvm::isa_and_nonnull<clang::SwitchStmt>(from.getTerminatorStmt()))
      return QStringLiteral("switch-fallthrough");
  }
  return QStringLiteral("flow");
}

QJsonObject serialize(const clang::FunctionDecl *function,
                      const clang::ReturnStmt *lastReturn,
                      clang::ASTContext &context, const clang::CFG &cfg,
                      const QString &sourceFile, const QJsonObject &facts) {
  const auto &sm = context.getSourceManager();
  const auto &lang = context.getLangOpts();
  QJsonArray blocks;
  QJsonArray edges;

  for (const clang::CFGBlock *block : cfg) {
    QJsonArray statements;
    int firstLine = 0;
    int lastLine = 0;
    bool returns = false;
    bool earlyReturn = false;
    for (const clang::CFGElement &element : *block) {
      const auto statement = element.getAs<clang::CFGStmt>();
      if (!statement)
        continue;
      const clang::Stmt *stmt = statement->getStmt();
      if (const auto *returnStmt = llvm::dyn_cast<clang::ReturnStmt>(stmt)) {
        returns = true;
        earlyReturn |= returnStmt != lastReturn;
      }
      const QJsonObject source = location(sm, lang, stmt->getSourceRange());
      if (source.isEmpty())
        continue;
      const int line = source.value(QStringLiteral("line")).toInt();
      const int endLine = source.value(QStringLiteral("endLine")).toInt(line);
      firstLine = firstLine == 0 ? line : std::min(firstLine, line);
      lastLine = std::max(lastLine, endLine);
      QJsonObject item{{QStringLiteral("kind"),
                        QString::fromLatin1(stmt->getStmtClassName())},
                       {QStringLiteral("source"), source}};
      if (const auto *call = llvm::dyn_cast<clang::CallExpr>(stmt)) {
        const auto *callee = call->getDirectCallee();
        item.insert(
            QStringLiteral("call"),
            QJsonObject{{QStringLiteral("qualifiedName"),
                         callee ? QString::fromStdString(
                                      callee->getQualifiedNameAsString())
                                : QStringLiteral("unresolved indirect call")},
                        {QStringLiteral("source"), source}});
      }
      statements.append(item);
    }
    QJsonObject source{{QStringLiteral("file"), sourceFile}};
    if (firstLine > 0) {
      source.insert(QStringLiteral("line"), firstLine);
      source.insert(QStringLiteral("endLine"), lastLine);
    }
    QJsonObject item{{QStringLiteral("id"), int(block->getBlockID())},
                     {QStringLiteral("entry"), block == &cfg.getEntry()},
                     {QStringLiteral("exit"), block == &cfg.getExit()},
                     {QStringLiteral("source"), source},
                     {QStringLiteral("statements"), statements},
                     {QStringLiteral("explicitReturn"), returns},
                     {QStringLiteral("earlyReturn"), earlyReturn}};
    if (const clang::Stmt *label = block->getLabel()) {
      item.insert(QStringLiteral("labelKind"),
                  QString::fromLatin1(label->getStmtClassName()));
      item.insert(QStringLiteral("labelSource"),
                  location(sm, lang, label->getSourceRange()));
    }
    if (const clang::Stmt *term = block->getTerminatorStmt()) {
      item.insert(QStringLiteral("terminatorKind"),
                  QString::fromLatin1(term->getStmtClassName()));
      item.insert(QStringLiteral("terminatorSource"),
                  location(sm, lang, term->getSourceRange()));
    }
    blocks.append(item);
  }

  clang::CFGDomTree dominators(const_cast<clang::CFG *>(&cfg));
  for (const clang::CFGBlock *block : cfg) {
    for (auto successor = block->succ_begin(); successor != block->succ_end();
         ++successor) {
      const clang::CFGBlock *target = successor->getReachableBlock();
      if (!target)
        continue;
      const unsigned fromId = block->getBlockID();
      const unsigned toId = target->getBlockID();
      QString kind = edgeKind(*block, *target);
      if (dominators.getBase().dominates(target, block))
        kind = QStringLiteral("loop-backedge");
      else if (kind == QLatin1String("flow") &&
               (llvm::isa_and_nonnull<clang::IfStmt>(
                    block->getTerminatorStmt()) ||
                llvm::isa_and_nonnull<clang::WhileStmt>(
                    block->getTerminatorStmt()) ||
                llvm::isa_and_nonnull<clang::ForStmt>(
                    block->getTerminatorStmt()) ||
                llvm::isa_and_nonnull<clang::DoStmt>(
                    block->getTerminatorStmt()) ||
                llvm::isa_and_nonnull<clang::ConditionalOperator>(
                    block->getTerminatorStmt()) ||
                llvm::isa_and_nonnull<clang::BinaryOperator>(
                    block->getTerminatorStmt()))) {
        kind = QStringLiteral("branch");
      }
      QJsonObject edge{{QStringLiteral("from"), int(fromId)},
                       {QStringLiteral("to"), int(toId)},
                       {QStringLiteral("kind"), kind},
                       {QStringLiteral("successorIndex"),
                        int(successor - block->succ_begin())}};
      if (kind == QLatin1String("branch")) {
        const clang::Stmt *term = block->getTerminatorStmt();
        if (llvm::isa<clang::SwitchStmt>(term)) {
          edge.insert(QStringLiteral("branchType"), QStringLiteral("switch"));
          if (const auto *caseStmt =
                  llvm::dyn_cast_or_null<clang::CaseStmt>(target->getLabel()))
            edge.insert(QStringLiteral("caseValue"),
                        location(sm, lang, caseStmt->getLHS()->getSourceRange())
                            .value(QStringLiteral("text"))
                            .toString());
          else
            edge.insert(QStringLiteral("caseValue"), QStringLiteral("default"));
        } else if (llvm::isa<clang::IfStmt>(term)) {
          edge.insert(QStringLiteral("condition"),
                      successor == block->succ_begin()
                          ? QStringLiteral("true")
                          : QStringLiteral("false"));
        }
      }
      if (kind == QLatin1String("switch-fallthrough"))
        edge.insert(
            QStringLiteral("semantics"),
            QStringLiteral("control continues into a later case label"));
      edges.append(edge);
    }
  }

  const auto begin = sm.getExpansionLoc(function->getBeginLoc());
  const auto presumed = sm.getPresumedLoc(begin);
  QJsonObject functionJson{
      {QStringLiteral("qualifiedName"),
       QString::fromStdString(function->getQualifiedNameAsString())},
      {QStringLiteral("file"), sourceFile},
      {QStringLiteral("line"), int(presumed.getLine())},
      {QStringLiteral("column"), int(presumed.getColumn())}};
  return {
      {QStringLiteral("schemaVersion"), 1},
      {QStringLiteral("kind"), QStringLiteral("clang-cfg")},
      {QStringLiteral("clangVersion"),
       QString::fromStdString(clang::getClangFullVersion())},
      {QStringLiteral("function"), functionJson},
      {QStringLiteral("policies"),
       QJsonObject{
           {QStringLiteral("exceptionEdges"), QStringLiteral("omitted")},
           {QStringLiteral("exceptionNote"),
            QStringLiteral("CFG built with AddEHEdges=false; thrown paths are "
                           "not modeled")},
           {QStringLiteral("coroutines"), QStringLiteral("rejected")},
           {QStringLiteral("implicitDestructors"), QStringLiteral("omitted")},
           {QStringLiteral("unreachableSuccessors"),
            QStringLiteral("omitted")}}},
      {QStringLiteral("limits"),
       QJsonObject{
           {QStringLiteral("maxBlocks"), kMaxBlocks},
           {QStringLiteral("maxOutputBytes"), qint64(kMaxOutputBytes)}}},
      {QStringLiteral("blocks"), blocks},
      {QStringLiteral("edges"), edges},
      {QStringLiteral("experimentalFacts"),
       QJsonObject{
           {QStringLiteral("schemaVersion"), 1},
           {QStringLiteral("scope"), QStringLiteral("selected-function")},
           {QStringLiteral("identityPolicy"),
            QStringLiteral("declaration/node identities are local to this "
                           "result, not cross-revision IDs")},
           {QStringLiteral("cfgCoverage"),
            QStringLiteral(
                "exception paths, implicit destructors and unreachable "
                "successors omitted; coroutines rejected")},
           {QStringLiteral("function"), functionJson},
           {QStringLiteral("clangVersion"),
            QString::fromStdString(clang::getClangFullVersion())},
           {QStringLiteral("coverage"),
            QStringLiteral("experimental current-source intraprocedural "
                           "observations; not Git-revision facts")},
           {QStringLiteral("enumAssignments"),
            QStringLiteral(
                "explicit enum/member writes; immediate equality guards only")},
           {QStringLiteral("pointerReferenceTypeObservations"),
            QStringLiteral("compiler-derived roles and observed bindings; "
                           "lifetimes not analyzed")},
           {QStringLiteral("localDefUseSlices"),
            QStringLiteral("experimental intraprocedural may-def-use; unknown "
                           "influences retained")},
           {QStringLiteral("facts"), facts}}}};
}

class Consumer final : public clang::ASTConsumer {
public:
  Consumer(QString file, int line, QString name, QString *error,
           QByteArray *json)
      : m_file(std::move(file)), m_line(line), m_name(std::move(name)),
        m_error(error), m_json(json) {}

  void HandleTranslationUnit(clang::ASTContext &context) override {
    class Finder final : public clang::RecursiveASTVisitor<Finder> {
    public:
      Finder(clang::ASTContext &context, const QString &file, int line,
             const QString &name)
          : context(context), file(file), line(line), name(name) {}
      bool VisitFunctionDecl(clang::FunctionDecl *decl) {
        if (!decl->doesThisDeclarationHaveABody())
          return true;
        const auto &sm = context.getSourceManager();
        const auto loc = sm.getExpansionLoc(decl->getBeginLoc());
        const auto presumed = sm.getPresumedLoc(loc);
        if (loc.isInvalid() || presumed.isInvalid() ||
            canonical(QString::fromUtf8(presumed.getFilename())) != file ||
            int(presumed.getLine()) != line ||
            QString::fromStdString(decl->getQualifiedNameAsString()) != name)
          return true;
        matches.append(decl);
        return true;
      }
      clang::ASTContext &context;
      QString file;
      int line;
      QString name;
      QVector<clang::FunctionDecl *> matches;
    };

    Finder finder(context, m_file, m_line, m_name);
    finder.TraverseDecl(context.getTranslationUnitDecl());
    if (finder.matches.size() != 1) {
      *m_error =
          finder.matches.isEmpty()
              ? QStringLiteral("No definition matches file, definition line, "
                               "and qualified name")
              : QStringLiteral(
                    "The selector matched more than one function definition");
      return;
    }
    clang::FunctionDecl *function = finder.matches.first();
    if (llvm::isa<clang::CoroutineBodyStmt>(function->getBody())) {
      *m_error = QStringLiteral(
          "Coroutine CFG is not supported; no graph was produced");
      return;
    }
    const auto *body = llvm::dyn_cast<clang::CompoundStmt>(function->getBody());
    const auto *finalReturn =
        body && !body->body_empty()
            ? llvm::dyn_cast<clang::ReturnStmt>(body->body_back())
            : nullptr;

    clang::CFG::BuildOptions options;
    options.AddEHEdges = false;
    options.AddImplicitDtors = false;
    options.AddTemporaryDtors = false;
    std::unique_ptr<clang::CFG> cfg =
        clang::CFG::buildCFG(function, function->getBody(), &context, options);
    if (!cfg) {
      *m_error =
          QStringLiteral("Clang could not construct a CFG for this function");
      return;
    }
    if (int(cfg->size()) > kMaxBlocks) {
      *m_error = QStringLiteral("Function exceeds the %1-block CFG limit")
                     .arg(kMaxBlocks);
      return;
    }
    const QJsonObject facts = experimentalFacts(function, context, *cfg);
    *m_json = QJsonDocument(serialize(function, finalReturn, context, *cfg,
                                      m_file, facts))
                  .toJson(QJsonDocument::Compact);
    if (m_json->size() > kMaxOutputBytes) {
      m_json->clear();
      *m_error = QStringLiteral("CFG JSON exceeds the %1-byte output limit")
                     .arg(kMaxOutputBytes);
    }
  }

private:
  QString m_file;
  int m_line;
  QString m_name;
  QString *m_error;
  QByteArray *m_json;
};

class Action final : public clang::ASTFrontendAction {
public:
  Action(QString file, int line, QString name, QString *error, QByteArray *json)
      : m_file(std::move(file)), m_line(line), m_name(std::move(name)),
        m_error(error), m_json(json) {}
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &, llvm::StringRef) override {
    return std::make_unique<Consumer>(m_file, m_line, m_name, m_error, m_json);
  }

private:
  QString m_file;
  int m_line;
  QString m_name;
  QString *m_error;
  QByteArray *m_json;
};

class ActionFactory final : public clang::tooling::FrontendActionFactory {
public:
  ActionFactory(QString file, int line, QString name, QString *error,
                QByteArray *json)
      : m_file(std::move(file)), m_line(line), m_name(std::move(name)),
        m_error(error), m_json(json) {}

  std::unique_ptr<clang::FrontendAction> create() override {
    return std::make_unique<Action>(m_file, m_line, m_name, m_error, m_json);
  }

private:
  QString m_file;
  int m_line;
  QString m_name;
  QString *m_error;
  QByteArray *m_json;
};

} // namespace

QByteArray generate(const QString &projectPath, const QString &sourceFile,
                    int definitionLine, const QString &qualifiedName,
                    QString *error) {
  if (error)
    error->clear();
  const QString source = canonical(sourceFile);
  if (definitionLine < 1 || qualifiedName.trimmed().isEmpty()) {
    if (error)
      *error = QStringLiteral("A positive definition line and qualified "
                              "function name are required");
    return {};
  }
  const ProjectSetup setup = Project::discover(projectPath);
  if (!setup.error.isEmpty() ||
      setup.source != ProjectSetup::CompilationDatabase) {
    if (error)
      *error = setup.error.isEmpty()
                   ? QStringLiteral("A compile_commands.json is required to "
                                    "build a project-accurate CFG")
                   : setup.error;
    return {};
  }
  const auto unit = std::find_if(setup.units.cbegin(), setup.units.cend(),
                                 [&](const CompileUnit &candidate) {
                                   return canonical(candidate.file) == source;
                                 });
  if (unit == setup.units.cend()) {
    if (error)
      *error = QStringLiteral("The source file is not a translation unit in "
                              "this project's compilation database");
    return {};
  }

  std::vector<std::string> arguments;
  arguments.reserve(size_t(unit->args.size()));
  for (const QString &arg : unit->args)
    arguments.push_back(arg.toStdString());
  clang::tooling::FixedCompilationDatabase database(
      unit->directory.toStdString(), arguments);
  clang::tooling::ClangTool tool(database, {source.toStdString()});
  QByteArray json;
  QString parseError;
  ActionFactory factory(source, definitionLine, qualifiedName, &parseError,
                        &json);
  const int result = tool.run(&factory);
  if (result != 0 || json.isEmpty()) {
    if (error)
      *error = parseError.isEmpty()
                   ? QStringLiteral("Clang failed to parse the translation "
                                    "unit with its compile-database arguments")
                   : parseError;
    return {};
  }
  return json;
}

} // namespace Unravel::Flow
