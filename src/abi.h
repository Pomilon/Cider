#pragma once

namespace cider {

// CID-005: function-calling conventions. Default resolves to the target's
// descriptor: SysV for native/pomeos, MS for uefi. Explicit Ms/SysV always
// win. For fn TYPES, Default canonicalizes to SysV (spec §3).
enum class Abi { Default, Ms, SysV };

inline bool abiIsMs(Abi a) { return a == Abi::Ms; }

}  // namespace cider
