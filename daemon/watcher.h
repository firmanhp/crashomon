// daemon/watcher.h — inotify + Crashpad event loop
//
// SetupSignalHandlers installs SIGTERM/SIGINT handlers that stop the watcher
// loop.  RunWatcher runs the main poll loop: accepts new client connections,
// enqueues new minidumps for the worker, and shuts down cleanly on signal.

#pragma once

#include <string>

#include "daemon/disk_manager.h"

namespace crashomon {

// Install SIGTERM/SIGINT handlers.  Must be called before RunWatcher.
void SetupSignalHandlers();

// Run the main event loop.  Returns 0 on clean shutdown, 1 on startup error.
int RunWatcher(const std::string& db_path, const std::string& socket_path,
               const DiskManagerConfig& prune_cfg, const std::string& export_path);

}  // namespace crashomon
