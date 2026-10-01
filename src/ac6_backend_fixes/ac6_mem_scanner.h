#pragma once

namespace rex::memory {
class Memory;
}

namespace ac6::devtools {

// Starts the developer memory scanner thread if ac6_dev_scanner is set; a no-op
// otherwise. Safe to call once the runtime's guest memory exists.
void StartMemScanner(rex::memory::Memory* memory);

}  // namespace ac6::devtools
