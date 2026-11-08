#include "subagent-limits.h"

#include <string>

namespace {

// Move pos back to the start of the UTF-8 sequence containing it (never
// inside a multi-byte character).
size_t utf8_floor(const std::string & s, size_t pos) {
    while (pos > 0 && pos < s.size() &&
           (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80) {
        --pos;
    }
    return pos;
}

// Move pos forward to the next UTF-8 lead byte.
size_t utf8_ceil(const std::string & s, size_t pos) {
    while (pos < s.size() &&
           (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80) {
        ++pos;
    }
    return pos;
}

} // namespace

std::string subagent_cap_result(const std::string & text, const std::string & task_id) {
    if (text.size() <= SUBAGENT_RESULT_CAP_BYTES) {
        return text;
    }

    const size_t total = text.size();
    const size_t head_budget = SUBAGENT_RESULT_CAP_BYTES * 60 / 100;
    const size_t tail_budget = SUBAGENT_RESULT_CAP_BYTES - head_budget;

    const size_t head = utf8_floor(text, head_budget);
    const size_t tail_start = utf8_ceil(text, total - tail_budget);
    const size_t shown = head + (total - tail_start);

    std::string out;
    out.reserve(shown + 128);
    out.append(text, 0, head);
    out.append("\n[... output capped: showing ");
    out.append(std::to_string(shown));
    out.append(" of ");
    out.append(std::to_string(total));
    out.append(" bytes; full output available via resume task_id=");
    out.append(task_id);
    out.append(" ...]\n");
    out.append(text, tail_start, std::string::npos);
    return out;
}

bool subagent_should_stop_on_token_budget(int64_t generated_tokens, int64_t max_generate_tokens) {
    if (max_generate_tokens <= 0) {
        return false;  // budget off
    }
    return generated_tokens > max_generate_tokens;
}

std::string subagent_stop_status(bool task_interrupted, bool parent_interrupted, int reason) {
    if (task_interrupted) {
        // Task-scoped flag wins: report the reason that set it.
        switch (reason) {
            case SUBAGENT_STOP_TIMEOUT: return "timeout";
            case SUBAGENT_STOP_CANCEL:  return "cancelled";
            default:                    return "cancelled";
        }
    }
    if (parent_interrupted) {
        return "interrupted";
    }
    return "";
}

void subagent_append_draft(std::string & draft, const std::string & text) {
    if (text.empty()) {
        return;
    }
    if (!draft.empty()) {
        draft += "\n";
    }
    draft += text;
    // Keep only the tail: a cut-off loop's most recent narration is the most
    // useful part, and the parent context must stay bounded.
    constexpr size_t kDraftCap = 8192;
    if (draft.size() > kDraftCap) {
        size_t cut = draft.size() - kDraftCap;
        while (cut < draft.size() &&
               (static_cast<unsigned char>(draft[cut]) & 0xC0) == 0x80) {
            cut++;  // snap forward to a UTF-8 boundary
        }
        draft.erase(0, cut);
    }
}
