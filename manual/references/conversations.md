# Conversations, Goals, and Skills

## Project Instructions and Sessions

OCA reads the workspace-root `AGENTS.md` when creating a session and preserves
that project context. It does not search parent or nested instruction files.
Resuming restores saved context; editing the current file does not replace the
saved instructions.
A missing `AGENTS.md` contributes no project context. An unreadable or invalid
file reports an error when creating a new session. Resuming restores the
project context already stored in that session.

```sh
oca sessions
oca resume SESSION_ID
oca resume SESSION_ID -m k3 --reasoning high
oca resume SESSION_ID -C /path/to/project
oca delete SESSION_ID
oca delete SESSION_ID --yes
```

`sessions` lists full IDs, titles, and local last-used dates in
`YYYY-MM-DD HH:MM` format, newest first. Session listing and deletion load only
session data, independently of model configuration and credentials. `resume` opens the TUI
with the saved workspace unless `-C` overrides it. Model, reasoning, and diagnostic
overrides are available; `resume` does not accept a new positional prompt.
Delete asks for confirmation unless `-y` or `--yes` is supplied.
The model example uses the bundled `k3` ID with `high` reasoning. A saved
model absent from the current catalog requires another explicit selection.
Reading a saved session does not rewrite it or its provider state.
Historical `plan` sessions open in Normal mode.

Sessions are not saved automatically. Leaving a dirty conversation through exit,
new-session creation, or resume asks whether to save. Failed saves offer recovery
choices. Compaction reduces active model context using a summary and recent
messages while retaining the durable conversation log.

## Goals

```text
/goal Add the requested feature and run its checks.
/goal
/goal pause
/goal resume
/goal edit
/goal clear
```

An objective starts or replaces a goal. Bare `/goal` immediately displays
`goal: OBJECTIVE`, even during generation. The label is purple and the objective
is gray. State and active duration appear in the status bar. With no goal, it
reports only `No goal set.` in light lilac. These local display messages do not
enter the model context; the active objective still guides the agent.
Active goals continue automatically until completion, blocking, or pause;
queued prompts run before automatic continuations.

Pause prevents future continuations without canceling current work. Esc interrupts current work and pauses the goal when no menu is open.
Ctrl+C clears a draft or removes the oldest queued prompt without interrupting work.
With empty input and no queue, it does nothing during work. Edit pauses the goal and returns its
objective to an empty composer. Resume accepts paused or blocked goals. Clear
removes the objective. Goals have no configurable token or time budget.
Starting a goal adds `goal: OBJECTIVE` to chat, with the objective in gray.
Completion adds only `goal completed: OBJECTIVE`; the status bar shows duration.
The duration excludes paused and blocked periods. It shows seconds below one minute,
then minutes or hours and minutes.
A blocked goal appears as `goal blocked`.
The status bar retains the completed state and duration until the goal is replaced or cleared.
Elapsed goal time survives session saving and resuming.

## Temporary Chat

`/btw` opens one temporary text conversation using a snapshot of the main chat's
latest stable context. `/btw your question` also submits an initial message.
Ctrl+/ switches views while preserving each draft.
While the temporary chat is open, the status bar names the visible conversation:
`Main chat (Ctrl+/ to switch)` or
`Temporary chat (Ctrl+/ to switch · Ctrl+C to close)`.
Shortcut names are cyan; the labels and descriptions are gray.

The temporary conversation has no tools and does not continue the main goal.
Later main-chat changes do not update its snapshot. Its responses are not merged
back into the main conversation or saved as a session. Closing it discards it;
copy any text you want to retain. In its view, Ctrl+C or `/quit` closes it, while
Ctrl+D deletes the next grapheme.

## Programming Hints

`/hints` toggles programming hints. The status bar shows `hints` while enabled, and the
change applies at the next conversation boundary. Hints are optional
model-generated prose, not a separate tool or a guarantee of a hint on every
response.

New and resumed main sessions start with hints disabled. A temporary chat
inherits the main chat's setting when created, then keeps its own setting.

## Tools

OCA provides bounded `read`, `write`, `edit`, and synchronous `bash` tools.
Relative paths and Bash commands start in the workspace. Tools inherit ordinary
filesystem and network access, as described in [configuration](commands.md).

`write` creates or replaces a text file; `edit` replaces one exact matching byte
sequence. Bash accepts a single `command` string. That string can contain
multiple shell commands; this does not create additional tool calls by itself.

`ask_user` presents one to three questions. Each supplies choices; the interface
adds `None of the above` and optional details. If the model requests user input
during `exec`, the run reports an error and cancels instead of assuming an answer.

## Prompt History

OCA stores submitted main prompts in `~/.oca/history` as JSON Lines: each line
is one JSON string. Escaped newlines preserve the exact submitted text when
decoded. The file contains no headers, entry counts, numbering, or conversation
transcripts. Existing size, entry-count, and UTF-8 limits still apply.

## Skills

Each skill occupies one direct child directory under `~/.oca/skills/`. Its
`SKILL.md` starts with `name` and `description` frontmatter; the name must match
the installed directory. The Markdown body contains instructions, with relative
links to supporting resources.

Use `/skills` to inspect the catalog. OCA gives the model each discovered
skill's name, description, and absolute `SKILL.md` path. Metadata does not load
the instructions. Name a skill in a normal prompt, for example:
`Use oca-manual to explain goals.` The model reads its listed file with `read`.

Resolve relative links from the directory containing the skill file, rather
than from the project workspace. For example, a `references/conversations.md`
link in the installed manual refers to that manual's `references` directory.
Use an absolute path in the `read` call. The same method loads other skills.

Create or update skill files with `write` and `edit`, using Bash for directory
operations when needed. Ordinary file tools apply their usual limits and error
handling. Startup discovery validates `SKILL.md` metadata; writes do not run a
separate skill validator. A changed file can be read immediately. New packages
or changed metadata appear in `/skills` and model metadata after restarting OCA.
There is no dedicated `skill` tool. Display follows `show.read`, `show.write`,
and `show.edit` like any other file operation.

Skills cannot register tools or execute scripts automatically. The bundled manual is maintained in `manual/` and installed by
`make install`; it has no dependency on personal skills or development documents.
