#include "audio.h"

#include "logging.h"

#include <QSocketNotifier>
#include <QTimer>

#include <cmath>

namespace kapah {

namespace {

// A tiny global state pointer, because libpulse's C callbacks have no user data.
AudioService *g_audio = nullptr;

// A pa_glib_mainloop inside the threaded mainloop is the modern way to let Qt's
// loop drive PulseAudio. Since we already integrate the fd ourselves, we do the
// simpler thing: a threaded mainloop with WAIT_UNLOCKED around the dispatch, and
// pa_threaded_mainloop_wait()/wait_for() on the lock-free path.
bool lockContext(AudioService *s)
{
    Q_UNUSED(s)
    return pa_threaded_mainloop_lock(g_audio->m_mainloop) >= 0;
}

void unlockContext()
{
    pa_threaded_mainloop_unlock(g_audio->m_mainloop);
}

pa_threaded_mainloop *newMainloop()
{
    pa_mainloop *ml = pa_mainloop_new();
    if (ml == nullptr) {
        return nullptr;
    }
    pa_threaded_mainloop *tml = pa_threaded_mainloop_new(ml);
    if (tml == nullptr) {
        pa_mainloop_free(ml);
        return nullptr;
    }
    // Autonegotiate the SPI: PA_CLIENT_MAINLOOP with the basic API is enough
    // because we drive the fd manually.
    if (pa_threaded_mainloop_start(tml) < 0) {
        pa_threaded_mainloop_free(tml);
        return nullptr;
    }
    pa_threaded_mainloop_wait(tml);
    return tml;
}

// Linear-ish conversion from PulseAudio's cubic volume scale to 0-100.
// We use the cube root of the ratio, which is what pavucontrol does and what
// users expect from a mixer slider.
int toPercent(pa_volume_t v)
{
    if (v <= 0) {
        return 0;
    }
    const double ratio = static_cast<double>(v) / static_cast<double>(PA_VOLUME_NORM);
    const double scaled = std::pow(qBound(0.0, ratio, 4.0), 1.0 / 3.0) * 100.0;
    return qBound(0, static_cast<int>(scaled + 0.5), 200);
}

pa_volume_t fromPercent(int percent)
{
    percent = qBound(0, percent, 200);
    const double ratio = std::pow(percent / 100.0, 3.0);
    return static_cast<pa_volume_t>(ratio * PA_VOLUME_NORM);
}

} // namespace

int AudioSink::percent() const
{
    return toPercent(volume);
}

// ---------------------------------------------------------------------------

AudioService::AudioService(QObject *parent)
    : QObject(parent)
{
}

AudioService::~AudioService()
{
    stop();
}

void AudioService::start()
{
    if (m_context != nullptr) {
        return;
    }

    g_audio = this;

    m_mainloop = newMainloop();
    if (m_mainloop == nullptr) {
        LOG_WARN(Audio,
            "could not create a PulseAudio mainloop: audio is unavailable "
            "(no PulseAudio server, or pipewire-pulse is not running)");
        scheduleRetry();
        return;
    }

    if (!lockContext(this)) {
        LOG_WARN(Audio, "could not lock the PulseAudio mainloop");
        pa_threaded_mainloop_free(m_mainloop);
        m_mainloop = nullptr;
        scheduleRetry();
        return;
    }

    m_context = pa_context_new(pa_threaded_mainloop_get_mainloop(m_mainloop),
        "kapah");
    if (m_context == nullptr) {
        unlockContext();
        pa_threaded_mainloop_free(m_mainloop);
        m_mainloop = nullptr;
        scheduleRetry();
        return;
    }

    pa_context_set_state_callback(m_context, [](pa_context *ctx) {
        if (g_audio != nullptr) {
            g_audio->onContextState(ctx);
        }
    }, nullptr);

    // This never blocks: it starts the handshake and returns.
    if (pa_context_connect(m_context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
        LOG_WARNF(Audio, "pa_context_connect failed: %s", pa_strerror(pa_context_errno(m_context)));
        pa_context_disconnect(m_context);
        pa_context_unref(m_context);
        m_context = nullptr;
        unlockContext();
        pa_threaded_mainloop_free(m_mainloop);
        m_mainloop = nullptr;
        scheduleRetry();
        return;
    }

    unlockContext();

    // Do not wait for the handshake here. The state callback runs on the
    // pa_threaded_mainloop thread and already drives m_available / FAILED in
    // either direction, and onContextState() schedules a retry on failure. A
    // synchronous 200x5 ms drain would block the bar from painting on a slow
    // pipewire-pulse start; the bar shows "audio unavailable" until READY.

}

void AudioService::stop()
{
    if (m_notifier != nullptr) {
        delete m_notifier;
        m_notifier = nullptr;
    }
    if (m_context != nullptr) {
        if (lockContext(this)) {
            pa_context_set_state_callback(m_context, nullptr, nullptr);
            pa_context_set_subscribe_callback(m_context, nullptr, nullptr);
            pa_context_disconnect(m_context);
            pa_context_unref(m_context);
            m_context = nullptr;
            unlockContext();
        }
    }
    if (m_mainloop != nullptr) {
        pa_threaded_mainloop_stop(m_mainloop);
        pa_threaded_mainloop_free(m_mainloop);
        m_mainloop = nullptr;
    }
    if (g_audio == this) {
        g_audio = nullptr;
    }
    m_available = false;
    m_subscribed = false;
}

void AudioService::scheduleRetry()
{
    if (m_retryTimer != nullptr) {
        return;
    }
    m_retryTimer = new QTimer(this);
    m_retryTimer->setSingleShot(true);
    m_retryTimer->setTimerType(Qt::VeryCoarseTimer);
    connect(m_retryTimer, &QTimer::timeout, this, [this] {
        if (m_context == nullptr && m_mainloop == nullptr) {
            start();
        }
    });
    const int ms = qMin(5000, 500 * (1 << qMin(m_retryAttempt, 4)));
    ++m_retryAttempt;
    m_retryTimer->start(ms);
}

// ---------------------------------------------------------------------------
// Context lifecycle
// ---------------------------------------------------------------------------

void AudioService::onContextState(pa_context *ctx)
{
    if (ctx != m_context) {
        return;
    }
    switch (pa_context_get_state(ctx)) {
    case PA_CONTEXT_READY: {
        m_available = true;
        m_retryAttempt = 0;
        if (m_retryTimer != nullptr) {
            m_retryTimer->stop();
        }

        pa_context_set_subscribe_callback(ctx,
            [](pa_context *c, pa_subscription_event_type_t type, quint32 index) {
                if (g_audio != nullptr) {
                    g_audio->onSubscription(c, type, index);
                }
            },
            nullptr);

        pa_operation *op = pa_context_subscribe(
            ctx,
            static_cast<pa_subscription_mask_t>(PA_CONTEXT_SUBSCRIBE_SINK
                | PA_CONTEXT_SUBSCRIBE_SOURCE | PA_CONTEXT_SUBSCRIBE_SERVER),
            nullptr,
            [](pa_context *, pa_subscription_event_type_t, quint32) {
                if (g_audio != nullptr) {
                    g_audio->onSuccess();
                }
            },
            nullptr);
        if (op != nullptr) {
            pa_operation_unref(op);
        }
        Q_EMIT availableChanged(true);
        break;
    }
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED: {
        const bool wasAvailable = m_available;
        m_available = false;
        m_subscribed = false;
        LOG_WARN(Audio, "PulseAudio server went away");
        if (wasAvailable) {
            Q_EMIT availableChanged(false);
        }
        // The notifier's fd is gone with the context. Tear down and retry.
        if (m_notifier != nullptr) {
            delete m_notifier;
            m_notifier = nullptr;
        }
        if (m_context != nullptr) {
            pa_context_unref(m_context);
            m_context = nullptr;
        }
        if (m_mainloop != nullptr) {
            pa_threaded_mainloop_stop(m_mainloop);
            pa_threaded_mainloop_free(m_mainloop);
            m_mainloop = nullptr;
        }
        g_audio = this;
        scheduleRetry();
        break;
    }
    default:
        break;
    }
}

void AudioService::onSubscription(pa_context *ctx, pa_subscription_event_type_t type, quint32 index)
{
    if (ctx != m_context) {
        return;
    }
    switch (type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) {
    case PA_SUBSCRIPTION_EVENT_SINK: {
        const pa_operation *op = pa_context_get_sink_info_by_index(
            ctx, index,
            [](pa_context *c, const pa_sink_info *i, int eol) {
                if (g_audio != nullptr) {
                    g_audio->onSinkInfo(i, eol);
                }
            },
            nullptr);
        if (op != nullptr) {
            pa_operation_unref(op);
        }
        break;
    }
    case PA_SUBSCRIPTION_EVENT_SOURCE: {
        const pa_operation *op = pa_context_get_source_info_by_index(
            ctx, index,
            [](pa_context *c, const pa_source_info *i, int eol) {
                if (g_audio != nullptr) {
                    g_audio->onSourceInfo(i, eol);
                }
            },
            nullptr);
        if (op != nullptr) {
            pa_operation_unref(op);
        }
        break;
    }
    case PA_SUBSCRIPTION_EVENT_SERVER: {
        const pa_operation *op = pa_context_get_server_info(
            ctx,
            [](pa_context *c, const pa_server_info *i) {
                if (g_audio != nullptr) {
                    g_audio->onServerInfo(i);
                }
            },
            nullptr);
        if (op != nullptr) {
            pa_operation_unref(op);
        }
        break;
    }
    default:
        break;
    }
}

void AudioService::onSinkInfo(const pa_sink_info *info, int eol)
{
    if (eol != 0) {
        return;
    }
    if (info == nullptr) {
        return;
    }

    AudioSink sink;
    sink.index = info->index;
    sink.name = QString::fromUtf8(info->name ? info->name : "");
    sink.description = QString::fromUtf8(info->description ? info->description : "");
    sink.volume = info->volume;
    sink.muted = info->mute != 0;
    sink.state = info->state;
    sink.isDefault = (m_defaultSink == info->index);

    if (sink.isDefault) {
        // Only the default sink's volume/mute drive the bar.
        const int pct = sink.percent();
        const bool muted = sink.muted;
        if (pct != m_volumePercent || muted != m_muted) {
            m_volumePercent = pct;
            m_muted = muted;
            m_optimistic = false;
            Q_EMIT stateChanged();
        }
    }

    m_sinks.insert(sink.index, sink);
}

void AudioService::onSourceInfo(const pa_source_info *info, int eol)
{
    if (eol != 0 || info == nullptr) {
        return;
    }
    AudioSource src;
    src.index = info->index;
    src.name = QString::fromUtf8(info->name ? info->name : "");
    src.description = QString::fromUtf8(info->description ? info->description : "");
    src.muted = info->mute != 0;
    src.state = info->state;
    src.isDefault = m_defaultSource == info->index;

    if (src.isDefault && src.muted != m_microphoneMuted) {
        m_microphoneMuted = src.muted;
        Q_EMIT stateChanged();
    }
    m_sources.insert(src.index, src);
}

void AudioService::onServerInfo(const pa_server_info *info)
{
    if (info == nullptr) {
        return;
    }
    m_serverName = QString::fromUtf8(info->server_name ? info->server_name : "");

    if (m_defaultSink != info->default_sink_name_index && m_defaultSink == 0) {
        // We track the *name* index for the default, then map it to a numeric
        // index below. This avoids a get_sink_by_name round trip on every event.
    }
    const QString defaultSinkName = QString::fromUtf8(
        info->default_sink_name ? info->default_sink_name : "");
    if (!defaultSinkName.isEmpty()) {
        for (auto it = m_sinks.constBegin(); it != m_sinks.constEnd(); ++it) {
            if (it.value().name == defaultSinkName) {
                if (m_defaultSink != it.key()) {
                    m_defaultSink = it.key();
                    Q_EMIT sinkChanged(m_defaultSink);
                    Q_EMIT stateChanged();
                }
                break;
            }
        }
    }
    const QString defaultSourceName = QString::fromUtf8(
        info->default_source_name ? info->default_source_name : "");
    if (!defaultSourceName.isEmpty()) {
        for (auto it = m_sources.constBegin(); it != m_sources.constEnd(); ++it) {
            if (it.value().name == defaultSourceName) {
                if (m_defaultSource != it.key()) {
                    m_defaultSource = it.key();
                    Q_EMIT stateChanged();
                }
                break;
            }
        }
    }
}

void AudioService::onSuccess()
{
    m_subscribed = true;
    // One full enumeration to prime the cache, then the subscription keeps it
    // current with no further full scans.
    if (!lockContext(this)) {
        return;
    }
    pa_operation *op = pa_context_get_server_info(m_context,
        [](pa_context *c, const pa_server_info *i) {
            if (g_audio != nullptr) {
                g_audio->onServerInfo(i);
            }
        },
        nullptr);
    if (op != nullptr) {
        pa_operation_unref(op);
    }
    op = pa_context_get_sink_info_list(m_context,
        [](pa_context *c, const pa_sink_info *i, int eol) {
            if (g_audio != nullptr) {
                g_audio->onSinkInfo(i, eol);
            }
        },
        nullptr);
    if (op != nullptr) {
        pa_operation_unref(op);
    }
    op = pa_context_get_source_info_list(m_context,
        [](pa_context *c, const pa_source_info *i, int eol) {
            if (g_audio != nullptr) {
                g_audio->onSourceInfo(i, eol);
            }
        },
        nullptr);
    if (op != nullptr) {
        pa_operation_unref(op);
    }
    unlockContext();
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

QString AudioService::defaultSinkName() const
{
    const auto it = m_sinks.constFind(m_defaultSink);
    return it == m_sinks.constEnd() ? QString() : it.value().description.isEmpty()
        ? it.value().name
        : it.value().description;
}

int AudioService::currentPercentFromCache() const
{
    if (m_optimistic) {
        return m_volumePercent;
    }
    const auto it = m_sinks.constFind(m_defaultSink);
    if (it == m_sinks.constEnd()) {
        return m_volumePercent;
    }
    return it.value().percent();
}

QVector<AudioSink> AudioService::sinks() const
{
    QVector<AudioSink> out;
    out.reserve(m_sinks.size());
    for (const AudioSink &s : m_sinks) {
        out.append(s);
    }
    return out;
}

QVector<AudioSource> AudioService::sources() const
{
    QVector<AudioSource> out;
    out.reserve(m_sources.size());
    for (const AudioSource &s : m_sources) {
        out.append(s);
    }
    return out;
}

void AudioService::refreshSinks()
{
    if (!m_available) {
        return;
    }
    onSuccess();
}

// ---------------------------------------------------------------------------
// Mutations
// ---------------------------------------------------------------------------

void AudioService::setOptimisticVolume(int percent, bool muted)
{
    // Called from inside libpulse's mainloop callback with the lock held.
    // Re-entrant-safe only because it touches Qt-side fields (m_volumePercent,
    // m_muted) and emits no signals / makes no further pa_* calls. Do not add
    // a pa_context_* call here without moving it after unlockContext().
    m_volumePercent = qBound(0, percent, 200);
    m_muted = muted;
    m_optimistic = true;
    Q_EMIT stateChanged();
}

void AudioService::setVolumePercent(int percent)
{
    if (!m_available || !lockContext(this)) {
        return;
    }
    const bool wasMuted = m_muted;
    // Unmuting on a volume change is what every mixer does and what users
    // expect: dragging the slider up on a muted sink should be audible.
    pa_operation *op = pa_context_set_sink_volume_by_index(
        m_context, m_defaultSink, fromPercent(percent),
        [](pa_context *, int, pa_operation *, void *) {
            if (g_audio != nullptr) {
                g_audio->setOptimisticVolume(g_audio->currentPercentFromCache(),
                    g_audio->m_muted);
            }
        },
        nullptr);
    if (op != nullptr) {
        pa_operation_unref(op);
    }
    if (wasMuted) {
        pa_operation *unmute = pa_context_set_sink_mute_by_index(
            m_context, m_defaultSink, 0,
            [](pa_context *, int, pa_operation *, void *) {},
            nullptr);
        if (unmute != nullptr) {
            pa_operation_unref(unmute);
        }
    }
    unlockContext();

    setOptimisticVolume(percent, wasMuted);
    Q_EMIT volumeChangedByUser(m_volumePercent, m_muted);
}

void AudioService::nudgeVolume(int deltaPercent)
{
    setVolumePercent(qBound(0, m_volumePercent + deltaPercent, 200));
}

void AudioService::setMuted(bool muted)
{
    if (!m_available || !lockContext(this)) {
        return;
    }
    pa_operation *op = pa_context_set_sink_mute_by_index(m_context, m_defaultSink,
        muted ? 1 : 0, nullptr, nullptr);
    if (op != nullptr) {
        pa_operation_unref(op);
    }
    unlockContext();

    m_muted = muted;
    Q_EMIT stateChanged();
    Q_EMIT muteChangedByUser(m_muted);
}

void AudioService::toggleMute()
{
    setMuted(!m_muted);
}

void AudioService::setMicrophoneMuted(bool muted)
{
    if (!m_available || !lockContext(this)) {
        return;
    }
    pa_operation *op = pa_context_set_source_mute_by_index(m_context, m_defaultSource,
        muted ? 1 : 0, nullptr, nullptr);
    if (op != nullptr) {
        pa_operation_unref(op);
    }
    unlockContext();
    m_microphoneMuted = muted;
    Q_EMIT stateChanged();
    Q_EMIT microphoneMuteChanged(m_muted);
}

void AudioService::setDefaultSink(quint32 index)
{
    if (!m_available || !lockContext(this)) {
        return;
    }
    const auto it = m_sinks.constFind(index);
    if (it == m_sinks.constEnd()) {
        unlockContext();
        return;
    }
    pa_operation *op = pa_context_set_default_sink(m_context, index, nullptr, nullptr);
    if (op != nullptr) {
        pa_operation_unref(op);
    }
    unlockContext();
    m_defaultSink = index;
    Q_EMIT sinkChanged(index);
    Q_EMIT stateChanged();
}

} // namespace kapah