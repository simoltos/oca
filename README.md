# OCA

OCA is a small terminal agent. It is the place for models that are now
scattered across different harnesses, making them easily accessible without
switching interfaces. The plan is also to ship OCA preinstalled in a Linux
distribution and even with local models so it can work even without network
access or initial configuration.

## Build and install

A C11 compiler, make, libcurl, libcjson and OpenSSL's libcrypto are needed
to build the agent. Use `make oca` and then run `./oca`. Run `make install`
to install OCA in `~/.local/bin` by default. `PREFIX` changes the installation
prefix; the executable goes in `PREFIX/bin`. The [manual](manual/SKILL.md) is also
installed by that command as a skill in `~/.oca/skills/oca-manual`.
If it's alredy present, it is updated when needed.

The `google_search` and `visit_page` tools use Firefox 140 or newer through its
native DevTools protocol. They follow ds4's browser workflow: visible browsing,
a persistent dedicated profile, and a browser that remains open when OCA exits.
`OCA_FIREFOX` selects the executable. See the [tool documentation](manual/references/conversations.md#tools)
for startup approval, output limits, and the pinned upstream reference.

## Models and aliases

The model catalog is built into OCA and updating the executable updates its model
definitions. You can set an alias for every supported model in `~/.oca/config.json`.
Sipported models are listed with `oca models`.

## The manual

The installed manual is a skill that OCA discovers at startup, from any working
directory. Its content is read when needed. It can also be used by other
agents, and it's the fastest and easiest way to understand the software and
its configuration. The manual is fundamentally written to let agents manage
that task easily.

If the configuration is broken, OCA ignores it and starts with default settings.
However, with a model selected and the manual installed, OCA can fix its own
configuration from any working directory. Other agents can use the manual to do
the same.

## First run

Simply start a supported model, like:
```sh
oca -m gpt-6-astra
```

or, if you set `astra` as an alias for this:
```sh
oca -m astra
```

You can set the reasoning level alongside the model with the `--reasoning`
flag, the `alt+./alt+,` shortcuts in the TUI, or the `/model` command:
after selecting a model, you can choose its reasoning level.

Each model corresponds to a provider. If its credentials aren't saved,
OCA prompts you to authenticate with an API key or OAuth before running the
model.

The interactive interface uses the terminal's alternate screen. PageUp/PageDown
browse conversation output. Exiting restores the previous terminal screen;
save the conversation to reopen it later.

## Why?

Almost every major vendor has its own harness and, while this seems to allow
choice, it's fragmenting ideas across different harnesses, each with its own
sessions, models and methods. Above all, however, one of these vendors can
decide to take a bad direction for its agent, making it practically unusable.

## Security

OCA runs within the boundaries and permissions of the user that
started it. Accordingly, it's the user's responsability to sandbox
or containerize the agent.
