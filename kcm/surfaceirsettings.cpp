// SPDX-License-Identifier: MIT
#include <KPluginFactory>
#include <KQuickConfigModule>

#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QProcess>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QUrl>

#include <algorithm>
#include <utility>

class FrameProvider final : public QQuickImageProvider
{
public:
    FrameProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

    void replace(QImage image)
    {
        QMutexLocker lock(&m_mutex);
        m_image = std::move(image);
    }

    QImage requestImage(const QString &, QSize *size, const QSize &requestedSize) override
    {
        QImage image;
        {
            QMutexLocker lock(&m_mutex);
            image = m_image;
        }
        if (size)
            *size = image.size();
        return requestedSize.isValid() ? image.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation) : image;
    }

private:
    QMutex m_mutex;
    QImage m_image;
};

class SurfaceIrSettings : public KQuickConfigModule
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(int captured READ captured NOTIFY changed)
    Q_PROPERTY(int total READ total NOTIFY changed)
    Q_PROPERTY(QString phase READ phase NOTIFY changed)
    Q_PROPERTY(QString hint READ hint NOTIFY changed)
    Q_PROPERTY(QUrl preview READ preview NOTIFY previewChanged)

public:
    SurfaceIrSettings(QObject *parent, const KPluginMetaData &data)
        : KQuickConfigModule(parent, data)
        , m_providerName(QStringLiteral("surface-ir-%1").arg(quintptr(this), 0, 16))
    {
        setButtons(NoAdditionalButton);
        connect(&m_process, &QProcess::readyReadStandardOutput, this, &SurfaceIrSettings::readOutput);
        connect(&m_process, &QProcess::readyReadStandardError, this, [this] { m_process.readAllStandardError(); });
        connect(&m_process, &QProcess::stateChanged, this, &SurfaceIrSettings::changed);
        connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                setPhase(QStringLiteral("error"), QStringLiteral("unavailable"));
            }
        });
        connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
            readOutput();
            clearFrame();
            if (m_phase == QLatin1String("cancelling") || code == 126 || code == 130) {
                setPhase(QStringLiteral("cancelled"));
            } else if (code == 127) {
                setPhase(QStringLiteral("error"), QStringLiteral("authorization"));
            } else if (status == QProcess::CrashExit || (m_phase != QLatin1String("saved") && m_phase != QLatin1String("error"))) {
                setPhase(QStringLiteral("error"), QStringLiteral("camera"));
            }
            Q_EMIT changed();
        });
    }

    ~SurfaceIrSettings() override
    {
        m_process.disconnect(this);
        if (busy()) {
            m_process.closeWriteChannel();
            m_process.waitForFinished(200);
        }
        if (m_provider) {
            if (const auto qml = engine())
                qml->removeImageProvider(m_providerName);
        }
    }

    bool busy() const { return m_process.state() != QProcess::NotRunning; }
    int captured() const { return m_captured; }
    int total() const { return m_total; }
    QString phase() const { return m_phase; }
    QString hint() const { return m_hint; }
    QUrl preview() const { return m_preview; }

    Q_INVOKABLE void enroll()
    {
        if (busy())
            return;
        m_captured = m_total = 0;
        clearFrame();
        setPhase(QStringLiteral("permission"));
        m_process.start(QStringLiteral("/usr/bin/pkexec"),
                        {QStringLiteral("--disable-internal-agent"), QStringLiteral(SURFACE_IR_HELPER_PATH)});
    }

    Q_INVOKABLE void verify()
    {
        if (!busy() || m_phase != QLatin1String("ready"))
            return;
        setPhase(QStringLiteral("verifying"));
        m_process.write("verify\n");
    }

    Q_INVOKABLE void cancel()
    {
        if (!busy() || m_phase == QLatin1String("cancelling"))
            return;
        setPhase(QStringLiteral("cancelling"));
        clearFrame();
        m_process.closeWriteChannel();
    }

Q_SIGNALS:
    void changed();
    void previewChanged();

private:
    void setPhase(const QString &phase, const QString &hint = {})
    {
        if (phase != m_phase || hint != m_hint) {
            m_phase = phase;
            m_hint = hint;
            Q_EMIT changed();
        }
    }

    void clearFrame()
    {
        if (!m_preview.isEmpty()) {
            m_provider->replace({});
            m_preview.clear();
            Q_EMIT previewChanged();
        }
    }

    void readOutput()
    {
        QString latestImage;
        while (m_process.canReadLine()) {
            const auto message = QJsonDocument::fromJson(m_process.readLine()).object();
            if (m_phase == QLatin1String("cancelling"))
                continue;
            const auto event = message.value(QStringLiteral("event")).toString();
            if (event == QLatin1String("preview")) {
                const auto count = std::clamp(message.value(QStringLiteral("captured")).toInt(), 0, 5);
                const bool addedSample = count != m_captured;
                m_captured = count;
                setPhase(QStringLiteral("capture"), message.value(QStringLiteral("hint")).toString());
                if (addedSample)
                    Q_EMIT changed();
                latestImage = message.value(QStringLiteral("image")).toString();
            } else if (event == QLatin1String("ready")) {
                setPhase(QStringLiteral("ready"));
            } else if (event == QLatin1String("result")) {
                m_total = message.value(QStringLiteral("total")).toInt();
                if (m_total > 0)
                    setPhase(QStringLiteral("saved"));
                else
                    setPhase(QStringLiteral("error"), message.value(QStringLiteral("reason")).toString());
            }
        }
        if (latestImage.startsWith(QLatin1String("data:image/"))) {
            const auto header = latestImage.indexOf(QLatin1Char(','));
            if (header < 0)
                return;
            auto image = QImage::fromData(QByteArray::fromBase64(latestImage.mid(header + 1).toLatin1()));
            if (!image.isNull()) {
                if (!m_provider) {
                    const auto qml = engine();
                    if (!qml)
                        return;
                    m_provider = new FrameProvider;
                    qml->addImageProvider(m_providerName, m_provider);
                }
                m_provider->replace(std::move(image));
                m_preview = QUrl(QStringLiteral("image://%1/%2").arg(m_providerName).arg(++m_frameNumber));
                Q_EMIT previewChanged();
            }
        }
    }

    QProcess m_process;
    const QString m_providerName;
    FrameProvider *m_provider = nullptr;
    QUrl m_preview;
    QString m_phase = QStringLiteral("idle");
    QString m_hint;
    quint64 m_frameNumber = 0;
    int m_captured = 0;
    int m_total = 0;
};

K_PLUGIN_CLASS_WITH_JSON(SurfaceIrSettings, "kcm_surfaceir.json")

#include "surfaceirsettings.moc"
