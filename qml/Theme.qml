pragma Singleton
import QtQuick
import QtCore
import IgittStyle as Styles

// unravel's own colours, made from the roles of the igitt theme in use
// (Styles.TokenInterface) as Cache does, plus preferences. Symbols and
// relations keep Sourcetrail's scheme: types grey, functions yellow,
// variables blue.
QtObject {
    id: theme

    readonly property var s: Styles.TokenInterface.semantics
    readonly property bool dark: Styles.TokenInterface.dark

    readonly property Settings prefs: Settings {
        category: "preferences"
        property bool wheelZooms: true
        property string uiFamily: "Inter"
        property real uiSize: 10
        property string codeFamily: "JetBrains Mono"
        property real codeSize: 9.5
        property bool showGrid: true
        property bool showLegend: false
        property int snippetContext: 2
    }
    readonly property font uiFont: Qt.font({ family: prefs.uiFamily, pointSize: prefs.uiSize })
    readonly property font monoFont: Qt.font({ family: prefs.codeFamily, pointSize: prefs.codeSize })
    readonly property font smallFont: Qt.font({ family: prefs.uiFamily, pixelSize: Styles.Primitives.fontSize.small })

    readonly property color canvas: Qt.tint(s.background_default, Styles.Primitives.alpha(s.background_muted, 0.6))
    readonly property color gridDot: Styles.Primitives.alpha(s.stroke_field, 0.6)
    readonly property color card: s.background_elevated
    readonly property color cardBorder: s.stroke_subtle
    readonly property color accent: s.primary_default
    readonly property color text: s.text_default
    readonly property color textMuted: s.text_muted
    readonly property color link: s.text_link
    readonly property color danger: s.danger_default
    readonly property color warning: s.warning_default
    readonly property color success: s.success_default
    readonly property color info: s.info_default
    readonly property int radius: Styles.Primitives.radius.r4

    // What a symbol is
    readonly property color typeColor: Qt.tint(s.text_muted, Styles.Primitives.alpha(s.syntax_type, 0.25))
    readonly property color functionColor: s.warning_default
    readonly property color variableColor: s.info_default
    readonly property color namespaceColor: s.special_default
    readonly property color macroColor: s.syntax_preprocessor
    readonly property color fileColor: s.icon_folder

    // SymbolKind (src/core/model.h) to a colour; -2 files, -3 folders
    function kindColor(kind) {
        switch (kind) {
        case -3: case -2: return fileColor
        case 1: return namespaceColor
        case 2: case 3: case 4: case 5: case 7: case 16: return typeColor
        case 8: case 9: case 10: case 11: case 12: return functionColor
        case 6: case 13: case 14: return variableColor
        case 15: return macroColor
        default: return textMuted
        }
    }
    function kindIcon(kind) {
        switch (kind) {
        case -3: return "folder"
        case -2: return "file-code"
        case 1: return "braces"
        case 5: case 6: return "list"
        case 7: return "type"
        case 2: case 3: case 4: case 16: return "box"
        case 8: case 9: case 10: case 11: case 12: return "function-square"
        case 15: return "hash"
        default: return "variable"
        }
    }
    // A card's fill: the kind's colour over the card surface
    function nodeFill(kind, strong) {
        const k = kindColor(kind)
        return Qt.tint(card, Styles.Primitives.alpha(k, strong ? (dark ? 0.30 : 0.34) : (dark ? 0.13 : 0.15)))
    }

    // RefKind (src/core/model.h) to a colour
    readonly property var edgeColors: ({
        call: Qt.darker(s.warning_default, dark ? 1.0 : 1.15),
        type: Styles.Primitives.alpha(s.text_muted, 0.9),
        var: s.info_default,
        inherit: s.success_default,
        override: s.special_default,
        macro: s.syntax_preprocessor,
        include: s.icon_folder,
        cycle: s.danger_default,
        background: canvas,
    })
    function edgeColor(kind) {
        switch (kind) {
        case 0: return edgeColors.call
        case 1: return edgeColors.type
        case 2: return edgeColors["var"]
        case 3: return edgeColors.inherit
        case 4: return edgeColors.override
        case 5: return edgeColors.macro
        case 6: return edgeColors.include
        default: return textMuted
        }
    }
    readonly property var edgeKinds: [
        { kind: 0, name: qsTr("Calls") },
        { kind: 1, name: qsTr("Type uses") },
        { kind: 2, name: qsTr("Variable uses") },
        { kind: 3, name: qsTr("Inheritance") },
        { kind: 4, name: qsTr("Overrides") },
        { kind: 5, name: qsTr("Macros") },
    ]

    // For the painted views
    readonly property var viewColors: ({
        background: canvas,
        text: s.text_default,
        textMuted: s.text_muted,
        frame: s.stroke_subtle,
        accent: s.primary_default,
        hover: Styles.Primitives.alpha(s.text_muted, 0.12),
        cycle: s.danger_default,
        source: s.success_default,
        target: s.danger_default,
        low: Qt.tint(s.background_elevated, Styles.Primitives.alpha(s.success_default, 0.55)),
        mid: Qt.tint(s.background_elevated, Styles.Primitives.alpha(s.warning_default, 0.85)),
        high: s.danger_default,
        typeNode: typeColor,
        functionNode: functionColor,
        variableNode: variableColor,
        call: edgeColors.call,
        type: edgeColors.type,
        "var": edgeColors["var"],
        inherit: edgeColors.inherit,
        override: edgeColors.override,
        macro: edgeColors.macro,
        include: edgeColors.include,
        node: card,
        nodeBorder: cardBorder,
    })

    // For the code view (CodeSnippets): colour names, as HTML wants them
    function hex(c) { return c.toString() }
    readonly property var codeColors: ({
        text: hex(s.text_default),
        keyword: hex(s.syntax_keyword),
        type: hex(s.syntax_type),
        string: hex(s.syntax_string),
        number: hex(s.syntax_number),
        comment: hex(s.syntax_comment),
        preprocessor: hex(s.syntax_preprocessor),
        "function": hex(s.syntax_function),
        symbol: hex(s.text_default),
        mark: hex(Styles.Primitives.alpha(s.warning_default, dark ? 0.38 : 0.32)),
        lineNumber: hex(Styles.Primitives.alpha(s.text_muted, 0.6)),
        marker: hex(s.primary_default),
    })
    readonly property color codeBackground: s.background_secondary

    function severityColor(sev) {
        return sev >= 2 ? s.danger_default : sev === 1 ? s.warning_default : s.text_muted
    }
}
