---
name: oca-manual
description: Explain OCA commands, configuration, models, terminal controls, tools, goals, sessions, and skills when users ask how to use OCA.
---

# OCA User Manual

Use this skill for questions about operating OCA, an independent terminal agent.

Read the reference for the user's topic with the ordinary `read` tool. Resolve
its relative path from this manual's directory, using the absolute `SKILL.md`
path in OCA's skill catalog. Then explain the relevant behavior
with practical examples. These files describe the OCA source distributed with this
manual; they do not require the development checkout or any other skill.

Use `oca --help`, command-specific help, or `oca models` to confirm the installed
interface when needed. If the installed executable differs from this manual,
identify the difference instead of presenting an unsupported command as usable.
Keep unimplemented proposals outside usage instructions.

- Read [commands and configuration](references/commands.md) for installation,
  startup, credentials, models, tool access, and state files.
- Read [interactive use](references/interactive.md) for editing, controls,
  commands, inline questions, output display, and status indicators.
- Read [conversations and skills](references/conversations.md) for project
  instructions, sessions, goals, temporary chat, hints, and skills.
