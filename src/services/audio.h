// Audio service on the PulseAudio client API (ADR-003).
//
// Event loop shape, which is the whole design problem here:
//   pa_context is driven by a pa_mainloop, but we do NOT run that mainloop on
//   its own thread. Instead:
//     * pa_context_get_mainloop / pa_mainloop_get_fd gives us a pollable fd;
//     * a QSocketNotifier on that fd calls pa_mainloop_prepare + dispatch when
//       the server has something for us;
//     * every API call is bracketed by prepare/dispatch so the callbacks run
//       synchronously on the main thread.
//
// Result: zero extra threads, zero wakeups at idle (the fd is only readable when
// the server sent us an event), and no blocking calls on the main thread -- the
// subscribe-based state cache means we never call a blocking getter on a
// user-visible path.

#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

#include <functional>

#include <pulse/pulseaudio.h>



class QSocketNotifier;

namespace kapah {

struct AudioSink {
    quint32 index = 0;
    QString name;
    QString description;
    quint32 volume = PA_VOLUME_NORM;
    bool muted = false;
    // PA_SINK_STATE_RUNNING == 1, SUSPENDED == 2, IDLE == 3. We only treat
    // SUSPENDED as "not really available" because that means no sink device.
    int state = 1;
    bool isDefault = false;

    // 0-100, which is what the UI wants. PulseAudio's cubic scale is converted
    // here so the bar shows a linear-looking bar like every other mixer.
    int percent() const;
};

struct AudioSource {
    quint32 index = 0;
    QString name;
    QString description;
    bool muted = false;
    int state = 1;
    bool isDefault = false;
};

class AudioService : public QObject {
    Q_OBJECT
public:
    explicit AudioService(QObject *parent = nullptr);
    ~AudioService() override;

    // Connects to the default server. Never blocks; available() stays false until
    // the connection completes.
    void start();
    void stop();

    bool available() const { return m_available; }
    QString serverName() const { return m_serverName; }

    // --- default sink -----------------------------------------------------
    quint32 defaultSink() const { return m_defaultSink; }
    QString defaultSinkName() const;
    int volumePercent() const { return m_volumePercent; }
    bool muted() const { return m_muted; }

    // --- default source (microphone) ---------------------------------------
    quint32 defaultSource() const { return m_defaultSource; }
    bool microphoneMuted() const { return m_microphoneMuted; }

    // Setting volume: we apply a *delta* to the current volume rather than an
    // absolute value, because two rapid key presses must accumulate. niri is
    // single-threaded from our point of view, so there is no race, but there IS
    // a latency: the volume-changed callback arrives after we return. Holding a
    // volume key therefore reads current volume from our own optimistic value,
    // not from the server, and the server round trip re-syncs us (Phase 6: "no
    // flicker when holding a volume key").
    void setVolumePercent(int percent);
    void nudgeVolume(int deltaPercent);
    void setMuted(bool muted);
    void toggleMute();

    void setMicrophoneMuted(bool muted);

    // --- enumeration (only while a menu is open, roadmap 7.4) ----------------
    QVector<AudioSink> sinks() const;
    QVector<AudioSource> sources() const;
    void setDefaultSink(quint32 index);
    // Brings the sink list up to date. Cheap when already subscribed.
    void refreshSinks();

    int step() const { return m_step; }
    void setStep(int step) { m_step = qBound(1, step, 25); }

Q_SIGNALS:
    void stateChanged();
    void availableChanged(bool available);
    // Emitted when the volume changed because of *our* action, so the OSD can
    // fire once per gesture rather than once per server round trip.
    void volumeChangedByUser(int percent, bool muted);
    void muteChangedByUser(bool muted);
    void sinkChanged(quint32 index);
    void microphoneMuteChanged(bool muted);

private:
    void onNotifierActivated();
    void onContextState(pa_context *ctx);
    void onSubscription(pa_context *ctx, pa_subscription_event_type_t type, quint32 index);
    void onSinkInfo(const pa_sink_info *info, int eol);
    void onSourceInfo(const pa_source_info *info, int eol);
    void onServerInfo(const pa_server_info *info);
    void onSuccess();
    int currentPercentFromCache() const;
    void setOptimisticVolume(int percent, bool muted);

    pa_threaded_mainloop *m_mainloop = nullptr;
    pa_context *m_context = nullptr;
    QSocketNotifier *m_notifier = nullptr;
    bool m_available = false;
    bool m_subscribed = false;
    bool m_optimistic = false;

    QString m_serverName;

    QHash<quint32, AudioSink> m_sinks;
    QHash<quint32, AudioSource> m_sources;
    quint32 m_defaultSink = 0;
    quint32 m_defaultSource = 0;
    int m_volumePercent = 0;
    bool m_muted = false;
    bool m_microphoneMuted = false;
    bool m_pendingSinkList = false;
    bool m_pendingSourceList = false;

    // Retry after the server disappears. Single-shot, exponential, capped 5 s.
    class QTimer *m_retryTimer = nullptr;
    int m_retryAttempt = 0;

    int m_step = 5;
};

} // namespace kapah