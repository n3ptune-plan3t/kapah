#include "pam.h"

#include "logging.h"

#include <QByteArray>
#include <QCoreApplication>

#if __has_include(<security/pam_appl.h>)
#include <security/pam_appl.h>
#else
#include <pam/pam_appl.h>
#endif

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace kapah {

namespace {

// Portability shim for the glibc explicit_bzero; other libcs get the idiom
// every security document lists and the compiler is forbidden from eliding
// only on glibc, but we check with a memory barrier fallback.
void scrub(void *p, std::size_t n)
{
#if defined(__GLIBC__) || defined(__FreeBSD__)
    explicit_bzero(p, n);
#else
    volatile unsigned char *v = static_cast<volatile unsigned char *>(p);
    while (n-- > 0) {
        *v++ = 0;
    }
#endif
}

namespace {
// The worker holds the password copy in a QByteArray so we can scrub it in
// place. The QString handed in is const; the *caller's* copy is scrubbed by
// PasswordField before we get here.
QByteArray g_password;

// pam_conv: this conversation only answers the echo-off password prompt.
int conv(int numMessages, const struct pam_message **messages, struct pam_response **responses,
    void *appDataPtr)
{
    if (numMessages <= 0 || messages == nullptr || responses == nullptr) {
        return PAM_CONV_ERR;
    }
    auto *responsesOut = static_cast<pam_response *>(std::calloc(static_cast<std::size_t>(numMessages),
        sizeof(pam_response)));
    if (responsesOut == nullptr) {
        return PAM_CONV_ERR;
    }
    for (int i = 0; i < numMessages; ++i) {
        if (messages[i]->msg_style == PAM_PROMPT_ECHO_OFF
            || messages[i]->msg_style == PAM_PROMPT_ECHO_ON) {
            // Only an echo-off prompt gets the password; an echo-on prompt
            // would print it, so answer "user not given" instead.
            if (messages[i]->msg_style == PAM_PROMPT_ECHO_OFF) {
                const std::size_t n = static_cast<std::size_t>(g_password.size());
                char *copy = static_cast<char *>(std::malloc(n + 1));
                if (copy == nullptr) {
                    std::free(responsesOut);
                    return PAM_CONV_ERR;
                }
                std::memcpy(copy, g_password.constData(), n);
                copy[n] = '\0';
                responsesOut[i].resp = copy;
                responsesOut[i].resp_retcode = 0;
            } else {
                return PAM_CONV_ERR;
            }
        } else if (messages[i]->msg_style == PAM_ERROR_MSG
            || messages[i]->msg_style == PAM_TEXT_INFO) {
            responsesOut[i].resp = nullptr;
            responsesOut[i].resp_retcode = 0;
        } else {
            std::free(responsesOut);
            return PAM_CONV_ERR;
        }
    }
    *responses = responsesOut;
    return PAM_SUCCESS;
}

} // namespace

// Kept as a file-local object: the worker thread is the only writer.
pam_handle_t *g_handle = nullptr;

} // namespace

class PamWorker : public QObject {
    Q_OBJECT
public:
    explicit PamWorker(const QString &user, const QString &service, QObject *parent = nullptr)
        : QObject(parent)
        , m_user(user)
        , m_service(service)
    {
    }

    ~PamWorker() override
    {
        if (g_handle != nullptr) {
            pam_end(g_handle, PAM_ABORT);
            g_handle = nullptr;
        }
        scrub(g_password.data(), static_cast<std::size_t>(g_password.size()));
        g_password.clear();
    }

public Q_SLOTS:
    void run(const QString &password)
    {
        g_password = password.toUtf8();
        pam_handle_t *handle = nullptr;
        pam_conv convStruct { &conv, nullptr };
        int rc = pam_start(m_service.toLocal8Bit().constData(),
            m_user.toLocal8Bit().constData(), &convStruct, &handle);
        if (rc != PAM_SUCCESS) {
            scrub(g_password.data(), static_cast<std::size_t>(g_password.size()));
            g_password.clear();
            Q_EMIT done(false, rc);
            return;
        }
        g_handle = handle;
        rc = pam_authenticate(handle, 0);
        if (rc == PAM_SUCCESS) {
            rc = pam_acct_mgmt(handle, 0);
        }
        pam_end(handle, rc == PAM_SUCCESS ? PAM_SUCCESS : PAM_ABORT);
        g_handle = nullptr;
        scrub(g_password.data(), static_cast<std::size_t>(g_password.size()));
        g_password.clear();
        Q_EMIT done(rc == PAM_SUCCESS, rc);
    }

Q_SIGNALS:
    void done(bool ok, int pamError);

private:
    QString m_user;
    QString m_service;
};

PamAuthenticator::PamAuthenticator(const QString &user, const QString &service, QObject *parent)
    : QObject(parent)
{
    m_worker = new PamWorker(user, service);
    m_worker->moveToThread(&m_thread);
    m_thread.start();
    connect(m_worker, &PamWorker::done, this,
        [this](bool ok, int err) {
            m_inFlight = false;
            Q_EMIT result(ok, err);
        });
}

PamAuthenticator::~PamAuthenticator()
{
    m_thread.quit();
    m_thread.wait();
    // Worker is deleted via its thread parent chain: request it explicitly to
    // guarantee the pam_end above runs before we leave main.
    if (m_worker != nullptr) {
        // We are no longer in m_thread, so delete via deleteLater would never
        // run; delete synchronously, which is safe now that the thread is
        // stopped. The worker's destructor scrubs g_password and pam_ends.
        delete m_worker;
        m_worker = nullptr;
    }
}

void PamAuthenticator::start(const QString &password)
{
    if (m_inFlight) {
        return;
    }
    m_inFlight = true;
    QMetaObject::invokeMethod(m_worker, "run", Qt::QueuedConnection, Q_ARG(QString, password));
}

} // namespace kapah

#include "pam.moc"
