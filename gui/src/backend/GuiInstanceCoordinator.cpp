#include "backend/GuiInstanceCoordinator.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace isvn::gui {
namespace {

constexpr int kHandshakeTimeoutMs = 750;
constexpr auto kSchema = "isvn-gui-activate/v1";

QString runtimeDirectory() {
    const QByteArray test_dir = qgetenv("ISVN_GUI_TEST_RUNTIME_DIR");
    if (!test_dir.isEmpty()) return QString::fromUtf8(test_dir);
    QString directory = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (directory.isEmpty()) directory = QDir::tempPath() + QStringLiteral("/isvn-gui-runtime");
    return directory;
}

QString userScope() {
#ifdef Q_OS_UNIX
    return QString::number(::geteuid());
#else
    return QString::fromUtf8(qgetenv("USER"));
#endif
}

}  // namespace

GuiInstanceCoordinator::GuiInstanceCoordinator(QObject* parent) : QObject(parent) {}

GuiInstanceCoordinator::~GuiInstanceCoordinator() {
    if (server_ != nullptr) {
        server_->close();
        QLocalServer::removeServer(endpoint_);
    }
}

QString GuiInstanceCoordinator::endpointName() const { return endpoint_; }

bool GuiInstanceCoordinator::ensureRuntimeDirectory(QString* error) {
    const QString directory = runtimeDirectory();
    if (!QDir().mkpath(directory)) {
        if (error) *error = QStringLiteral("unable to create private runtime directory");
        return false;
    }
    QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                         QFileDevice::ExeOwner);
    const QFileInfo info(directory);
    if (!info.isDir()) {
        if (error) *error = QStringLiteral("private runtime location is not a directory");
        return false;
    }
    return true;
}

QString GuiInstanceCoordinator::endpointFor(const QString& project_identity) {
    const QByteArray digest = QCryptographicHash::hash(
        (userScope() + QLatin1Char('\n') + project_identity).toUtf8(), QCryptographicHash::Sha256)
                                  .toHex()
                                  .left(32);
    return QDir(runtimeDirectory()).filePath(QStringLiteral("isvn-%1.sock")
                                                  .arg(QString::fromLatin1(digest)));
}

GuiInstanceCoordinator::AcquireResult GuiInstanceCoordinator::acquire(
    const QString& project_identity, const QString& shell_mode, QString* error) {
    if (project_identity.isEmpty() || (shell_mode != QStringLiteral("new") &&
                                      shell_mode != QStringLiteral("legacy"))) {
        if (error) *error = QStringLiteral("invalid GUI instance identity");
        return AcquireResult::Error;
    }
    if (!ensureRuntimeDirectory(error)) return AcquireResult::Error;
    endpoint_ = endpointFor(project_identity);
    shell_mode_ = shell_mode;
    // Probe before listen: QLocalServer may otherwise replace a same-process
    // endpoint on some platforms.  A successfully connected peer is always
    // authenticated through the tiny schema handshake; an unexpected live
    // endpoint remains fail-closed.
    QLocalSocket probe;
    probe.connectToServer(endpoint_);
    if (probe.waitForConnected(kHandshakeTimeoutMs)) {
        probe.disconnectFromServer();
        return activateExisting(endpoint_, shell_mode_, error);
    }
    const auto probe_error = probe.error();
    if (probe_error != QLocalSocket::ServerNotFoundError &&
        probe_error != QLocalSocket::ConnectionRefusedError) {
        if (error) *error = QStringLiteral("existing GUI endpoint could not be authenticated");
        return AcquireResult::Error;
    }
    // Refusal proves there is no listening owner for this Unix-domain socket.
    // Remove only this stale endpoint before the owner-election listen call.
    if (probe_error == QLocalSocket::ConnectionRefusedError) QLocalServer::removeServer(endpoint_);
    if (startOwner(endpoint_, shell_mode_, error)) return AcquireResult::Owner;
    return activateExisting(endpoint_, shell_mode_, error);
}

bool GuiInstanceCoordinator::startOwner(const QString& endpoint, const QString& shell_mode,
                                        QString* error) {
    server_ = new QLocalServer(this);
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server_->listen(endpoint)) {
        server_->deleteLater();
        server_ = nullptr;
        return false;
    }
    QFile::setPermissions(endpoint, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    connect(server_, &QLocalServer::newConnection, this, [this, shell_mode]() {
        QLocalSocket* socket = server_->nextPendingConnection();
        if (socket == nullptr) return;
        connect(socket, &QLocalSocket::readyRead, this, [this, socket, shell_mode]() {
            const QJsonDocument document = QJsonDocument::fromJson(socket->readAll());
            const QJsonObject object = document.object();
            QString result = QStringLiteral("invalid");
            if (document.isObject() && object.value(QStringLiteral("schema")).toString() ==
                                           QLatin1String(kSchema)) {
                const QString requested = object.value(QStringLiteral("mode")).toString();
                if (requested == shell_mode) {
                    result = QStringLiteral("activate");
                    emit activationRequested();
                } else if (requested == QStringLiteral("new") || requested == QStringLiteral("legacy")) {
                    result = QStringLiteral("mode_conflict");
                }
            }
            socket->write(QJsonDocument(QJsonObject{{QStringLiteral("result"), result}})
                              .toJson(QJsonDocument::Compact));
            socket->flush();
            socket->disconnectFromServer();
        });
    });
    Q_UNUSED(error)
    return true;
}

GuiInstanceCoordinator::AcquireResult GuiInstanceCoordinator::activateExisting(
    const QString& endpoint, const QString& shell_mode, QString* error) const {
    QLocalSocket socket;
    socket.connectToServer(endpoint);
    if (!socket.waitForConnected(kHandshakeTimeoutMs)) {
        if (error) *error = QStringLiteral("an existing GUI endpoint could not be authenticated");
        return AcquireResult::Error;
    }
    socket.write(QJsonDocument(QJsonObject{{QStringLiteral("schema"), QLatin1String(kSchema)},
                                             {QStringLiteral("mode"), shell_mode}})
                     .toJson(QJsonDocument::Compact));
    if (!socket.waitForBytesWritten(kHandshakeTimeoutMs)) {
        if (error) *error = QStringLiteral("an existing GUI endpoint did not acknowledge activation");
        return AcquireResult::Error;
    }
    QElapsedTimer timer;
    timer.start();
    while (socket.bytesAvailable() == 0 && timer.elapsed() < kHandshakeTimeoutMs) {
        // Normally this is a distinct process.  Processing events here also
        // makes the protocol deterministic when exercised in one-process
        // tests, without creating a polling worker or a second GUI session.
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        socket.waitForReadyRead(10);
    }
    if (socket.bytesAvailable() == 0) {
        if (error) *error = QStringLiteral("an existing GUI endpoint did not acknowledge activation");
        return AcquireResult::Error;
    }
    const QJsonDocument response = QJsonDocument::fromJson(socket.readAll());
    const QString result = response.object().value(QStringLiteral("result")).toString();
    if (result == QStringLiteral("activate")) return AcquireResult::ActivatedExisting;
    if (result == QStringLiteral("mode_conflict")) {
        if (error) *error = QStringLiteral("existing GUI uses a different shell mode; exit it before switching");
        return AcquireResult::ModeConflict;
    }
    if (error) *error = QStringLiteral("existing GUI endpoint rejected activation");
    return AcquireResult::Error;
}

}  // namespace isvn::gui
