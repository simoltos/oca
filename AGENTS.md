# Working on OCA

OCA is an experimental, independent terminal agent written in C11. Follow its
own contracts; do not infer behavior or compatibility from other agents.

## Scope

Implement only explicitly requested or selected features. Discussing or
documenting an idea does not authorize implementation.

Treat omitted features and declined proposals as intentional. Add them only
after an explicit user decision. Do not expand scope to match other tools or
conventions. Keep implementation choices and tests within the agreed scope.
Keep these scope rules in this file.

## Architecture

Keep application sources in the repository root:

- `oca.c`: startup, shutdown, CLI parsing, and configuration.
- `oca_core.c/h`: conversations, goals, compaction, tools, skills, workers, and sessions.
- `oca_llm.c/h`: model catalog, credentials, and provider adapters.
- `oca_tui.c/h`: terminal input and presentation.

The TUI calls the core. The core uses normalized LLM requests and events.
Keep provider wire formats and protocol details in the LLM layer.
Regression tests and fixtures live in `tests/`; C test files use `test_<area>.c`.

## C Style

Prefer compact, direct C. Introduce helpers for concrete reuse or clarity.
Remove code made obsolete by a change. Avoid speculative abstractions, layers,
and features.

Use POSIX interfaces, four-space indentation, tabs in Make recipes, and
same-line opening braces. Use `snake_case`, public `oca_` prefixes, uppercase
constants, and `static` for internal symbols. Group related functions under
section comments.

Keep public types opaque unless callers need their fields. Document ownership,
lifetimes, invariants, and reasons that the code does not make clear.
Keep builds warning-free with `-Wall -Wextra -Wpedantic`.

## Build and Verification

Build with a C11 compiler, Make, libcurl, libcjson, and OpenSSL's libcrypto.
Use `CPPFLAGS` and `LDFLAGS` for libraries outside compiler search paths.

- `make oca`: build the executable.
- `./oca --help`: show CLI usage.
- `make install`: install the executable and manual; `PREFIX` sets the executable's installation prefix.
- `make test`: run the offline regression suite.
- `make sanitize`: rebuild and test with AddressSanitizer and UndefinedBehaviorSanitizer.
- `make clean`: remove build outputs.

Match verification to the change:

- For documentation changes, check wording, paths, links, and command accuracy.
- For small code changes, build and run the existing checks for the affected behavior.
- For substantial changes, changes across components, or changes to memory
  ownership, concurrency, parsing, or persistence, run `make test` and `make sanitize`.
- Run both full checks before a release.

Add or update regression coverage where it can catch a meaningful failure.
After sanitizer runs, restore a normal build with `make clean` and `make oca`.
Live provider tests require explicit authorization and limits. Report what was
verified and any remaining limitations.

## Documentation

Use concise American English and active voice. Explain behavior, interfaces,
limits, and non-obvious decisions. Keep documentation consistent with the code
and avoid comments that merely repeat it. Keep private material out of public
documentation.
