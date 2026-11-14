#pragma once

#include <QString>

namespace isvn::gui {

// Absolute path of the isvn CLI that belongs to this GUI binary.
//
// The CLI sits one directory above the GUI binary in both supported layouts:
//   build tree    <build>/gui/isvn-gui      -> <build>/isvn
//   install tree  <prefix>/libexec/isvn-gui -> <prefix>/isvn
//
// The result must be an executable regular file at exactly that location; a
// symbolic link is rejected so the pair cannot be redirected elsewhere.  On
// failure an empty string is returned and `error` describes the reason.  The
// caller must not fall back to a PATH lookup.
QString pairedCliPath(const QString& gui_binary_dir, QString* error);

// Root of the member workspace that contains `start_path`, found the same way
// the CLI finds it: the nearest directory, at or above `start_path`, holding a
// `.isvn_local` directory.  Empty when `start_path` is not inside a workspace.
// Used for display only; the CLI remains the authority on where it writes.
QString workspaceRootFor(const QString& start_path);

}  // namespace isvn::gui
