// Popup: the base class every overlay panel derives from.
//
// Roadmap 4.2: "never allocate full-screen transparent layer surfaces except
// when strictly necessary (e.g. click-outside-to-dismiss -- prefer
// keyboard-interactivity grab and `exclusive` layer instead)". So a Popup is:
//
//   * an `overlay`-layer surface sized to its content, never full-screen;
//   * keyboard_interactivity = exclusive, so the compositor routes all keys to it
//     and there is no need for an invisible full-screen click catcher;
//   * `exclusive_zone = -1` so it never disturbs other exclusive surfaces;
//   * destroyed after a grace period, per roadmap 4.3.
//
// Click-outside still works, because a keyboard-interactive overlay means the
// compositor has already given us the keyboard and the seat's focus: a pointer
// press outside our surface goes to the window under it, and we detect losing
// focus rather than catching the press.

#pragma once

#include "layersurface.h"

#include <QPointer>
#include <QWidget>

class QScreen;

namespace kapah {

class Theme;
class IconCache;

class Popup : public LayerShellWindow {
    Q_OBJECT
public:
    Popup(Theme *theme, IconCache *icons, const QString &nameSpace, QWidget *parent = nullptr);
    ~Popup() override;

    void setTheme(Theme *theme);
    Theme *theme() const { return m_theme; }
    IconCache *icons() const { return m_icons; }

    // Grace period after hide() during which the surface is kept alive so that
    // re-opening is instant. 0 destroys immediately. Default 15 s per roadmap
    // 4.3; the launcher overrides it from [launcher] destroy-grace.
    void setDestroyGraceMs(int ms);
    int destroyGraceMs() const { return m_destroyGraceMs; }

    // Where to place the popup relative to an anchor rect on `screen`.
    enum class Placement {
        BelowAnchor,     // quick settings, power menu
        AboveAnchor,     // calendar attached to the clock
        Centered,        // calculator fallback, confirm dialogs
        TopRight,        // notification centre
        BottomCenter,    // error toasts
    };
    void setPlacement(Placement placement);
    void setAnchorWidget(const QWidget *anchor);

    // Shows on the screen that contains the anchor, or the focused output.
    void popup(QScreen *screen = nullptr);
    void dismiss();

    bool isPopupVisible() const { return isVisible(); }

    // Size the surface to exactly the content. Call after laying out children.
    void resizeToContent();

Q_SIGNALS:
    void popupShown();
    void popupDismissed();

    // The user clicked outside, pressed Escape, or the surface lost focus.
    void dismissRequested();

protected:
    // Builds the overlay-layer config used by every popup subclass.
    static LayerConfig makeConfig(const QString &nameSpace);

    bool event(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

    // Overridable: called before showing so subclasses can refresh their
    // contents. This is the moment "all background discovery stops after close"
    // (roadmap 7.4) is inverted into "discovery starts on open".
    virtual void onAboutToShow();
    virtual void onHidden();

    // Schedule the destroy timer. Called by hideEvent.
    void scheduleDestroy();
    void cancelDestroy();

private:
    QRect computePlacementRect(QScreen *screen) const;
    void anchorToScreen(QScreen *screen);

    Theme *m_theme = nullptr;
    IconCache *m_icons = nullptr;
    Placement m_placement = Placement::BelowAnchor;
    QPointer<const QWidget> m_anchor;
    int m_destroyGraceMs = 15000;
    QTimer *m_destroyTimer = nullptr;
    bool m_hideIsDismissal = false;
    bool m_popupOpen = false;
};

} // namespace kapah