#include "facts.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/ParentMapContext.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Analysis/CFG.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Basic/Version.h>
#include <clang/Lex/Lexer.h>
#include <llvm/ADT/StringExtras.h>

#include <QHash>
#include <QJsonArray>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <algorithm>

// A class's type. Clang 22 stopped keeping it on the declaration
static clang::QualType classType(const clang::CXXRecordDecl *record) {
#if CLANG_VERSION_MAJOR >= 22
  return record->getASTContext().getCanonicalTagType(record);
#else
  return record->getASTContext().getRecordType(record);
#endif
}

namespace Unravel::Flow {

QJsonObject location(const clang::SourceManager &sm,
                     const clang::LangOptions &lang, clang::SourceRange range) {
  if (range.isInvalid())
    return {};
  const auto begin = sm.getExpansionLoc(range.getBegin());
  const auto end = sm.getExpansionLoc(range.getEnd());
  if (begin.isInvalid() || end.isInvalid() || sm.isInSystemHeader(begin))
    return {};
  const auto start = sm.getPresumedLoc(begin);
  const auto finish = sm.getPresumedLoc(end);
  if (start.isInvalid() || finish.isInvalid())
    return {};
  QJsonObject out{{"file", QString::fromUtf8(start.getFilename())},
                  {"line", int(start.getLine())},
                  {"column", int(start.getColumn())},
                  {"endLine", int(finish.getLine())},
                  {"endColumn", int(finish.getColumn())}};
  const auto text = clang::Lexer::getSourceText(
      clang::CharSourceRange::getTokenRange(range), sm, lang);
  if (!text.empty()) {
    QString snippet =
        QString::fromUtf8(text.data(), qsizetype(text.size())).simplified();
    if (snippet.size() > 200)
      snippet = snippet.left(197) + QStringLiteral("...");
    out.insert("text", snippet);
  }
  return out;
}

namespace {
QString name(const clang::NamedDecl *decl) {
  QString result =
      decl ? QString::fromStdString(decl->getQualifiedNameAsString())
           : QString();
  result.replace(QStringLiteral("std::__1::"), QStringLiteral("std::"));
  result.replace(QStringLiteral("std::__cxx11::"), QStringLiteral("std::"));
  return result;
}

const clang::Expr *plain(const clang::Expr *expr) {
  while (expr) {
    expr = expr->IgnoreParenImpCasts();
    if (const auto *clean = llvm::dyn_cast<clang::ExprWithCleanups>(expr))
      expr = clean->getSubExpr();
    else if (const auto *temp =
                 llvm::dyn_cast<clang::MaterializeTemporaryExpr>(expr))
      expr = temp->getSubExpr();
    else if (const auto *bind =
                 llvm::dyn_cast<clang::CXXBindTemporaryExpr>(expr))
      expr = bind->getSubExpr();
    else
      break;
  }
  return expr;
}

class Evidence {
public:
  explicit Evidence(clang::ASTContext &context) : context(context) {}
  QJsonObject source(clang::SourceRange range) const {
    return location(context.getSourceManager(), context.getLangOpts(), range);
  }
  QString position(clang::SourceLocation loc) const {
    const auto &sm = context.getSourceManager();
    const auto p = sm.getPresumedLoc(sm.getExpansionLoc(loc));
    return p.isInvalid() ? QString()
                         : QStringLiteral("%1:%2:%3")
                               .arg(QString::fromUtf8(p.getFilename()))
                               .arg(p.getLine())
                               .arg(p.getColumn());
  }
  QString identity(const clang::ValueDecl *decl) const {
    return decl ? position(decl->getCanonicalDecl()->getLocation()) + ":" +
                      QString::fromStdString(decl->getNameAsString()) + "#" +
                      QString::number(decl->getCanonicalDecl()->getID())
                : QString();
  }
  QString storage(const clang::Expr *expr) const {
    expr = plain(expr);
    if (const auto *ref = llvm::dyn_cast_or_null<clang::DeclRefExpr>(expr))
      return identity(ref->getDecl());
    if (const auto *member = llvm::dyn_cast_or_null<clang::MemberExpr>(expr)) {
      const auto base = storage(member->getBase());
      return base.isEmpty() ? QString()
                            : base + "." + identity(member->getMemberDecl());
    }
    if (llvm::isa_and_nonnull<clang::CXXThisExpr>(expr))
      return QStringLiteral("this");
    return {};
  }
  clang::ASTContext &context;
};

QString templateName(clang::QualType type) {
  type = type.getNonReferenceType().getCanonicalType();
  const auto *record = type->getAsCXXRecordDecl();
  const auto *specialization =
      llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(record);
  if (!specialization)
    return {};
  QString result = name(specialization->getSpecializedTemplate());
  result.replace(QStringLiteral("std::__1::"), QStringLiteral("std::"));
  result.replace(QStringLiteral("std::__cxx11::"), QStringLiteral("std::"));
  return result;
}

QString role(clang::QualType type) {
  if (type->isReferenceType())
    return QStringLiteral("borrowing");
  if (type->isPointerType())
    return QStringLiteral("unknown");
  const QString t = templateName(type);
  if (t == QLatin1String("std::unique_ptr") ||
      t == QLatin1String("QScopedPointer"))
    return QStringLiteral("ownership");
  if (t == QLatin1String("std::shared_ptr") ||
      t == QLatin1String("QSharedPointer"))
    return QStringLiteral("shared-ownership");
  if (t == QLatin1String("std::weak_ptr") ||
      t == QLatin1String("QWeakPointer") || t == QLatin1String("QPointer"))
    return QStringLiteral("observation");
  return QStringLiteral("value-containment");
}

const clang::EnumConstantDecl *enumerator(const clang::Expr *expr) {
  const auto *ref = llvm::dyn_cast_or_null<clang::DeclRefExpr>(plain(expr));
  return ref ? llvm::dyn_cast<clang::EnumConstantDecl>(ref->getDecl())
             : nullptr;
}

class Observations final : public clang::RecursiveASTVisitor<Observations>,
                           public Evidence {
public:
  Observations(clang::FunctionDecl *function, clang::ASTContext &context)
      : Evidence(context), function(function) {}

  bool TraverseLambdaExpr(clang::LambdaExpr *expr) {
    for (auto *init : expr->capture_inits())
      if (init && !TraverseStmt(init))
        return false;
    return true;
  }

  bool TraverseCXXRecordDecl(clang::CXXRecordDecl *) { return true; }

  void collectFields() {
    const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(function);
    if (!method)
      return;
    const auto *record = method->getParent();
    const auto container = node("this", "selected-object",
                                source(record->getSourceRange()), name(record));
    for (const auto *field : record->fields()) {
      const auto fieldId = node("field:" + identity(field), "field",
                                source(field->getSourceRange()), name(field));
      ownershipEdges.append(
          QJsonObject{{"from", container},
                      {"to", fieldId},
                      {"role", "value-containment"},
                      {"source", source(field->getSourceRange())}});
      const auto type = field->getType();
      if (type->isPointerType() || type->isReferenceType() ||
          !templateName(type).isEmpty()) {
        const auto target = node("unknown:" + identity(field), "unknown-object",
                                 source(field->getSourceRange()), name(field));
        ownershipEdges.append(
            QJsonObject{{"from", fieldId},
                        {"to", target},
                        {"role", role(type)},
                        {"source", source(field->getSourceRange())}});
      }
    }
  }
  bool TraverseFunctionDecl(clang::FunctionDecl *decl) {
    return decl != function ||
           clang::RecursiveASTVisitor<Observations>::TraverseFunctionDecl(decl);
  }
  bool VisitVarDecl(clang::VarDecl *decl) {
    if (!decl->isLocalVarDeclOrParm())
      return true;
    const auto type = decl->getType().getCanonicalType();
    if (type->isPointerType() || type->isReferenceType()) {
      types.append(
          QJsonObject{{"name", QString::fromStdString(decl->getNameAsString())},
                      {"type", QString::fromStdString(type.getAsString())},
                      {"typeShape", type->isPointerType() ? "pointer-type"
                                    : type->isLValueReferenceType()
                                        ? "lvalue-reference-type"
                                        : "rvalue-reference-type"},
                      {"declaration", source(decl->getSourceRange())},
                      {"ownershipRole", role(type)},
                      {"lifetime", "not-analyzed"}});
    }
    if (decl->hasInit())
      assignment(identity(decl),
                 QString::fromStdString(decl->getNameAsString()), type,
                 decl->getInit(), decl->getSourceRange(), nullptr);
    bind(decl, decl->getInit(), decl->getSourceRange());
    return true;
  }
  bool VisitBinaryOperator(clang::BinaryOperator *op) {
    if (op->getOpcode() != clang::BO_Assign)
      return true;
    const auto *lhs = plain(op->getLHS());
    const auto key = storage(lhs);
    if (!key.isEmpty())
      assignment(key, source(lhs->getSourceRange()).value("text").toString(),
                 lhs->getType(), op->getRHS(), op->getSourceRange(), op);
    if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(lhs))
      if (const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl()))
        bind(var, op->getRHS(), op->getSourceRange());
    return true;
  }
  bool VisitCallExpr(clang::CallExpr *call) {
    const auto *callee = call->getDirectCallee();
    if (name(callee) == QLatin1String("std::move") && call->getNumArgs() == 1 &&
        role(call->getArg(0)->getType().getNonReferenceType()) !=
            QLatin1String("value-containment") &&
        !templateName(call->getArg(0)->getType()).isEmpty()) {
      unknownTransfers.append(source(call->getSourceRange()));
      if (call->getNumArgs() == 1) {
        const auto key = storage(call->getArg(0));
        if (!key.isEmpty())
          bindings[key] = allocation(call, "unknown-transfer");
      }
    }
    if (const auto *op = llvm::dyn_cast<clang::CXXOperatorCallExpr>(call);
        op && op->getOperator() == clang::OO_Equal && op->getNumArgs() == 2) {
      if (const auto *ref =
              llvm::dyn_cast<clang::DeclRefExpr>(plain(op->getArg(0))))
        if (const auto *var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl()))
          bind(var, op->getArg(1), op->getSourceRange());
    }
    const auto *methodCall = llvm::dyn_cast<clang::CXXMemberCallExpr>(call);
    const auto *method = methodCall ? methodCall->getMethodDecl() : nullptr;
    if (!method)
      return true;
    const QString api = name(method);
    if (api == QLatin1String("QState::addTransition") &&
        (call->getNumArgs() == 1 || call->getNumArgs() == 3)) {
      const auto target = call->getArg(call->getNumArgs() - 1);
      qtStates.append(QJsonObject{
          {"api", api},
          {"fromId", storage(methodCall->getImplicitObjectArgument())},
          {"toId", storage(target)},
          {"from",
           source(methodCall->getImplicitObjectArgument()->getSourceRange())},
          {"to", source(target->getSourceRange())},
          {"guard", call->getNumArgs() == 3
                        ? source(call->getArg(1)->getSourceRange())
                        : QJsonObject()},
          {"source", source(call->getSourceRange())},
          {"provenance",
           "explicit-registration; potential transition, not execution"}});
    } else if (api == QLatin1String("QAbstractTransition::setTargetState") &&
               call->getNumArgs() == 1) {
      qtStates.append(QJsonObject{
          {"api", api},
          {"transition",
           source(methodCall->getImplicitObjectArgument()->getSourceRange())},
          {"to", source(call->getArg(0)->getSourceRange())},
          {"source", source(call->getSourceRange())},
          {"origin", "unknown"}});
    } else if (api == QLatin1String("QObject::setParent") &&
               call->getNumArgs() == 1) {
      const auto child = object(methodCall->getImplicitObjectArgument());
      const auto parent = object(call->getArg(0));
      ownershipEdges.append(QJsonObject{
          {"from", parent},
          {"to", child},
          {"role", "qobject-parent"},
          {"source", source(call->getSourceRange())},
          {"certainty",
           "explicit-reparenting-event; null detaches; not a lifetime proof"}});
    } else if (role(method->getThisType()->getPointeeType()) !=
                   QLatin1String("value-containment") &&
               (method->getNameAsString() == "reset" ||
                method->getNameAsString() == "release" ||
                method->getNameAsString() == "swap")) {
      unknownTransfers.append(source(call->getSourceRange()));
      const auto key = storage(methodCall->getImplicitObjectArgument());
      if (!key.isEmpty())
        bindings[key] = allocation(call, "unknown-transfer");
    }
    return true;
  }
  bool VisitCXXConstructExpr(clang::CXXConstructExpr *expr) {
    const auto *constructor = expr->getConstructor();
    if (name(constructor) == QLatin1String("QObject::QObject") &&
        expr->getNumArgs() >= 1 &&
        !llvm::isa<clang::CXXDefaultArgExpr>(expr->getArg(0))) {
      ownershipEdges.append(QJsonObject{
          {"from", object(expr->getArg(0))},
          {"to", constructedObject(expr)},
          {"role", "qobject-parent"},
          {"source", source(expr->getSourceRange())},
          {"certainty",
           "explicit-constructor-parent; subsequent ownership may change"}});
    }
    return true;
  }

  QJsonObject ownership() const {
    return {
        {"status", "experimental"},
        {"nodes", ownershipNodes},
        {"edges", ownershipEdges},
        {"unknownTransfers", unknownTransfers},
        {"policy",
         "Result-local declaration identities. Flow-insensitive observed "
         "bindings, "
         "not simultaneous live owners. "
         "Object identities are allocation/value sites, not runtime instances. "
         "Raw pointers do not prove ownership; lifetimes and cycles are not "
         "proven."}};
  }
  QJsonObject stateGraph() const {
    QJsonArray stateNodes, stateEdges, targets;
    QSet<QString> seen, seenTargets;
    auto add = [&](const QString &id, const QString &label,
                   const QString &target, const QJsonObject &where,
                   bool unknown) {
      if (!seen.contains(id)) {
        seen.insert(id);
        stateNodes.append(
            QJsonObject{{"id", id},
                        {"label", label},
                        {"stateVariable", target},
                        {"kind", unknown ? "unknown-origin" : "state"},
                        {"unknownInfluence", unknown},
                        {"source", where}});
      }
    };
    for (const auto &value : assignments) {
      const auto item = value.toObject();
      const auto target = item["targetId"].toString();
      if (!seenTargets.contains(target)) {
        seenTargets.insert(target);
        targets.append(QJsonObject{{"id", target}, {"name", item["target"]}});
      }
      const auto prior = item["priorState"].toString();
      const bool unknownTarget =
          item["sourceProvenance"] != QLatin1String("explicit-enumerator");
      const auto next = unknownTarget
                            ? QStringLiteral("unknown target")
                            : item["enumType"].toString() +
                                  "::" + item["enumerator"].toString();
      const auto fromId = target + ":" + prior;
      const auto toId = target + ":" + next;
      add(fromId, prior, target, item["assignment"].toObject(),
          prior == QLatin1String("unknown"));
      add(toId, next, target, item["assignment"].toObject(), unknownTarget);
      stateEdges.append(QJsonObject{{"from", fromId},
                                    {"to", toId},
                                    {"stateVariable", target},
                                    {"role", item["priorStateProvenance"]},
                                    {"guard", item["guard"]},
                                    {"action", item["action"]},
                                    {"source", item["assignment"]}});
    }
    for (const auto &value : qtStates) {
      const auto item = value.toObject();
      const auto where = item["source"].toObject();
      const auto from = item["from"].toObject();
      const auto to = item["to"].toObject();
      const QString origin = item["fromId"].toString();
      const QString target = item["toId"].toString();
      const auto site = where["file"].toString() + ":" +
                        QString::number(where["line"].toInt()) + ":" +
                        QString::number(where["column"].toInt());
      const QString fromId =
          "qt:" + (origin.isEmpty() ? "unknown:" + site : origin);
      const QString toId =
          "qt:" + (target.isEmpty() ? "target:" + site : target);
      add(fromId, from["text"].toString("unknown origin"), "qt", where,
          origin.isEmpty());
      add(toId, to["text"].toString("unknown target"), "qt", where,
          target.isEmpty());
      stateEdges.append(QJsonObject{{"from", fromId},
                                    {"to", toId},
                                    {"stateVariable", "qt"},
                                    {"role", item["api"]},
                                    {"guard", item["guard"]},
                                    {"source", where}});
    }
    return {
        {"status", "experimental"},
        {"nodes", stateNodes},
        {"edges", stateEdges},
        {"targets", targets},
        {"policy",
         "Explicit enum writes and Qt state registrations only. Immediate "
         "equality guards "
         "can constrain origins; other writes remain unknown. Object aliases, "
         "runtime "
         "conditions and transition lifetimes are not resolved. No "
         "allowed-transition "
         "contract is assumed and no invalid-transition finding is emitted."}};
  }
  QJsonArray assignments, types, qtStates;

private:
  void assignment(const QString &key, const QString &target,
                  clang::QualType type, const clang::Expr *rhs,
                  clang::SourceRange range, const clang::Stmt *statement) {
    const auto *value = enumerator(rhs);
    const auto *enumType = type->getAs<clang::EnumType>();
    const auto *valueType =
        value ? value->getType()->getAs<clang::EnumType>() : nullptr;
    if (!enumType)
      return;
    if (value && (!valueType || enumType->getDecl()->getCanonicalDecl() !=
                                    valueType->getDecl()->getCanonicalDecl()))
      return;
    QJsonObject guard;
    QString prior;
    // Infer only an immediately guarded write. Earlier statements could
    // invalidate a condition even when they are in the same lexical branch.
    const clang::Stmt *child = statement;
    while (child) {
      const auto parents = context.getParents(*child);
      if (parents.size() != 1)
        break;
      const auto *parent = parents[0].get<clang::Stmt>();
      if (const auto *compound =
              llvm::dyn_cast_or_null<clang::CompoundStmt>(parent)) {
        if (compound->size() != 1)
          break;
        child = parent;
        continue;
      }
      const auto *condition = llvm::dyn_cast_or_null<clang::IfStmt>(parent);
      const auto *test = condition ? llvm::dyn_cast<clang::BinaryOperator>(
                                         plain(condition->getCond()))
                                   : nullptr;
      if (test) {
        guard = source(condition->getCond()->getSourceRange());
        const bool constrains = (condition->getThen() == child &&
                                 test->getOpcode() == clang::BO_EQ) ||
                                (condition->getElse() == child &&
                                 test->getOpcode() == clang::BO_NE);
        const auto *constant =
            storage(test->getLHS()) == key   ? enumerator(test->getRHS())
            : storage(test->getRHS()) == key ? enumerator(test->getLHS())
                                             : nullptr;
        if (constrains && constant &&
            constant->getType().getCanonicalType() == type.getCanonicalType())
          prior = name(constant);
      }
      break;
    }
    assignments.append(QJsonObject{
        {"target", target},
        {"targetId", key},
        {"enumType", name(enumType->getDecl())},
        {"enumerator", value ? QString::fromStdString(value->getNameAsString())
                             : QStringLiteral("unknown target")},
        {"value",
         value ? QString::fromStdString(llvm::toString(value->getInitVal(), 10))
               : QString()},
        {"assignment", source(range)},
        {"guard", guard},
        {"priorState", prior.isEmpty() ? QStringLiteral("unknown") : prior},
        {"sourceProvenance",
         value ? "explicit-enumerator" : "unknown-enum-expression"},
        {"priorStateProvenance",
         prior.isEmpty() ? "unknown" : "immediate-equality-guard"},
        {"action", source(range).value("text")}});
  }

  QString node(const QString &id, const QString &kind, const QJsonObject &where,
               const QString &label) {
    if (!nodes.contains(id)) {
      nodes.insert(id);
      ownershipNodes.append(QJsonObject{
          {"id", id}, {"kind", kind}, {"source", where}, {"label", label}});
    }
    return id;
  }
  QString allocation(const clang::Expr *expr, const QString &kind) {
    return node("object:" + position(expr->getBeginLoc()), kind,
                source(expr->getSourceRange()),
                source(expr->getSourceRange()).value("text").toString());
  }
  QString constructedObject(const clang::CXXConstructExpr *expr) {
    auto current = clang::DynTypedNode::create(*expr);
    for (int depth = 0; depth < 8; ++depth) {
      const auto parents = context.getParents(current);
      if (parents.size() != 1)
        break;
      if (const auto *var = parents[0].get<clang::VarDecl>())
        return node("value:" + identity(var), "value-object",
                    source(var->getSourceRange()), name(var));
      if (const auto *allocationExpr = parents[0].get<clang::CXXNewExpr>())
        return allocation(allocationExpr, "allocation-site");
      current = parents[0];
    }
    return allocation(expr, "qobject-construction");
  }
  QString object(const clang::Expr *expr) {
    expr = plain(expr);
    if (!expr)
      return {};
    if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(expr)) {
      const QString key = identity(ref->getDecl());
      if (bindings.contains(key))
        return bindings.value(key);
      if (!ref->getType()->isPointerType() &&
          !ref->getType()->isReferenceType() &&
          templateName(ref->getType()).isEmpty())
        return node("value:" + key, "value-object",
                    source(ref->getDecl()->getSourceRange()),
                    name(ref->getDecl()));
      return node("unknown:" + key, "unknown-object",
                  source(ref->getDecl()->getSourceRange()),
                  name(ref->getDecl()));
    }
    if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(expr))
      if (unary->getOpcode() == clang::UO_AddrOf)
        return object(unary->getSubExpr());
    if (const auto *construct = llvm::dyn_cast<clang::CXXConstructExpr>(expr)) {
      if (construct->getNumArgs() >= 1 &&
          !llvm::isa<clang::CXXDefaultArgExpr>(construct->getArg(0)))
        return object(construct->getArg(0));
    }
    if (const auto *member = llvm::dyn_cast<clang::CXXMemberCallExpr>(expr)) {
      const auto *method = member->getMethodDecl();
      if (method && method->getNameAsString() == "get" &&
          !templateName(method->getThisType()->getPointeeType()).isEmpty())
        return object(member->getImplicitObjectArgument());
    }
    if (llvm::isa<clang::CXXNewExpr>(expr))
      return allocation(expr, "allocation-site");
    if (expr->isNullPointerConstant(context,
                                    clang::Expr::NPC_ValueDependentIsNotNull))
      return allocation(expr, "null");
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(expr)) {
      const QString api = name(call->getDirectCallee());
      const auto *method =
          llvm::dyn_cast_or_null<clang::CXXMethodDecl>(call->getDirectCallee());
      const bool qtFactory =
          method && method->isStatic() &&
          method->getNameAsString() == "create" &&
          templateName(classType(method->getParent())) ==
              QLatin1String("QSharedPointer");
      if (api == QLatin1String("std::make_shared") ||
          api == QLatin1String("std::make_unique") || qtFactory)
        return allocation(expr, "allocation-site");
    }
    return allocation(expr, "unknown-object");
  }
  void bind(const clang::VarDecl *decl, const clang::Expr *init,
            clang::SourceRange range) {
    const auto type = decl->getType();
    if (!type->isRecordType() && !type->isPointerType() &&
        !type->isReferenceType())
      return;
    const QString key = identity(decl);
    const QString owner =
        node("variable:" + key, "variable", source(decl->getSourceRange()),
             QString::fromStdString(decl->getNameAsString()));
    QString relation = role(type);
    if (type->isPointerType()) {
      const auto *value = plain(init);
      const auto *unary = llvm::dyn_cast_or_null<clang::UnaryOperator>(value);
      const auto *call =
          llvm::dyn_cast_or_null<clang::CXXMemberCallExpr>(value);
      if ((unary && unary->getOpcode() == clang::UO_AddrOf) ||
          (call && call->getMethodDecl() &&
           call->getMethodDecl()->getNameAsString() == "get" &&
           (role(call->getMethodDecl()->getThisType()->getPointeeType()) ==
                QLatin1String("ownership") ||
            role(call->getMethodDecl()->getThisType()->getPointeeType()) ==
                QLatin1String("shared-ownership"))))
        relation = QStringLiteral("borrowing");
    }
    QString target;
    if (relation == QLatin1String("value-containment"))
      target = node("value:" + key, "value-object",
                    source(decl->getSourceRange()), name(decl));
    else if (type->isReferenceType() && init) {
      const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(plain(init));
      if (ref && (ref->getType()->isPointerType() ||
                  !templateName(ref->getType()).isEmpty()))
        target = node("variable:" + identity(ref->getDecl()), "variable",
                      source(ref->getDecl()->getSourceRange()),
                      name(ref->getDecl()));
      else
        target = object(init);
    } else if (init)
      target = object(init);
    else
      target =
          node("unknown:" + key, "unknown-object", source(range), name(decl));
    bindings.insert(key, target);
    ownershipEdges.append(QJsonObject{{"from", owner},
                                      {"to", target},
                                      {"role", relation},
                                      {"source", source(range)}});
  }
  clang::FunctionDecl *function;
  QSet<QString> nodes;
  QHash<QString, QString> bindings;
  QJsonArray ownershipNodes, ownershipEdges, unknownTransfers;
};

class Reads final : public clang::RecursiveASTVisitor<Reads> {
public:
  bool TraverseLambdaExpr(clang::LambdaExpr *) {
    unknown = true;
    return true;
  }
  bool VisitDeclRefExpr(clang::DeclRefExpr *expr) {
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(expr->getDecl()))
      if (var->isLocalVarDeclOrParm())
        variables.insert(var);
    return true;
  }
  bool VisitUnaryOperator(clang::UnaryOperator *expr) {
    unknown |= expr->getOpcode() == clang::UO_Deref;
    return true;
  }
  bool VisitMemberExpr(clang::MemberExpr *) {
    unknown = true;
    return true;
  }
  bool VisitArraySubscriptExpr(clang::ArraySubscriptExpr *) {
    unknown = true;
    return true;
  }
  bool VisitCallExpr(clang::CallExpr *) {
    unknown = true;
    return true;
  }
  QSet<const clang::VarDecl *> variables;
  bool unknown = false;
};

struct Event {
  QString id, target;
  QStringList reads;
  QJsonObject source;
  bool unknown = false;
  bool call = false;
};
using State = QHash<QString, QSet<QString>>;

QJsonObject defUse(clang::FunctionDecl *function, clang::ASTContext &context,
                   const clang::CFG &cfg) {
  Evidence evidence(context);
  QHash<unsigned, QVector<Event>> events;
  QHash<QString, const clang::VarDecl *> variables;
  QSet<QString> escaped;
  QJsonArray identities;
  auto remember = [&](const clang::VarDecl *var) {
    const QString key = evidence.identity(var);
    if (!variables.contains(key)) {
      variables.insert(key, var);
      identities.append(QJsonObject{
          {"id", key},
          {"name", QString::fromStdString(var->getNameAsString())},
          {"type", QString::fromStdString(var->getType().getAsString())},
          {"source", evidence.source(var->getSourceRange())}});
    }
    return key;
  };
  class Escapes : public clang::RecursiveASTVisitor<Escapes> {
  public:
    Escapes(const Evidence &evidence, QSet<QString> &escaped)
        : evidence(evidence), escaped(escaped) {}
    bool TraverseLambdaExpr(clang::LambdaExpr *expr) {
      for (const auto &capture : expr->captures())
        if (capture.capturesVariable() &&
            capture.getCaptureKind() == clang::LCK_ByRef)
          escaped.insert(evidence.identity(capture.getCapturedVar()));
      return true;
    }
    bool VisitUnaryOperator(clang::UnaryOperator *expr) {
      if (expr->getOpcode() == clang::UO_AddrOf)
        if (const auto *ref =
                llvm::dyn_cast<clang::DeclRefExpr>(plain(expr->getSubExpr())))
          escaped.insert(evidence.identity(ref->getDecl()));
      return true;
    }
    bool VisitCallExpr(clang::CallExpr *expr) {
      const auto *callee = expr->getDirectCallee();
      for (unsigned i = 0; i < expr->getNumArgs(); ++i) {
        const bool mayWrite =
            !callee || i >= callee->getNumParams() ||
            callee->getParamDecl(i)->getType()->isReferenceType();
        if (mayWrite)
          if (const auto *ref =
                  llvm::dyn_cast<clang::DeclRefExpr>(plain(expr->getArg(i))))
            escaped.insert(evidence.identity(ref->getDecl()));
      }
      return true;
    }
    bool VisitVarDecl(clang::VarDecl *decl) {
      if (decl->getType()->isReferenceType() && decl->hasInit())
        if (const auto *ref =
                llvm::dyn_cast<clang::DeclRefExpr>(plain(decl->getInit())))
          escaped.insert(evidence.identity(ref->getDecl()));
      return true;
    }
    bool VisitCXXConstructExpr(clang::CXXConstructExpr *expr) {
      const auto *constructor = expr->getConstructor();
      for (unsigned i = 0; i < expr->getNumArgs(); ++i)
        if (i < constructor->getNumParams() &&
            constructor->getParamDecl(i)->getType()->isReferenceType())
          if (const auto *ref =
                  llvm::dyn_cast<clang::DeclRefExpr>(plain(expr->getArg(i))))
            escaped.insert(evidence.identity(ref->getDecl()));
      return true;
    }
    const Evidence &evidence;
    QSet<QString> &escaped;
  } escapes(evidence, escaped);
  escapes.TraverseStmt(function->getBody());
  QVector<Event> parameters;
  for (const auto *param : function->parameters()) {
    const auto id = remember(param);
    parameters.append({"parameter:" + id,
                       id,
                       {},
                       evidence.source(param->getSourceRange()),
                       param->getType()->isReferenceType(),
                       false});
  }
  int count = 0;
  for (const auto *block : cfg) {
    auto &list = events[block->getBlockID()];
    if (block == &cfg.getEntry())
      list += parameters;
    auto append = [&](const clang::Stmt *stmt, const clang::Expr *value,
                      const clang::VarDecl *target, bool readsTarget,
                      bool call) {
      Event event;
      event.id =
          QStringLiteral("B%1:E%2").arg(block->getBlockID()).arg(list.size());
      event.source = evidence.source(stmt->getSourceRange());
      event.call = call;
      Reads reads;
      if (value)
        reads.TraverseStmt(const_cast<clang::Expr *>(value));
      if (readsTarget && target)
        reads.variables.insert(target);
      for (const auto *var : reads.variables) {
        event.reads.append(remember(var));
        event.unknown |= var->getType()->isReferenceType();
      }
      std::sort(event.reads.begin(), event.reads.end());
      if (target) {
        event.target = remember(target);
        event.unknown |= target->getType()->isReferenceType();
      }
      event.unknown |= reads.unknown || stmt->getBeginLoc().isMacroID() ||
                       stmt->getEndLoc().isMacroID() || (target && !value);
      list.append(event);
      ++count;
    };
    for (const auto &element : *block) {
      const auto entry = element.getAs<clang::CFGStmt>();
      if (!entry)
        continue;
      const auto *stmt = entry->getStmt();
      if (const auto *decls = llvm::dyn_cast<clang::DeclStmt>(stmt)) {
        for (const auto *decl : decls->decls())
          if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
            if (var->isLocalVarDecl())
              append(stmt, var->getInit(), var, false, false);
      } else if (const auto *binary =
                     llvm::dyn_cast<clang::BinaryOperator>(stmt);
                 binary && binary->isAssignmentOp()) {
        const auto *ref =
            llvm::dyn_cast<clang::DeclRefExpr>(plain(binary->getLHS()));
        const auto *var =
            ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
        if (var && var->isLocalVarDeclOrParm())
          append(stmt, binary->getRHS(), var, binary->isCompoundAssignmentOp(),
                 var->getType()->isReferenceType());
        else
          append(stmt, binary, nullptr, false, true);
      } else if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stmt);
                 unary && unary->isIncrementDecrementOp()) {
        const auto *ref =
            llvm::dyn_cast<clang::DeclRefExpr>(plain(unary->getSubExpr()));
        const auto *var =
            ref ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
        append(stmt, unary->getSubExpr(), var, true, !var);
      } else if (const auto *expr = llvm::dyn_cast<clang::Expr>(stmt)) {
        append(stmt, expr, nullptr, false,
               llvm::isa<clang::CallExpr>(expr) ||
                   llvm::isa<clang::CXXConstructExpr>(expr));
      } else if (const auto *ret = llvm::dyn_cast<clang::ReturnStmt>(stmt)) {
        append(stmt, ret->getRetValue(), nullptr, false, false);
      }
      if (count > 2000 || variables.size() > 2000)
        return {{"status", "limit-exceeded"},
                {"reason",
                 "More than 2000 local transfer events; no slice emitted"}};
    }
  }
  auto transfer = [&](State state, const QVector<Event> &list) {
    for (const auto &event : list) {
      if (!event.target.isEmpty())
        state[event.target] = {event.id};
      if (event.call)
        for (const auto &key : escaped)
          state[key] = {event.id + ":unknown:" + key};
    }
    return state;
  };
  QHash<unsigned, State> incoming, outgoing;
  if (qint64(count) * (escaped.size() + 1) > 200000)
    return {{"status", "limit-exceeded"},
            {"reason",
             "Alias/call-effect transfer budget exceeded; no slice emitted"}};
  bool changed = true;
  int iterations = 0;
  qint64 work = 0;
  while (changed) {
    if (++iterations > 2 * (count + int(cfg.size()) + 1))
      return {{"status", "limit-exceeded"},
              {"reason",
               "Reaching-definition convergence limit; no slice emitted"}};
    changed = false;
    for (const auto *block : cfg) {
      State joined;
      for (const auto &pred : block->preds())
        if (const auto *p = pred.getReachableBlock())
          for (auto it = outgoing[p->getBlockID()].cbegin(),
                    end = outgoing[p->getBlockID()].cend();
               it != end; ++it) {
            work += it.value().size();
            if (work > 2000000)
              return {{"status", "limit-exceeded"},
                      {"reason", "Reaching-definition work budget exceeded; no "
                                 "slice emitted"}};
            joined[it.key()].unite(it.value());
          }
      incoming[block->getBlockID()] = joined;
      auto out = transfer(joined, events[block->getBlockID()]);
      if (out != outgoing.value(block->getBlockID())) {
        changed = true;
        outgoing[block->getBlockID()] = std::move(out);
      }
    }
  }
  QJsonArray nodes, edges;
  QSet<QString> nodeIds, edgeIds;
  auto node = [&](const QString &id, const QString &kind,
                  const QString &variable, const QJsonObject &source,
                  bool unknown) {
    if (!nodeIds.contains(id)) {
      nodeIds.insert(id);
      nodes.append(QJsonObject{{"id", id},
                               {"label", QStringLiteral("%1 @ %2:%3: %4")
                                             .arg(kind)
                                             .arg(source["line"].toInt())
                                             .arg(source["column"].toInt())
                                             .arg(source["text"].toString())},
                               {"kind", kind},
                               {"variable", variable},
                               {"source", source},
                               {"unknownInfluence", unknown}});
    }
  };
  auto edge = [&](const QString &from, const QString &to,
                  const QJsonObject &source) {
    const auto key = from + "->" + to;
    if (!edgeIds.contains(key)) {
      edgeIds.insert(key);
      edges.append(QJsonObject{{"from", from}, {"to", to}, {"source", source}});
    }
  };
  for (const auto *block : cfg) {
    State state = incoming.value(block->getBlockID());
    for (const auto &event : events[block->getBlockID()]) {
      node(event.id, event.target.isEmpty() ? "expression-use" : "definition",
           event.target, event.source, event.unknown || event.call);
      for (const auto &variable : event.reads) {
        if (state.value(variable).isEmpty()) {
          const QString id = "unknown-input:" + variable;
          node(id, "unknown-input", variable, event.source, true);
          edge(id, event.id, event.source);
        }
        QStringList definitions = state.value(variable).values();
        std::sort(definitions.begin(), definitions.end());
        for (const auto &definition : definitions)
          edge(definition, event.id, event.source);
      }
      if (!event.target.isEmpty())
        state[event.target] = {event.id};
      if (event.call)
        for (const auto &key : escaped) {
          if (!variables.contains(key))
            continue;
          const auto id = event.id + ":unknown:" + key;
          node(id, "unknown-call-effect", key, event.source, true);
          edge(event.id, id, event.source);
          state[key] = {id};
        }
      if (nodes.size() > 4000 || edges.size() > 10000)
        return {
            {"status", "limit-exceeded"},
            {"reason",
             "Local graph exceeds 4000 nodes / 10000 edges; no slice emitted"}};
    }
  }
  return {{"status", "experimental"},
          {"variables", identities},
          {"nodes", nodes},
          {"edges", edges},
          {"policy", "Intraprocedural may-reaching definitions; kills on "
                     "explicit local assignment, "
                     "union at CFG joins. Address-escaped/reference arguments "
                     "are clobbered by calls. "
                     "Aliases, indirect writes, macros, member/array reads and "
                     "calls carry unknown influence. "
                     "Data dependencies only, not control dependencies or "
                     "sanitization proof."}};
}
} // namespace

QJsonObject experimentalFacts(clang::FunctionDecl *function,
                              clang::ASTContext &context,
                              const clang::CFG &cfg) {
  Observations observations(function, context);
  observations.collectFields();
  observations.TraverseFunctionDecl(function);
  return {{"enumAssignments", observations.assignments},
          {"qtStateTransitions", observations.qtStates},
          {"stateTransitions", observations.stateGraph()},
          {"pointerReferenceTypeObservations", observations.types},
          {"ownership", observations.ownership()},
          {"localDefUseSlices", defUse(function, context, cfg)}};
}
} // namespace Unravel::Flow
