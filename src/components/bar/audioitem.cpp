#include "audioitem.h"

#include "audio.h"
#include "popup.h"
#include "widgets.h"

#include <QAbstractButton>
#include <QPushButton>
#include <QWheelEvent>

#include <QHBoxLayout>
#include <QVBoxLayout>

namespace kapah {

AudioItem::AudioItem(const BarContext &ctx, QScreen *output, QWidget *parent)
    : BarModule(ctx, output, parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_label = new Label(ctx.theme, this);
    layout->addWidget(m_label);

    connect(ctx.audio, &AudioService::stateChanged, this, &AudioItem::refresh);
    connect(ctx.audio, &AudioService::availableChanged, this, [this](bool) { refresh(); });
    refresh();
}

void AudioItem::refresh()
{
    if (ctx().audio == nullptr || !ctx().audio->available()) {
        m_label->setText(QStringLiteral("no audio"));
        m_label->setDimmed(true);
        return;
    }
    m_label->setDimmed(false);
    if (ctx().audio->muted()) {
        m_label->setText(QStringLiteral("mute"));
    } else {
        m_label->setText(QStringLiteral("%1%").arg(ctx().audio->volumePercent()));
    }
}

void AudioItem::wheelEvent(QWheelEvent *event)
{
    if (ctx().audio != nullptr && ctx().audio->available()) {
        const int delta = event->angleDelta().y() > 0 ? ctx().audio->step()
                                                      : -ctx().audio->step();
        ctx().audio->nudgeVolume(delta);
    }
    event->accept();
}

void AudioItem::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && ctx().audio != nullptr) {
        ctx().audio->toggleMute();
        event->accept();
    } else if (event->button() == Qt::RightButton) {
        openSinkSelector();
        event->accept();
    }
}

void AudioItem::openSinkSelector()
{
    if (ctx().audio == nullptr || !ctx().audio->available()) {
        return;
    }
    if (m_popup == nullptr) {
        m_popup = new Popup(ctx().theme, ctx().icons, QStringLiteral("kapah-audio-sinks"), this);
        connect(m_popup, &Popup::dismissRequested, m_popup, &Popup::dismiss);
    }
    // Enumerate only now that the user asked: roadmap 7.4.
    ctx().audio->refreshSinks();
    auto *outer = m_popup->layout();
    if (outer == nullptr) {
        outer = new QVBoxLayout(m_popup);
    }
    // Replace the content: delete old children beyond the layout itself.
    QLayoutItem *child;
    while ((child = outer->takeAt(0)) != nullptr) {
        delete child->widget();
        delete child;
    }
    for (const AudioSink &sink : ctx().audio->sinks()) {
        auto *button = new QPushButton(sink.description.isEmpty() ? sink.name : sink.description,
            m_popup);
        button->setCheckable(true);
        button->setChecked(sink.index == ctx().audio->defaultSink());
        connect(button, &QPushButton::clicked, m_popup,
            [this, sink] { ctx().audio->setDefaultSink(sink.index); });
        outer->addWidget(button);
    }
    m_popup->setPlacement(Popup::Placement::BelowAnchor);
    m_popup->setAnchorWidget(this);
    m_popup->resizeToContent();
    m_popup->popup(output());
}

} // namespace kapah
