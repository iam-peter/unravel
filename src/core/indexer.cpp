#include "indexer.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QThread>

#include <clang-c/Index.h>

#include "python.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <thread>
#include <vector>

namespace Unravel {

namespace {

QString take(CXString s)
{
    const char *c = clang_getCString(s);
    const QString out = c ? QString::fromUtf8(c) : QString();
    clang_disposeString(s);
    return out;
}

QString usrOf(CXCursor c)
{
    return take(clang_getCursorUSR(c));
}

bool isFunctionCursor(CXCursorKind k)
{
    return k == CXCursor_FunctionDecl || k == CXCursor_CXXMethod || k == CXCursor_Constructor
        || k == CXCursor_Destructor || k == CXCursor_ConversionFunction || k == CXCursor_FunctionTemplate
        || k == CXCursor_LambdaExpr;
}

bool isTransparent(CXCursorKind k)
{
    return k == CXCursor_LinkageSpec || k == CXCursor_UnexposedDecl;
}

CXCursor semanticParent(CXCursor c)
{
    CXCursor p = clang_getCursorSemanticParent(c);
    int guard = 0;
    while (!clang_Cursor_isNull(p) && isTransparent(clang_getCursorKind(p)) && guard++ < 16)
        p = clang_getCursorSemanticParent(p);
    return p;
}

bool isLocal(CXCursor c)
{
    CXCursor p = clang_getCursorSemanticParent(c);
    int guard = 0;
    while (!clang_Cursor_isNull(p) && guard++ < 32) {
        const CXCursorKind k = clang_getCursorKind(p);
        if (k == CXCursor_TranslationUnit || clang_isInvalid(k))
            return false;
        if (isFunctionCursor(k))
            return true;
        p = clang_getCursorSemanticParent(p);
    }
    return false;
}

SymbolKind kindOf(CXCursor c)
{
    switch (clang_getCursorKind(c)) {
    case CXCursor_Namespace: return SymbolKind::Namespace;
    case CXCursor_ClassDecl: return SymbolKind::Class;
    case CXCursor_StructDecl: return SymbolKind::Struct;
    case CXCursor_UnionDecl: return SymbolKind::Union;
    case CXCursor_ClassTemplate:
    case CXCursor_ClassTemplatePartialSpecialization:
        switch (clang_getTemplateCursorKind(c)) {
        case CXCursor_StructDecl: return SymbolKind::Struct;
        case CXCursor_UnionDecl: return SymbolKind::Union;
        default: return SymbolKind::Class;
        }
    case CXCursor_EnumDecl: return SymbolKind::Enum;
    case CXCursor_EnumConstantDecl: return SymbolKind::EnumConstant;
    case CXCursor_TypedefDecl:
    case CXCursor_TypeAliasDecl:
    case CXCursor_TypeAliasTemplateDecl:
        return SymbolKind::Typedef;
    case CXCursor_FunctionDecl:
    case CXCursor_FunctionTemplate: {
        const QString name = take(clang_getCursorSpelling(c));
        if (name.startsWith(QLatin1String("operator")) && name.size() > 8 && !name[8].isLetterOrNumber()
            && name[8] != QLatin1Char('_'))
            return SymbolKind::Operator;
        if (clang_getCursorKind(c) == CXCursor_FunctionTemplate) {
            switch (clang_getTemplateCursorKind(c)) {
            case CXCursor_CXXMethod: return SymbolKind::Method;
            case CXCursor_Constructor: return SymbolKind::Constructor;
            case CXCursor_Destructor: return SymbolKind::Destructor;
            default: break;
            }
        }
        return SymbolKind::Function;
    }
    case CXCursor_CXXMethod: {
        const QString name = take(clang_getCursorSpelling(c));
        if (name.startsWith(QLatin1String("operator")) && name.size() > 8 && !name[8].isLetterOrNumber()
            && name[8] != QLatin1Char('_'))
            return SymbolKind::Operator;
        return SymbolKind::Method;
    }
    case CXCursor_ConversionFunction: return SymbolKind::Operator;
    case CXCursor_Constructor: return SymbolKind::Constructor;
    case CXCursor_Destructor: return SymbolKind::Destructor;
    case CXCursor_FieldDecl: return SymbolKind::Field;
    case CXCursor_VarDecl: return SymbolKind::Variable;
    case CXCursor_MacroDefinition: return SymbolKind::Macro;
    case CXCursor_ConceptDecl: return SymbolKind::Concept;
    default: return SymbolKind::Unknown;
    }
}

Access accessOf(CXCursor c)
{
    switch (clang_getCXXAccessSpecifier(c)) {
    case CX_CXXPublic: return Access::Public;
    case CX_CXXProtected: return Access::Protected;
    case CX_CXXPrivate: return Access::Private;
    default: return Access::None;
    }
}

QString signatureOf(CXCursor c, SymbolKind k)
{
    QString display = take(clang_getCursorDisplayName(c));
    if (!isFunction(k))
        return display;
    QString result;
    const CXCursorKind ck = clang_getCursorKind(c);
    if (k != SymbolKind::Constructor && k != SymbolKind::Destructor && ck != CXCursor_ConversionFunction) {
        const CXType rt = clang_getCursorResultType(c);
        if (rt.kind != CXType_Invalid)
            result = take(clang_getTypeSpelling(rt)) + QLatin1Char(' ');
    }
    QString tail;
    if (ck == CXCursor_CXXMethod || ck == CXCursor_ConversionFunction) {
        if (clang_CXXMethod_isConst(c))
            tail += QStringLiteral(" const");
        if (clang_CXXMethod_isPureVirtual(c))
            tail += QStringLiteral(" = 0");
    }
    return result + display + tail;
}

struct Position {
    CXFile file = nullptr;
    unsigned line = 0, col = 0;
};

Position expansion(CXSourceLocation loc)
{
    Position p;
    clang_getExpansionLocation(loc, &p.file, &p.line, &p.col, nullptr);
    return p;
}

// --- Measuring function bodies -------------------------------------------

quint64 fnv(const QByteArray &bytes, quint64 h = 1469598103934665603ULL)
{
    for (char ch : bytes) {
        h ^= quint8(ch);
        h *= 1099511628211ULL;
    }
    return h;
}

quint32 mix32(quint32 x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

struct BodyWalker {
    FunctionMetrics *m = nullptr;
    int depth = 0;

    static CXChildVisitResult visit(CXCursor c, CXCursor parent, CXClientData data)
    {
        auto *w = static_cast<BodyWalker *>(data);
        FunctionMetrics &m = *w->m;
        const CXCursorKind k = clang_getCursorKind(c);
        const CXCursorKind pk = clang_getCursorKind(parent);
        if (pk == CXCursor_CompoundStmt && clang_isStatement(k))
            m.statements++;
        bool nests = false;
        switch (k) {
        case CXCursor_IfStmt:
            m.complexity++;
            if (pk == CXCursor_IfStmt) {   // else if: a branch of the same decision
                m.cognitive += 1;
                return CXChildVisit_Recurse;
            }
            m.cognitive += 1 + w->depth;
            nests = true;
            break;
        case CXCursor_ForStmt:
        case CXCursor_CXXForRangeStmt:
        case CXCursor_WhileStmt:
        case CXCursor_DoStmt:
        case CXCursor_CXXCatchStmt:
            m.complexity++;
            m.cognitive += 1 + w->depth;
            nests = true;
            break;
        case CXCursor_SwitchStmt:
            m.cognitive += 1 + w->depth;
            nests = true;
            break;
        case CXCursor_CaseStmt:
            m.complexity++;
            break;
        case CXCursor_ConditionalOperator:
            m.complexity++;
            m.cognitive += 1 + w->depth;
            break;
        case CXCursor_BinaryOperator: {
            const CXBinaryOperatorKind op = clang_getCursorBinaryOperatorKind(c);
            if (op == CXBinaryOperator_LAnd || op == CXBinaryOperator_LOr) {
                m.complexity++;
                m.cognitive++;
            }
            break;
        }
        case CXCursor_GotoStmt:
            m.cognitive++;
            break;
        case CXCursor_ReturnStmt:
            m.returns++;
            break;
        case CXCursor_LambdaExpr:
            nests = true;
            break;
        default:
            break;
        }
        if (nests) {
            w->depth++;
            m.nesting = std::max(m.nesting, w->depth);
            clang_visitChildren(c, &BodyWalker::visit, w);
            w->depth--;
            return CXChildVisit_Continue;
        }
        return CXChildVisit_Recurse;
    }
};

CXChildVisitResult findBody(CXCursor c, CXCursor, CXClientData data)
{
    const CXCursorKind k = clang_getCursorKind(c);
    if (k == CXCursor_CompoundStmt || k == CXCursor_CXXTryStmt) {
        *static_cast<CXCursor *>(data) = c;
        return CXChildVisit_Break;
    }
    return CXChildVisit_Continue;
}

// Tokens of the body with names and literals made alike: the same
// sequence means the same code with other names
void cloneSignature(CXTranslationUnit tu, CXCursor body, FunctionMetrics &m)
{
    CXToken *tokens = nullptr;
    unsigned n = 0;
    clang_tokenize(tu, clang_getCursorExtent(body), &tokens, &n);
    if (!tokens)
        return;
    std::vector<quint32> norm;
    norm.reserve(n);
    for (unsigned i = 0; i < n; ++i) {
        switch (clang_getTokenKind(tokens[i])) {
        case CXToken_Comment:
            break;
        case CXToken_Identifier:
            norm.push_back(1);
            break;
        case CXToken_Literal:
            norm.push_back(2);
            break;
        default: {
            const QByteArray sp = take(clang_getTokenSpelling(tu, tokens[i])).toUtf8();
            norm.push_back(quint32(fnv(sp)) | 0x80000000U);
            break;
        }
        }
    }
    clang_disposeTokens(tu, tokens, n);
    m.tokens = qint32(norm.size());
    quint64 h = 1469598103934665603ULL;
    for (quint32 t : norm) {
        h ^= t;
        h *= 1099511628211ULL;
    }
    m.cloneHash = h;
    constexpr int kShingle = 6;
    constexpr int kHashes = 32;
    if (norm.size() < 40)
        return;
    std::array<quint32, kHashes> mins;
    mins.fill(0xffffffffU);
    for (size_t i = 0; i + kShingle <= norm.size(); ++i) {
        quint32 sh = 2166136261U;
        for (int j = 0; j < kShingle; ++j)
            sh = (sh ^ norm[i + j]) * 16777619U;
        for (int k = 0; k < kHashes; ++k) {
            const quint32 v = mix32(sh ^ (0x9e3779b9U * quint32(k + 1)));
            mins[k] = std::min(mins[k], v);
        }
    }
    m.minHash = QVector<quint32>(mins.begin(), mins.end());
}

// --- What one translation unit yields ------------------------------------

struct LocalSymbol {
    QString usr, name, qualified, signature, type, parentUsr;
    qint32 nameBytes = 0;
    SymbolKind kind = SymbolKind::Unknown;
    Access access = Access::None;
    quint32 flags = 0;
    Location decl, def;
    FunctionMetrics metrics;
    bool bodySeen = false;
    bool inProject = false;
};

struct LocalFile {
    QString path;
    bool project = false;
    bool skip = false;   // another unit indexes what is in it
    bool system = false;
    bool main = false;
    QList<Include> includes;
};

struct Shared {
    QString rootSlash;
    QString buildSlash;

    // Each project file is indexed by the first unit that sees it: a header
    // included by a hundred units is read once, not a hundred times
    QMutex claimMutex;
    QHash<QString, int> claims;
    bool claim(const QString &path, int unit)
    {
        QMutexLocker lock(&claimMutex);
        const auto it = claims.constFind(path);
        if (it != claims.cend())
            return *it == unit;
        claims.insert(path, unit);
        return true;
    }
    void release(int unit)
    {
        QMutexLocker lock(&claimMutex);
        for (auto it = claims.begin(); it != claims.end();) {
            if (*it == unit)
                it = claims.erase(it);
            else
                ++it;
        }
    }
};

struct Unit {
    Shared *shared = nullptr;
    int index = -1;
    QString config;   // what the preprocessor sees differently in this unit
    const CompileUnit *cu = nullptr;
    std::atomic<bool> *cancel = nullptr;
    CXTranslationUnit tu = nullptr;

    QVector<LocalFile> files;
    QHash<QString, qint32> fileIds;
    QHash<void *, qint32> cxFiles;
    QVector<LocalSymbol> symbols;
    QHash<QString, qint32> usrIds;
    QHash<qint32, qint32> parents;   // local ids
    QVector<Reference> refs;
    QVector<Diagnostic> diags;
    QVector<CodeModel::NameUse> macroNames;   // local ids
    qint32 mainFile = -1;

    qint32 file(CXFile f)
    {
        if (!f)
            return -1;
        const auto it = cxFiles.constFind(f);
        if (it != cxFiles.cend())
            return *it;
        QString path = take(clang_getFileName(f));
        if (!QDir::isAbsolutePath(path))
            path = cu->directory + QLatin1Char('/') + path;
        path = QDir::cleanPath(path);
        qint32 id = fileIds.value(path, -1);
        if (id < 0) {
            LocalFile lf;
            lf.path = path;
            lf.project = path.startsWith(shared->rootSlash);
            // Per configuration: a header compiled with other defines is other code
            lf.skip = lf.project && !shared->claim(path + config, index);
            files.append(lf);
            id = qint32(files.size() - 1);
            fileIds.insert(path, id);
        }
        cxFiles.insert(f, id);
        return id;
    }

    bool inProject(qint32 f) const { return f >= 0 && files[f].project; }
    // What this unit records: project files it claimed
    bool ours(qint32 f) const { return f >= 0 && files[f].project && !files[f].skip; }

    // The identifier written at a location, from the file itself
    QHash<qint32, QList<QByteArray>> sources;
    const QList<QByteArray> *sourceOf(qint32 file)
    {
        auto it = sources.find(file);
        if (it == sources.end()) {
            QFile f(files[file].path);
            QList<QByteArray> lines;
            if (f.open(QIODevice::ReadOnly))
                lines = f.readAll().split('\n');
            it = sources.insert(file, lines);
        }
        return &*it;
    }

    // Qt's meta-object system calls what moc and QML see: names in
    // Q_PROPERTY, Q_ENUM and Q_FLAG, per class
    QHash<QString, QSet<QByteArray>> metaNames;
    void scanMetaObject(const QString &usr, const Location &ext)
    {
        const QList<QByteArray> *src = sourceOf(ext.file);
        if (!src || ext.line < 1)
            return;
        QByteArray body;
        for (qint32 l = ext.line; l <= ext.endLine && l <= src->size(); ++l)
            body += src->at(l - 1) + ' ';
        QSet<QByteArray> &names = metaNames[usr];
        static const QList<QByteArray> macros = {"Q_PROPERTY(", "Q_ENUM(", "Q_FLAG(", "Q_ENUM_NS(", "Q_FLAG_NS("};
        for (const QByteArray &mac : macros) {
            qsizetype at = 0;
            while ((at = body.indexOf(mac, at)) >= 0) {
                const qsizetype end = body.indexOf(')', at);
                if (end < 0)
                    break;
                const QByteArray inner = body.mid(at + mac.size(), end - at - mac.size());
                const QList<QByteArray> words = inner.simplified().split(' ');
                if (mac == "Q_PROPERTY(") {
                    static const QSet<QByteArray> keys = {"READ", "WRITE", "RESET", "NOTIFY", "MEMBER", "BINDABLE"};
                    for (qsizetype i = 0; i + 1 < words.size(); ++i) {
                        if (keys.contains(words[i]))
                            names.insert(words[i + 1]);
                    }
                } else {
                    for (const QByteArray &w : words)
                        names.insert(w);
                }
                at = end;
            }
        }
        if (body.contains("QML_ELEMENT") || body.contains("QML_NAMED_ELEMENT") || body.contains("QML_SINGLETON")
            || body.contains("QML_ANONYMOUS") || body.contains("QML_UNCREATABLE") || body.contains("QML_VALUE_TYPE"))
            names.insert(QByteArrayLiteral("<qml>"));
    }

    // A method declared Q_INVOKABLE, or in a slots or signals section
    bool metaCallableAt(const Location &at)
    {
        const QList<QByteArray> *src = sourceOf(at.file);
        if (!src || at.line < 1 || at.line > src->size())
            return false;
        const QByteArray line = src->at(at.line - 1);
        if (line.contains("Q_INVOKABLE") || line.contains("Q_SLOT") || line.contains("Q_SIGNAL")
            || line.contains("Q_SCRIPTABLE"))
            return true;
        // Which lines are in a slots or signals section: once per file
        auto it = metaSections.find(at.file);
        if (it == metaSections.end()) {
            QVector<char> in(src->size(), 0);
            char state = 0;
            for (qsizetype l = 0; l < src->size(); ++l) {
                const QByteArray &raw = src->at(l);
                if (raw.contains(':') || raw.contains("class ") || raw.contains("struct ")) {
                    const QByteArray t = raw.trimmed();
                    if (t.endsWith(':') && (t.startsWith("public") || t.startsWith("protected") || t.startsWith("private")
                                            || t.startsWith("signals") || t.startsWith("Q_SIGNALS") || t.startsWith("Q_SLOTS")))
                        state = t.contains("slots") || t.contains("Q_SLOTS") || t.contains("signals") || t.contains("Q_SIGNALS");
                    else if (t.startsWith("class ") || t.startsWith("struct "))
                        state = 0;
                }
                in[l] = state;
            }
            it = metaSections.insert(at.file, in);
        }
        return it->value(at.line - 1) != 0;
    }
    QHash<qint32, QVector<char>> metaSections;

    QByteArray wordAt(const Location &at)
    {
        if (at.file < 0)
            return {};
        const QList<QByteArray> *src = sourceOf(at.file);
        if (at.line < 1 || at.line > src->size())
            return {};
        const QByteArray &line = src->at(at.line - 1);
        qsizetype i = at.col - 1, j = i;
        if (i < 0 || i >= line.size())
            return {};
        while (j < line.size() && (std::isalnum(uchar(line[j])) || line[j] == '_'))
            ++j;
        return line.mid(i, j - i);
    }

    Location location(CXSourceLocation loc)
    {
        const Position p = expansion(loc);
        Location l;
        l.file = file(p.file);
        l.line = qint32(p.line);
        l.col = qint32(p.col);
        return l;
    }

    Location extent(CXCursor c)
    {
        const CXSourceRange r = clang_getCursorExtent(c);
        Location l = location(clang_getRangeStart(r));
        const Position e = expansion(clang_getRangeEnd(r));
        if (file(e.file) == l.file) {
            l.endLine = qint32(e.line);
            l.endCol = qint32(e.col);
        } else {
            l.endLine = l.line;
        }
        return l;
    }

    // The symbol for a cursor, made (with its parents) if this unit hasn't
    // seen it declared: what project code uses from other code
    qint32 symbolFor(CXCursor c, int depth = 0, const char *usrHint = nullptr)
    {
        if (clang_Cursor_isNull(c) || depth > 24)
            return -1;
        const CXCursorKind ck = clang_getCursorKind(c);
        if (ck == CXCursor_TranslationUnit || clang_isInvalid(ck))
            return -1;
        // clang hands the indexer callbacks the USR already: no need to make it again
        const QString usr = usrHint && *usrHint ? QString::fromUtf8(usrHint) : usrOf(c);
        if (usr.isEmpty())
            return -1;
        const auto it = usrIds.constFind(usr);
        if (it != usrIds.cend())
            return *it;
        const SymbolKind k = kindOf(c);
        if (k == SymbolKind::Unknown)
            return -1;
        LocalSymbol s;
        s.usr = usr;
        s.kind = k;
        s.name = take(clang_getCursorSpelling(c));
        // Unnamed: clang 18 spells them "(unnamed enum at file:line:col)"
        if (s.name.isEmpty() || s.name.startsWith(QLatin1String("(unnamed")) || s.name.startsWith(QLatin1String("(anonymous")))
            s.name = k == SymbolKind::Namespace ? QStringLiteral("(anonymous namespace)") : QStringLiteral("(anonymous)");
        s.nameBytes = qint32(s.name.toUtf8().size());
        s.signature = signatureOf(c, k);
        const Location at = location(clang_getCursorLocation(c));
        s.inProject = inProject(at.file);
        s.decl = at;
        s.access = accessOf(c);
        if (!s.inProject)
            s.flags |= External;
        symbols.append(s);
        const qint32 id = qint32(symbols.size() - 1);
        usrIds.insert(usr, id);
        const qint32 parent = symbolFor(semanticParent(c), depth + 1);
        if (parent >= 0) {
            symbols[id].parentUsr = symbols[parent].usr;
            parents.insert(id, parent);
        }
        return id;
    }

    // The declared symbol a reference is made in: walks out of lambdas
    // and other local entities to the function they are in
    qint32 ownerFor(CXCursor c, const char *usrHint = nullptr)
    {
        if (usrHint && *usrHint) {
            const auto it = usrIds.constFind(QString::fromUtf8(usrHint));
            if (it != usrIds.cend())
                return *it;
        }
        int guard = 0;
        while (!clang_Cursor_isNull(c) && guard++ < 32) {
            const CXCursorKind k = clang_getCursorKind(c);
            if (k == CXCursor_TranslationUnit || clang_isInvalid(k))
                return -1;
            if (!isLocal(c) && k != CXCursor_LambdaExpr) {
                const QString usr = usrOf(c);
                const auto it = usrIds.constFind(usr);
                if (it != usrIds.cend())
                    return *it;
                if (!usr.isEmpty() && kindOf(c) != SymbolKind::Unknown)
                    return symbolFor(c);
            }
            c = clang_getCursorSemanticParent(c);
        }
        return -1;
    }

    void declare(const CXIdxDeclInfo *info)
    {
        const CXCursor c = info->cursor;
        const SymbolKind k = kindOf(c);
        if (k == SymbolKind::Unknown)
            return;
        const Location at = location(clang_getCursorLocation(c));
        if (!ours(at.file))
            return;   // other code's declarations come in as they are used; claimed files from their unit
        if (isLocal(c))
            return;
        const qint32 id = symbolFor(c, 0, info->entityInfo ? info->entityInfo->USR : nullptr);
        if (id < 0)
            return;
        LocalSymbol &s = symbols[id];
        s.inProject = true;
        s.flags &= ~External;
        if (!s.decl.valid() || !inProject(s.decl.file))
            s.decl = at;
        if (s.access == Access::None)
            s.access = accessOf(c);
        if (info->isImplicit)
            s.flags |= Implicit;
        // Declared by a macro (Q_OBJECT, Q_ENUM…): the source there doesn't
        // spell the name
        const qint32 parentId = parents.value(id, -1);
        if (parentId >= 0 && (symbols[parentId].flags & FromMacro)) {
            s.flags |= FromMacro;
        } else if (!s.name.startsWith(QLatin1Char('('))) {
            const QByteArray spelled = wordAt(at);
            const QByteArray name = s.name.toUtf8();
            if (!spelled.isEmpty() && spelled != name && !name.startsWith(spelled) && !name.startsWith('~'))
                s.flags |= FromMacro;
        }
        if (info->entityInfo && info->entityInfo->templateKind != CXIdxEntity_NonTemplate)
            s.flags |= Template;
        const CXCursorKind ck = clang_getCursorKind(c);
        if (ck == CXCursor_CXXMethod || ck == CXCursor_ConversionFunction || ck == CXCursor_Destructor
            || ck == CXCursor_Constructor || ck == CXCursor_FunctionTemplate) {
            if (clang_CXXMethod_isVirtual(c))
                s.flags |= Virtual;
            if (clang_CXXMethod_isPureVirtual(c))
                s.flags |= PureVirtual | Virtual;
            if (clang_CXXMethod_isStatic(c))
                s.flags |= Static;
            if (clang_CXXMethod_isConst(c))
                s.flags |= Const;
            if (clang_CXXMethod_isDefaulted(c))
                s.flags |= Defaulted;
            if (clang_CXXMethod_isDeleted(c))
                s.flags |= Deleted;
            if (clang_CXXMethod_isExplicit(c))
                s.flags |= Explicit;
        }
        if ((k == SymbolKind::Method || k == SymbolKind::Constructor) && metaCallableAt(at))
            s.flags |= MetaCallable;
        if (isFunction(k)) {
            if (clang_Cursor_getStorageClass(c) == CX_SC_Static)
                s.flags |= Static;
            if (clang_Cursor_isFunctionInlined(c))
                s.flags |= Inline;
            const CXType ft = clang_getCursorType(c);
            const int spec = clang_getCursorExceptionSpecificationType(c);
            if (spec == CXCursor_ExceptionSpecificationKind_BasicNoexcept
                || spec == CXCursor_ExceptionSpecificationKind_ComputedNoexcept
                || spec == CXCursor_ExceptionSpecificationKind_DynamicNone)
                s.flags |= Noexcept;
            Q_UNUSED(ft)
            s.metrics.params = std::max(0, clang_Cursor_getNumArguments(c));
        }
        if (k == SymbolKind::Variable || k == SymbolKind::Field) {
            s.type = take(clang_getTypeSpelling(clang_getCursorType(c)));
            if (clang_Cursor_getStorageClass(c) == CX_SC_Static)
                s.flags |= Static;
            if (clang_isConstQualifiedType(clang_getCursorType(c)))
                s.flags |= Const;
            if (k == SymbolKind::Field && clang_CXXField_isMutable(c))
                s.flags |= Mutable;
            if (k == SymbolKind::Variable && clang_getCursorKind(semanticParent(c)) != CXCursor_Namespace
                && clang_getCursorKind(semanticParent(c)) != CXCursor_TranslationUnit)
                s.flags |= Static;   // a static data member
        } else if (k == SymbolKind::Typedef) {
            s.type = take(clang_getTypeSpelling(clang_getTypedefDeclUnderlyingType(c)));
        } else if (k == SymbolKind::Enum) {
            s.type = take(clang_getTypeSpelling(clang_getEnumDeclIntegerType(c)));
        }

        // Where the name is, for the code view
        Reference d;
        d.to = id;
        d.kind = RefKind::Declaration;
        d.loc = at;
        d.loc.endLine = at.line;
        d.loc.endCol = at.col + s.nameBytes;
        if (info->isDefinition)
            d.flags |= RefWrite;
        refs.append(d);

        if (info->isDefinition) {
            const Location ext = extent(c);
            bool body = false;
            FunctionMetrics m = s.metrics;
            if (isFunction(k)) {
                CXCursor b = clang_getNullCursor();
                clang_visitChildren(c, &findBody, &b);
                if (!clang_Cursor_isNull(b)) {
                    body = true;
                    m.complexity = 1;
                    m.statements = m.nesting = m.returns = m.cognitive = 0;
                    BodyWalker w;
                    w.m = &m;
                    clang_visitChildren(b, &BodyWalker::visit, &w);
                    cloneSignature(clang_Cursor_getTranslationUnit(c), b, m);
                }
                m.lines = ext.endLine >= ext.line ? ext.endLine - ext.line + 1 : 0;
            } else if (isType(k)) {
                m.lines = ext.endLine >= ext.line ? ext.endLine - ext.line + 1 : 0;
            }
            if (!s.def.valid() || (body && !s.bodySeen)) {
                s.def = ext;
                s.metrics = m;
                s.bodySeen = s.bodySeen || body;
            }
            s.flags |= Defined;
            if (isType(k) && k != SymbolKind::Typedef)
                scanMetaObject(s.usr, ext);
        }

        // Bases
        if (const CXIdxCXXClassDeclInfo *cls = clang_index_getCXXClassDeclInfo(info)) {
            for (unsigned i = 0; i < cls->numBases; ++i) {
                const CXIdxBaseClassInfo *b = cls->bases[i];
                if (!b || !b->base)
                    continue;
                const qint32 base = symbolFor(b->base->cursor);
                if (base < 0)
                    continue;
                Reference r;
                r.from = id;
                r.to = base;
                r.kind = RefKind::Inherit;
                r.loc = location(clang_getCursorLocation(b->cursor));
                r.loc.endLine = r.loc.line;
                r.loc.endCol = r.loc.col;
                if (clang_isVirtualBase(b->cursor))
                    r.flags |= RefVirtualBase;
                refs.append(r);
            }
        }
        // What it overrides
        if (k == SymbolKind::Method || k == SymbolKind::Operator || k == SymbolKind::Destructor) {
            CXCursor *overridden = nullptr;
            unsigned n = 0;
            clang_getOverriddenCursors(c, &overridden, &n);
            for (unsigned i = 0; i < n; ++i) {
                const qint32 base = symbolFor(overridden[i]);
                if (base < 0)
                    continue;
                Reference r;
                r.from = id;
                r.to = base;
                r.kind = RefKind::Override;
                r.loc = d.loc;
                refs.append(r);
                symbols[base].flags |= Virtual;
            }
            if (overridden)
                clang_disposeOverriddenCursors(overridden);
        }
    }

    void reference(const CXIdxEntityRefInfo *info)
    {
        if (!info->referencedEntity)
            return;
        const Location at = location(clang_indexLoc_getCXSourceLocation(info->loc));
        if (!ours(at.file))
            return;
        const CXCursor target = info->referencedEntity->cursor;
        // Known already: its kind too (and it isn't a local)
        qint32 to = -1;
        SymbolKind k = SymbolKind::Unknown;
        const char *usr = info->referencedEntity->USR;
        if (usr && *usr) {
            const auto known = usrIds.constFind(QString::fromUtf8(usr));
            if (known != usrIds.cend()) {
                to = *known;
                k = symbols[to].kind;
            }
        }
        if (to < 0) {
            k = kindOf(target);
            if (k == SymbolKind::Unknown || k == SymbolKind::Namespace)
                return;
            if (isLocal(target))
                return;   // a local variable or a lambda's capture
            to = symbolFor(target, 0, usr);
            if (to < 0)
                return;
        }
        if (k == SymbolKind::Namespace)
            return;
        Reference r;
        r.to = to;
        r.from = info->parentEntity ? ownerFor(info->parentEntity->cursor, info->parentEntity->USR) : -1;
        r.loc = at;
        r.loc.endLine = at.line;
        r.loc.endCol = at.col + symbols[to].nameBytes;
        const int role = int(info->role);
        if (info->kind == CXIdxEntityRef_Implicit || (role & CXSymbolRole_Implicit))
            r.flags |= RefImplicit;
        if (isFunction(k)) {
            r.kind = RefKind::Call;
            if (role & CXSymbolRole_AddressOf)
                r.flags |= RefAddressOf;
            if (role & CXSymbolRole_Dynamic)
                r.flags |= RefDynamic;
        } else if (isType(k)) {
            r.kind = RefKind::TypeUse;
        } else if (isVariable(k)) {
            r.kind = RefKind::VarUse;
            if (role & CXSymbolRole_Read)
                r.flags |= RefRead;
            if (role & CXSymbolRole_Write)
                r.flags |= RefWrite;
            if (!(role & (CXSymbolRole_Read | CXSymbolRole_Write)))
                r.flags |= RefRead;
        } else {
            return;
        }
        if (r.from == to && r.kind == RefKind::TypeUse)
            return;   // a class naming itself
        refs.append(r);
    }

    void include(const CXIdxIncludedFileInfo *info)
    {
        const Location at = location(clang_indexLoc_getCXSourceLocation(info->hashLoc));
        const qint32 target = file(info->file);
        // Only a project file's includes, once: system headers include
        // hundreds of others in every unit
        if (at.file < 0 || target < 0 || !ours(at.file))
            return;
        files[at.file].includes.append({target, at.line});
        if (clang_Location_isInSystemHeader(clang_indexLoc_getCXSourceLocation(info->hashLoc)))
            files[at.file].system = true;
    }

    void diagnostics(CXDiagnosticSet set)
    {
        const unsigned n = clang_getNumDiagnosticsInSet(set);
        for (unsigned i = 0; i < n && diags.size() < 400; ++i) {
            CXDiagnostic d = clang_getDiagnosticInSet(set, i);
            const CXDiagnosticSeverity sev = clang_getDiagnosticSeverity(d);
            if (sev >= CXDiagnostic_Warning) {
                Diagnostic out;
                out.severity = sev == CXDiagnostic_Fatal ? Diagnostic::Fatal
                             : sev == CXDiagnostic_Error ? Diagnostic::Error : Diagnostic::Warning;
                out.loc = location(clang_getDiagnosticLocation(d));
                const bool project = inProject(out.loc.file);
                if (project || out.severity >= Diagnostic::Error) {
                    out.message = take(clang_getDiagnosticSpelling(d));
                    out.category = take(clang_getDiagnosticCategoryText(d));
                    out.unit = mainFile;
                    diags.append(out);
                }
            }
            clang_disposeDiagnostic(d);
        }
    }

    // Macros: clang's indexer leaves them out, the preprocessing record has them
    void macros()
    {
        if (!tu)
            return;
        clang_visitChildren(clang_getTranslationUnitCursor(tu), [](CXCursor c, CXCursor, CXClientData data) {
            auto *u = static_cast<Unit *>(data);
            const CXCursorKind k = clang_getCursorKind(c);
            if (k == CXCursor_MacroDefinition) {
                if (clang_Cursor_isMacroBuiltin(c))
                    return CXChildVisit_Continue;
                const Location at = u->location(clang_getCursorLocation(c));
                if (!u->ours(at.file))
                    return CXChildVisit_Continue;
                const qint32 id = u->symbolFor(c);
                if (id < 0)
                    return CXChildVisit_Continue;
                LocalSymbol &s = u->symbols[id];
                s.inProject = true;
                s.flags = (s.flags & ~External) | Defined;
                s.decl = at;
                s.def = u->extent(c);
                s.metrics.lines = s.def.endLine >= s.def.line ? s.def.endLine - s.def.line + 1 : 1;
                s.signature = clang_Cursor_isMacroFunctionLike(c) ? s.name + QStringLiteral("(…)") : s.name;
                // The names in its definition: other macros it uses
                {
                    CXToken *tokens = nullptr;
                    unsigned n = 0;
                    clang_tokenize(u->tu, clang_getCursorExtent(c), &tokens, &n);
                    QSet<QString> names;
                    for (unsigned t = 1; t < n; ++t) {
                        if (clang_getTokenKind(tokens[t]) != CXToken_Identifier)
                            continue;
                        const QString name = take(clang_getTokenSpelling(u->tu, tokens[t]));
                        if (name != s.name && !names.contains(name)) {
                            names.insert(name);
                            u->macroNames.append({id, name, at});
                        }
                    }
                    if (tokens)
                        clang_disposeTokens(u->tu, tokens, n);
                }
                Reference d;
                d.to = id;
                d.kind = RefKind::Declaration;
                d.flags = RefWrite;
                d.loc = at;
                d.loc.endLine = at.line;
                d.loc.endCol = at.col + s.nameBytes;
                u->refs.append(d);
            } else if (k == CXCursor_MacroExpansion) {
                const Location at = u->location(clang_getCursorLocation(c));
                if (!u->ours(at.file))
                    return CXChildVisit_Continue;
                const CXCursor def = clang_getCursorReferenced(c);
                if (clang_Cursor_isNull(def))
                    return CXChildVisit_Continue;
                qint32 to = u->usrIds.value(usrOf(def), -1);
                // Defined in a header another unit indexes
                if (to < 0 && u->inProject(u->location(clang_getCursorLocation(def)).file))
                    to = u->symbolFor(def);
                if (to < 0 || !u->symbols[to].inProject)
                    return CXChildVisit_Continue;
                const auto it = &to;
                Reference r;
                r.to = *it;
                r.kind = RefKind::MacroUse;
                r.loc = at;
                r.loc.endLine = at.line;
                r.loc.endCol = at.col + u->symbols[*it].nameBytes;
                u->refs.append(r);
            }
            return CXChildVisit_Continue;
        }, this);
    }
};

// --- libclang's callbacks --------------------------------------------------

int cbAbort(CXClientData data, void *)
{
    auto *u = static_cast<Unit *>(data);
    return u->cancel && u->cancel->load() ? 1 : 0;
}

void cbDiagnostic(CXClientData data, CXDiagnosticSet set, void *)
{
    static_cast<Unit *>(data)->diagnostics(set);
}

CXIdxClientFile cbMainFile(CXClientData data, CXFile file, void *)
{
    auto *u = static_cast<Unit *>(data);
    u->mainFile = u->file(file);
    if (u->mainFile >= 0)
        u->files[u->mainFile].main = true;
    return nullptr;
}

CXIdxClientFile cbInclude(CXClientData data, const CXIdxIncludedFileInfo *info)
{
    static_cast<Unit *>(data)->include(info);
    return nullptr;
}

CXIdxClientASTFile cbImported(CXClientData, const CXIdxImportedASTFileInfo *)
{
    return nullptr;
}

CXIdxClientContainer cbStarted(CXClientData, void *)
{
    return nullptr;
}

void cbDeclaration(CXClientData data, const CXIdxDeclInfo *info)
{
    static_cast<Unit *>(data)->declare(info);
}

void cbReference(CXClientData data, const CXIdxEntityRefInfo *info)
{
    static_cast<Unit *>(data)->reference(info);
}

IndexerCallbacks callbacks()
{
    IndexerCallbacks cb = {};
    cb.abortQuery = cbAbort;
    cb.diagnostic = cbDiagnostic;
    cb.enteredMainFile = cbMainFile;
    cb.ppIncludedFile = cbInclude;
    cb.importedASTFile = cbImported;
    cb.startedTranslationUnit = cbStarted;
    cb.indexDeclaration = cbDeclaration;
    cb.indexEntityReference = cbReference;
    return cb;
}

// --- Merging a unit into the model -----------------------------------------

void merge(Unit &u, CodeModel &model)
{
    QVector<qint32> fileMap(u.files.size());
    for (qint32 i = 0; i < u.files.size(); ++i) {
        const LocalFile &lf = u.files[i];
        fileMap[i] = model.addFile(lf.path);
        File &f = model.files[fileMap[i]];
        f.indexed = f.indexed || lf.main;
        f.system = f.system || lf.system;
    }
    for (qint32 i = 0; i < u.files.size(); ++i) {
        File &f = model.files[fileMap[i]];
        for (const Include &inc : std::as_const(u.files[i].includes))
            f.includes.append({fileMap[inc.file], inc.line});
    }
    auto mapLoc = [&](Location l) {
        if (l.file >= 0)
            l.file = fileMap[l.file];
        return l;
    };

    QVector<qint32> symMap(u.symbols.size());
    for (qint32 i = 0; i < u.symbols.size(); ++i)
        symMap[i] = model.addSymbol(u.symbols[i].usr);
    for (qint32 i = 0; i < u.symbols.size(); ++i) {
        const LocalSymbol &l = u.symbols[i];
        Symbol &g = model.symbols[symMap[i]];
        const bool fresh = g.kind == SymbolKind::Unknown;
        const bool takeOver = fresh || (l.inProject && g.has(External));
        if (takeOver) {
            g.name = l.name;
            g.signature = l.signature;
            g.kind = l.kind;
            g.decl = mapLoc(l.decl);
            if (fresh)
                g.flags = l.flags;
            g.flags = l.inProject ? (g.flags & ~External) : (g.flags | External);
        }
        g.flags |= (l.flags & ~External);
        if (g.signature.isEmpty())
            g.signature = l.signature;
        if (g.type.isEmpty())
            g.type = l.type;
        if (g.access == Access::None)
            g.access = l.access;
        if (!g.decl.valid())
            g.decl = mapLoc(l.decl);
        if (g.parent < 0 && !l.parentUsr.isEmpty()) {
            const qint32 local = u.usrIds.value(l.parentUsr, -1);
            if (local >= 0 && symMap[local] != symMap[i])
                g.parent = symMap[local];
        }
        if (l.def.valid()) {
            const bool haveBody = g.metrics.statements > 0 || g.metrics.tokens > 0;
            if (!g.def.valid() || (l.bodySeen && !haveBody)) {
                g.def = mapLoc(l.def);
                const qint32 params = std::max(g.metrics.params, l.metrics.params);
                g.metrics = l.metrics;
                g.metrics.params = params;
            }
        } else if (g.metrics.params == 0) {
            g.metrics.params = l.metrics.params;
        }
        if (l.qualified.size() && g.qualified.isEmpty())
            g.qualified = l.qualified;
    }
    model.refs.reserve(model.refs.size() + u.refs.size());
    for (const Reference &r : std::as_const(u.refs)) {
        Reference g = r;
        g.from = r.from >= 0 ? symMap[r.from] : -1;
        g.to = r.to >= 0 ? symMap[r.to] : -1;
        g.loc = mapLoc(r.loc);
        model.refs.append(g);
    }
    for (const CodeModel::NameUse &n : std::as_const(u.macroNames))
        model.macroNameUses.append({symMap[n.from], n.name, mapLoc(n.loc)});
    for (const Diagnostic &d : std::as_const(u.diags)) {
        Diagnostic g = d;
        g.loc = mapLoc(d.loc);
        g.unit = d.unit >= 0 ? fileMap[d.unit] : -1;
        model.diagnostics.append(g);
    }
}

QVector<QByteArray> toArgv(const QStringList &args)
{
    QVector<QByteArray> out;
    out.reserve(args.size());
    for (const QString &a : args)
        out.append(a.toUtf8());
    return out;
}

// clang's own headers (stddef.h, the intrinsics) for the libclang that is
// loaded. libclang looks for them beside itself, which works where LLVM is
// installed but not where a package put libclang next to the program, so a
// copy in the package is named explicitly: lib/clang/<major> beside the
// program (Windows), ../lib/clang/<major> (Linux, the AppImage and the
// Debian package) or ../Resources/clang/<major> (a macOS bundle). Only
// one for the same major version as libclang's.
QString resourceDir()
{
    static const QString dir = [] {
        const QString given = qEnvironmentVariable("UNRAVEL_CLANG_RESOURCE_DIR");
        if (!given.isEmpty() || !QCoreApplication::instance())
            return given;
        CXString version = clang_getClangVersion();
        const QString text = QString::fromUtf8(clang_getCString(version));
        clang_disposeString(version);
        const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("version (\\d+)")).match(text);
        if (!m.hasMatch())
            return QString();
        const QString app = QCoreApplication::applicationDirPath(), major = m.captured(1);
        for (const QString &c : {app + QStringLiteral("/lib/clang/"), app + QStringLiteral("/../lib/clang/"),
                                 app + QStringLiteral("/../Resources/clang/")}) {
            if (QFileInfo::exists(c + major + QStringLiteral("/include/stddef.h")))
                return QDir::cleanPath(c + major);
        }
        // Run from the build folder: the LLVM installation it was built with
        const QString built = QStringLiteral(UNRAVEL_CLANG_BUILD_RESOURCE_DIR);
        if (!built.isEmpty() && QFileInfo::exists(built + QStringLiteral("/include/stddef.h")))
            return built;
        return QString();
    }();
    return dir;
}

// What a compiler on this system adds by itself and libclang doesn't: on
// macOS the SDK, where the C and C++ headers are (clang finds it through
// xcrun or its configuration files, which libclang reads neither of); on
// Windows a pass on the compiler version check of Visual Studio's headers
QStringList platformArguments()
{
#ifdef Q_OS_MACOS
    static const QStringList args = [] {
        QString sdk = qEnvironmentVariable("SDKROOT");
        if (sdk.isEmpty()) {
            QProcess xcrun;
            xcrun.start(QStringLiteral("xcrun"), {QStringLiteral("--show-sdk-path")});
            if (xcrun.waitForFinished(10000) && xcrun.exitCode() == 0)
                sdk = QString::fromLocal8Bit(xcrun.readAllStandardOutput()).trimmed();
        }
        if (sdk.isEmpty() && QFileInfo::exists(QStringLiteral("/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk")))
            sdk = QStringLiteral("/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk");
        return sdk.isEmpty() ? QStringList() : QStringList{QStringLiteral("-isysroot"), sdk};
    }();
    return args;
#elif defined(Q_OS_WIN)
    // Visual Studio's standard library refuses a clang older than the one
    // it was released with; the bundled one may well be
    return {QStringLiteral("-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH")};
#else
    return {};
#endif
}

} // namespace

void Indexer::countLines(const QString &path, qint32 *lines, qint32 *codeLines)
{
    *lines = 0;
    *codeLines = 0;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QByteArray text = f.readAll();
    bool inBlock = false;
    qint32 n = 0, code = 0;
    qsizetype start = 0;
    while (start <= text.size()) {
        qsizetype end = text.indexOf('\n', start);
        if (end < 0)
            end = text.size();
        if (start == text.size() && end == start)
            break;
        ++n;
        bool hasCode = false;
        for (qsizetype i = start; i < end; ++i) {
            const char ch = text[i];
            if (inBlock) {
                if (ch == '*' && i + 1 < end && text[i + 1] == '/') {
                    inBlock = false;
                    ++i;
                }
                continue;
            }
            if (ch == '/' && i + 1 < end && text[i + 1] == '/')
                break;
            if (ch == '/' && i + 1 < end && text[i + 1] == '*') {
                inBlock = true;
                ++i;
                continue;
            }
            if (ch != ' ' && ch != '\t' && ch != '\r')
                hasCode = true;
        }
        if (hasCode)
            ++code;
        start = end + 1;
    }
    *lines = n;
    *codeLines = code;
}

IndexResult Indexer::run(const ProjectSetup &setup, CodeModel &model, std::atomic<bool> *cancel,
                         const Progress &progress, int threads)
{
    IndexResult result;
    QElapsedTimer timer;
    timer.start();
    model.clear();
    model.root = QDir::cleanPath(setup.root);
    model.buildDir = setup.buildDir;

    Shared shared;
    shared.rootSlash = withSlash(model.root);
    shared.buildSlash = setup.buildDir.isEmpty() ? QString() : withSlash(setup.buildDir);

    const int total = int(setup.units.size()) + (setup.pythonFiles.isEmpty() ? 0 : 1);
    if (threads <= 0)
        threads = std::clamp(QThread::idealThreadCount() - 1, 1, 12);   // each holds a parsed unit: ~100 MB with Qt
    threads = std::min(threads, std::max(1, total));

    // The preprocessor configuration of each unit: its defines, forced
    // includes and language
    const int unitCount = int(setup.units.size());
    QVector<QString> configs(unitCount);
    {
        QHash<QString, QString> ids;
        for (int i = 0; i < unitCount; ++i) {
            QStringList key;
            const QStringList &a = setup.units[i].args;
            for (qsizetype k = 0; k < a.size(); ++k) {
                const QString &x = a[k];
                if ((x == QLatin1String("-D") || x == QLatin1String("-U") || x == QLatin1String("-include")
                     || x == QLatin1String("-x")) && k + 1 < a.size())
                    key << x + a[++k];
                else if (x.startsWith(QLatin1String("-D")) || x.startsWith(QLatin1String("-U"))
                         || x.startsWith(QLatin1String("-std=")) || x.startsWith(QLatin1String("-include")))
                    key << x;
            }
            key.sort();
            const QString joined = key.join(QLatin1Char('\x1f'));
            auto it = ids.find(joined);
            if (it == ids.end())
                it = ids.insert(joined, QStringLiteral("\x1f") + QString::number(ids.size()));
            configs[i] = *it;
        }
    }
    std::atomic<int> next{0};
    std::atomic<int> done{0};
    std::atomic<int> failed{0};
    QMutex mutex;
    std::atomic<qint64> waitNs{0}, mergeNs{0}, parseNs{0};

    const QString resources = resourceDir();
    const QStringList platform = platformArguments();
    auto worker = [&] {
        CXIndex index = clang_createIndex(0, 0);
        CXIndexAction action = clang_IndexAction_create(index);
        IndexerCallbacks cb = callbacks();
        for (;;) {
            if (cancel && cancel->load())
                break;
            const int i = next.fetch_add(1);
            if (i >= int(setup.units.size()))
                break;
            const CompileUnit &cu = setup.units[i];
            const QByteArray file = cu.file.toUtf8();
            const unsigned tuOptions = CXTranslationUnit_DetailedPreprocessingRecord | CXTranslationUnit_KeepGoing
                                     | CXTranslationUnit_IgnoreNonErrorsFromIncludedFiles;
            const unsigned indexOptions = CXIndexOpt_SkipParsedBodiesInSession;
            Unit unit;
            int rc = 1;
            QElapsedTimer pt;
            pt.start();
            for (int attempt = 0; attempt < 2 && rc != 0; ++attempt) {
                unit = Unit();
                unit.shared = &shared;
                unit.index = i;
                unit.config = configs[i];
                unit.cu = &cu;
                unit.cancel = cancel;
                QStringList args = attempt == 0 ? cu.args : Project::minimalArguments(cu.args)
                                                                << QStringLiteral("-ferror-limit=0");
                if (!resources.isEmpty() && !args.contains(QLatin1String("-resource-dir")))
                    args << QStringLiteral("-resource-dir") << resources;
                if (!platform.isEmpty() && !args.contains(platform.first()))
                    args << platform;
                const QVector<QByteArray> argv = toArgv(args);
                std::vector<const char *> ptrs;
                for (const QByteArray &a : argv)
                    ptrs.push_back(a.constData());
                CXTranslationUnit tu = nullptr;
                // The unit's own pointer is what the callbacks see
                rc = clang_indexSourceFile(action, &unit, &cb, sizeof(cb), indexOptions, file.constData(),
                                           ptrs.data(), int(ptrs.size()), nullptr, 0, &tu, tuOptions);
                if (rc == 0 && tu) {
                    unit.tu = tu;
                    unit.macros();
                }
                // Measuring bodies needs the unit while the callbacks run,
                // clang_indexSourceFile only hands it over at the end
                if (tu)
                    clang_disposeTranslationUnit(tu);
                unit.tu = nullptr;
                if (cancel && cancel->load())
                    break;
            }
            parseNs += pt.nsecsElapsed();
            // Members the meta-object system reaches
            for (LocalSymbol &ls : unit.symbols) {
                const auto own = unit.metaNames.constFind(ls.usr);
                if (own != unit.metaNames.cend() && own->contains(QByteArrayLiteral("<qml>")))
                    ls.flags |= MetaCallable;
                if (ls.parentUsr.isEmpty())
                    continue;
                const auto it = unit.metaNames.constFind(ls.parentUsr);
                if (it != unit.metaNames.cend() && it->contains(ls.name.toUtf8()))
                    ls.flags |= MetaCallable;
            }
            if (rc != 0) {
                failed++;
                shared.release(i);
                Diagnostic d;
                d.severity = Diagnostic::Fatal;
                d.message = QStringLiteral("clang could not parse this file (error %1)").arg(rc);
                unit.files.clear();
                unit.fileIds.clear();
                unit.cxFiles.clear();
                LocalFile lf;
                lf.path = cu.file;
                lf.project = true;
                lf.main = true;
                unit.files.append(lf);
                d.loc.file = 0;
                d.loc.line = 1;
                d.unit = 0;
                unit.symbols.clear();
                unit.usrIds.clear();
                unit.refs.clear();
                unit.macroNames.clear();
                unit.diags = {d};
            }
            {
                QElapsedTimer wait;
                wait.start();
                QMutexLocker lock(&mutex);
                waitNs += wait.nsecsElapsed();
                QElapsedTimer mt;
                mt.start();
                merge(unit, model);
                mergeNs += mt.nsecsElapsed();
            }
            const int n = ++done;
            if (progress)
                progress(n, total, cu.file);
        }
        clang_IndexAction_dispose(action);
        clang_disposeIndex(index);
    };

    std::vector<std::thread> pool;
    // Python: one process for all its files, beside the clang workers
    bool pythonOk = setup.pythonFiles.isEmpty();
    QString pythonError;
    if (!setup.pythonFiles.isEmpty()) {
        pool.emplace_back([&] {
            pythonOk = Python::run(model.root, setup.pythonFiles, model, &pythonError, cancel, &mutex);
            const int n = ++done;
            if (progress)
                progress(n, total, QStringLiteral("%1 Python files").arg(setup.pythonFiles.size()));
        });
    }
    for (int t = 0; t < std::min(threads, unitCount); ++t)
        pool.emplace_back(worker);
    for (std::thread &t : pool)
        t.join();

    result.cancelled = cancel && cancel->load();
    result.units = done.load();
    result.failedUnits = failed.load();

    // Project files no unit includes: worth knowing in a review
    const qint64 tParsed = timer.elapsed();
    const QStringList onDisk = Project::sourceFiles(model.root, true);
    for (const QString &path : onDisk)
        model.addFile(path);
    for (File &f : model.files) {
        if (f.path.startsWith(shared.rootSlash))
            countLines(f.path, &f.lines, &f.codeLines);
    }

    model.indexedAt = QDateTime::currentMSecsSinceEpoch();
    const qint64 tLines = timer.elapsed();
    model.indexMs = timer.elapsed();
    model.finalize();
    if (qEnvironmentVariableIsSet("UNRAVEL_TIMING"))
        qInfo("index: parse+merge %lld ms (parse cpu %lld ms, merge %lld ms, lock wait %lld ms), files/lines %lld ms, finalize %lld ms",
              tParsed, parseNs.load() / 1000000, mergeNs.load() / 1000000, waitNs.load() / 1000000, tLines - tParsed,
              timer.elapsed() - tLines);
    if (!pythonOk) {
        Diagnostic d;
        d.severity = Diagnostic::Error;
        d.message = pythonError;
        model.diagnostics.append(d);
    }
    result.ok = !result.cancelled && (result.units > int(setup.pythonFiles.isEmpty() ? 0 : 1) || (pythonOk && !setup.pythonFiles.isEmpty()));
    if (total == 0)
        result.error = QStringLiteral("Nothing to index");
    else if (!result.ok && !pythonError.isEmpty())
        result.error = pythonError;
    return result;
}

} // namespace Unravel
