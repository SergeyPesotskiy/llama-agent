#pragma once
// Anthropic-style policy for model reasoning on the wire. Reasoning is never
// deleted from session/history here; only the *copy* sent to the inference
// server is trimmed: everything up to the last user turn is stripped (field
// `reasoning_content` and literal <think>...</think> blocks inside assistant
// content), while the current turn's chain of thought stays so the model can
// see its own reasoning between tool calls. Stripping happens exactly at the
// turn boundary where the prompt prefix-cache resets anyway, so it is free.
#include "nlohmann/json.hpp"

void prune_inter_turn_reasoning(nlohmann::ordered_json & messages);
