#include "backend/SafeNotifier.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QVariantMap>

namespace isvn::gui {

SafeNotifier::SafeNotifier(QObject* parent) : QObject(parent) {}

bool SafeNotifier::safeField(const QString& value) {
    return !value.contains(QLatin1Char('\n')) && !value.contains(QLatin1Char('\r')) &&
           !value.startsWith(QLatin1Char('/')) && !value.contains(QStringLiteral("://")) &&
           !value.contains(QStringLiteral("\\"));
}

bool SafeNotifier::notify(const QString& operation_type, const QString& status,
                          const QString& target, const QString& revision) {
    if (operation_type.isEmpty() || status.isEmpty() || !safeField(operation_type) ||
        !safeField(status) || !safeField(target) || !safeField(revision)) {
        return false;
    }
    QDBusInterface notifications(QStringLiteral("org.freedesktop.Notifications"),
                                 QStringLiteral("/org/freedesktop/Notifications"),
                                 QStringLiteral("org.freedesktop.Notifications"),
                                 QDBusConnection::sessionBus());
    if (!notifications.isValid()) return false;
    QString body = status;
    if (!target.isEmpty()) body += QStringLiteral(" · ") + target;
    if (!revision.isEmpty()) body += QStringLiteral(" · r") + revision;
    // asyncCall is deliberately fire-and-forget: notification failure never
    // delays a journal/result transition or alters lifecycle policy.
    notifications.asyncCall(QStringLiteral("Notify"), QStringLiteral("isvn"),
                            static_cast<uint>(0), QString(), operation_type, body,
                            QStringList{}, QVariantMap{}, 5000);
    return true;
}

}  // namespace isvn::gui
