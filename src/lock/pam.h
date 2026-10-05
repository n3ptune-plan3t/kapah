// PAM authentication for kapah-lock, on one dedicated QThread.
//
// SECURITY (docs/SECURITY.md, item S3):
//   * PAM is the only way in. There is no fallback path that unlocks.
//   * The password buffer is explicit_bzero'd immediately after pam_authenticate
//     returns, and again when the worker is destroyed. The UI clears its own
//     copy as soon as it hands the string to the worker.
//   * PAM's blocking conversation runs on m_thread; the UI thread only ever
//     marshals a queued slot call, so a slow NSS module cannot wedge the
//     lock surface.
//
// The standard PAM conversation (pam_conv) is used with PAM_PROMPT_ECHO_OFF:
// the lock screen never displays the password, and PAM itself enforces the
// per-attempt delay for pam_faillock/pam_unix on most distros. We additionally
// rate-limit retries in the UI (one in-flight attempt at a time; the field is
// dead while PAM is working).

#pragma once

#include <QObject>
#include <QString>
#include <QThread>

class PamWorker;

namespace kapah {

class PamAuthenticator : public QObject {
    Q_OBJECT
public:
    // service defaults to "kapah" and is expected to exist as
    // /etc/pam.d/kapah (see contrib/). If it does not exist PAM falls back to
    // the "other" stack, which is deliberately *stricter*, never weaker.
    explicit PamAuthenticator(const QString &user, const QString &service = QStringLiteral("kapah"),
        QObject *parent = nullptr);
    ~PamAuthenticator() override;

    // One attempt at a time. While attemptInFlight() is true, start() is a
    // no-op. The result arrives as result(); there is no synchronous path.
    void start(const QString &password);
    bool attemptInFlight() const { return m_inFlight; }

Q_SIGNALS:
    // ok == true only on pam_authenticate() == PAM_SUCCESS *and*
    // pam_acct_mgmt() == PAM_SUCCESS. pamError is the raw PAM return code of
    // the failing call for logging; it is never shown to the user beyond a
    // generic "authentication failed".
    void result(bool ok, int pamError);

private:
    QThread m_thread;
    PamWorker *m_worker = nullptr;
    bool m_inFlight = false;
};

} // namespace kapah
