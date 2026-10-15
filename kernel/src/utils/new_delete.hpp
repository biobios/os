#pragma once
#include <cstddef>

namespace oz {
// In a capability-based microkernel, kernel dynamic heap allocation (malloc/new) is eliminated.
// All kernel objects must be explicitly retyped from UntypedMemory capabilities via placement new.
} // namespace oz