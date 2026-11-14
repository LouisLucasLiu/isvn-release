#pragma once

#include <QObject>
#include <QString>

namespace isvn::gui {

// Best-effort desktop notification adapter. It is presentation only: no call
// waits for a daemon and payload validation rejects absolute/external/raw data.
class SafeNotifier final : public QObject {
    Q_OBJECT

public:
    explicit SafeNotifier(QObject* parent = nullptr);
    Q_INVOKABLE bool notify(const QString& operation_type, const QString& status,
                            const QString& target = QString(),
                            const QString& revision = QString());

private:
    static bool safeField(const QString& value);
};

}  // namespace isvn::gui
