// The conversation and the Messages API: the transcript, the request body, the reply.
//
// The transcript is a list of messages, each a list of blocks, kept in the
// shape the API wants them in. The rules the API enforces, and that this
// file and the agent keep so a request is never refused for its shape:
//   - after an assistant message with tool_use blocks, the next user message
//     starts with a tool_result for every one of them, in any order;
//   - no empty text blocks and no empty messages (the builder drops them);
//   - consecutive messages of one role are fine (the API merges them), but
//     the agent adds to the last user message instead of starting another;
//   - blocks the core does not know (thinking, and whatever comes next) are
//     kept as raw JSON and sent back exactly as they came, because the API
//     requires thinking blocks to come back unchanged.
// Nothing here makes a system call.
#ifndef BARB_API_H
#define BARB_API_H

#include "json.h"

typedef enum { BK_TEXT, BK_TOOL_USE, BK_TOOL_RESULT, BK_RAW } bkind_t;

typedef struct block {
    struct block* next;
    bkind_t kind;
    str_t text;      // TEXT: the text. TOOL_RESULT: the content. RAW: the block's JSON.
    str_t id;        // TOOL_USE: its id. TOOL_RESULT: the id it answers.
    str_t name;      // TOOL_USE
    str_t input;     // TOOL_USE: the arguments, a JSON object as text
    int is_error;    // TOOL_RESULT
} block_t;

typedef struct msg {
    struct msg* next;
    int assistant;   // 0: user, 1: assistant
    block_t* first;
    block_t* last;
} msg_t;

typedef struct {
    msg_t* first;
    msg_t* last;
} transcript_t;

// All of these allocate from the arena and return 0 when it is out of memory.
block_t* block_new(arena_t* a, bkind_t kind);
msg_t* tr_new_msg(arena_t* a, transcript_t* t, int assistant);
void msg_add(msg_t* m, block_t* b);
// The user message to add blocks to: the last message if that is a user's, else a new one.
msg_t* tr_user_msg(arena_t* a, transcript_t* t);
int tr_add_text(arena_t* a, msg_t* m, str_t text);  // 0 on out of memory; empty text is skipped
// A copy of `in` with new messages and blocks (the strings are shared), so blocks can be added
// to it without touching `in`. 0 on out of memory.
int tr_copy(arena_t* a, const transcript_t* in, transcript_t* out);
size_t tr_count(const transcript_t* t);  // the number of messages
// Make a transcript that was cut off (Barb was closed while tools ran, or a file was edited by
// hand) one the API accepts: empty messages go, two messages of one side in a row become one, and
// every tool_use gets a tool_result at the start of the next user message. The number of results
// added, or -1 on out of memory.
int tr_repair(arena_t* a, transcript_t* t);

typedef struct {
    str_t model;
    int64_t max_tokens;
    str_t system;
    str_t tools;     // a JSON array of tool definitions, as text; empty for none
    int thinking;    // ask for adaptive thinking, with its summarized text in the reply
} api_cfg_t;

// Write the request body for POST /v1/messages. Two prompt-cache breakpoints
// are set, on the system prompt (which caches the tools before it) and on the
// last block of the conversation, so a turn pays full price only for what is new.
// 0 if the buffer ran out of memory.
int api_request(sb_t* out, const api_cfg_t* cfg, const transcript_t* t);

// ---- saved sessions ----
//
// A session file is one JSON object (the format is in docs/SESSIONS.md, shared with the Rust
// version): {"barb_session": 1, "model": ..., "cwd": ..., "messages": [...]}, where "messages" is
// exactly what a request would send, without the cache marks.

#define SESSION_VERSION 1

typedef struct {
    str_t model, cwd;    // empty if the file has none
    transcript_t tr;
} session_t;

// Write the session file for this conversation. 0 if the buffer ran out of memory.
int api_session(sb_t* out, str_t model, str_t cwd, const transcript_t* t);
// Read a session file into the arena, repaired. 0 if it is not one (not JSON, the wrong shape, a
// newer version) or memory ran out (then a->failed is set).
int api_load_session(arena_t* a, str_t data, session_t* out);

// Where they are kept, in the working directory. The platform gives .barb/ a .gitignore of "*"
// when it first writes a file in here, so they stay out of the project's git.
#define SESSIONS_DIR ".barb/sessions"
// The name of a conversation started at this local time (t: year, month, day, hour, minute,
// second): YYYYMMDD-HHMMSS for try 0, then "-b" to "-z" after it for tries 1 to 25. `out` holds
// at least 20 bytes. 0 for a try past those (the platform adds something of its own then).
int session_new_name(char* out, const int t[6], int try_);
// The file that --resume ARG means: ARG itself if it has a slash in it (or, with `backslash`, a
// backslash), else ARG in SESSIONS_DIR with ".json" added if it has none. 0 if over `cap` bytes.
int session_file(char* out, size_t cap, const char* arg, int backslash);
// For a list of saved conversations: how many messages one has and the first line (60 bytes at
// most, not cut inside a character) of what the user said first, Barb's own notes skipped. 0 if it is not a session file.
int session_summary(arena_t* a, str_t data, size_t* nmsgs, str_t* first);

typedef struct {
    int is_message;      // 1: a message. 0: an error object from the API.
    str_t err_type;      // for an error: its type (invalid_request_error, overloaded_error, ...)
    str_t err_msg;       // and its message
    msg_t* msg;          // for a message: the assistant message (not added to any transcript)
    str_t stop_reason;   // end_turn, tool_use, max_tokens, refusal, pause_turn, ...
    int64_t in_tok, out_tok, cache_read, cache_write;
} api_reply_t;

// Read a response body. 1 if it was a message or an API error; 0 if it is
// anything else (not JSON, wrong shape) or memory ran out. Everything the
// result points at is copied into the arena.
int api_parse(arena_t* a, str_t body, api_reply_t* out);

#endif
