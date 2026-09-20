// update.h - GitHub Releases self-update: check, download, swap on restart.
// All functions are async (detached worker thread); callbacks fire on the
// worker thread, so the caller must marshal them back to the GUI thread.
#pragma once

#include <functional>
#include <string>

namespace sr {

struct UpdateStatus {
    int state = 0;              // 0 idle, 1 checking, 2 up-to-date, 3 available,
                                // 4 downloading, 5 ready (restart pending), 6 error
    std::string latestVersion;  // newest version found (UTF-8, no leading 'v')
    std::string message;        // error text or progress note (UTF-8)
};

void UpdateCheckAsync(std::function<void(const UpdateStatus&)> onDone);
void UpdateDownloadAndInstallAsync(std::function<void(const UpdateStatus&)> onProgress);

} // namespace sr
