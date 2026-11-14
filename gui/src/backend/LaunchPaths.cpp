#include "backend/LaunchPaths.h"

#include <QDir>
#include <QFileInfo>

namespace isvn::gui {
namespace {

void setError(QString* error, const QString& message) {
    if (error != nullptr) *error = message;
}

}  // namespace

QString pairedCliPath(const QString& gui_binary_dir, QString* error) {
    const QFileInfo dir_info(gui_binary_dir);
    const QString canonical_dir = dir_info.canonicalFilePath();
    if (canonical_dir.isEmpty() || !dir_info.isDir()) {
        setError(error, QStringLiteral("cannot determine the isvn-gui directory: %1")
                            .arg(gui_binary_dir));
        return {};
    }
    QDir parent(canonical_dir);
    if (!parent.cdUp()) {
        setError(error, QStringLiteral("isvn-gui directory has no parent: %1").arg(canonical_dir));
        return {};
    }
    // The parent of a canonical directory is itself canonical, so `candidate`
    // contains no symbolic link above the final component.
    const QString candidate = parent.absoluteFilePath(QStringLiteral("isvn"));
    const QFileInfo info(candidate);
    if (info.isSymLink()) {
        setError(error, QStringLiteral("paired isvn CLI must not be a symbolic link: %1")
                            .arg(candidate));
        return {};
    }
    if (!info.exists()) {
        setError(error, QStringLiteral("paired isvn CLI not found: %1").arg(candidate));
        return {};
    }
    if (!info.isFile() || info.canonicalFilePath() != candidate) {
        setError(error, QStringLiteral("paired isvn CLI is not a regular file: %1").arg(candidate));
        return {};
    }
    if (!info.isExecutable()) {
        setError(error, QStringLiteral("paired isvn CLI is not executable: %1").arg(candidate));
        return {};
    }
    return candidate;
}

QString workspaceRootFor(const QString& start_path) {
    QDir current(QFileInfo(start_path).absoluteFilePath());
    if (!current.exists()) return {};
    while (true) {
        if (QFileInfo(current.filePath(QStringLiteral(".isvn_local"))).isDir()) {
            return current.absolutePath();
        }
        if (!current.cdUp()) return {};
    }
}

}  // namespace isvn::gui
