#include "Extension/Input/input_source_environment.h"
#include <cstdio>
#include <cstdlib>

// This executable links the real actor-free publication provider. Scripted-pad
// setters need only its gate/revision/invalidation path, which never samples
// environment facts. An accidental request/tick/raw-observation path must fail
// the regression rather than receiving invented game or input readiness.
namespace dingosdk::native_input_source::environment {
Facts sample() noexcept {
    std::fputs("Unexpected passive native environment query in scripted-pad regression\n", stderr);
    std::abort();
}
}
