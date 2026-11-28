// Compaction: when the conversation grows too big, the model writes a summary of it, and the
// summary takes its place.
//
// The summary is asked for with one extra request: the conversation as it is, with the same
// system prompt and tools (so the prompt cache still applies), and a last user message saying to
// summarize instead of going on. The reply's summary then becomes the whole conversation: one
// user message, followed by the user's newest words if they have not been answered yet. Nothing
// older is kept verbatim, so no earlier turn is sent again in changed form (no thinking block is
// left to fail its check) and no tool_use is cut from its tool_result.
//
// This file holds the parts that are plain functions: the prompt, reading the summary out of the
// reply, the message that replaces the conversation, and the size setting. agent.c runs it.
#ifndef BARB_COMPACT_H
#define BARB_COMPACT_H

#include "api.h"

// Where automatic compaction starts, in tokens, unless the user sets it.
#define DEFAULT_COMPACT_AT 150000
// The smallest setting taken: below it the summary would hardly be smaller than what it replaces.
#define MIN_COMPACT_AT 1000

// The start of the message that replaces a compacted conversation.
#define COMPACT_MARK "[barb] This conversation was compacted"

// A compaction setting: "off" (0), or a token count such as "150000", "150k" or "1m". -1 if it is
// neither, or under MIN_COMPACT_AT.
int64_t parse_compact_at(str_t s);
// Whether an API error message says the request is bigger than the model's context window.
int is_overflow(str_t msg);
// The last user message of the summary request; `focus` is what the user asked it to keep in view.
str_t compact_instruction(arena_t* a, str_t focus);
// The summary in a reply's text: what is inside <summary>, or else the text without its
// <analysis>. Empty if there is none.
str_t compact_extract(arena_t* a, str_t reply);
// The message that takes the place of the compacted conversation.
str_t compact_summary_message(arena_t* a, str_t summary);
// `in` made smaller for a summary request that has to fit a window the conversation no longer
// fits: long tool results and texts lose their middles, big tool inputs are replaced by a note,
// and thinking goes. 0 on out of memory.
int compact_trimmed(arena_t* a, const transcript_t* in, transcript_t* out);

#endif
