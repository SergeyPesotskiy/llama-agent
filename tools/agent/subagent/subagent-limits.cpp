#include "subagent-limits.h"

#include <map>
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

bool subagent_bash_allowed(const std::string & command, const std::vector<std::string> & prefixes) {
    if (prefixes.empty()) {
        return true;  // no restriction configured
    }
    // Split into segments on shell separators; every segment must pass.
    size_t start = 0;
    const std::string seps = ";|&\n";
    auto check_segment = [&prefixes](const std::string & seg_in) {
        // trim
        size_t b = seg_in.find_first_not_of(" \t");
        if (b == std::string::npos) {
            return true;  // empty between separators
        }
        size_t e = seg_in.find_last_not_of(" \t");
        std::string seg = seg_in.substr(b, e - b + 1);
        for (const auto & pat : prefixes) {
            if (pat.empty()) continue;
            if (seg.size() >= pat.size() && seg.compare(0, pat.size(), pat) == 0) {
                if (seg.size() == pat.size() || seg[pat.size()] == ' ' || seg[pat.size()] == '\t') {
                    return true;  // whole-token prefix match
                }
            }
        }
        return false;
    };
    bool any = false;
    while (start <= command.size()) {
        size_t cut = command.find_first_of(seps, start);
        std::string seg = command.substr(start, cut == std::string::npos ? std::string::npos : cut - start);
        if (!check_segment(seg)) {
            return false;
        }
        if (seg.find_first_not_of(" \t") != std::string::npos) {
            any = true;
        }
        if (cut == std::string::npos) break;
        start = cut + 1;
    }
    return any;
}

subagent_tool_budget_state subagent_tool_budget_check(int64_t used, int64_t max_calls) {
    if (max_calls <= 0) {
        return SUBAGENT_TOOL_BUDGET_OK;
    }
    if (used >= max_calls) {
        return SUBAGENT_TOOL_BUDGET_EXHAUSTED;
    }
    if (used * 5 >= max_calls * 4) {  // >= 80% without float
        return SUBAGENT_TOOL_BUDGET_WARNING;
    }
    return SUBAGENT_TOOL_BUDGET_OK;
}

std::string subagent_tool_budget_note(int64_t used, int64_t max_calls) {
    return "[tool budget: used " + std::to_string(used) + " of " + std::to_string(max_calls) + "]";
}

std::vector<std::string> subagent_aggregate_tool_calls(const std::vector<std::string> & entries) {
    struct agg {
        int64_t count = 0;
        long long total_ms = 0;
    };
    std::vector<std::string> order;                 // first-appearance order of names
    std::map<std::string, agg> by_name;
    std::vector<std::string> passthrough;

    for (const auto & line : entries) {
        // Expected shape: "name (123ms)". Anything else is passed through.
        const size_t open = line.rfind(" (");
        const bool closes = open != std::string::npos &&
                            line.size() > 2 && line.compare(line.size() - 3, 3, "ms)") == 0;
        std::string ms_part;
        if (closes) {
            ms_part = line.substr(open + 2, line.size() - 3 - (open + 2));
        }
        bool numeric = !ms_part.empty();
        long long ms = 0;
        if (numeric) {
            try {
                size_t pos = 0;
                ms = std::stoll(ms_part, &pos);
                if (pos != ms_part.size()) {
                    numeric = false;
                }
            } catch (...) {
                numeric = false;
            }
        }
        if (!numeric || open == 0) {
            passthrough.push_back(line);
            continue;
        }
        const std::string name = line.substr(0, open);
        if (by_name.find(name) == by_name.end()) {
            order.push_back(name);
        }
        by_name[name].count++;
        by_name[name].total_ms += ms;
    }

    std::vector<std::string> out;
    out.reserve(order.size() + passthrough.size());
    for (const auto & name : order) {
        const auto & a = by_name[name];
        out.push_back(name + " x" + std::to_string(a.count) +
                      ", total " + std::to_string(a.total_ms) + "ms");
    }
    for (const auto & raw : passthrough) {
        out.push_back(raw);
    }
    return out;
}

std::string subagent_offload_note(const std::string & text, const std::string & path,
                                  const std::string & task_id, size_t preview_bytes) {
    size_t cut = preview_bytes < text.size() ? utf8_floor(text, preview_bytes) : text.size();
    std::string out = "[large answer offloaded: " + std::to_string(text.size()) +
                      " bytes written to " + path +
                      ". First " + std::to_string(cut) + " bytes below - read the file with" +
                      " offset/limit only if the preview is insufficient (full output also" +
                      " stays available via resume task_id=" + task_id + ").]\n\n";
    out += text.substr(0, cut);
    if (cut < text.size()) {
        out += "\n[... remainder in the file ...]";
    }
    return out;
}
