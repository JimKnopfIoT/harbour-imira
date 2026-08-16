/*
 * harbour-imira — device orientation watcher.
 * Keeps a rotation atomic (0/90/270) in sync with the physical device
 * orientation so landscape content gets rotated for the 16:9 sink.
 */
#ifndef IMIRA_ORIENTATION_H
#define IMIRA_ORIENTATION_H

#include <atomic>

namespace imira {

// Spawns a detached background thread running a Qt event loop with the
// orientation sensor; updates *rotation on every change. A manual value in
// /tmp/imira-rotate (polled by main) overrides the sensor while present.
void startOrientationWatcher(std::atomic<int> *rotation);

} // namespace imira

#endif
