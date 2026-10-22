CC ?= cc
CFLAGS ?= -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -O2
LDLIBS = -pthread -lm -lcurl -lcjson -lcrypto
PREFIX ?= $(HOME)/.local
MANUALDIR ?= $(HOME)/.oca/skills/oca-manual
MANUAL_FILES = SKILL.md references/commands.md references/interactive.md references/conversations.md

SOURCES = oca.c oca_core.c oca_llm.c oca_tui.c
OBJECTS = $(SOURCES:.c=.o)
TESTS = tests/test_llm tests/test_core tests/test_cli tests/test_tui tests/test_kimi tests/test_config tests/test_executor tests/test_browser tests/test_login tests/test_providers tests/test_compaction tests/test_transcript tests/test_tui_core tests/test_ephemeral
TEST_OBJECTS = $(TESTS:%=%.o)

.PHONY: all install test sanitize clean

all: oca

oca: $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -c -o $@ $<

# ============================================================================
# Installation
# ============================================================================

# Record packaged files so obsolete references can be removed without pruning user additions.
install: oca $(addprefix manual/,$(MANUAL_FILES))
	@set -eu; \
	install_if_changed() { \
		source=$$1; target=$$2; mode=$$3; item=$$4; quiet=$${5:-0}; \
		if [ -f "$$target" ] && [ ! -L "$$target" ] && \
			cmp -s "$$source" "$$target" && \
			[ -n "$$(find "$$target" -prune -perm "$$mode" -print)" ]; then return; fi; \
		action=updated; [ -e "$$target" ] || [ -L "$$target" ] || action=installed; \
		install -m "$$mode" "$$source" "$$target"; \
		if [ "$$quiet" = 1 ]; then manual_changed=1; else echo "$$item: $$action"; fi; \
	}; \
	bin_dir="$(DESTDIR)$(PREFIX)/bin"; \
	[ -d "$$bin_dir" ] || install -d "$$bin_dir"; \
	install_if_changed oca "$$bin_dir/oca" 0755 oca; \
	manual_dir="$(DESTDIR)$(MANUALDIR)"; \
	manual_changed=0; manual_action=updated; \
	[ -e "$$manual_dir/.installed-files" ] || manual_action=installed; \
	for dir in "$$manual_dir" "$$manual_dir/references"; do \
		if [ -L "$$dir" ]; then echo "Manual directory is a symbolic link: $$dir" >&2; exit 1; fi; \
		[ -d "$$dir" ] || install -d "$$dir"; \
	done; \
	manifest="$$manual_dir/.installed-files"; \
	if [ -e "$$manifest" ] || [ -L "$$manifest" ]; then \
		if [ ! -f "$$manifest" ] || [ -L "$$manifest" ]; then \
			echo "Manual file list must be a regular file: $$manifest" >&2; exit 1; \
		fi; \
		while IFS= read -r file || [ -n "$$file" ]; do \
			case "$$file" in \
				SKILL.md) ;; \
				references/*) \
					name=$${file#references/}; \
					case "$$name" in ""|.*|*/*) echo "Invalid manual file: $$file" >&2; exit 1;; esac ;; \
				*) echo "Invalid manual file: $$file" >&2; exit 1 ;; \
			esac; \
			case " $(MANUAL_FILES) " in *" $$file "*) ;; *) rm -f "$$manual_dir/$$file"; manual_changed=1 ;; esac; \
		done < "$$manifest"; \
	fi; \
	for file in $(MANUAL_FILES); do \
		install_if_changed "manual/$$file" "$$manual_dir/$$file" 0644 "$$file" 1; \
	done; \
	if ! printf '%s\n' $(MANUAL_FILES) | cmp -s - "$$manifest"; then \
		record=$$(mktemp "$$manual_dir/.installed-files.XXXXXX"); \
		trap 'rm -f "$$record"' 0; \
		trap 'exit 1' 1 2 3 15; \
		printf '%s\n' $(MANUAL_FILES) > "$$record"; \
		chmod 0644 "$$record"; \
		mv -f "$$record" "$$manifest"; \
		manual_changed=1; \
	elif [ -z "$$(find "$$manifest" -prune -perm 0644 -print)" ]; then \
		chmod 0644 "$$manifest"; \
		manual_changed=1; \
	fi; \
	[ "$$manual_changed" = 0 ] || echo "manual: $$manual_action"

# ============================================================================
# Tests
# ============================================================================

tests/test_llm: tests/test_llm.o tests/fixture_llm.o tests/fixture_server.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

tests/test_core: tests/test_core.o tests/oca_core_fixture.o tests/fixture_llm.o tests/fixture_server.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

tests/test_cli: tests/test_cli.o tests/fixture_server.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

tests/test_tui: tests/test_tui.o tests/fixture_server.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

tests/test_kimi: tests/test_kimi.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

tests/test_transcript: tests/test_transcript.o oca_core.o oca_llm.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

tests/test_tui_core: tests/test_tui_core.o tests/fixture_llm.o tests/fixture_server.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

tests/test_ephemeral: tests/test_ephemeral.o tests/oca_core_fixture.o tests/fixture_server.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

tests/test_login: tests/test_login.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

tests/test_providers: tests/test_providers.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

tests/test_compaction: tests/test_compaction.o oca_core.o tests/compaction_llm.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

tests/test_config: tests/test_config.o oca_core.o oca_llm.o oca_tui.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

tests/test_executor: tests/test_executor.c oca_core.c oca_core.h oca_llm.c oca_llm.h tests/fixture_llm.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -I. -o $@ tests/test_executor.c tests/fixture_llm.c $(LDLIBS)

tests/test_browser: tests/test_browser.c oca_core.c oca_core.h oca_llm.c oca_llm.h tests/fixture_llm.c tests/fixture_models.h tests/fixture_server.c tests/fixture_server.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -I. -o $@ tests/test_browser.c tests/fixture_llm.c tests/fixture_server.c $(LDLIBS)

tests/oca_core_fixture.o: oca_core.c oca_core.h oca_llm.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -c -o $@ $<

tests/oca-fixture: oca.o tests/oca_core_fixture.o tests/fixture_llm.o oca_tui.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

tests/fixture_browser_core.o: tests/fixture_browser_core.c oca_core.c oca_core.h oca_llm.h

tests/oca-browser-fixture: oca.o tests/fixture_browser_core.o tests/fixture_llm.o oca_tui.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Ordinary tests run real tool workers and local provider fixtures.
test: oca tests/oca-fixture tests/oca-browser-fixture $(TESTS)
	./tests/test_llm
	./tests/test_core
	./tests/test_cli ./tests/oca-fixture ./oca
	./tests/test_tui ./tests/oca-fixture
	./tests/test_kimi
	./tests/test_config
	./tests/test_executor ./tests/test_executor
	./tests/test_browser
	./tests/test_browser --tui ./tests/oca-browser-fixture
	./tests/test_login ./tests/oca-fixture
	./tests/test_providers
	./tests/test_compaction
	./tests/test_transcript
	./tests/test_tui_core
	./tests/test_ephemeral
	sh ./tests/install_regression.sh

# ============================================================================
# Sanitizers
# ============================================================================

sanitize: CFLAGS += -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='$(CFLAGS)' test

# ============================================================================
# Cleanup
# ============================================================================

clean:
	rm -f oca $(OBJECTS) $(TEST_OBJECTS) $(TESTS) tests/oca tests/oca-fixture tests/oca-browser-fixture tests/oca_core_fixture.o tests/fixture_browser_core.o tests/fixture_llm.o tests/fixture_server.o tests/compaction_llm.o

# ============================================================================
# Dependencies
# ============================================================================

oca.o: oca.c oca_core.h oca_llm.h oca_tui.h
oca_core.o: oca_core.c oca_core.h oca_llm.h
oca_llm.o: oca_llm.c oca_llm.h
oca_tui.o: oca_tui.c oca_tui.h oca_core.h oca_llm.h
tests/fixture_llm.o: tests/fixture_llm.c oca_llm.c oca_llm.h tests/fixture_models.h
tests/test_llm.o: tests/test_llm.c oca_llm.h tests/fixture_models.h
tests/test_core.o: tests/test_core.c oca_core.h oca_llm.h tests/fixture_models.h
tests/test_cli.o: tests/test_cli.c tests/fixture_models.h
tests/test_tui.o: tests/test_tui.c tests/fixture_models.h
tests/test_kimi.o: tests/test_kimi.c oca_llm.c oca_llm.h tests/fixture_models.h
tests/test_kimi.o tests/test_tui_core.o: tests/fixture_io.h

tests/test_config.o: tests/test_config.c oca.c oca_core.h oca_llm.h oca_tui.h tests/fixture_models.h
tests/test_executor.o: tests/test_executor.c oca_core.h oca_llm.h tests/fixture_llm.c
tests/test_login.o: tests/test_login.c

tests/fixture_server.o: tests/fixture_server.c tests/fixture_server.h

tests/test_llm.o tests/test_core.o tests/test_cli.o tests/test_tui.o: tests/fixture_server.h

tests/test_providers.o: tests/test_providers.c oca_llm.c oca_llm.h tests/fixture_models.h

tests/test_compaction.o: tests/test_compaction.c oca_core.h oca_llm.h tests/fixture_models.h
tests/compaction_llm.o: tests/compaction_llm.c oca_llm.c oca_llm.h
tests/test_transcript.o: tests/test_transcript.c oca_tui.c oca_tui.h oca_core.h oca_llm.h
tests/test_tui_core.o: tests/test_tui_core.c oca_core.c oca_core.h oca_llm.h tests/fixture_models.h
tests/test_ephemeral.o: tests/test_ephemeral.c oca_llm.c oca_core.h oca_llm.h tests/fixture_models.h
