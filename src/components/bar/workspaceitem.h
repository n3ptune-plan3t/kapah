// Workspace indicators for the output this bar sits on. One Pill per
// workspace, indicator-only style (no wall of coloured pills). Click focuses;
// wheel switches with a <=100 ms debounce single-shot.

#pragma once

#include "modules.h"

#include <QHash>
#include <QTimer>

namespace kapah {

class Pill;

class WorkspaceItem : public BarModule {
    Q_OBJECT
public:
    WorkspaceItem(const BarContext &ctx, QScreen *output, QWidget *parent = nullptr);

protected:
    void wheelEvent(QWheelEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void refresh();
    void flushPendingSwitch();

    QHash<quint64, Pill *> m_pills;
    QTimer m_debounce;
    int m_pendingDelta = 0;
};

} // namespace kapah
