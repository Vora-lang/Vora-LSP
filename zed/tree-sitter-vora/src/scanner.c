/**
 * Tree-sitter external scanner for Vora.
 *
 * Handles block comments with nesting support (/* ... * /).
 * Strings (single-quoted and template with ${}) are handled in grammar.js.
 *
 * The scanner is stateless — block comments cannot span tokens, so no
 * serialization is needed.
 */

#include <tree_sitter/parser.h>

// ═══════════════════════════════════════════════════════════════════════════════
// Token type — must match externals in grammar.js
// ═══════════════════════════════════════════════════════════════════════════════

enum TokenType {
    BLOCK_COMMENT,
};

// ═══════════════════════════════════════════════════════════════════════════════
// Helpers
// ═══════════════════════════════════════════════════════════════════════════════

static inline void consume(TSLexer *lexer) {
    lexer->advance(lexer, false);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Lifecycle
// ═══════════════════════════════════════════════════════════════════════════════

void *tree_sitter_vora_external_scanner_create(void) {
    return NULL;
}

void tree_sitter_vora_external_scanner_destroy(void *payload) {
    (void)payload;
}

unsigned tree_sitter_vora_external_scanner_serialize(void *payload, char *buffer) {
    (void)payload;
    (void)buffer;
    return 0;
}

void tree_sitter_vora_external_scanner_deserialize(
    void *payload, const char *buffer, unsigned length) {
    (void)payload;
    (void)buffer;
    (void)length;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Block comment — scans `/* ... */` with support for nested /* */ pairs
// ═══════════════════════════════════════════════════════════════════════════════

static bool scan_block_comment(TSLexer *lexer) {
    // Must start with '/'
    if (lexer->lookahead != '/') return false;
    consume(lexer);

    // Followed by '*'
    if (lexer->lookahead != '*') return false;
    consume(lexer);

    int nesting = 1;

    while (nesting > 0) {
        if (lexer->eof(lexer)) return false;

        if (lexer->lookahead == '/') {
            consume(lexer);
            if (lexer->lookahead == '*') {
                consume(lexer);
                nesting++;
                continue;
            }
            continue;
        }

        if (lexer->lookahead == '*') {
            consume(lexer);
            if (lexer->lookahead == '/') {
                consume(lexer);
                nesting--;
                continue;
            }
            continue;
        }

        consume(lexer);
    }

    lexer->result_symbol = BLOCK_COMMENT;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Entry point — tree-sitter calls this when expecting an external token
// ═══════════════════════════════════════════════════════════════════════════════

bool tree_sitter_vora_external_scanner_scan(
    void *payload,
    TSLexer *lexer,
    const bool *valid_symbols
) {
    (void)payload;

    if (valid_symbols[BLOCK_COMMENT]) {
        return scan_block_comment(lexer);
    }

    return false;
}
