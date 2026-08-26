// Entry point for the privileged helper.
//
// Kept separate from qtmonitorhelper.cpp so that the validation logic can be
// linked into a test harness without KAUTH_HELPER_MAIN's main() colliding
// with the harness's own.
#include "qtmonitorhelper.h"

#include <KAuth/HelperSupport>

KAUTH_HELPER_MAIN("org.qtmonitor", QtmonitorHelper)
