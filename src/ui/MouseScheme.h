#pragma once

#include <QSettings>
#include <QString>

/**
 * What each mouse button does in the reader (Settings → Reading → Mouse). Read fresh on every press,
 * so changes apply immediately. With the pen / highlighter / eraser the left button always writes.
 */
struct MouseScheme {
    enum class Action { None, Pan, Select };

    QString leftOnText = "select";  ///< left drag that starts on text: "select" | "pan"
    QString leftElsewhere = "select"; ///< left drag that starts elsewhere: "pan" | "select" | "none"
    QString leftHold = "select";    ///< left press and hold: "select" | "none"
    QString right = "menuPan";      ///< click opens the menu, plus drag: "menu" (none) | "menuPan" | "menuSelect"
    QString middle = "pan";         ///< middle drag: "pan" | "none"
    QString doubleClick = "define"; ///< double-click a word: "define" | "none"
    int holdMs = 350;

    static MouseScheme load()
    {
        QSettings s;
        MouseScheme m;
        m.leftOnText = s.value("mouse/leftOnText", m.leftOnText).toString();
        m.leftElsewhere = s.value("mouse/leftElsewhere", m.leftElsewhere).toString();
        m.leftHold = s.value("mouse/leftHold", m.leftHold).toString();
        m.right = s.value("mouse/right", m.right).toString();
        m.middle = s.value("mouse/middle", m.middle).toString();
        m.doubleClick = s.value("mouse/doubleClick", m.doubleClick).toString();
        m.holdMs = s.value("mouse/holdMs", m.holdMs).toInt();
        return m;
    }
    void save() const
    {
        QSettings s;
        s.setValue("mouse/leftOnText", leftOnText);
        s.setValue("mouse/leftElsewhere", leftElsewhere);
        s.setValue("mouse/leftHold", leftHold);
        s.setValue("mouse/right", right);
        s.setValue("mouse/middle", middle);
        s.setValue("mouse/doubleClick", doubleClick);
        s.setValue("mouse/holdMs", holdMs);
    }

    static Action parse(const QString &v) { return v == "pan" ? Action::Pan : v == "select" ? Action::Select : Action::None; }

    /// What a drag with [button] does, given whether it started on text.
    Action drag(Qt::MouseButton button, bool onText) const
    {
        if (button == Qt::LeftButton) return parse(onText ? leftOnText : leftElsewhere);
        if (button == Qt::RightButton) return right == "menuPan" ? Action::Pan : right == "menuSelect" ? Action::Select : Action::None;
        if (button == Qt::MiddleButton) return parse(middle);
        return Action::None;
    }
    /// Whether pressing and holding [button] starts a text selection.
    bool holdSelects(Qt::MouseButton button) const
    {
        if (button == Qt::LeftButton) return leftHold == "select";
        return false;
    }
    /// Right click opens the menu straight away (the right button doesn't drag).
    /// Otherwise it opens on release, if the mouse didn't drag.
    bool menuOnPress() const { return right == "menu"; }
};
