#define VIEW_HISTORY_MAX_BYTES 4096
#define VIEW_HISTORY_MAX_ROWS 64
#include "../oca_tui.c"

#include <assert.h>

static void test_wrapping(void) {
    display_lines lines = {0};
    conversation_wrap(&lines, CONVERSATION_ASSISTANT, 0, "abcdef\nz", 6);
    assert(lines.count == 2);
    assert(!strcmp(lines.items[0].text, "abcdef"));
    assert(!strcmp(lines.items[1].text, "z"));
    assert(lines.items[0].source_end == 7 && lines.items[1].source_end == 8);
    lines_free(&lines);

    /* Words move to the next row instead of splitting at the column edge. */
    conversation_wrap(&lines, CONVERSATION_ASSISTANT, 0, "hello world", 8);
    assert(lines.count == 2);
    assert(!strcmp(lines.items[0].text, "hello "));
    assert(!strcmp(lines.items[1].text, "world"));
    assert(lines.items[0].soft_wrap && lines.items[0].source_end == 6);
    lines_free(&lines);

    /* A word too long for a fresh row fills the current row and splits. */
    conversation_wrap(&lines, CONVERSATION_ASSISTANT, 0, "a bbbbbbbb", 4);
    assert(lines.count == 3);
    assert(!strcmp(lines.items[0].text, "a bb"));
    assert(!strcmp(lines.items[1].text, "bbbb"));
    assert(!strcmp(lines.items[2].text, "bb"));
    lines_free(&lines);

    /* Styles spanning the break reopen on the next row; rows rejoin exactly. */
    conversation_wrap(&lines, CONVERSATION_ASSISTANT, 0,
                      "one \x1b[0;1mtwo three\x1b[0m four", 10);
    assert(lines.count == 2);
    assert(!strcmp(lines.items[0].text, "one \x1b[0;1mtwo "));
    assert(lines.items[0].source_end == strlen("one \x1b[0;1mtwo "));
    assert(!strcmp(lines.items[1].text, "\x1b[0;1mthree\x1b[0m four"));
    lines_free(&lines);

    conversation_wrap(&lines, CONVERSATION_USER, 0, "👩‍💻éB", 5);
    assert(lines.count == 2);
    assert(!strcmp(lines.items[0].text, STYLE_USER "›" STYLE_RESET " 👩‍💻é"));
    assert(!strcmp(lines.items[1].text, "  B"));
    assert(!strcmp(lines.items[0].style, STYLE_RESET));
    lines_free(&lines);

    conversation_wrap(&lines, CONVERSATION_ASSISTANT, 0, "界x", 1);
    assert(lines.count == 2);
    assert(lines.items[0].source_end == strlen("界"));
    assert(lines.items[1].source_end == strlen("界x"));
    lines_free(&lines);

    char *safe = conversation_safe("a\x1b]0;bad\a\r\nb\t\x7f");
    assert(!strcmp(safe, "a\\x1B]0;bad\\x07\nb    \\x7F"));
    free(safe);

    conversation_wrap(&lines, CONVERSATION_DIFF, 0, "abcdefghij\nz", 10);
    assert(lines.count == 2);
    assert(!strcmp(lines.items[0].text, "abcdefghij"));
    assert(!strcmp(lines.items[1].text, "z"));
    lines_free(&lines);

    conversation_wrap(&lines, CONVERSATION_BASH, 0, "abcdefgh\n\nx\n", 6);
    assert(lines.count == 4);
    assert(!strcmp(lines.items[0].text, "  abcd"));
    assert(!strcmp(lines.items[1].text, "  efgh"));
    assert(!strcmp(lines.items[2].text, "  "));
    assert(!strcmp(lines.items[3].text, "  x"));
    lines_free(&lines);

    tui_app input = {.input = "abcdefghi\nxy", .input_len = 12, .cursor = 12};
    int row = 0, column = 0;
    composer_wrap(&input, &lines, STYLE_RESET, 8, &row, &column);
    assert(lines.count == 3 && row == 2 && column == 4);
    assert(!strcmp(lines.items[0].text, STYLE_USER "›" STYLE_RESET " abcdef"));
    assert(!strcmp(lines.items[1].text, "  ghi") && !strcmp(lines.items[2].text, "  xy"));
    lines_free(&lines);
    input.input = "! printf x";
    input.input_len = input.cursor = strlen(input.input);
    composer_wrap(&input, &lines, STYLE_RESET, 20, &row, &column);
    assert(!strcmp(lines.items[0].text, STYLE_DANGER "!" STYLE_RESET " printf x"));
    lines_free(&lines);
    input.input = "";
    input.input_len = input.cursor = 0;
    composer_wrap(&input, &lines, STYLE_SUBTLE, 80, &row, &column);
    assert(row == 0 && column == 2);
    assert(!strcmp(lines.items[0].text, STYLE_USER "›" STYLE_SUBTLE " Ask OCA anything"));
    lines_free(&lines);
    for (int cols = 1; cols <= 3; cols++) {
        composer_wrap(&input, &lines, STYLE_SUBTLE, cols, &row, &column);
        for (size_t i = 0; i < lines.count; i++) assert(text_columns(lines.items[i].text) <= cols);
        lines_free(&lines);
    }
    input.menu = MENU_INTERACTION;
    input.input = "!literal";
    input.input_len = input.cursor = strlen(input.input);
    composer_wrap(&input, &lines, STYLE_RESET, 40, &row, &column);
    assert(!strcmp(lines.items[0].text, STYLE_USER "›" STYLE_RESET " !literal"));
    lines_free(&lines);
}

static void test_help_layout(void) {
    for (int side = 0; side < 2; side++) {
        tui_app app = {.cols = 120, .side_chat = side};
        help_menu(&app);
        display_lines lines = {0};
        size_t columns = menu_columns(&app);
        assert(columns == 2);
        size_t count = (app.menu_count + columns - 1) / columns;
        append_menu(&app, &lines, count, count, columns);
        size_t description_column = 0;
        for (size_t i = 0; i < lines.count; i++) {
            const display_line *row = &lines.items[i];
            assert(row->text[0] == ' ' && row->text[1] != ' ');
            size_t key_end = 1 + strcspn(row->text + 1, " ");
            size_t description = key_end + strspn(row->text + key_end, " ");
            if (!i) description_column = description;
            assert(description == description_column);
            assert(row->accent_start == 1 && row->accent_end == key_end);
            assert(!strcmp(row->accent_style, STYLE_INLINE));
            assert(!strcmp(row->style, STYLE_MUTED));
            if (i * columns + 1 < app.menu_count) {
                size_t second = (size_t)app.cols / columns;
                assert(row->text[second] == ' ' && row->text[second + 1] != ' ');
                size_t second_key_end = second + 1 + strcspn(row->text + second + 1, " ");
                size_t second_description = second_key_end + strspn(row->text + second_key_end, " ");
                assert(second_description - second == description_column);
                assert(row->accent2_start == second + 1 && row->accent2_end == second_key_end);
                assert(!strcmp(row->accent2_style, STYLE_INLINE));
            } else assert(!row->accent2_style);
        }
        lines_free(&lines);
        app.cols = 80;
        assert(menu_columns(&app) == 1);
        app.cols = 8;
        columns = menu_columns(&app);
        assert(columns == 1);
        append_menu(&app, &lines, 3, app.menu_count, columns);
        for (size_t i = 0; i < lines.count; i++) {
            assert(text_columns(lines.items[i].text) <= 8);
            assert(lines.items[i].accent_end <= strlen(lines.items[i].text));
        }
        lines_free(&lines);
        app.menu_selection = app.menu_count - 1;
        app.cols = 120;
        columns = menu_columns(&app);
        count = (app.menu_count + columns - 1) / columns;
        append_menu(&app, &lines, 3, count, columns);
        assert(app.menu_selection == count - 1);
        lines_free(&lines);
        app_free(&app);
    }
}

static void test_question_editor_marker(void) {
    char *options[] = {"first choice", "second choice"};
    oca_interaction_question question = {
        .question = "Choose one", .options = options, .option_count = 2
    };
    tui_app app = {
        .menu = MENU_INTERACTION, .interaction_details = 1,
        .questions = &question, .question_count = 1,
        .input = "detail text that wraps\nsecond line", .cols = 80
    };
    app.input_len = app.cursor = strlen(app.input);
    for (int cols = 80; cols >= 8; cols = cols == 80 ? 11 : 8) {
        app.cols = cols;
        display_lines rows = {0};
        question_lines(&app, &rows, NULL, NULL);
        assert(rows.count > 0 && rows.items[0].card_role == 1);
        size_t markers = 0;
        for (size_t i = 0; i < rows.count; i++)
            markers += strstr(rows.items[i].text, "›") != NULL;
        assert(markers == 1);
        assert(!strstr(rows.items[1].text, "detail text"));
        lines_free(&rows);
        if (cols == 8) break;
    }
    display_lines composer = {0};
    int row = 0, column = 0;
    composer_wrap(&app, &composer, STYLE_RESET, 80, &row, &column);
    assert(strstr(composer.items[0].text, STYLE_USER "›"));
    lines_free(&composer);
}

static void test_metrics(void) {
    struct { int hit_known, input_known; long hit, input; const char *expected; } cases[] = {
        { 1, 1, 1600, 2000, "cache hit 80%" },
        { 1, 1, 0, 20, "cache hit 0%" },
        { 1, 1, 20, 20, "cache hit 100%" },
        { 1, 1, 2, 3, "cache hit 67%" },
        { 1, 1, LONG_MAX, LONG_MAX, "cache hit 100%" },
        { 1, 1, 0, 0, "cache hit —" },
        { 0, 1, 0, 20, "cache hit —" },
        { 1, 0, 10, 0, "cache hit —" },
        { 1, 1, 21, 20, "cache hit —" }
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        oca_core_status status = { .has_cache = cases[i].hit_known,
            .has_cache_input = cases[i].input_known, .cache_hit_tokens = cases[i].hit,
            .cache_input_tokens = cases[i].input };
        char text[96];
        cache_text(&status, text, sizeof(text));
        assert(!strcmp(text, cases[i].expected));
    }
    oca_core_status status = { .working_ms = 0 };
    char text[96];
    working_text(&status, text, sizeof(text));
    assert(!strcmp(text, "Working 0s"));
    status.working_ms = 59999;
    working_text(&status, text, sizeof(text));
    assert(!strcmp(text, "Working 59s"));
    status.working_ms = 72000;
    working_text(&status, text, sizeof(text));
    assert(!strcmp(text, "Working 1m 12s"));
    status.working_ms = 3723000;
    working_text(&status, text, sizeof(text));
    assert(!strcmp(text, "Working 1h 02m 03s"));
}

static char *without_styles(const char *text) {
    text_buffer plain = {0};
    for (size_t at = 0; text[at];) {
        size_t length = sgr_length(text + at);
        if (length) at += length;
        else buffer_append(&plain, text + at++, 1);
    }
    return plain.data ? plain.data : copy_text("");
}

static void test_change_counts(void) {
    const char *valid[] = {
        "Edit applied. (+2 -1)\n1 - old\n1 + new\n2 + extra\n",
        "Edit applied. (+0 -0)",
        "Edit applied. (+999999999999999999999999999999999 -1)\n",
        "Edit applied. (+1 -1)\n1 + \x1b[31mtext\n",
        "File written. (+3 -1)"
    };
    const char *expected[] = {" (+2 -1)", " (+0 -0)", " (+999999999999999999999999999999999 -1)", " (+1 -1)",
        " (+3 -1)"};
    const char *styled[] = {
        " (" STYLE_SELECTED "+2" STYLE_RESET " " STYLE_DIFF_REMOVE "-1" STYLE_RESET ")",
        " (" STYLE_SELECTED "+0" STYLE_RESET " " STYLE_DIFF_REMOVE "-0" STYLE_RESET ")",
        " (" STYLE_SELECTED "+999999999999999999999999999999999" STYLE_RESET " " STYLE_DIFF_REMOVE "-1" STYLE_RESET ")",
        " (" STYLE_SELECTED "+1" STYLE_RESET " " STYLE_DIFF_REMOVE "-1" STYLE_RESET ")",
        " (" STYLE_SELECTED "+3" STYLE_RESET " " STYLE_DIFF_REMOVE "-1" STYLE_RESET ")"};
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        text_buffer output = {0};
        append_change_counts(&output, valid[i]);
        char *plain = without_styles(output.data);
        assert(!strcmp(plain, expected[i]));
        assert(!strcmp(output.data, styled[i]));
        assert(!strstr(output.data, "\x1b[31mtext"));
        display_lines lines = {0};
        conversation_wrap(&lines, CONVERSATION_NONE, 0, output.data, 12);
        for (size_t k = 0; k < lines.count; k++) assert(text_columns(lines.items[k].text) <= 12);
        lines_free(&lines);
        free(plain);
        buffer_free(&output);
    }
    const char *literal[] = {NULL, "", "Edit", "Edit applied. (+1 -)", "Edit applied. (+ -1)",
        "Edit applied. (+1 +2)", "Edit applied. (+1 -2", "Edit applied. (+1 -2) extra", "other (+2 -1)"};
    for (size_t i = 0; i < sizeof(literal) / sizeof(literal[0]); i++) {
        text_buffer output = {0};
        append_change_counts(&output, literal[i]);
        assert(!output.length);
        buffer_free(&output);
    }
    char oversized[160];
    snprintf(oversized, sizeof(oversized), "Edit applied. (+%0100d -1)", 1);
    text_buffer output = {0};
    append_change_counts(&output, oversized);
    assert(!output.length);
    buffer_free(&output);
}

static void test_markdown(void) {
    const char *source = "## Heading\n"
                         "> **Hint:** [reference](https://example.test/a_(b))\n"
                         "Link [**literal**](https://example.test/a_(b))\n"
                         "\x60inline\x60 **bold** _italic_ ***both***\n"
                         "- \x60item\x60 **label**\n# *Title*\n"
                         "\x60\x60\x60" "c\n"
                         "int answer = 42; /* literal */\n"
                         "| value | not a table |\n"
                         "\x60not inline\x60\n"
                         "\x60\x60\x60\n";
    const char *expected = "## Heading\n"
                           " Hint  [reference](https://example.test/a_(b))\n"
                           "Link [**literal**](https://example.test/a_(b))\n"
                           "inline bold italic both\n"
                           "- item label\n# Title\n"
                           "\x60\x60\x60" "c\n"
                           "int answer = 42; /* literal */\n"
                           "| value | not a table |\n"
                           "\x60not inline\x60\n"
                           "\x60\x60\x60\n";
    markdown_state md = {0};
    text_buffer whole = {0}, pieces = {0};
    markdown_feed(&md, &whole, source);
    markdown_finish(&md, &whole);
    for (size_t i = 0; source[i]; i++) {
        char byte[2] = {source[i], 0};
        markdown_feed(&md, &pieces, byte);
        text_buffer preview = {0};
        markdown_preview(&md, &preview);
        buffer_free(&preview);
    }
    markdown_finish(&md, &pieces);
    assert(!strcmp(whole.data, pieces.data));
    char *plain = without_styles(whole.data);
    assert(!strcmp(plain, expected));
    free(plain);
    assert(strstr(whole.data, STYLE_HINT " Hint " STYLE_RESET));
    assert(strstr(whole.data, "- " STYLE_INLINE "item"));
    assert(strstr(whole.data, "Link [**literal**](https://example.test/a_(b))"));
    assert(strstr(whole.data, "| value | not a table |"));
    assert(strstr(whole.data, STYLE_INLINE));
    assert(strstr(whole.data, "\x1b[0;1m"));
    assert(strstr(whole.data, "\x1b[0;3m"));
    assert(strstr(whole.data, "\x1b[0;1;3m"));
    assert(!strstr(whole.data, "\x1b[38;5;244m"));
    assert(!strstr(whole.data, "\x1b[38;5;81m"));
    assert(!strstr(whole.data, "\x1b[38;5;203m"));
    assert(!strstr(whole.data, "\x1b[38;5;150m"));
    display_lines lines = {0};
    conversation_wrap(&lines, CONVERSATION_ASSISTANT, 0, whole.data, 9);
    for (size_t i = 0; i < lines.count; i++)
        assert(text_columns(lines.items[i].text) <= 9);
    lines_free(&lines);
    buffer_free(&whole);
    buffer_free(&pieces);

    const char *literal = "unclosed **span\nname_with_underscores\n\\*literal*\n";
    markdown_feed(&md, &whole, literal);
    markdown_finish(&md, &whole);
    plain = without_styles(whole.data);
    assert(!strcmp(plain, "unclosed **span\nname_with_underscores\n*literal*\n"));
    free(plain);
    buffer_free(&whole);
    char *safe = conversation_safe("\x60" "code\x60 \x1b[31mRED\x1b]0;bad\a");
    markdown_feed(&md, &whole, safe);
    markdown_finish(&md, &whole);
    plain = without_styles(whole.data);
    assert(strstr(plain, "\\x1B[31mRED\\x1B]0;bad\\x07"));
    assert(!strstr(whole.data, "\x1b[31m") && !strstr(whole.data, "\x1b]"));
    free(safe);
    free(plain);
    buffer_free(&whole);
    markdown_feed(&md, &whole, "~~~python\nvalue = 123\n~~~");
    markdown_finish(&md, &whole);
    plain = without_styles(whole.data);
    assert(!strcmp(plain, "~~~python\nvalue = 123\n~~~"));
    free(plain);
    buffer_free(&whole);

    size_t length = 160000;
    char *long_line = malloc(length + 2);
    assert(long_line);
    memset(long_line, 'a', length);
    memcpy(long_line + length, "\n", 2);
    markdown_feed(&md, &whole, long_line);
    markdown_finish(&md, &whole);
    assert(whole.length == length + 1 && !strcmp(whole.data, long_line));
    free(long_line);
    buffer_free(&whole);
}

static void test_hints(void) {
    tui_app app = {0};
    char input[160];
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
        snprintf(input, sizeof(input), "%s argument", COMMANDS[i].name);
        app.input = input;
        app.input_len = strlen(input);
        assert(!command_hint(&app));
        snprintf(input, sizeof(input), "%s ", COMMANDS[i].name);
        app.input_len = strlen(input);
        char *hint = command_hint(&app);
        if (*COMMANDS[i].syntax) assert(hint && *hint);
        free(hint);
    }
    app.input = "/goal edit";
    app.input_len = strlen(app.input);
    assert(!command_hint(&app));
}

static void test_programming_hint_rendering(void) {
    const char *source = "> **Hint:** Save `errno` before **cleanup**.\n"
                         "> Keep the original error.\nNormal prose.\n"
                         "> quoted text\nInline > **Hint:** literal\n"
                         "```text\n> **Hint:** literal code\n```\n";
    for (size_t split = 0; split <= strlen(source); split++) {
        markdown_state md = {0};
        text_buffer output = {0}, preview = {0};
        char *first = strndup(source, split);
        assert(first);
        markdown_feed(&md, &output, first);
        markdown_preview(&md, &preview);
        buffer_free(&preview);
        markdown_feed(&md, &output, source + split);
        markdown_finish(&md, &output);
        const char *badge = strstr(output.data, STYLE_HINT " Hint " STYLE_RESET);
        assert(badge && !strstr(badge + 1, STYLE_HINT " Hint " STYLE_RESET));
        assert(strstr(output.data, "  Keep the original error."));
        assert(!strstr(output.data, "│"));
        assert(strstr(output.data, "> quoted text") && strstr(output.data, "> **Hint:** literal code"));
        assert(strstr(output.data, STYLE_INLINE "errno") && strstr(output.data, "\x1b[0;1mcleanup"));
        free(first);
        buffer_free(&output);
    }
    const char *prefix = "> **Hint:**";
    for (size_t i = 1; i < strlen(prefix); i++) {
        markdown_state md = {0};
        text_buffer output = {0}, preview = {0};
        char *part = strndup(prefix, i);
        markdown_feed(&md, &output, part);
        markdown_preview(&md, &preview);
        assert(!preview.length);
        markdown_finish(&md, &output);
        assert(!strstr(output.data, STYLE_HINT));
        buffer_free(&preview);
        buffer_free(&output);
        free(part);
    }
}

static void test_tables(void) {
    const char *source = "tool | output\n:--- | ---:\nread | header only\nwrite | full diff\n";
    text_buffer whole = {0}, chunks = {0};
    markdown_state one = {.columns = 32}, many = {.columns = 32};
    markdown_feed(&one, &whole, source);
    markdown_finish(&one, &whole);
    for (size_t i = 0; source[i]; i++) {
        char byte[] = {source[i], 0};
        markdown_feed(&many, &chunks, byte);
        text_buffer preview = {0};
        markdown_preview(&many, &preview);
        buffer_free(&preview);
    }
    markdown_finish(&many, &chunks);
    assert(!strcmp(whole.data, chunks.data));
    char *plain = without_styles(whole.data);
    assert(strstr(plain, "┌") && strstr(plain, "├") && strstr(plain, "└"));
    assert(strstr(plain, "header only") && strstr(plain, "full diff"));
    assert(strstr(whole.data, STYLE_MUTED) && strstr(whole.data, "\x1b[0;1m"));
    free(plain);
    buffer_free(&whole); buffer_free(&chunks);
    const char *followed = "key | value\n--- | ---\nleft | right\n"
                           "```text\nliteral\n```\n";
    markdown_state md = {.columns = 80};
    markdown_feed(&md, &whole, followed);
    markdown_finish(&md, &whole);
    plain = without_styles(whole.data);
    char *fence = strstr(plain, "```text");
    assert(fence && strstr(plain, "└") < fence);
    assert(strstr(fence, "literal\n```\n"));
    free(plain);
    buffer_free(&whole);
    const char *wide = "| label | value |\n| - | :-: |\n| 宽度 | a long value that wraps |\n";
    for (int columns = 2; columns <= 40; columns++) {
        markdown_state md = {.columns = columns};
        text_buffer output = {0};
        markdown_feed(&md, &output, wide);
        markdown_finish(&md, &output);
        plain = without_styles(output.data);
        char *save = NULL;
        for (char *line = strtok_r(plain, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
            assert(text_columns(line) <= columns);
        assert(!strchr(output.data, '?'));
        free(plain);
        buffer_free(&output);
    }
    const char *cases[] = {
        "| value |\n| - |\n| a\\|b |\n| `x\\|y` |\n",
        "| left | right |\n| - | - |\nonly\n| a | b | ignored |\n",
        "a | b\n-- |\nc | d\n",
        "```text\n| value |\n| - |\n| literal |\n```"
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        markdown_state md = {.columns = 80};
        text_buffer output = {0};
        markdown_feed(&md, &output, cases[i]);
        markdown_finish(&md, &output);
        plain = without_styles(output.data);
        if (!i) {
            assert(strstr(plain, "a|b") && strstr(plain, "x|y") &&
                   !strstr(plain, "\\|"));
            assert(strstr(output.data, STYLE_INLINE));
        }
        else if (i == 1) assert(strstr(plain, "only") && !strstr(plain, "ignored"));
        else if (i == 3)
            assert(!strstr(plain, "┌") && !strcmp(plain, cases[i]));
        else assert(!strstr(plain, "┌") && strchr(plain, '|'));
        free(plain);
        buffer_free(&output);
    }
}

static void test_retained_reflow(void) {
    tui_app app = {.hidden = 1, .cols = 80, .rows = 24};
    display_lines narrow = {0}, wide = {0};
    conversation_wrap(&narrow, CONVERSATION_USER, 0, "abc界👩‍💻def\nnext", 3);
    history_emit(&app, &narrow, narrow.count, 0);
    lines_free(&narrow);
    overlay_history(&app, &wide, 24);
    assert(wide.count == 2);
    assert(!strcmp(wide.items[0].text, STYLE_USER "›" STYLE_RESET " abc界👩‍💻def"));
    assert(!strcmp(wide.items[1].text, "  next"));
    lines_free(&wide);
    app.cols = 2;
    overlay_history(&app, &narrow, 64);
    lines_free(&narrow);
    app.cols = 80;
    overlay_history(&app, &wide, 24);
    assert(!strcmp(wide.items[0].text, STYLE_USER "›" STYLE_RESET " abc界👩‍💻def"));
    lines_free(&wide);
    app_free(&app);
}

static void test_history_paging_after_resize(void) {
    tui_app app = {.cols = 80, .rows = 8, .history_rows = 4};
    const char text[] = "A000B001C002D003E004F005G006H007I008J009"
                        "K010L011M012N013O014P015Q016R017S018T019";
    display_lines lines = {0};
    conversation_wrap(&lines, CONVERSATION_ASSISTANT, 0, text, 80);
    history_emit(&app, &lines, lines.count, 0);
    lines_free(&lines);
    assert(app.retained.count == 1);
    app.cols = 4;
    unsigned seen = 0;
    for (int i = 0; i < 20; i++) {
        overlay_history(&app, &lines, 4);
        size_t first = lines.count > 4 ? lines.count - 4 : 0;
        for (size_t k = first; k < lines.count; k++) {
            unsigned letter = (unsigned)(lines.items[k].text[0] - 'A');
            assert(letter < 20);
            seen |= 1u << letter;
        }
        lines_free(&lines);
        strcpy(app.escape, "\x1b[5~");
        assert(escape_action(&app));
    }
    assert(seen == (1u << 20) - 1);
    overlay_history(&app, &lines, 4);
    assert(lines.count == 1 && !strcmp(lines.items[0].text, "A000"));
    lines_free(&lines);
    size_t end = app.view_end, skip = app.view_skip;
    display_line newest = {.text = "new", .style = STYLE_RESET};
    history_retain(&app, &newest);
    assert(app.view_end == end && app.view_skip == skip);
    for (int i = 0; i < 20; i++) {
        strcpy(app.escape, "\x1b[6~");
        assert(escape_action(&app));
    }
    assert(app.view_end == app.retained.count && !app.view_skip);
    history_scroll(&app, -1, 10);
    assert(app.view_skip);
    app.cols = 80;
    overlay_history(&app, &lines, 4);
    assert(!app.view_skip && lines.count && !strcmp(lines.items[lines.count - 1].text, text));
    lines_free(&lines);
    app.cols = 4;
    history_scroll(&app, -1, 10);
    for (int i = 0; i < 80; i++) history_retain(&app, &newest);
    assert(app.history_omitted && !app.view_skip);
    history_scroll(&app, 1, 1000);
    assert(app.view_end == app.retained.count && !app.view_skip);
    app_free(&app);

    for (int rows = 1; rows <= 2; rows++) {
        memset(&app, 0, sizeof(app));
        app.cols = 80;
        app.history_rows = rows;
        conversation_wrap(&lines, CONVERSATION_ASSISTANT, 0, "first\nlast", 80);
        history_emit(&app, &lines, lines.count, 2);
        lines_free(&lines);
        history_scroll(&app, -1, 1);
        overlay_history(&app, &lines, (size_t)rows);
        assert(lines.count && lines.items[lines.count - 1].text[0]);
        lines_free(&lines);
        history_scroll(&app, 1, 1);
        overlay_history(&app, &lines, (size_t)rows);
        assert(app.view_end == app.retained.count && !app.view_skip);
        int latest = 0;
        for (size_t i = 0; i < lines.count; i++) latest |= !strcmp(lines.items[i].text, "last");
        assert(latest);
        lines_free(&lines);
        app_free(&app);
    }
}

static void test_mouse_scroll(void) {
    tui_app app = {.cols = 80, .rows = 24, .history_rows = 10};
    app.input = calloc(OCA_PROMPT_MAX_BYTES + 1, 1);
    assert(app.input);
    input_set(&app, "draft");
    app.history = checked_realloc(NULL, sizeof(*app.history));
    app.history[0] = copy_text("previous prompt");
    app.history_count = app.history_at = 1;
    for (int i = 0; i < 30; i++) {
        display_line row = {.text = "output", .style = STYLE_RESET};
        history_retain(&app, &row);
    }
    const char *up = "\x1b[<64;40;10M";
    for (size_t i = 0; up[i]; i++) handle_byte(&app, (unsigned char)up[i]);
    assert(app.view_end < app.retained.count || app.view_skip);
    assert(!strcmp(app.input, "draft") && app.cursor == 5);
    const char *down = "\x1b[<65;65535;65535M";
    for (size_t i = 0; down[i]; i++) handle_byte(&app, (unsigned char)down[i]);
    assert(app.view_end == app.retained.count && !app.view_skip);
    const char *ignored[] = {"\x1b[<0;2;2M", "\x1b[<0;2;2m", "\x1b[<64;2;2m",
        "\x1b[<66;2;2M", "\x1b[<64;0;2M", "\x1b[<64;-1;2M", "\x1b[<64;;2M", "\x1b[<64;999999999;2M",
        "\x1b[<64;999999999999999999999999999999999999;2M"};
    for (size_t n = 0; n < sizeof(ignored) / sizeof(ignored[0]); n++) {
        for (size_t i = 0; ignored[n][i]; i++) handle_byte(&app, (unsigned char)ignored[n][i]);
        assert(!strcmp(app.input, "draft") && app.cursor == 5 && !app.escape_len);
        assert(app.view_end == app.retained.count && !app.view_skip);
    }
    app.menu = MENU_HELP;
    for (size_t i = 0; up[i]; i++) handle_byte(&app, (unsigned char)up[i]);
    assert(app.menu == MENU_HELP && !strcmp(app.input, "draft"));
    app.menu = MENU_NONE;
    const char *partial = "\x1b[<64;";
    for (size_t i = 0; partial[i]; i++) handle_byte(&app, (unsigned char)partial[i]);
    escape_fallback(&app);
    assert(!strcmp(app.input, "draft") && !app.escape_len);
    input_set(&app, "");
    const char *arrow = "\x1b[A";
    for (size_t i = 0; arrow[i]; i++) handle_byte(&app, (unsigned char)arrow[i]);
    assert(!strcmp(app.input, "previous prompt"));
    app_free(&app);
}

static void test_question_replay(void) {
    tui_app app = {.hidden = 1, .cols = 80, .rows = 24};
    app.tool_arguments = copy_text("{\"questions\":[{\"question\":\"Pick a `compiler`?\",\"options\":[\"gcc\",\"clang\"]}]}");
    replay_interaction(&app, "Q: Pick a `compiler`?\nA: None of the above\n\nDetails: custom compiler\n");
    text_buffer output = {0};
    for (size_t i = 0; i < app.retained.count; i++) {
        char *plain = without_styles(app.retained.items[i].text);
        buffer_append(&output, plain, strlen(plain));
        buffer_append(&output, "\n", 1);
        free(plain);
    }
    assert(strstr(output.data, "Pick a compiler?"));
    assert(strstr(output.data, "Answer: None of the above"));
    assert(strstr(output.data, "Details: custom compiler"));
    assert(!strstr(output.data, "Q: ") && !strstr(output.data, "A: "));
    assert(!strstr(output.data, "gcc") && !strstr(output.data, "clang"));
    for (size_t i = 0; i < app.retained.count; i++)
        assert(app.retained.items[i].conversation_kind != CONVERSATION_USER);
    buffer_free(&output);
    app_free(&app);
}

static void test_inline_question(void) {
    tui_app app = {.cols = 80, .rows = 24};
    app.input = calloc(OCA_PROMPT_MAX_BYTES + 1, 1);
    assert(app.input);
    input_set(&app, "draft text");
    app.cursor = 5;
    char *options[] = {"Brief", "Detailed"};
    oca_interaction_question question = {.question = "Which format?", .option_count = 2, .options = options};
    oca_core_event event = {.question_count = 1, .questions = &question};
    interaction_copy(&app, &event);
    app.menu_selection = 1;
    app.interaction_details = 1;
    input_set(&app, "first\nsecond");
    display_lines lines = {0};
    int cursor_row = 0, cursor_col = 0;
    question_lines(&app, &lines, &cursor_row, &cursor_col);
    assert(lines.items[0].card_role == 1 && lines.items[lines.count - 1].card_role == 3);
    assert(!strcmp(lines.items[0].style, STYLE_ACCENT));
    assert(strstr(lines.items[cursor_row].text, "second") && cursor_col == 10);
    assert(!strstr(lines.items[cursor_row].text, "Details"));
    for (size_t i = 0; i < lines.count; i++) assert(!strstr(lines.items[i].text, "Ctrl+J"));
    int has_key_color = 0;
    for (size_t i = 0; i < lines.count; i++) {
        has_key_color |= strstr(lines.items[i].text, STYLE_INLINE "Ctrl+C") != NULL;
        assert(!strstr(lines.items[i].text, "Tab"));
    }
    assert(has_key_color);
    lines_free(&lines);
    input_clear(&app);
    app.interaction_details = 0;
    question_lines(&app, &lines, NULL, NULL);
    has_key_color = 0;
    for (size_t i = 0; i < lines.count; i++) {
        has_key_color |= strstr(lines.items[i].text, STYLE_INLINE "Tab") != NULL;
        assert(!strstr(lines.items[i].text, "Ctrl+C") && !strstr(lines.items[i].text, "↑/↓"));
    }
    assert(has_key_color);
    lines_free(&lines);
    app.interaction_details = 1;
    question_lines(&app, &lines, NULL, NULL);
    int go_back = 0;
    for (size_t i = 0; i < lines.count; i++) go_back |= strstr(lines.items[i].text, ": go back") != NULL;
    assert(go_back);
    lines_free(&lines);
    input_set(&app, "first\nsecond");
    for (int cols = 1; cols <= 24; cols++) {
        app.cols = cols;
        question_lines(&app, &lines, NULL, NULL);
        for (size_t i = 0; i < lines.count; i++) assert(text_columns(lines.items[i].text) <= cols);
        lines_free(&lines);
    }
    question_answer_lines(&lines, &question, "Detailed\n\nDetails: first\nsecond", 80);
    assert(!strcmp(lines.items[0].style, STYLE_MUTED));
    assert(lines.count == 6 && strstr(lines.items[2].text, "Answer: Detailed"));
    assert(strstr(lines.items[3].text, "Details: first"));
    assert(strstr(lines.items[4].text, "second"));
    lines_free(&lines);
    tui_app history = {.hidden = 1, .cols = 80, .rows = 24};
    question_answer_lines(&lines, &question, "Detailed\n\nDetails: first\nsecond", 8);
    history_emit(&history, &lines, lines.count, 0);
    lines_free(&lines);
    overlay_history(&history, &lines, 24);
    assert(lines.count == 6 && strstr(lines.items[2].text, "Answer: Detailed"));
    assert(strstr(lines.items[4].text, "second"));
    lines_free(&lines);
    app_free(&history);
    char *ambiguous[] = {"A", "A\n\nDetails: B"};
    question.options = ambiguous;
    question_answer_lines(&lines, &question, "A\n\nDetails: B", 80);
    assert(lines.count == 6 && strstr(lines.items[3].text, "│"));
    assert(strstr(lines.items[4].text, "Details: B"));
    lines_free(&lines);
    question_answer_lines(&lines, &question, "Legacy free answer", 80);
    assert(lines.count == 4 && strstr(lines.items[2].text, "Answer: Legacy free answer"));
    lines_free(&lines);
    interaction_restore(&app);
    assert(!strcmp(app.input, "draft text") && app.cursor == 5 && app.menu == MENU_NONE);
    app_free(&app);
}

static void test_question_resize_reflow(void) {
    char *options[] = {"👩‍💻"};
    oca_interaction_question question = {
        .question = "界?", .options = options, .option_count = 1
    };
    const char *answer = "👩‍💻\n\nDetails: é\nz\n";
    display_lines expected = {0};
    question_answer_lines(&expected, &question, answer, 80);
    assert(expected.count == 7);
    assert(!strcmp(expected.items[1].accent_style, STYLE_RESET STYLE_BOLD));
    assert(text_columns(expected.items[5].text) == 80);
    display_lines fields = {0};
    question_wrap(&fields, STYLE_SELECTED, "a\n", 8);
    assert(fields.count == 2 && !strcmp(fields.items[0].text, "a"));
    assert(!*fields.items[1].text && !strcmp(fields.items[1].style, STYLE_SELECTED));
    lines_free(&fields);
    for (int cols = 1; cols <= 24; cols++) {
        tui_app history = {.hidden = 1, .cols = 80, .rows = 24};
        display_lines lines = {0};
        question_answer_lines(&lines, &question, answer, cols);
        assert(lines.items[0].card_first && lines.items[lines.count - 1].card_last);
        for (size_t i = 0; i < lines.count; i++)
            assert(text_columns(lines.items[i].text) <= cols);
        history_emit(&history, &lines, lines.count, 0);
        lines_free(&lines);
        for (int pass = 0; pass < 2; pass++) {
            overlay_history(&history, &lines, 24);
            assert(lines.count == expected.count);
            for (size_t i = 0; i < lines.count; i++) {
                assert(!strcmp(lines.items[i].text, expected.items[i].text));
                assert(!strcmp(lines.items[i].style, expected.items[i].style));
                assert((lines.items[i].accent_style != NULL) ==
                       (expected.items[i].accent_style != NULL));
                if (lines.items[i].accent_style)
                    assert(!strcmp(lines.items[i].accent_style, expected.items[i].accent_style));
            }
            lines_free(&lines);
            history.cols = 1;
            overlay_history(&history, &lines, 24);
            lines_free(&lines);
            history.cols = 80;
        }
        app_free(&history);
    }
    lines_free(&expected);
}

static void test_ambiguous_question_replay(void) {
    tui_app app = {.hidden = 1, .cols = 80, .rows = 24};
    app.tool_arguments = copy_text("{\"questions\":[{\"question\":\"First?\",\"options\":[\"A\",\"B\"]},"
                                  "{\"question\":\"Second?\",\"options\":[\"C\",\"D\"]}]}");
    const char *result = "Q: First?\nA: A\n\nDetails: Q boundary\nQ: Second?\nA: embedded\nQ: Second?\nA: C\n";
    replay_interaction(&app, result);
    int heading = 0, embedded = 0;
    for (size_t i = 0; i < app.retained.count; i++) {
        assert(app.retained.items[i].conversation_kind != CONVERSATION_USER);
        if (strstr(app.retained.items[i].text, "Recorded answers")) heading++;
        if (strstr(app.retained.items[i].text, "embedded")) embedded++;
    }
    assert(heading == 1 && embedded == 1);
    app_free(&app);
}
static int history_contains(const tui_app *app, const char *text) {
    for (size_t i = 0; i < app->retained.count; i++) if (strstr(app->retained.items[i].text, text)) return 1;
    return 0;
}

static void test_bash_output(void) {
    tui_app app = {.hidden = 1, .cols = 80, .rows = 24};
    oca_core_event event = {.kind = OCA_CE_TOOL_START, .name = "bash", .detail = "run-1",
                           .text = "{\"command\":\"printf log\"}"};
    handle_event(&app, &event);
    for (int i = 0; i < 20; i++) {
        char line[64];
        snprintf(line, sizeof(line), "output-%03d\n", i);
        event = (oca_core_event){.kind = OCA_CE_TOOL_OUTPUT, .text = line};
        handle_event(&app, &event);
    }
    assert(!app.retained.count);
    event = (oca_core_event){.kind = OCA_CE_TOOL_RESULT, .name = "bash", .detail = "run-1",
                            .text = "output-000\noutput-019\n[exit 0]",
                            .tool_state = OCA_TOOL_COMPLETE};
    handle_event(&app, &event);
    assert(history_contains(&app, "[bash] printf log"));
    assert(!history_contains(&app, "output-") && !history_contains(&app, "[exit"));
    size_t rows = app.retained.count;
    handle_byte(&app, 0x0f);
    assert(app.retained.count == rows);

    event = (oca_core_event){.kind = OCA_CE_TOOL_START, .name = "bash", .detail = "run-2",
                            .text = "{\"command\":\"exit 7\"}"};
    handle_event(&app, &event);
    event = (oca_core_event){.kind = OCA_CE_TOOL_RESULT, .name = "bash", .detail = "run-2",
                            .text = "failure detail\n[exit 7]", .tool_state = OCA_TOOL_COMPLETE};
    handle_event(&app, &event);
    assert(history_contains(&app, "[bash] exit 7") && history_contains(&app, "(exit 7)"));
    assert(!history_contains(&app, "failure detail") && !history_contains(&app, "[exit 7]"));

    event = (oca_core_event){.kind = OCA_CE_TOOL_START, .name = "bash", .detail = "run-3",
                            .text = "{\"command\":\"sleep 30\"}"};
    handle_event(&app, &event);
    event = (oca_core_event){.kind = OCA_CE_TOOL_RESULT, .name = "bash", .detail = "run-3",
                            .text = "partial output\n[exit -1]", .tool_state = OCA_TOOL_INTERRUPTED};
    handle_event(&app, &event);
    assert(history_contains(&app, "[bash] sleep 30") && history_contains(&app, "(interrupted)"));
    assert(!history_contains(&app, "partial output"));
    event = (oca_core_event){.kind = OCA_CE_TOOL_START, .name = "read", .detail = "read-1", .text = "{\"path\":\"file\"}"};
    handle_event(&app, &event);
    event = (oca_core_event){.kind = OCA_CE_TOOL_RESULT, .name = "read", .detail = "read-1",
                            .text = "read output must stay hidden", .tool_state = OCA_TOOL_COMPLETE};
    handle_event(&app, &event);
    assert(!history_contains(&app, "read output must stay hidden"));
    assert(!history_contains(&app, "Arguments") && !history_contains(&app, "Tool output"));
    app_free(&app);
}

static char *capture_read(FILE *capture) {
    assert(fflush(stdout) == 0);
    assert(fseek(capture, 0, SEEK_END) == 0);
    long length = ftell(capture);
    assert(length >= 0);
    char *text = malloc((size_t)length + 1);
    assert(text);
    rewind(capture);
    assert(fread(text, 1, (size_t)length, capture) == (size_t)length);
    text[length] = 0;
    assert(fseek(capture, 0, SEEK_END) == 0);
    return text;
}

static void test_tool_output(void) {
    tui_app app = {.cols = 80, .rows = 24};
    app.tool_name = copy_text("edit");
    app.tool_arguments = copy_text("{\"path\":\"file.c\"}");
    display_lines lines = {0};
    tool_action_lines(&app, &lines, OCA_TOOL_COMPLETE, "Edit applied. (+1 -1)\n1 - old text\n1 + new text\n");
    assert(lines.count == 1);
    tool_diff_lines(&app, &lines, "Edit applied. (+1 -1)\n1 - old text\n1 + new text\n");
    assert(lines.count == 3 && !strcmp(lines.items[0].text,
           "[edit] file.c (" STYLE_SELECTED "+1" STYLE_RESET " "
           STYLE_DIFF_REMOVE "-1" STYLE_RESET ")"));
    assert(!strcmp(lines.items[1].text, STYLE_DIFF_DELETE STYLE_MUTED "1"
           STYLE_DIFF_DELETE " " STYLE_DIFF_REMOVE "-" STYLE_DIFF_DELETE "  old text"));
    assert(!strcmp(lines.items[2].text, STYLE_DIFF_ADD STYLE_MUTED "1"
           STYLE_DIFF_ADD " " STYLE_SELECTED "+" STYLE_DIFF_ADD "  new text"));
    assert(lines.items[1].fill && lines.items[2].fill);
    history_retain(&app, &lines.items[1]);
    display_lines resized = {0};
    retained_wrap(&resized, &app.retained.items[0], 6);
    assert(resized.count > 1);
    for (size_t i = 0; i < resized.count; i++) {
        assert(resized.items[i].fill && !strcmp(resized.items[i].style, STYLE_DIFF_DELETE));
        assert(text_columns(resized.items[i].text) <= 6);
    }
    lines_free(&resized);
    tool_diff_lines(&app, &lines, "Edit applied. (+0 -0)\n2   context\n");
    assert(!strcmp(lines.items[3].text, STYLE_RESET STYLE_MUTED "2" STYLE_RESET "  " STYLE_RESET "  context"));
    assert(!lines.items[3].fill && !strcmp(lines.items[3].style, STYLE_RESET));
    tool_diff_lines(&app, &lines, "Edit applied. (+1 -0)\n\n40 + later\n41   \n");
    assert(lines.count == 7);
    assert(!strcmp(lines.items[4].text, "⋮"));
    assert(!strcmp(lines.items[4].style, STYLE_MUTED) && !lines.items[4].fill);
    assert(strstr(lines.items[6].text, "41"));
    assert(!strstr(lines.items[6].text, "⋮"));
    assert(!lines.items[6].fill);
    assert(!strncmp(lines.items[0].text, "[edit] file.c (", strlen("[edit] file.c (")));
    lines_free(&lines);
    app_free(&app);
    memset(&app, 0, sizeof(app));
    oca_core_event event = {.name = "read", .detail = "failed-read", .text = "{\"path\":\"file.txt\"}"};
    tool_start(&app, &event);
    event.text = "file unavailable";
    event.tool_state = OCA_TOOL_ERROR;
    tool_result(&app, &event);
    assert(app.retained.count > 0);
    char *plain = without_styles(app.retained.items[0].text);
    assert(strstr(plain, "[read] file.txt (failed)") && !strstr(plain, "file unavailable"));
    assert(!strstr(plain, "✦") && !strstr(plain, "ctrl"));
    free(plain);
    app_free(&app);
}

static void test_brand_paths(void) {
    char long_path[512];
    snprintf(long_path, sizeof(long_path), "/tmp/");
    for (int i = 0; i < 100; i++) strcat(long_path, "界");
    const char *paths[] = {"/", long_path, "/tmp/\x1b]0;PWN\a"};
    const int widths[] = {12, 40, 80, 160};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        for (size_t k = 0; k < sizeof(widths) / sizeof(widths[0]); k++) {
            tui_app app = {.workspace = paths[i], .cols = widths[k], .rows = 24};
            display_lines lines = {0};
            brand_lines(&app, &lines);
            assert(lines.count == 5);
            for (size_t row = 0; row < lines.count; row++) {
                assert(!strstr(lines.items[row].text, "\x1b]0;PWN"));
                char *plain = without_styles(lines.items[row].text);
                assert(text_columns(plain) <= app.cols);
                if (row > 0 && row < 4) {
                    size_t length = strlen(plain);
                    assert(!memcmp(plain + length - strlen("│"), "│", strlen("│")));
                }
                free(plain);
            }
            lines_free(&lines);
        }
    }
}

static void test_goal_output(void) {
    tui_app app = {.cols = 80, .rows = 24};
    oca_core_event event = {.kind = OCA_CE_GOAL, .goal = OCA_GOAL_COMPLETED,
        .goal_elapsed_ms = 3723004, .text = "Done"};
    handle_event(&app, &event);
    event.goal = OCA_GOAL_BLOCKED;
    handle_event(&app, &event);
    assert(history_contains(&app, "goal completed: " STYLE_MUTED "Done"));
    assert(!history_contains(&app, "Active time:"));
    assert(history_contains(&app, "goal blocked"));
    assert(!history_contains(&app, "goal_status"));
    display_lines page = {0};
    overlay_history(&app, &page, 24);
    int colored = 0;
    for (size_t i = 0; i < page.count; i++) {
        assert(!strstr(page.items[i].text, "\\x1B"));
        colored |= strstr(page.items[i].text, STYLE_PURPLE "goal completed") != NULL;
    }
    assert(colored);
    lines_free(&page);
    app_free(&app);
}

static void test_stream(FILE *capture) {
    tui_app app = {0};
    conversation_print(&app, CONVERSATION_USER, "user prompt");
    assert(app.retained.count == 3);
    oca_core_event event = {.kind = OCA_CE_ASSISTANT_REASON, .text = "private reasoning"};
    handle_event(&app, &event);
    assert(!app.stream_text_len);
    event = (oca_core_event){.kind = OCA_CE_ASSISTANT_TEXT, .text = "answer\n"};
    handle_event(&app, &event);
    event = (oca_core_event){.kind = OCA_CE_TURN_DONE};
    handle_event(&app, &event);
    assert(history_contains(&app, "answer") && !history_contains(&app, "private reasoning"));
    assert(!history_contains(&app, "Reasoned") && !history_contains(&app, "Reasoning"));
    char *text;

    for (int i = 0; i < 180; i++) {
        char row[64];
        snprintf(row, sizeof(row), "answer-row-%03d\n", i);
        stream_append(&app, CONVERSATION_ASSISTANT, row);
        stream_commit(&app, 0);
        assert(app.stream_text_len == 0);
        row[strlen(row) - 1] = 0;
        assert(history_contains(&app, row));
    }
    stream_commit(&app, 1);
    assert(!history_contains(&app, "answer-row-000"));
    assert(history_contains(&app, "answer-row-179"));
    assert(app.retained.count <= VIEW_HISTORY_MAX_ROWS);
    assert(app.retained_bytes <= VIEW_HISTORY_MAX_BYTES);
    app_free(&app);

    tui_app visible = { .cols = 80, .rows = 24 };
    tui_app hidden = { .hidden = 1, .side_chat = 1, .main_view = &visible, .cols = 80, .rows = 24 };
    visible.active_view = &visible;
    visible.display.show_reasoning = hidden.display.show_reasoning = 1;
    event = (oca_core_event){.kind = OCA_CE_ASSISTANT_REASON, .text = "visible thought"};
    handle_event(&visible, &event);
    event.text = "hidden thought";
    handle_event(&hidden, &event);
    stream_commit(&hidden, 1);
    assert(history_contains(&hidden, "hidden thought"));
    assert(!history_contains(&visible, "hidden thought"));
    stream_commit(&visible, 1);
    assert(history_contains(&visible, "visible thought"));
    for (int i = 0; i < 180; i++) {
        char row[64];
        snprintf(row, sizeof(row), "hidden-row-%03d\n", i);
        stream_append(&hidden, CONVERSATION_ASSISTANT, row);
        stream_commit(&hidden, 0);
    }
    stream_commit(&hidden, 1);
    assert(!visible.restore_pending && hidden.history_omitted);
    assert(hidden.retained.count <= VIEW_HISTORY_MAX_ROWS);
    assert(hidden.retained_bytes <= VIEW_HISTORY_MAX_BYTES);
    text = capture_read(capture);
    for (int i = 0; i < 180; i++) {
        char row[64];
        snprintf(row, sizeof(row), "hidden-row-%03d", i);
        assert(!strstr(text, row));
    }
    display_lines page = {0};
    overlay_history(&hidden, &page, 24);
    int latest = 0;
    for (size_t i = 0; i < page.count; i++) if (strstr(page.items[i].text, "hidden-row-179")) latest = 1;
    assert(latest);
    lines_free(&page);
    free(text);
    app_free(&hidden);
    app_free(&visible);
}

/* Exercise the same event gate in live and replayed main/temporary views.
 * Control sequences and Markdown stay literal in reasoning and tool bodies. */
static void test_visibility(void) {
    const char *tools[] = {"read", "write", "edit", "bash", "goal_status"};
    const char *body = "BODY_SECRET **literal** `code` \x1b[31m\n";
    for (int enabled = 0; enabled < 2; enabled++) {
        for (int side = 0; side < 2; side++) {
            for (int replay = 0; replay < 2; replay++) {
                tui_app main = {.cols = 100, .rows = 24};
                tui_app app = {.cols = 100, .rows = 24, .hidden = side, .side_chat = side,
                    .main_view = side ? &main : NULL,
                    .display = {.show_reasoning = enabled, .show_read = enabled, .show_write = enabled,
                                .show_edit = enabled, .show_bash = enabled}};
                for (int state = OCA_TOOL_COMPLETE; state <= OCA_TOOL_ERROR; state++) {
                    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++) {
                        oca_transcript_entry entries[] = {
                            {.kind = OCA_CE_ASSISTANT_REASON, .text = "THOUGHT_SECRET **literal** `code` \x1b[31m\n"},
                            {.kind = OCA_CE_TOOL_START, .name = (char *)tools[i],
                             .detail = "call", .text = "{\"path\":\"file\",\"command\":\"run\",\"name\":\"review\"}"},
                            {.kind = OCA_CE_TOOL_RESULT, .name = (char *)tools[i],
                             .detail = "call", .text = (char *)body, .tool_state = state},
                            {.kind = OCA_CE_TURN_DONE}
                        };
                        if ((!strcmp(tools[i], "write") || !strcmp(tools[i], "edit")) && state == OCA_TOOL_COMPLETE)
                            entries[2].text = "Edit applied. (+1 -1)\n1 - BODY_SECRET old\n1 + BODY_SECRET new\n";
                        if (replay) {
                            oca_core_event event = {.kind = OCA_CE_TRANSCRIPT, .entries = entries, .entry_count = 4};
                            handle_event(&app, &event);
                        } else for (size_t j = 0; j < 4; j++) {
                            oca_core_event event = {.kind = entries[j].kind, .text = entries[j].text,
                                .name = entries[j].name, .detail = entries[j].detail, .tool_state = entries[j].tool_state};
                            handle_event(&app, &event);
                        }
                        assert(!!history_contains(&app, "BODY_SECRET") ==
                               (enabled && strcmp(tools[i], "goal_status")));
                        assert(!tool_body_visible(&app, "ask_user"));
                        assert(!!history_contains(&app, "THOUGHT_SECRET") == enabled);
                        assert(!history_contains(&app, "Reasoned") && !history_contains(&app, "Reasoning"));
                        if (enabled) {
                            int italic = 0;
                            for (size_t row = 0; row < app.retained.count; row++) {
                                display_line *line = &app.retained.items[row];
                                if (line->conversation_kind == CONVERSATION_REASONING && *line->text) {
                                    assert(!strcmp(line->style, STYLE_REASONING));
                                    assert(strstr(line->text, "**literal** `code` \\x1B[31m"));
                                    italic++;
                                }
                            }
                            assert(italic);
                        }
                        lines_free(&app.retained);
                        app.retained_bytes = app.view_end = 0;
                    }
                }
                app_free(&app);
                app_free(&main);
            }
        }
    }
    for (int enabled = 0; enabled < 2; enabled++) for (int cancel = 0; cancel < 2; cancel++) {
        tui_app app = {.cols = 100, .rows = 24, .display = {.show_reasoning = enabled}};
        oca_core_event event = {.kind = OCA_CE_ASSISTANT_REASON, .text = "partial thought"};
        handle_event(&app, &event);
        event = (oca_core_event){.kind = OCA_CE_ERROR, .name = cancel ? "cancelled" : "adapter_failed", .text = "stopped"};
        handle_event(&app, &event);
        assert(!!history_contains(&app, "partial thought") == enabled);
        assert(history_contains(&app, "stopped"));
        assert(!history_contains(&app, "Reasoned") && !history_contains(&app, "Reasoning"));
        app_free(&app);
    }
    for (int enabled = 0; enabled < 2; enabled++) for (int replay = 0; replay < 2; replay++) {
        tui_app app = {.cols = 100, .rows = 24, .replaying = replay,
            .display = {.show_read = enabled, .show_write = enabled, .show_edit = enabled, .show_bash = enabled}};
        oca_core_event event = {.kind = OCA_CE_TOOL_START, .name = "ask_user", .detail = "ask",
            .text = "{\"questions\":[{\"question\":\"Pick?\",\"options\":[\"red\",\"blue\"]}]}"};
        handle_event(&app, &event);
        event = (oca_core_event){.kind = OCA_CE_TOOL_RESULT, .name = "ask_user", .detail = "ask",
            .text = "Q: Pick?\nA: red\n", .tool_state = OCA_TOOL_COMPLETE};
        handle_event(&app, &event);
        assert(!history_contains(&app, "Q: Pick?"));
        assert(!!history_contains(&app, "Answer: red") == replay);
        assert(!history_contains(&app, "[ask_user]"));
        app_free(&app);
    }
    for (int state = OCA_TOOL_COMPLETE; state <= OCA_TOOL_ERROR; state++) {
        tui_app app = {.cols = 100, .rows = 24,
            .display = {.show_bash = 1}};
        oca_core_event event = {.kind = OCA_CE_TOOL_START, .name = "bash", .detail = "manual_1",
            .text = "{\"command\":\"run\"}"};
        handle_event(&app, &event);
        event = (oca_core_event){.kind = OCA_CE_TOOL_OUTPUT, .text = "STREAM_ONLY\n"};
        handle_event(&app, &event);
        stream_commit(&app, 0);
        assert(history_contains(&app, "STREAM_ONLY"));
        const char *result = state == OCA_TOOL_COMPLETE ? "STREAM_ONLY\n\n[exit 0]" :
            state == OCA_TOOL_INTERRUPTED ? "STREAM_ONLY\n\nTool execution was canceled.\n[exit -1]" :
            "STREAM_ONLY\n\n[exit 1]";
        event = (oca_core_event){.kind = OCA_CE_TOOL_RESULT, .name = "bash", .detail = "manual_1",
            .text = (char *)result, .tool_state = state};
        handle_event(&app, &event);
        int occurrences = 0;
        for (size_t i = 0; i < app.retained.count; i++)
            occurrences += strstr(app.retained.items[i].text, "STREAM_ONLY") != NULL;
        assert(occurrences == 1 && !history_contains(&app, "[exit "));
        if (state == OCA_TOOL_COMPLETE) {
            assert(!history_contains(&app, "(interrupted)") && !history_contains(&app, "(failed)"));
        } else assert(history_contains(&app, state == OCA_TOOL_INTERRUPTED ? "(interrupted)" : "(exit 1)"));
        int headers = 0;
        for (size_t i = 0; i < app.retained.count; i++) headers += strstr(app.retained.items[i].text, "[bash]") != NULL;
        assert(headers == 1);
        if (state == OCA_TOOL_ERROR) {
            size_t completion = 0;
            while (completion < app.retained.count && !strstr(app.retained.items[completion].text, "(exit 1)")) completion++;
            assert(completion > 0 && completion < app.retained.count);
            assert(!strcmp(app.retained.items[completion - 1].text, "  STREAM_ONLY"));
            assert(!strcmp(app.retained.items[completion].text, "  (exit 1)"));
        }
        if (state == OCA_TOOL_INTERRUPTED) {
            int output_blank = 0;
            for (size_t i = 0; i < app.retained.count; i++)
                output_blank |= app.retained.items[i].conversation_kind == CONVERSATION_BASH &&
                                !strcmp(app.retained.items[i].text, "  ");
            assert(output_blank);
        }
        if (state == OCA_TOOL_INTERRUPTED) assert(history_contains(&app, "Tool execution was canceled."));
        app_free(&app);
    }
    for (int replay = 0; replay < 2; replay++) {
        tui_app app = {.cols = 100, .rows = 24, .replaying = replay,
            .display = {.show_bash = 1}};
        oca_core_event event = {.name = "bash", .text = "[exit 9]\n[exit 0]",
            .tool_state = OCA_TOOL_COMPLETE};
        tool_result(&app, &event);
        assert(history_contains(&app, "[exit 9]")); /* Actual command output. */
        for (size_t i = 0; i < app.retained.count; i++)
            if (strstr(app.retained.items[i].text, "[exit 9]"))
                assert(!strcmp(app.retained.items[i].text, "  [exit 9]"));
        assert(!history_contains(&app, "[exit 0]") && !history_contains(&app, "(failed)"));
        app_free(&app);
    }
    for (int replay = 0; replay < 2; replay++) {
        tui_app app = {.cols = 100, .rows = 24, .replaying = replay,
            .display = {.show_bash = 1}};
        oca_core_event event = {.name = "bash", .text = "OUTPUT\n\n[exit 2]",
            .tool_state = OCA_TOOL_ERROR};
        tool_result(&app, &event);
        assert(app.retained.count == 4);
        assert(!strcmp(app.retained.items[1].text, "  OUTPUT"));
        assert(!strcmp(app.retained.items[2].text, "  (exit 2)"));
        assert(!strcmp(app.retained.items[2].style, STYLE_DANGER));
        app_free(&app);
    }
    tui_app narrow = {.cols = 80, .rows = 24};
    oca_core_event failed = {.name = "bash", .text = "[exit 255]", .tool_state = OCA_TOOL_ERROR};
    tool_result(&narrow, &failed);
    display_lines wrapped = {0};
    for (size_t i = 0; i < narrow.retained.count; i++) {
        display_line *row = &narrow.retained.items[i];
        if (row->conversation_kind == CONVERSATION_BASH && *row->text) {
            assert(!strcmp(row->style, STYLE_DANGER));
            retained_wrap(&wrapped, row, 6);
        }
    }
    assert(wrapped.count > 1);
    for (size_t i = 0; i < wrapped.count; i++) {
        assert(!strcmp(wrapped.items[i].style, STYLE_DANGER));
        assert(text_columns(wrapped.items[i].text) <= 6);
    }
    lines_free(&wrapped);
    app_free(&narrow);
}

static void test_prompt_history(void) {
    char home[] = "/tmp/oca-history-test-XXXXXX", directory[256], path[512];
    assert(mkdtemp(home) && setenv("HOME", home, 1) == 0);
    snprintf(directory, sizeof(directory), "%s/.oca", home);
    assert(mkdir(directory, 0700) == 0);
    snprintf(path, sizeof(path), "%s/history", directory);
    tui_app app = {0};
    const char *prompt = "line one\nline \"two\" \\ café";
    history_add(&app, prompt);
    FILE *file = fopen(path, "rb");
    assert(file);
    char text[512] = {0};
    assert(fread(text, 1, sizeof(text) - 1, file) > 0 && fclose(file) == 0);
    assert(text[0] == '"' && strstr(text, "\\n") && strstr(text, "\\\"two\\\"") &&
           !strstr(text, "oca-history") && !strstr(text, "entry "));
    free(app.history[0]); free(app.history);
    memset(&app, 0, sizeof(app));
    history_load(&app);
    assert(app.history_count == 1 && !strcmp(app.history[0], prompt));
    free(app.history[0]); free(app.history);
    memset(&app, 0, sizeof(app));

    file = fopen(path, "wb");
    assert(file && fputs("  \"first\"  \r\n\"second\"", file) >= 0 && fclose(file) == 0);
    history_load(&app);
    assert(app.history_count == 2 && !strcmp(app.history[0], "first") &&
           !strcmp(app.history[1], "second"));
    free(app.history[0]); free(app.history[1]); free(app.history);
    memset(&app, 0, sizeof(app));
    file = fopen(path, "wb");
    assert(file && fputs("\"valid\" junk\n", file) >= 0 && fclose(file) == 0);
    history_load(&app);
    assert(app.history_count == 0);
    file = fopen(path, "wb");
    assert(file && fputs("oca-history 1\nentries 1\nentry 3\none\nend\n", file) >= 0 &&
           fclose(file) == 0);
    history_load(&app);
    assert(app.history_count == 0);
    assert(unlink(path) == 0 && rmdir(directory) == 0 && rmdir(home) == 0);
}

int main(void) {
    assert(setlocale(LC_CTYPE, "C.UTF-8"));
    test_wrapping();
    test_help_layout();
    test_question_editor_marker();
    test_metrics();
    test_change_counts();
    test_markdown();
    test_hints();
    test_programming_hint_rendering();
    test_tables();
    test_retained_reflow();
    test_history_paging_after_resize();
    test_mouse_scroll();
    test_question_replay();
    test_inline_question();
    test_question_resize_reflow();
    test_ambiguous_question_replay();
    test_prompt_history();
    test_bash_output();
    FILE *capture = tmpfile();
    assert(capture);
    int saved_stdout = dup(STDOUT_FILENO);
    assert(saved_stdout >= 0);
    assert(dup2(fileno(capture), STDOUT_FILENO) >= 0);
    test_brand_paths();
    test_goal_output();
    test_stream(capture);
    test_visibility();
    test_tool_output();
    assert(dup2(saved_stdout, STDOUT_FILENO) >= 0);
    close(saved_stdout);
    fclose(capture);
    puts("test_transcript: passed");
    return 0;
}
