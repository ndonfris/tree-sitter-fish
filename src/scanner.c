#include <tree_sitter/parser.h>
#include <wctype.h>
#include <stdlib.h>

enum TokenType {
    CONCAT,
    BRACKET_CONCAT,
    CONCAT_LIST,
    BEGIN_BRACE,
    STRING_LINE_CONTINUATION,
    LINE_CONTINUATION,
    CONTINUED_COMMENT,
    CONTINUED_NEWLINE,
    END_CONTINUATION,
};

// State carried between scans. Note that tree-sitter only persists these
// mutations when a scan returns true (produces a token); changes made on a scan
// that returns false are rolled back.
typedef struct {
    // Set once a `\`+newline line continuation (or a comment that is itself part
    // of a continuation) has been seen; keeps following comment lines
    // transparent so their newlines do not terminate the statement.
    bool in_continuation;
    // Set once a comment on a continuation line has swallowed its newline. The
    // next real content must then be split off from the line continuation
    // instead of being glued onto it by CONCAT.
    bool saw_comment;
    // The scanner has emitted a continued comment but has not yet consumed its
    // trailing newline as the hidden CONTINUED_NEWLINE token.
    bool pending_comment_newline;
    // Whether horizontal whitespace separated the line-continuation backslash
    // from the preceding expression. This determines whether the next content
    // begins a new argument or remains concatenated with that expression.
    bool continuation_separated;
} Scanner;

// Consume a newline (`\n`, `\r`, or `\r\n`) at the current lookahead.
static void consume_newline(TSLexer *lexer) {
    if (lexer->lookahead == '\r') {
        lexer->advance(lexer, false);
        if (lexer->lookahead == '\n') {
            lexer->advance(lexer, false);
        }
    } else if (lexer->lookahead == '\n') {
        lexer->advance(lexer, false);
    }
}

void *tree_sitter_fish_external_scanner_create() {
    return calloc(1, sizeof(Scanner));
}

void tree_sitter_fish_external_scanner_destroy(void *payload) {
    free(payload);
}

unsigned tree_sitter_fish_external_scanner_serialize(void *payload, char *buffer) {
    Scanner *scanner = (Scanner *)payload;
    buffer[0] = scanner->in_continuation ? 1 : 0;
    buffer[1] = scanner->saw_comment ? 1 : 0;
    buffer[2] = scanner->pending_comment_newline ? 1 : 0;
    buffer[3] = scanner->continuation_separated ? 1 : 0;
    return 4;
}

void tree_sitter_fish_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    Scanner *scanner = (Scanner *)payload;
    scanner->in_continuation = (length > 0 && buffer[0] == 1);
    scanner->saw_comment = (length > 1 && buffer[1] == 1);
    scanner->pending_comment_newline = (length > 2 && buffer[2] == 1);
    scanner->continuation_separated = (length > 3 && buffer[3] == 1);
}


// Consume a comment on a continuation line, starting at the `#`. Its trailing
// newline is emitted separately as a hidden CONTINUED_NEWLINE token so it does
// not terminate the statement or extend the visible comment node's range.
static bool scan_continued_comment(TSLexer *lexer) {
    while (lexer->lookahead != 0 && lexer->lookahead != '\n' && lexer->lookahead != '\r') {
        lexer->advance(lexer, false);
    }
    lexer->mark_end(lexer);
    lexer->result_symbol = CONTINUED_COMMENT;
    return true;
}

bool tree_sitter_fish_external_scanner_scan(
    void *payload, TSLexer *lexer, const bool *valid_symbols
) {
    Scanner *scanner = (Scanner *)payload;

    // Quoted `\\`+newline escapes look identical to command continuations but
    // must not enter continuation-comment state: `#` on the following source
    // line is still string content. A dedicated external symbol lets the parse
    // state disambiguate the two forms.
    if (valid_symbols[STRING_LINE_CONTINUATION] && lexer->lookahead == '\\') {
        lexer->advance(lexer, false);
        if (lexer->lookahead == '\n' || lexer->lookahead == '\r') {
            consume_newline(lexer);
            lexer->mark_end(lexer);
            lexer->result_symbol = STRING_LINE_CONTINUATION;
            return true;
        }
        return false;
    }

    // Hide the newline following a continued comment. Keeping this separate
    // from CONTINUED_COMMENT preserves the comment's source range while making
    // the newline transparent to the parser.
    if (valid_symbols[CONTINUED_NEWLINE] && scanner->pending_comment_newline &&
        (lexer->lookahead == '\n' || lexer->lookahead == '\r')) {
        consume_newline(lexer);
        lexer->mark_end(lexer);

        // A blank line or EOF ends the current statement. In those cases, leave
        // this newline to the internal lexer instead of hiding it as part of the
        // continuation. A pipe is real continued content: after the hidden
        // newline, the grammar can attach it to the command on the preceding
        // line.
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
            lexer->advance(lexer, true);
        }
        if (lexer->lookahead == 0 || lexer->lookahead == '\n' ||
            lexer->lookahead == '\r') {
            return false;
        }

        lexer->result_symbol = CONTINUED_NEWLINE;
        scanner->pending_comment_newline = false;
        return true;
    }

    // Persistently leave continuation mode before the next real token when the
    // grammar needs to preserve an argument or operator boundary.
    // Returning false after mutating the scanner would roll the mutation back.
    if (valid_symbols[END_CONTINUATION] && scanner->in_continuation &&
        (scanner->saw_comment || scanner->continuation_separated) &&
        !scanner->pending_comment_newline) {
        // For an ordinary separated continuation, reset before the internal
        // lexer can consume indentation and the following token in one pass.
        // After a continued comment, leave indentation alone so the transition
        // remains at the real token and the comment stays outside the command.
        if (scanner->continuation_separated && !scanner->saw_comment) {
            while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                lexer->advance(lexer, true);
            }
        }

        if (lexer->lookahead != 0 &&
            lexer->lookahead != '#' &&
            lexer->lookahead != '\\' &&
            lexer->lookahead != ' ' &&
            lexer->lookahead != '\t' &&
            lexer->lookahead != '\n' &&
            lexer->lookahead != '\r') {
            lexer->mark_end(lexer);
            lexer->result_symbol = END_CONTINUATION;
            scanner->in_continuation = false;
            scanner->saw_comment = false;
            scanner->continuation_separated = false;
            return true;
        }
    }

    // BEGIN_BRACE: { followed by whitespace or ; (for begin_statement)
    // Must take priority over internal '{' token used by brace_expansion
    if (valid_symbols[BEGIN_BRACE]) {
        // Skip leading whitespace (since whitespace is in extras)
        while (iswspace(lexer->lookahead)) {
            lexer->advance(lexer, true);  // skip=true for whitespace
        }

        if (lexer->lookahead == '{') {
            lexer->advance(lexer, false);  // consume '{'
            if (lexer->lookahead == ';' || iswspace(lexer->lookahead)) {
                lexer->mark_end(lexer);
                lexer->result_symbol = BEGIN_BRACE;
                return true;
            }
        }
        // Not matched - return false to let internal lexer try
        // (returning false resets lexer state)
    }

    // External scanners run before the internal whitespace extra. Look across
    // horizontal indentation here so a continued comment is recognized in all
    // parse states, not only states where BEGIN_BRACE happened to skip it first.
    if ((valid_symbols[LINE_CONTINUATION] ||
         (valid_symbols[CONTINUED_COMMENT] && scanner->in_continuation)) &&
        (lexer->lookahead == ' ' || lexer->lookahead == '\t')) {
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
            lexer->advance(lexer, true);
        }

        if (valid_symbols[CONTINUED_COMMENT] && scanner->in_continuation &&
            lexer->lookahead == '#') {
            scanner->saw_comment = true;
            scanner->pending_comment_newline = true;
            return scan_continued_comment(lexer);
        }

        if (valid_symbols[LINE_CONTINUATION] && lexer->lookahead == '\\') {
            lexer->advance(lexer, false);
            if (lexer->lookahead == '\n' || lexer->lookahead == '\r') {
                consume_newline(lexer);
                lexer->mark_end(lexer);
                lexer->result_symbol = LINE_CONTINUATION;
                scanner->in_continuation = true;
                scanner->continuation_separated = true;
                return true;
            }
        }

        // No relevant external token follows the indentation. Returning false
        // restores the lexer to its original position for the internal lexer.
        return false;
    }

    // At column zero there is no indentation branch to give this token
    // priority. Recognize it before zero-width CONCAT so a continuation comment
    // cannot be mistaken for the next element of a concatenation.
    if (valid_symbols[CONTINUED_COMMENT] && scanner->in_continuation &&
        lexer->lookahead == '#') {
        scanner->saw_comment = true;
        scanner->pending_comment_newline = true;
        return scan_continued_comment(lexer);
    }

    if (valid_symbols[CONCAT_LIST]) {
        if (!(
            lexer->lookahead == 0 ||
            lexer->lookahead != '['
        )) {
            lexer->result_symbol = CONCAT_LIST;
            return true;
        }
    }

    // After a comment on a continuation line has swallowed its newline, the next
    // real content (not another continuation `\`, comment `#`, whitespace, or a
    // blank line) must be split off from the preceding line continuation.
    // Returning false -- rather than emitting CONCAT -- keeps that content from
    // being glued onto the line continuation now that the comment removed the
    // whitespace that would normally separate them (issue #30).
    if (scanner->saw_comment &&
        (valid_symbols[LINE_CONTINUATION] || valid_symbols[CONTINUED_COMMENT]) &&
        lexer->lookahead != 0 &&
        lexer->lookahead != '\\' &&
        lexer->lookahead != '#' &&
        lexer->lookahead != ' ' &&
        lexer->lookahead != '\t' &&
        lexer->lookahead != '\n' &&
        lexer->lookahead != '\r') {
        scanner->in_continuation = false;
        scanner->saw_comment = false;
        return false;
    }

    // A backslash needs special care: `\` immediately followed by a newline is
    // a line continuation, while `\` followed by anything else starts an escape
    // sequence that concatenates onto the preceding word (e.g. `foo\tbar`). We
    // must distinguish the two here so a line continuation is never glued to the
    // previous word by CONCAT.
    if ((valid_symbols[LINE_CONTINUATION] || valid_symbols[CONCAT]) &&
        lexer->lookahead == '\\') {
        lexer->mark_end(lexer); // zero-width CONCAT point, before the backslash
        lexer->advance(lexer, false); // consume '\'

        if (lexer->lookahead == '\n' || lexer->lookahead == '\r') {
            // A line continuation directly adjacent to the previous token (no
            // separating whitespace -- the space-separated case is handled from
            // the whitespace branch below). Emit a zero-width CONCAT first so the
            // continuation joins that token, e.g. `'echo'\<newline>location`. The
            // LINE_CONTINUATION itself is produced by the next scan at this same
            // position.
            if (valid_symbols[CONCAT]) {
                lexer->result_symbol = CONCAT;
                return true;
            }
            // LINE_CONTINUATION must be valid here: the enclosing branch
            // requires LINE_CONTINUATION or CONCAT, and CONCAT was just ruled
            // out above.
            consume_newline(lexer);
            lexer->mark_end(lexer);
            lexer->result_symbol = LINE_CONTINUATION;
            scanner->in_continuation = true;
            scanner->continuation_separated = false;
            return true;
        }

        // Not a line continuation: the backslash starts an escape sequence. Emit
        // a zero-width CONCAT (ending at the mark_end set above, before the
        // backslash) so the escape sequence concatenates onto the previous word.
        if (valid_symbols[CONCAT]) {
            lexer->result_symbol = CONCAT;
            return true;
        }
        return false;
    }

    if (valid_symbols[CONCAT]) {
        if (!(
            lexer->lookahead == 0 ||
            lexer->lookahead == '>' ||
            lexer->lookahead == '<' ||
            lexer->lookahead == ')' ||
            lexer->lookahead == ';' ||
            lexer->lookahead == '&' ||
            lexer->lookahead == '|' ||
            iswspace(lexer->lookahead)
        )) {
            // An adjacent continuation remains part of the same expression.
            // CONCAT is a real token, so clearing state here is persisted.
            if (scanner->in_continuation && !scanner->saw_comment) {
                scanner->in_continuation = false;
                scanner->continuation_separated = false;
            }
            lexer->result_symbol = CONCAT;
            return true;
        }
    }

    if (valid_symbols[BRACKET_CONCAT]) {
        if (!(
            lexer->lookahead == 0 ||
            lexer->lookahead == ')' ||
            lexer->lookahead == '(' ||
            lexer->lookahead == '}' ||
            lexer->lookahead == ',' ||
            iswspace(lexer->lookahead)
        )) {
            lexer->result_symbol = BRACKET_CONCAT;
            return true;
        }
    }

    // Nothing below is relevant unless a continuation comment or a line
    // continuation could appear here.
    if (!valid_symbols[LINE_CONTINUATION] && !valid_symbols[CONTINUED_COMMENT]) {
        return false;
    }

    // Any other lookahead -- real content, a blank line (newline), or EOF --
    // ends the continuation.
    scanner->in_continuation = false;
    scanner->saw_comment = false;
    scanner->continuation_separated = false;
    return false;
}
