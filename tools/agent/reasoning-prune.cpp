#include "reasoning-prune.h"

std::size_t strip_wire_reasoning(json & messages) {
    if (!messages.is_array()) {
        return 0;
    }

    std::size_t removed = 0;
    for (json & msg : messages) {
        if (!msg.is_object() || msg.value("role", "") != "assistant") {
            continue;
        }
        if (msg.contains("reasoning_content")) {
            msg.erase("reasoning_content");
            removed++;
        }
    }
    return removed;
}
