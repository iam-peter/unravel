#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>

// The facts unravel knows about a code base: its files, its symbols
// (namespaces, types, functions, variables, macros) and every reference
// between them, as clang reports them. Free of any GUI, so the indexer,
// the analyses and the tests work on it alone.
//
// Ids are indexes into the vectors. A model is built by the indexer
// (Indexer), finished once (CodeModel::finalize), and only read after
// that, from any thread.
namespace Unravel {

enum class SymbolKind : quint8 {
    Unknown,
    Namespace,
    Class,
    Struct,
    Union,
    Enum,
    EnumConstant,
    Typedef,     // typedef and using aliases
    Function,    // free functions
    Method,
    Constructor,
    Destructor,
    Operator,    // operator overloads and conversions
    Field,
    Variable,    // globals and static members
    Macro,
    Concept,
};

// What a reference between two symbols means
enum class RefKind : quint8 {
    Call,        // calls or takes the address of a function
    TypeUse,     // names a type (in a declaration, a cast, a template argument)
    VarUse,      // reads or writes a variable, field or enum constant
    Inherit,     // a class derives from a class (from = derived)
    Override,    // a method overrides another (from = overrider)
    MacroUse,    // a macro is expanded
    Include,     // only between files, see File::includes
    Declaration, // where a symbol is declared or defined (from == to)
    Count
};

enum SymbolFlag : quint32 {
    Defined = 1 << 0,     // the definition is in the indexed sources
    External = 1 << 1,    // declared outside the project (system, third party)
    Virtual = 1 << 2,
    PureVirtual = 1 << 3,
    Static = 1 << 4,
    Const = 1 << 5,
    Template = 1 << 6,
    Inline = 1 << 7,
    Implicit = 1 << 8,    // made up by the compiler
    Deleted = 1 << 9,
    Defaulted = 1 << 10,
    Abstract = 1 << 11,   // a class with pure virtual methods
    Generated = 1 << 12,  // declared in a generated file (moc, build tree)
    EntryPoint = 1 << 13, // main and friends
    Explicit = 1 << 14,
    Noexcept = 1 << 15,
    Mutable = 1 << 16,
    FromMacro = 1 << 17,  // declared by a macro's expansion (Q_OBJECT and the like)
    MetaCallable = 1 << 18, // reachable through Qt's meta-object system (slots, Q_INVOKABLE, Q_PROPERTY, QML)
};

enum class Access : quint8 { None, Public, Protected, Private };

enum RefFlag : quint8 {
    RefRead = 1 << 0,
    RefWrite = 1 << 1,
    RefImplicit = 1 << 2,  // an implicit call (a constructor, a conversion)
    RefVirtualBase = 1 << 3,
    RefAddressOf = 1 << 4,
    RefDynamic = 1 << 5,   // a call that dispatches virtually
};

struct Location {
    qint32 file = -1;
    qint32 line = 0;  // 1-based
    qint32 col = 0;   // 1-based, in bytes
    qint32 endLine = 0;
    qint32 endCol = 0;

    bool valid() const { return file >= 0 && line > 0; }
    bool contains(const Location &o) const
    {
        if (file != o.file || !valid())
            return false;
        if (o.line < line || o.line > endLine)
            return false;
        if (o.line == line && o.col < col)
            return false;
        if (o.line == endLine && endCol > 0 && o.col > endCol)
            return false;
        return true;
    }
    friend bool operator==(const Location &a, const Location &b) = default;
};

// Per function: what the indexer measured in its body
struct FunctionMetrics {
    qint32 lines = 0;        // lines of the definition
    qint32 statements = 0;
    qint32 complexity = 0;   // McCabe: 1 + decisions
    qint32 nesting = 0;      // deepest nesting of blocks
    qint32 params = 0;
    qint32 returns = 0;
    qint32 cognitive = 0;    // a cognitive complexity in the spirit of SonarSource's
    quint64 cloneHash = 0;   // of the body's tokens, identifiers and literals made alike
    QVector<quint32> minHash; // signature of token shingles, for near clones
    qint32 tokens = 0;
};

struct Symbol {
    QString usr;
    QString name;
    QString qualified;   // with namespaces and classes
    QString signature;   // "void f(int, const Bar &) const" for functions
    QString type;        // the type of a variable, a field or an alias
    SymbolKind kind = SymbolKind::Unknown;
    Access access = Access::None;
    quint32 flags = 0;
    qint32 parent = -1;  // the namespace, class or function it is declared in
    Location decl;       // first declaration
    Location def;        // the definition, with its extent
    FunctionMetrics metrics;

    // Filled in by CodeModel::finalize
    QVector<qint32> children;
    qint32 fanIn = 0;    // distinct symbols that reference this one (not its own members)
    qint32 fanOut = 0;   // distinct symbols it references

    bool has(SymbolFlag f) const { return flags & f; }
    Location where() const { return def.valid() ? def : decl; }
};

struct Reference {
    qint32 from = -1;    // the symbol the reference is in (a function, a field…), -1 at file level
    qint32 to = -1;
    RefKind kind = RefKind::TypeUse;
    quint8 flags = 0;
    Location loc;        // line and column of the name; endCol is its end
};

struct EventConnection {
    qint32 signal = -1, target = -1;
    Location loc, targetLoc;
    QString sender, receiver;
    QString delivery; // declared type only; AutoConnection depends on runtime affinity
    QString limitation;
};

// An aggregated relation, one per (from, to, kind)
struct Edge {
    qint32 from = -1;
    qint32 to = -1;
    RefKind kind = RefKind::TypeUse;
    qint32 count = 0;
    quint8 flags = 0;    // ORed over the references
    qint32 firstRef = -1;
};

struct Include {
    qint32 file = -1;    // the included file
    qint32 line = 0;     // line of the #include
};

struct File {
    QString path;        // absolute, cleaned
    QString relative;    // to the project root, or the absolute path for outside files
    QString module;      // directory relative to the root ("" at the root)
    bool project = false;   // inside the project root
    bool generated = false; // in a build directory
    bool header = false;
    bool system = false;
    bool indexed = false;   // a translation unit's main file
    qint32 lines = 0;
    qint32 codeLines = 0;   // non-blank, non-comment-only
    QList<Include> includes;
    QVector<qint32> includedBy;   // filled in by finalize
    QVector<qint32> symbols;      // declared or defined here, filled in by finalize
};

struct Diagnostic {
    enum Severity : quint8 { Note, Warning, Error, Fatal };
    Severity severity = Error;
    QString message;
    QString category;
    Location loc;
    qint32 unit = -1;    // the translation unit's main file
};

struct Stats {
    qint32 files = 0, projectFiles = 0, units = 0;
    qint32 symbols = 0, types = 0, functions = 0, variables = 0;
    qint64 references = 0, lines = 0, codeLines = 0;
    qint32 errors = 0, warnings = 0;
    qint64 indexMs = 0;
};

// A folder with one slash at the end ("/" stays "/"), for prefix tests
inline QString withSlash(const QString &dir)
{
    return dir.endsWith(QLatin1Char('/')) ? dir : dir + QLatin1Char('/');
}

QString kindName(SymbolKind k);
QString refKindName(RefKind k);
bool isType(SymbolKind k);
bool isFunction(SymbolKind k);
bool isVariable(SymbolKind k);
bool isScope(SymbolKind k);   // namespaces and types: what other symbols are declared in

class CodeModel
{
public:
    QString root;              // the project root
    QString buildDir;          // the compile database's directory
    QVector<File> files;
    QVector<Symbol> symbols;
    QVector<Reference> refs;
    QVector<EventConnection> eventConnections;
    QVector<Diagnostic> diagnostics;
    // Names used in project macros' definitions, made references by finalize:
    // the preprocessing record leaves out expansions inside other macros
    struct NameUse {
        qint32 from = -1;
        QString name;
        Location loc;
    };
    QVector<NameUse> macroNameUses;
    qint64 indexedAt = 0;      // msecs since epoch
    qint64 indexMs = 0;

    // Filled in by finalize
    QVector<Edge> edges;
    QVector<QVector<qint32>> outEdges;   // per symbol, indexes into edges
    QVector<QVector<qint32>> inEdges;
    QVector<QVector<qint32>> refsInFile; // per file, references sorted by position
    QVector<QVector<qint32>> refsTo;     // per symbol, its references (not declarations)
    QVector<QVector<qint32>> refsFrom;   // per symbol, outgoing references in source order
    QVector<QVector<qint32>> declsOf;    // per symbol, its declaration references
    QStringList modules;                 // sorted
    QHash<QString, qint32> moduleIndex;

    qint32 fileId(const QString &path) const { return m_fileIds.value(path, -1); }
    qint32 symbolId(const QString &usr) const { return m_usrIds.value(usr, -1); }

    // Used while building
    qint32 addFile(const QString &path);
    qint32 addSymbol(const QString &usr);

    void finalize();
    Stats stats() const;

    // The innermost namespace-level owner: a class for a method, the
    // function for a free function. What the graph view draws as a node.
    qint32 nodeOwner(qint32 symbol) const;
    // The class or namespace chain up to the top
    QVector<qint32> ancestors(qint32 symbol) const;
    // The topmost class (not namespace) a symbol belongs to, or itself
    qint32 outermostType(qint32 symbol) const;
    // The function (or other symbol) whose definition contains a location
    qint32 enclosingFunction(const Location &loc) const;
    // The innermost function or type whose definition spans a line of a file
    qint32 innermostAt(qint32 file, qint32 line) const;
    // Whether a symbol is the project's own (defined or declared in a project file)
    bool isProjectSymbol(qint32 symbol) const;
    QString displayName(qint32 symbol) const;  // qualified, with the signature for functions
    QString locationText(const Location &loc) const;

    // Edges from or to a symbol of a kind (or every kind with RefKind::Count)
    QVector<qint32> edgesFrom(qint32 symbol, RefKind kind = RefKind::Count) const;
    QVector<qint32> edgesTo(qint32 symbol, RefKind kind = RefKind::Count) const;

    bool save(const QString &path) const;
    bool load(const QString &path);
    void clear();

private:
    void rebuildIndexes();
    QHash<QString, qint32> m_fileIds;
    QHash<QString, qint32> m_usrIds;
    QVector<QVector<qint32>> m_functionsInFile; // per file, functions sorted by def line
};

} // namespace Unravel
