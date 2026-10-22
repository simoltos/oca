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

`google_search` accepts a nonempty `query` and searches Google in a visible
Firefox browser. It returns up to 20 visible external links plus a 1,200-character
snapshot of page text. `visit_page` accepts a nonempty `url`, opens it in Firefox,
and extracts rendered Markdown. Neither tool requires a search API key.

The workflow follows [ds4's browser implementation](https://github.com/antirez/ds4/blob/5379c1c9ac315f1540371b06a3db88491d4059ff/ds4_web.c)
and [tool output wrappers](https://github.com/antirez/ds4/blob/5379c1c9ac315f1540371b06a3db88491d4059ff/ds4_agent.c)
at revision `5379c1c9ac315f1540371b06a3db88491d4059ff`. Firefox DevTools replaces
Chrome CDP. The consent, scrolling, search, and page extraction scripts are
preserved from that revision; offline fixtures pin those scripts.

OCA first tries the local browser endpoint at `127.0.0.1:9333`. If Firefox must
be started, the TUI asks for `y` or `n`, with automatic denial after 30 seconds.
Approval lasts for that OCA invocation. Noninteractive execution can use an
already running browser, but cannot approve a new browser startup.

Firefox uses the persistent profile `~/.oca/browser`, separate from the user's
regular profile. The browser and profile remain after OCA exits or a tool is
canceled; later invocations can reuse them, including their site cookies.
Each call opens a background tab and closes that tab afterward, keeping at
least one tab open. If the user closes Firefox, the next call can start it again.
Set `OCA_FIREFOX` to an executable path or command name when Firefox is not on
`PATH`. Firefox 140 or newer is required; the implementation is tested with 157.
The profile enables local DevTools access and suppresses first-run prompts,
default-browser checks, browser-account sync, and audio, corresponding to the
upstream Chrome startup options. It does not alter the user's regular profile.

Like ds4, navigation waits for page readiness and stabilization. The original
consent script clicks matching accept/agree controls. `visit_page` additionally
probes scrolling for lazy-loaded content, then uses the original full-body DOM
extractor and includes up to 80 visible links. The extractor marks truncation
after its approximately 900,000-character threshold. A browser protocol packet
is limited to 4 MiB. This is DOM extraction, without separate PDF, OCR, image,
or video processing; other formats can be handled through `bash`.

The complete extracted text is written to `/tmp/oca_agent_web_XXXXXX`. Results
include `output_path`, byte and line counts, and either the full Markdown or a
preview limited to 100 lines and 8 KiB. Use `read` with `offset` and `limit` to
inspect the saved text. OCA preserves UTF-8 at the preview boundary. Temporary
page files remain after shutdown and follow the system's normal `/tmp` lifetime.

CAPTCHA and access-denial pages can appear in the rendered output, as in ds4;
successful extraction does not prove that search results or the requested
article were obtained. Browser choice and engine-specific behavior can still
affect site responses. Firefox DevTools uses connection-scoped actors, allowing
reconnection after interruption without a retained WebDriver session.

The upstream search script unwraps `/url?q=...` links but filters out other
Google-hosted URLs, including `/goto?url=...`. A results page using the latter
format can therefore produce a text snapshot without result links. This is
preserved upstream behavior. Firefox also lacks Chrome's optional console
`getEventListeners` helper; the unchanged scrolling script uses its existing
DOM-marker and `onscroll` checks in that case.

Page text is untrusted source material. Keep its URL when using it as evidence.
Browser results use the `show.read` preference; query and URL summaries remain visible.

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
