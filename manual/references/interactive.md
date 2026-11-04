# Interactive Use

## Editing and Controls

| Control | Action |
| --- | --- |
| Enter | Execute a selected command or submit input. During work, queue a follow-up. |
| Ctrl+J | Insert a newline. |
| Tab | Switch between question choices and details when the details field is empty. |
| Up/Down | Move through input rows, history, or menu choices. |
| Mouse wheel / touchpad | Scroll conversation output. Hold Shift to select text with the mouse. |
| PageUp/PageDown | Browse retained conversation output in the current chat. |
| Esc | Close a menu or interrupt work. Pending messages then send as soon as possible. |
| Ctrl+X | Retrieve the oldest queued message into the editor, replacing the draft without interrupting work. |
| Ctrl+C during main-chat work | Clear a nonempty draft. With empty input, remove the oldest queued message if present; otherwise do nothing. Use Esc to interrupt work. |
| Ctrl+C while idle | Clear a nonempty draft. With empty input, remove the oldest queued message if present; otherwise show `Press Ctrl+D to exit` above the input box. Another press briefly hides and restores the same hint. |
| Ctrl+D in the main chat | Start exit immediately when idle with empty input; otherwise delete the next grapheme in idle input. |
| `?` in empty input | Show keyboard shortcuts. |
| Alt+K/L | Cycle available models. |
| Alt+,/. | Cycle supported reasoning efforts. |
| Ctrl+/ | Switch between the main and an open temporary conversation. |

Bracketed paste preserves multiline input without submitting it. Cursor movement
and deletion follow UTF-8 graphemes. Chat fills the upper area. Lower controls
contain queued previews, notices, the input box, and status bar. Pending messages
appear above the composer, separated from chat by one blank row and no divider.
OCA delivers them together in order after the active turn's final response.
Tool calls and their continuation requests finish within that turn. The queued
messages then start the next turn, before any automatic goal continuation.

The oldest queued message starts with a bold `queued:` label in the terminal's
normal text color, with one leading space aligned with Working. The message uses
normal weight. Every legend row has the same leading space. Key names are cyan
and descriptions are gray. Esc interrupts active work to send the queue.
Ctrl+X retrieves the oldest message for editing. Multiple pending messages use
`edit first` in the legend. Small views shorten or omit the preview to retain
its shortcut legend.

In the main chat, the legend shows `Ctrl+C: remove` when input is empty.
This removes the oldest queued message without loading it into the editor or interrupting the agent.
With a draft present, Ctrl+C clears only that draft and leaves the queue intact;
the removal hint then becomes available. Further presses remove queued messages
one at a time. The removal hint disappears when the queue is empty.

Menus open below the input box. When room permits, a gray range such as
`1–8 of 9` shows the visible entries. Cyan direction arrows appear together
before the range: `↓`, `↑↓`, or `↑`. The visible window stays in place until the
selection crosses its upper or lower edge. A narrow terminal keeps the arrows
when the range does not fit.

Type `!` into empty input to run a Bash command directly. The composer shows `!`
in place of the user symbol. This uses the same tool executor as agent
commands, and its result becomes part of the conversation.

## Chat Commands

| Command | Action |
| --- | --- |
| `/help` | Show keyboard shortcuts. |
| `/model` | Select model and reasoning. |
| `/skills` | Inspect discovered skill metadata. |
| `/new` | Offer to save, then start a session. |
| `/resume` | Offer to save, then select a saved session. |
| `/goal [objective or action]` | Show or manage a goal. |
| `/hints` | Toggle programming hints; the menu says `Toggle on programming hints` or `Toggle off programming hints`. |
| `/btw [message]` | Open a temporary side chat. |
| `/quit` | Offer to save, then exit or close the temporary chat. |

See [conversations](conversations.md) for goal, hint, and temporary-chat
behavior. Temporary chat exposes only its supported commands and controls.
Enter executes the selected command, including commands with optional arguments.
Argument placeholders disappear when you start typing an argument and return
when you clear it. `/` lists commands. `?` and `/help` show only keyboard
shortcuts, including the `/` control. The help table has one leading space,
cyan key names and command symbols, and gray descriptions aligned in a second column.
When both groups fit, the help entries appear in two side-by-side groups. Smaller
views use one group. Each group keeps its own key and description alignment.

## Inline Questions

When the model asks questions, one card contains the question, choices, optional
details, and guidance. The ordinary input box disappears; the status bar remains.
Up/Down selects a green highlighted choice. Tab and Shift+Tab switch between
choices and the details editor only when details are empty. The return shortcut
reads `Tab: go back`. With text present, both shortcuts leave the editor focused. A green `›` precedes the details field.
`Add details (optional)` is a gray placeholder and disappears when typing starts.
The question uses bold normal text. Choices use normal text except for the
green selection. In the card guidance, key names are cyan and descriptions gray.
Ctrl+J inserts a newline; the card guidance omits that shortcut.
Enter confirms the answer. Ctrl+C in nonempty details clears those details and
keeps the question open. Otherwise Ctrl+C does nothing. Esc cancels the question.
Guidance omits the arrow keys, shows Ctrl+C only with nonempty details, and
shows Tab only when it can switch focus.

The active card has a thin blue rounded border. A confirmed card has a gray
border and retains the question and answer without controls.
Confirmation or cancellation restores the ordinary input box and its previous draft.

Confirmed answers remain attached to their questions instead of appearing as
ordinary user chat messages. The model receives the selected answers and
details as the tool result. Work waits for the complete answer set.

## Tool Output and Markdown

Starting the dedicated Firefox browser requires a `y`/`n` answer, with automatic
denial after 30 seconds. An already running browser is reused without a startup
prompt. The composer draft is retained while this prompt is displayed.

Startup `show` preferences control result bodies. Reasoning, write/edit changes,
and Bash output are visible by default; file-read and browser-result bodies are
hidden by default. Settings apply to the main chat, temporary views, manual
Bash, and saved-session replay. Ctrl+O has no output-expansion action.
The model and saved conversation retain complete results regardless of display.

Lowercase tool labels, paths, search queries, URLs, change counts, and concise failure or interruption
states remain visible. Hidden bodies do not appear inside failure headers.
Questions and goal notices remain usable independently of body settings.

Enabled read bodies, including skills, appear as ordinary text. Enabled writes
and edits retain numbered changes and surrounding context. Line numbers are
gray. Added and deleted lines use dark green and dark red backgrounds across
the full terminal row, including the numbers, with light text. The `+` and `-`
markers are green and red, with one space after the number and two before the text.
Context lines keep the normal background. Omitted sections between visible blocks
appear as a gray `⋮` at the start of the row. Actual blank file lines keep their numbers.
Header counts keep their existing colors.
Enabled Bash bodies stream while the command runs. Each command has one header.
Output rows, wrapped continuations, and completion states have two leading spaces.
A nonzero exit adds a compact red `(exit N)` line directly after the output.
The TUI adds no blank rows before this state; blank rows produced by the command
remain visible. Success adds no completion line; interruption uses `(interrupted)`.
Replay uses the same format.
The stored execution footer remains available to the model.

Model text formats inline code in cyan, bold text, italic text, and boxed tables
with aligned, wrapped cells. Recognized inline delimiters become terminal styles.
Headings, quotes, links, and code fences retain their source markers and use
ordinary terminal text. Link destinations remain visible.
Inline code and emphasis still render inside headings, quotes, and list text.
Fenced code stays literal, without syntax colors or Markdown interpretation.
User prompts remain literal.

Programming hints have a bold white `Hint` badge on a dark teal background.
Quoted continuation lines align below the badge.
Only the exact `> **Hint:**` marker at a prose line boundary starts a hint;
code blocks and inline occurrences remain ordinary content.

## Status and Transcript

The welcome banner starts at the top of the screen. It shows the workspace and
`[AGENTS.md]` when project instructions are loaded. Use `/skills` to list available
skills. The status bar shows the selected model, reasoning, active-context usage,
and the most recent available cache-hit
percentage. The context count is the provider-reported input for the latest
completed request. Before a measurement is available, it appears as a dash.
The denominator is OCA's input budget after the output reserve, not the model's
full context window. See the [catalog limits](commands.md#configuration).
Goal state and active duration appear at the right with one trailing space,
after `hints` when enabled.
The entire goal group uses purple; `hints` uses plain teal text.
The duration shows seconds below one minute, then minutes or hours and minutes.
A separate notice row displays work
duration or transient messages. Working and status rows start with one space.
Working includes `(Esc to interrupt)`, with `Esc` in cyan, when no messages are queued.
Queued messages have their own interruption legend.
The idle exit hint occupies the same operational row in bold yellow, aligned with Working.
Repeated Ctrl+C presses hide that single hint for 50 ms and restore it. Typing hides it.
A single Ctrl+D starts the normal exit procedure from idle, empty main input.

Ordinary user messages and the input prompt use a darker green `›`. Model messages have no icon or reserved gutter.
Reasoning is visible when available unless `show.reasoning` is false at startup. It
appears as plain gray italic text, including in replay and temporary chat.
There are no Reasoning or Reasoned timers or completion labels.
The Working indicator continues to show elapsed work time.
The transcript separates messages with two blank lines.

OCA uses the terminal's alternate screen throughout the interactive session.
Resizing reflows retained conversation output and updates the editor, menus,
and status bar. PageUp/PageDown browse retained output in both the main and
temporary chats. Exiting restores the terminal's previous screen and scrollback.
Save the conversation to reopen it later; chat output does not enter the shell's
scrollback.
