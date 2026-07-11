#pragma once

namespace wimforge::startup {

enum class ElevationAction {
    Continue,
    Relaunched,
    Failed,
};

// Identifies the exact, non-sensitive startup step that failed.  Paths and
// command-line values are deliberately excluded so the result can be written
// to the structured application log even before AppController exists.
enum class ElevationFailureStage {
    None,
    OpenProcessToken,
    QueryTokenElevation,
    ResolveExecutable,
    ParseArguments,
    RequestRelaunch,
};

struct ElevationResult {
    ElevationAction action = ElevationAction::Continue;
    unsigned long nativeError = 0;
    ElevationFailureStage failureStage = ElevationFailureStage::None;
};

// The embedded requireAdministrator manifest is the primary elevation path.
// This guard is a defence in depth for development or repackaged executables
// whose manifest resource was omitted or replaced.
[[nodiscard]] ElevationResult ensureElevated();

} // namespace wimforge::startup
