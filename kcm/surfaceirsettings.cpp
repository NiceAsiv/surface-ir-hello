// SPDX-License-Identifier: MIT
#include <KLocalizedString>
#include <KPluginFactory>
#include <KQuickConfigModule>

#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QUrl>

class SurfaceIrSettings : public KQuickConfigModule
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool canVerify READ canVerify NOTIFY changed)
    Q_PROPERTY(int captured READ captured NOTIFY changed)
    Q_PROPERTY(QUrl preview READ preview NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)

public:
    SurfaceIrSettings(QObject *parent, const KPluginMetaData &data)
        : KQuickConfigModule(parent, data)
    {
        setButtons(NoAdditionalButton);
        connect(&m_process, &QProcess::readyReadStandardOutput, this, &SurfaceIrSettings::readOutput);
        connect(&m_process, &QProcess::readyReadStandardError, this, [this] {
            m_process.readAllStandardError();
        });
        connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                m_status = i18nd("kcm_surfaceir", "Could not start the face enrollment helper.");
                Q_EMIT changed();
            }
        });
        connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
            readOutput();
            m_canVerify = false;
            m_preview.clear();
            if (m_cancelled || code == 126) {
                m_status = i18nd("kcm_surfaceir", "Enrollment cancelled.");
            } else if (code == 127) {
                m_status = i18nd("kcm_surfaceir", "Administrator authorization was unavailable.");
            } else if (!m_hasResult) {
                m_status = i18nd("kcm_surfaceir", "Enrollment stopped before saving.");
            }
            Q_EMIT changed();
        });
    }

    ~SurfaceIrSettings() override
    {
        if (busy()) {
            m_process.closeWriteChannel();
            m_process.waitForFinished(200);
        }
    }

    bool busy() const { return m_process.state() != QProcess::NotRunning; }
    bool canVerify() const { return m_canVerify; }
    int captured() const { return m_captured; }
    QUrl preview() const { return m_preview; }
    QString status() const { return m_status; }

    Q_INVOKABLE void enroll()
    {
        if (busy()) {
            return;
        }
        m_captured = 0;
        m_preview.clear();
        m_canVerify = false;
        m_cancelled = false;
        m_hasResult = false;
        m_status = i18nd("kcm_surfaceir", "Authorize camera access, then look at the top of the screen.");
        m_process.start(QStringLiteral("/usr/bin/pkexec"),
                        {QStringLiteral("--disable-internal-agent"), QStringLiteral(SURFACE_IR_HELPER_PATH)});
        Q_EMIT changed();
    }

    Q_INVOKABLE void verify()
    {
        if (m_canVerify && busy()) {
            m_canVerify = false;
            m_status = i18nd("kcm_surfaceir", "Checking the new face samples. Look at the camera.");
            m_process.write("verify\n");
            Q_EMIT changed();
        }
    }

    Q_INVOKABLE void cancel()
    {
        if (busy()) {
            m_cancelled = true;
            m_canVerify = false;
            m_preview.clear();
            m_process.closeWriteChannel();
            m_status = i18nd("kcm_surfaceir", "Cancelling enrollment…");
            Q_EMIT changed();
        }
    }

Q_SIGNALS:
    void changed();

private:
    void readOutput()
    {
        while (m_process.canReadLine()) {
            const auto message = QJsonDocument::fromJson(m_process.readLine()).object();
            const auto event = message.value(QStringLiteral("event")).toString();
            if (m_cancelled || event.isEmpty()) {
                continue;
            }
            m_status = message.value(QStringLiteral("message")).toString();
            if (event == QLatin1String("preview")) {
                m_captured = message.value(QStringLiteral("captured")).toInt();
                m_preview = QUrl(message.value(QStringLiteral("image")).toString());
            } else if (event == QLatin1String("ready")) {
                m_canVerify = true;
            } else if (event == QLatin1String("result")) {
                m_hasResult = true;
            }
            Q_EMIT changed();
        }
    }

    QProcess m_process;
    QUrl m_preview;
    QString m_status;
    int m_captured = 0;
    bool m_canVerify = false;
    bool m_cancelled = false;
    bool m_hasResult = false;
};

K_PLUGIN_CLASS_WITH_JSON(SurfaceIrSettings, "kcm_surfaceir.json")

#include "surfaceirsettings.moc"
