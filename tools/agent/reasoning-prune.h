#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>

// Agent headers alias `json` to nlohmann::ordered_json: insertion-order key
// stability is what keeps serialized prompts deterministic. Match the alias
// so the payload is edited in place with no conversions.
using json = nlohmann::ordered_json;

// Strip reasoning_content from assistant messages in a wire payload.
//
// Models that return reasoning_content keep it in history, so every later
// request would re-send all stored thinking blocks: prompt tokens and model
// attention spent on notes the server cannot use anyway. Stripping ALL of
// them (not just old turns) is deliberate: prompt bytes must only ever
// append for the server's prefix KV-cache to stay valid, and thinking sent
// once can never be removed later without invalidating the cache. Either it
// never goes on the wire, or it stays forever — this module picks "never".
//
// Operates on the request copy only: the canonical history and the session
// file keep every reasoning block (raw data stays on disk; the model can
// still be shown its notes by turning the feature off with
// --no-reasoning-prune). No-op when nothing carries the field: the payload
// compares byte-identical afterwards.

std::size_t strip_wire_reasoning(json & messages);
