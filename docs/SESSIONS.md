# Saved conversations

Both versions save the conversation as it goes and can take it up again. They write and read the
same files, so this is the format, not one version's notes about it. A version that cannot keep to
it bumps the version number below.

## Where

`.barb/sessions/` in the working directory, one file per conversation, named for when it started
in local time: `YYYYMMDD-HHMMSS.json`. If that name is taken, `-b`, `-c`, ... `-z` go after it.
After `/clear` the conversation goes on in a new file next to the old one, `<stem>-2.json`, then
`-3`, so nothing saved is overwritten by what comes after it.

When the first conversation is saved, `.barb/.gitignore` is written with `*` in it (if there is no
such file yet), so the saved conversations stay out of the project's git. Barb's background task
logs live in `.barb/` too.

## The file

One JSON object:

```json
{"barb_session":1,"model":"claude-...","cwd":"/home/t/proj","messages":[...]}
```

`barb_session` is the format's version, an integer; this is version 1. A reader refuses a file
whose version it does not know (a newer one) as it refuses any other file that is not a saved
conversation.

`model` is the model it was talking to, and `cwd` the working directory, as the platform spells it
(`C:\Users\...` on Windows). Either may be missing or empty.

`messages` is exactly the `messages` array of the API request the conversation would make next,
without the `cache_control` marks: `{"role":"user"|"assistant","content":[blocks]}`, with the
content always an array. Blocks a version does not know (thinking, redacted thinking, whatever the
API adds) are kept as they came and sent back as they came. A `tool_result`'s content is a string.

The file is written whole and atomically (a temporary file renamed over it), when a request is
sent (not when it is retried), when a reply arrives, and when an interrupt changes the transcript.
So a file can end at any of those points, and in particular with an assistant message whose tool
calls never got results, because Barb was closed while they ran.

## Taking it up

A reader checks the shape (an object; the version; `messages` an array of objects, each with a
role of `user` or `assistant` and content) and refuses anything else as "not a saved Barb
conversation". It then repairs what a cut-off file may hold, so the next request is one the API
accepts:

- messages with nothing that can be sent are dropped, and two messages of one side in a row then
  become one;
- every `tool_use` that has no `tool_result` in the next user message gets one at the start of it
  (made if there is none), `is_error` true, with the content
  `Not run: Barb was closed before this tool call finished.`

The next user message the agent sends then starts with a note that the conversation was saved and
is being resumed, and that files may have changed, so the model reads a file again before it edits
it. If the saved `cwd` is not the working directory now, the note says both, and so does the screen.
The note is sent once; `/clear` drops it.

On a terminal the last 40 messages are shown again as they looked: what the user typed (not
Barb's own `[barb]` lines), the replies, a line for each tool call, and the thinking if verbose is
on.

## The command line

`-c`/`--continue` takes up the newest file (by modification time) in this directory's
`.barb/sessions`; `-r NAME`/`--resume NAME` takes up `NAME` there (with or without `.json`), or
`NAME` itself if it has a slash in it (or, on Windows, a backslash). Resuming goes on saving to the same file. The saved model
is used unless `-m` or `BARB_MODEL` names one. `--sessions` lists the newest 20, with their message
counts and first lines, and needs no API key. `--no-save` saves nothing (and, with `-c` or `-r`,
leaves the file as it was). An interactive run ends with the line
`Saved as NAME; barb -r NAME takes it up again.`
