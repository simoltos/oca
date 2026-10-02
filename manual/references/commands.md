# Commands and Configuration

## Installation

Build with a C11 compiler, Make, libcurl, libcjson, and libcrypto. The current
build is verified on Linux.

```sh
make oca
make install
```

The default destinations are `~/.local/bin/oca` and
`~/.oca/skills/oca-manual/`. `PREFIX` changes the binary prefix; `MANUALDIR`
changes the manual destination. `DESTDIR` prefixes both destinations for staged
installations. The model catalog is compiled into the executable.
Installation copies files only when their content or
permissions need an update. The manual's
`.installed-files` manifest lets later installations remove obsolete packaged
documents while preserving other files. Unchanged installs are silent.

## Startup

```sh
oca
oca -C /path/to/project
oca models
```

`oca` opens the TUI. A quoted positional argument supplies the initial prompt.
The current directory is the workspace unless `-C` selects another directory.
Starting in a workspace does not require a trust question or registration.

```sh
oca -m k3 --reasoning high
oca -m gpt-6-astra --reasoning high "Explain this project."
oca exec -m deepseek-flash --reasoning off "Summarize AGENTS.md."
```

`-m` accepts a model alias or its remote identifier. `exec` runs one agent turn,
including tool continuations, then exits. Its final answer goes to stdout;
diagnostics and `session_id=<ID>` go to stderr. Syntax errors explain the problem
and point to the relevant command's help. An unsupported reasoning level lists
the available values. `--save` saves a successful
session. Configure credentials before scripted use. A missing usable selection
fails without a provider request or final answer.

| Option | Action |
| --- | --- |
| `-m`, `--model MODEL` | Select a model alias or remote identifier. |
| `-C`, `--chdir PATH` | Select the workspace. |
| `--reasoning LEVEL` | Select a reasoning level declared for the model. |
| `--diagnose` | Create a diagnostic file for this run. |
| `-h`, `--help` | Show help without starting a model request. |
| `--` | Treat following text as a positional argument. |

## Credentials and Models

Explicit model selection requests missing credentials in an interactive
terminal. API keys appear as asterisks while typing. OpenAI uses browser login.
`exec` and redirected runs require credentials to be configured beforehand.
Kimi Code membership uses the Kimi API key flow; it does not use a Moonshot
pay-as-you-go key.

| Provider identifier | Authentication |
| --- | --- |
| `kimi` | API key |
| `openai` | OAuth |
| `deepseek` | API key |

`oca models` lists canonical remote IDs, providers, local authentication state,
and supported reasoning levels. It reads the compiled catalog and credentials
without loading preferences or transferring aliases. `logout` also operates
independently of model preferences. Provider protocols remain part of OCA.
Stored credentials take precedence over `KIMI_API_KEY` and
`DEEPSEEK_API_KEY`. Logout does not unset environment variables.

To replace saved credentials, run `oca logout PROVIDER`, then select a model
using that provider. `--yes` skips the logout confirmation. Credentials remain
in `auth.json`, separate from model definitions and preferences.

## Configuration

The model definitions are compiled into OCA. `~/.oca/config.json` holds optional
aliases and preferences. Credentials remain in `auth.json`.

```json
{
  "model_aliases": {
    "gpt-6-astra": "work"
  },
  "last_used": {
    "model": "k3",
    "reasoning": "high"
  },
  "show": {
    "reasoning": true,
    "read": false,
    "write": true,
    "edit": true,
    "bash": true
  }
}
```

`model_aliases` maps remote IDs to optional user names. IDs remain available
without aliases. Alias names must be unique and cannot match another model ID.
Preferences and saved sessions use canonical remote IDs. An alias for a model
absent from the compiled catalog has no effect.

On the first start after this change, OCA copies aliases for recognized model IDs
from an older `~/.oca/models.json` if `model_aliases` is absent. It leaves the
old file untouched. An existing `model_aliases` field takes precedence.
Invalid or conflicting old aliases are skipped with a warning. Personal-only
model definitions remain in the old file and are unavailable until added to the C catalog.

Explicit `-m` selection uses the model's default reasoning unless `--reasoning`
overrides it. The default is the level at one-based position `round(N / 2)`,
where N is the number of configured levels, ordered lowest to highest; halves
round up. The five ordinary GPT levels select `high`, DeepSeek Flash selects
`low`, and the Kimi entries select `high`. IDs and aliases behave identically.
Starting without model options restores the last usable selection. Resuming
without overrides uses the session's saved selection. OCA has no default model.
Before saving a preference, it reloads the current configuration. An invalid edit stays untouched
and disables preference saving for that invocation.

The catalog is defined in `oca_llm.c`. Change that table and rebuild OCA to
add a model or change its provider, limits, or reasoning levels. Installation
updates the catalog when it updates the executable. No catalog file is required
at runtime.

`context_limit` is OCA's context cap for the selected provider route.
`input_limit` is its local input budget, leaving room for output. The status bar
shows `used/input_limit`, not `used/context_limit`: its denominator is the input
budget, not the advertised model window. The numerator is the provider-reported
input count after a completed request. Before sending a new
request, OCA uses a conservative serialized-byte estimate to decide when to
compact. This estimate can compact earlier than the provider requires.

The compiled catalog includes these models:

| Remote ID | Provider | OCA context cap | OCA input budget | Reasoning levels |
| --- | --- | ---: | ---: | --- |
| `gpt-6-astra` | OpenAI | 872000 | 828400 | `low`, `medium`, `high`, `xhigh`, `max` |
| `gpt-6.1-sol` | OpenAI | 872000 | 828400 | `low`, `medium`, `high`, `xhigh`, `max` |
| `gpt-6-sol` | OpenAI | 872000 | 828400 | `low`, `medium`, `high`, `xhigh`, `max` |
| `gpt-6-luna` | OpenAI | 872000 | 828400 | `low`, `medium`, `high`, `xhigh`, `max` |
| `gpt-5.6-sol` | OpenAI | 872000 | 828400 | `low`, `medium`, `high`, `xhigh`, `max` |
| `gpt-5.6-terra` | OpenAI | 872000 | 828400 | `low`, `medium`, `high`, `xhigh`, `max` |
| `gpt-5.6-luna` | OpenAI | 872000 | 828400 | `low`, `medium`, `high`, `xhigh`, `max` |
| `deepseek-flash` | DeepSeek | 1048576 | 917504 | `off`, `low`, `high`, `max` |
| `k3` | Kimi | 262144 | 245760 | `low`, `high`, `max` |
| `k3-256k` | Kimi | 262144 | 245760 | `low`, `high`, `max` |
| `kimi-for-coding` | Kimi | 1048576 | 1032192 | `low`, `high`, `max` |

For DeepSeek, `off` disables thinking. OpenAI uses browser OAuth; Kimi and
DeepSeek request API keys when needed. Access depends on your provider account.

Catalog metadata was checked on October 1, 2026:

- OpenAI entries use the inspected Codex OAuth profile: an 872,000-token cap
  with 95% usable input, or 828,400 tokens. These local route limits differ from
  public API model limits: the [GPT-6.1 Sol API page](https://developers.openai.com/api/docs/models/gpt-6.1-sol)
  lists 1,050,000 total and 922,000 input tokens. OCA uses the five ordinary
  reasoning efforts; [Codex Ultra](https://learn.chatgpt.com/docs/models)
  requires multi-agent behavior that OCA does not implement.
- DeepSeek Flash's [published model configuration](https://huggingface.co/deepseek-ai/DeepSeek-V4.1-Flash/blob/main/config.json)
  specifies a 1,048,576-token window. OCA subtracts 131,072, leaving
  917,504 for input. This reserve matches the largest default output at `max`
  reasoning. It is not DeepSeek's maximum output, which is 393,216 tokens.
  See the [DeepSeek Chat API](https://api-docs.deepseek.com/api/create-chat-completion/).
- Kimi entries retain OCA's local 16,384-token output reserve. Thus
  `k3-256k` has 262,144 total tokens and 245,760 available for input;
  `kimi-for-coding` has 1,048,576 and 1,032,192 respectively. This reserve is
  an OCA policy, not a documented Kimi output ceiling. `k3` uses the
  Plus/legacy Moderato cap of 262,144; higher plans can support 1,048,576.
  `kimi-for-coding` currently routes to K2.8 Preview.
  See [Kimi Code model configuration](https://www.kimi.com/code/docs/en/kimi-code/models.html).

These input reserves do not set output-token limits in provider requests.
Generation uses provider defaults. The catalog check establishes metadata;
it does not test account access or requests near the context boundary.

Unknown root fields, duplicate keys, and invalid types in `config.json` leave
the file untouched. OCA warns and starts with empty preferences. Models remain
available for explicit selection.

Use `/model` or startup options to select a model and reasoning level.

### Display Preferences

`show` controls display at startup. An omitted object or field uses the defaults
below. An explicit `true` shows that content; `false` hides it.

| Field | Default | Content |
| --- | --- | --- |
| `reasoning` | `true` | Provider-exposed reasoning in gray italic text, when available. |
| `read` | `false` | Read results, including skill instructions and references. |
| `write` | `true` | Changes produced by file creation or replacement. |
| `edit` | `true` | Changes produced by edits. |
| `bash` | `true` | Command output, including live streaming. |

Specify only exceptions. For example, this displays reads and hides reasoning
while retaining the defaults for writes, edits, and Bash:

```json
{
  "show": {
    "read": true,
    "reasoning": false
  }
}
```

Merge the `show` object into your existing configuration to retain aliases and
`last_used`. Restart OCA after changing display settings. Visibility affects
presentation; the model and saved messages retain full tool results and reasoning.
Tool summaries, concise execution states, questions, and goal notices remain visible.
The configurable fields are exactly those in the table. The dedicated `skill`
tool is removed; skill-file visibility follows the ordinary file tools.

To update an older configuration, remove the root fields `show_reasoning` and
`tool_output`, and place your chosen booleans under `show`. There is no automatic
conversion of those fields. Unknown fields, duplicates, and nonboolean `show`
values trigger the existing warning and empty-preferences recovery. The rejected
file stays untouched. Setting defaults explicitly is optional; preference saving
does not add omitted display settings.

See [output display](interactive.md#tool-output-and-markdown) for streaming and replay.

## Tool Access and Temporary Files

OCA runs tools with the process environment and operating-system permissions.
It adds no filesystem or network confinement, permission grants, or `permissions`
tool. `--yolo`, filesystem rules, and network settings have been removed.
Any restrictions supplied by the user or an outer environment still apply.

Relative file paths and Bash commands start in the workspace. `~/.oca` can be
used as an ordinary workspace to edit configuration and installed skills.
Dedicated skill operations resolve files inside the named package. Tool output
bounds, timeouts, cancellation, and worker cleanup remain active.

Tools use the actual system `/tmp` and any configured `$TMPDIR`. Files created
by tools remain ordinary host files; OCA does not remove them at shutdown.

## State Files and Diagnostics

| Path below `~/.oca/` | Content |
| --- | --- |
| `auth.json` | Provider credentials. |
| `config.json` | Model aliases, last selection, and display preferences. |
| `models.json` | Older catalog retained after alias transfer; no longer used for definitions. |
| `sessions/` | Explicitly saved conversations. |
| `history` | Submitted prompt history. |
| `skills/` | Installed instruction packages. |
| `diagnose/` | Diagnostic files created with `--diagnose`. |

Diagnostic files can include prompts, tool arguments, results, and workspace
content; keep them and other state files out of public repositories.

Successful commands return zero, operational failures return one, and invalid
syntax returns two.
