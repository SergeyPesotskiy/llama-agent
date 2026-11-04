#pragma once

#include <string>

// Tool output guard (opt-in via --tool-output-guard, default off).
//
// When enabled, every tool result content entering the conversation is wrapped
// in an <external_data> tag and the system prompt carries one extra line
// stating that external data is data, not instructions. With the flag off,
// tool_output_guard_wrap is the identity function so the prompt protocol stays
// byte-identical to the historical behavior (unit-tested).

// Wrap tool result text for the conversation. Returns text unchanged when
// disabled or when the text is empty / already wrapped (idempotent).
std::string tool_output_guard_wrap(const std::string & text, bool enabled);

// The extra system-prompt line added when the guard is enabled.
const char * tool_output_guard_system_line();
