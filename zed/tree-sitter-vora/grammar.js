/// <reference types="tree-sitter-cli/dsl" />

// ═════════════════════════════════════════════════════════════════════════════
// Tree-sitter grammar for the Vora programming language
// ═════════════════════════════════════════════════════════════════════════════

// Mirrors parser.cpp getPrecedence() (looser = lower number):
// ?: shares level with ||/??, then bitwise C-style | ^ &, then equality,
// then comparison/in, shift, additive, multiplicative, power.
const PREC = {
  assignment: 1,
  ternary: 2,
  null_coalescing: 3,
  logical_or: 4,
  logical_and: 5,
  bitwise_or: 6,
  bitwise_xor: 7,
  bitwise_and: 8,
  equality: 9,
  comparison: 10,
  shift: 11,
  addition: 12,
  multiplication: 13,
  power: 14,
  unary: 15,
  postfix: 16,
  call: 17,
  member: 18,
  primary: 19,
};

module.exports = grammar({
  name: "vora",

  extras: ($) => [
    $.comment,
    /[\s]+/,
  ],

  externals: ($) => [
    $.block_comment,
  ],

  inline: ($) => [
    $._statement,
  ],

  word: ($) => $.identifier,

  conflicts: ($) => [
    // Pattern vs expression (Vora uses []{} for both)
    [$.array_pattern, $.array],
    [$.array_pattern, $._primary_expression],
    [$.object_pattern, $.dict],
    [$.object_pattern, $.shorthand_property_identifier],
    [$.pair_pattern, $.pair],
    [$.pair_pattern, $._primary_expression],
    [$.shorthand_pattern, $._primary_expression],
    [$.shorthand_pattern, $.shorthand_property_identifier],
    [$.shorthand_property_identifier, $._primary_expression],
    [$.rest_pattern, $._primary_expression],
    [$.rest_parameter, $._primary_expression],
    // Arrow / anonymous function vs expressions
    [$.arrow_function, $.parenthesized_expression],
    [$.formal_parameters, $._primary_expression],
    [$.formal_parameters, $.parenthesized_expression],
    // anonymous_function vs method_definition (same syntax: func params block)
    [$.anonymous_function, $.method_definition],
    // method_definition vs function_declaration (inside class body)
    [$.function_declaration, $.method_definition],
    // Declarations with optional semicolon (ambiguous in for-loop init)
    [$.expression_statement],
    [$.let_declaration],
    [$.const_declaration],
    // Keyword + { — dict literal vs block statement
    [$.return_statement],
    [$.throw_statement],
    [$.yield_expression],
    [$.defer_statement],
    [$.expression_statement, $.dict],
    [$.block_statement, $.dict],
    // Call arguments vs various parenthesized constructs
    [$.arguments, $.parenthesized_expression],
    [$.arguments, $.formal_parameters],
    [$.call_expression, $.parenthesized_expression],
    // Dangling else
    [$.if_statement],
    // List comprehension starts with [ like array
    [$.list_comprehension, $.array],
  ],

  rules: {
    // ════════════════════════════════════════════════════════════════════
    // Top-level
    // ════════════════════════════════════════════════════════════════════

    source_file: ($) =>
      repeat($._statement),

    _statement: ($) =>
      choice(
        $.expression_statement,
        $.let_declaration,
        $.const_declaration,
        $.block_statement,
        $.return_statement,
        $.if_statement,
        $.while_statement,
        $.do_while_statement,
        $.for_in_statement,
        $.c_style_for_statement,
        $.function_declaration,
        $.class_declaration,
        $.break_statement,
        $.continue_statement,
        $.try_statement,
        $.throw_statement,
        $.import_statement,
        $.from_import_statement,
        $.export_statement,
        $.defer_statement,
      ),

    // ════════════════════════════════════════════════════════════════════
    // Expression statement
    // ════════════════════════════════════════════════════════════════════

    expression_statement: ($) =>
      seq($._expression, optional(";")),

    // ════════════════════════════════════════════════════════════════════
    // Block
    // ════════════════════════════════════════════════════════════════════

    block_statement: ($) =>
      prec(2, seq("{", repeat($._statement), "}")),

    // ════════════════════════════════════════════════════════════════════
    // Variable declarations
    // ════════════════════════════════════════════════════════════════════

    let_declaration: ($) =>
      seq(
        "let",
        choice(
          seq(field("name", $.identifier), optional(field("type", $.type_annotation)), "=", field("value", $._expression)),
          $.destructuring_declaration,
        ),
        optional(";"),
      ),

    const_declaration: ($) =>
      seq(
        "const",
        choice(
          seq(field("name", $.identifier), optional(field("type", $.type_annotation)), "=", field("value", $._expression)),
          $.destructuring_declaration,
        ),
        optional(";"),
      ),

    type_annotation: ($) =>
      seq(":", field("type_name", $.identifier)),

    // ════════════════════════════════════════════════════════════════════
    // Destructuring
    // ════════════════════════════════════════════════════════════════════

    destructuring_declaration: ($) =>
      seq(
        field("pattern", choice($.array_pattern, $.object_pattern)),
        optional(field("type", $.type_annotation)),
        "=",
        field("value", $._expression),
      ),

    array_pattern: ($) =>
      seq(
        "[",
        commaSep(choice(
          $.identifier,
          $.array_pattern,
          $.object_pattern,
        )),
        optional(seq(",", $.rest_pattern)),
        "]",
      ),

    object_pattern: ($) =>
      seq(
        "{",
        commaSep(choice(
          $.pair_pattern,
          $.shorthand_pattern,
        )),
        optional(seq(",", $.rest_pattern)),
        "}",
      ),

    pair_pattern: ($) =>
      seq(field("key", $.identifier), ":", field("value", choice($.identifier, $.array_pattern, $.object_pattern))),

    shorthand_pattern: ($) =>
      seq(field("name", $.identifier), optional(seq("=", field("default", $._expression)))),

    rest_pattern: ($) =>
      seq("...", field("name", $.identifier)),

    // ════════════════════════════════════════════════════════════════════
    // Return
    // ════════════════════════════════════════════════════════════════════

    return_statement: ($) =>
      seq("return", optional(field("value", $._expression))),

    // ════════════════════════════════════════════════════════════════════
    // If / else
    // ════════════════════════════════════════════════════════════════════

    if_statement: ($) =>
      seq(
        "if", "(",
        field("condition", $._expression),
        ")",
        field("consequence", $._statement),
        optional(seq("else", field("alternative", $._statement))),
      ),

    // ════════════════════════════════════════════════════════════════════
    // Loops
    // ════════════════════════════════════════════════════════════════════

    while_statement: ($) =>
      seq(
        "while", "(",
        field("condition", $._expression),
        ")",
        field("body", $._statement),
      ),

    do_while_statement: ($) =>
      seq(
        "do",
        field("body", $._statement),
        "while", "(",
        field("condition", $._expression),
        ")",
        optional(";"),
      ),

    for_in_statement: ($) =>
      seq(
        "for",
        field("variable", $.identifier),
        "in",
        field("iterable", $._expression),
        field("body", $._statement),
      ),

    c_style_for_statement: ($) =>
      seq(
        "for", "(",
        field("initializer", optional(choice($.let_declaration, $.const_declaration, $.expression_statement))),
        ";",
        field("condition", optional($._expression)),
        ";",
        field("increment", optional($._expression)),
        ")",
        field("body", $._statement),
      ),

    // ════════════════════════════════════════════════════════════════════
    // Function
    // ════════════════════════════════════════════════════════════════════

    function_declaration: ($) =>
      seq(
        "func",
        field("name", $.identifier),
        field("parameters", $.formal_parameters),
        optional(field("return_type", $.type_annotation)),
        field("body", $.block_statement),
      ),

    formal_parameters: ($) =>
      seq(
        "(",
        commaSep(choice(
          $.identifier,
          $.rest_parameter,
          $.array_pattern,
          $.object_pattern,
        )),
        ")",
      ),

    rest_parameter: ($) =>
      seq("...", field("name", $.identifier)),

    // ════════════════════════════════════════════════════════════════════
    // Class
    // ════════════════════════════════════════════════════════════════════

    class_declaration: ($) =>
      seq(
        "Obj",
        field("name", $.identifier),
        optional(seq(":", commaSep1($.identifier))),
        field("body", $.class_body),
      ),

    class_body: ($) =>
      seq("{", repeat(choice(
        $.method_definition,
        $._statement,
      )), "}"),

    // Inside a class body, func name(params): returnType { ... } is a method.
    // Constructor is a method whose name matches the class — distinguished
    // at the semantic level, not syntax.
    method_definition: ($) =>
      seq(
        "func",
        field("name", $.identifier),
        field("parameters", $.formal_parameters),
        optional(field("return_type", $.type_annotation)),
        field("body", $.block_statement),
      ),

    // ════════════════════════════════════════════════════════════════════
    // Break / Continue
    // ════════════════════════════════════════════════════════════════════

    break_statement: ($) => token(seq("break", optional(";"))),
    continue_statement: ($) => token(seq("continue", optional(";"))),

    // ════════════════════════════════════════════════════════════════════
    // Try / Catch / Finally
    // ════════════════════════════════════════════════════════════════════

    try_statement: ($) =>
      seq(
        "try",
        field("body", $.block_statement),
        optional(seq(
          "catch",
          optional(seq("(", field("variable", $.identifier), ")")),
          field("handler", $.block_statement),
        )),
        optional(seq("finally", field("finalizer", $.block_statement))),
      ),

    throw_statement: ($) =>
      seq("throw", field("value", $._expression)),

    // ════════════════════════════════════════════════════════════════════
    // Import / Export
    // ════════════════════════════════════════════════════════════════════

    import_statement: ($) =>
      seq(
        "import",
        field("path", $.string),
        optional(seq("as", field("alias", $.identifier))),
        optional(";"),
      ),

    from_import_statement: ($) =>
      seq(
        "from",
        field("path", $.string),
        "import",
        commaSep1(seq(
          field("name", $.identifier),
          optional(seq("as", field("alias", $.identifier))),
        )),
        optional(";"),
      ),

    export_statement: ($) =>
      seq(
        "export",
        choice(
          seq("func", field("name", $.identifier), field("parameters", $.formal_parameters), optional($.type_annotation), field("body", $.block_statement)),
          seq("let", field("name", $.identifier), optional($.type_annotation), "=", field("value", $._expression)),
          seq("const", field("name", $.identifier), optional($.type_annotation), "=", field("value", $._expression)),
          seq("Obj", field("name", $.identifier), optional(seq(":", commaSep1($.identifier))), field("body", $.class_body)),
        ),
        optional(";"),
      ),

    // ════════════════════════════════════════════════════════════════════
    // Defer
    // ════════════════════════════════════════════════════════════════════

    defer_statement: ($) =>
      seq("defer", $._statement),

    // ════════════════════════════════════════════════════════════════════
    // Expressions
    // ════════════════════════════════════════════════════════════════════

    _expression: ($) =>
      choice(
        $.assignment_expression,
        $.ternary_expression,
        $.null_coalescing_expression,
        $.binary_expression,
        $.unary_expression,
        $.update_expression,
        $.call_expression,
        $.member_expression,
        $.subscript_expression,
        $.optional_chaining_expression,
        $.range_expression,
        $.spread_expression,
        $.arrow_function,
        $.anonymous_function,
        $.match_expression,
        $.yield_expression,
        $.list_comprehension,
        $._primary_expression,
      ),

    // ── Assignment ────────────────────────────────────────────────────

    assignment_expression: ($) =>
      prec.right(PREC.assignment, seq(
        field("left", choice($.identifier, $.member_expression, $.subscript_expression)),
        field("operator", choice("=", "+=", "-=", "*=", "/=", "%=", "**=")),
        field("right", $._expression),
      )),

    // ── Ternary ────────────────────────────────────────────────────────

    ternary_expression: ($) =>
      prec.right(PREC.ternary, seq(
        field("condition", $._expression),
        "?",
        field("consequence", $._expression),
        ":",
        field("alternative", $._expression),
      )),

    // ── Null-coalescing ────────────────────────────────────────────────

    null_coalescing_expression: ($) =>
      prec.left(PREC.null_coalescing, seq(
        field("left", $._expression),
        "??",
        field("right", $._expression),
      )),

    // ── Binary operators ───────────────────────────────────────────────

    binary_expression: ($) =>
      choice(
        prec.left(PREC.logical_or, seq(field("left", $._expression), choice("||", "or"), field("right", $._expression))),
        prec.left(PREC.logical_and, seq(field("left", $._expression), choice("&&", "and"), field("right", $._expression))),
        // P1-F bitwise operators (int64 only in the runtime)
        prec.left(PREC.bitwise_or, seq(field("left", $._expression), field("operator", "|"), field("right", $._expression))),
        prec.left(PREC.bitwise_xor, seq(field("left", $._expression), field("operator", "^"), field("right", $._expression))),
        prec.left(PREC.bitwise_and, seq(field("left", $._expression), field("operator", "&"), field("right", $._expression))),
        prec.left(PREC.shift, seq(field("left", $._expression), field("operator", choice("<<", ">>")), field("right", $._expression))),
        prec.left(PREC.equality, seq(field("left", $._expression), field("operator", choice("==", "!=")), field("right", $._expression))),
        prec.left(PREC.comparison, seq(field("left", $._expression), field("operator", choice("<", "<=", ">", ">=")), field("right", $._expression))),
        // `x in xs` — membership test (P1-G); shares precedence with relational.
        prec.left(PREC.comparison, seq(field("left", $._expression), field("operator", "in"), field("right", $._expression))),
        prec.left(PREC.addition, seq(field("left", $._expression), field("operator", choice("+", "-")), field("right", $._expression))),
        prec.left(PREC.multiplication, seq(field("left", $._expression), field("operator", choice("*", "/", "%")), field("right", $._expression))),
        prec.right(PREC.power, seq(field("left", $._expression), "**", field("right", $._expression))),
      ),

    // ── Unary ──────────────────────────────────────────────────────────

    unary_expression: ($) =>
      prec(PREC.unary, seq(
        field("operator", choice("-", "+", "!", "~", "not")),
        field("argument", $._expression),
      )),

    // ── Update (++/--, prefix and postfix) ─────────────────────────────

    update_expression: ($) =>
      choice(
        prec(PREC.postfix, seq(
          field("argument", choice($.identifier, $.member_expression, $.subscript_expression)),
          field("operator", choice("++", "--")),
        )),
        prec(PREC.unary, seq(
          field("operator", choice("++", "--")),
          field("argument", choice($.identifier, $.member_expression, $.subscript_expression)),
        )),
      ),

    // ── Call ───────────────────────────────────────────────────────────

    call_expression: ($) =>
      prec(PREC.call, seq(
        field("function", $._expression),
        field("arguments", $.arguments),
      )),

    arguments: ($) =>
      seq("(", commaSep($._expression), ")"),

    // ── Member / Subscript ─────────────────────────────────────────────

    member_expression: ($) =>
      prec.left(PREC.member, seq(
        field("object", $._expression),
        ".",
        field("property", $.identifier),
      )),

    subscript_expression: ($) =>
      prec.left(PREC.member, seq(
        field("object", $._expression),
        "[",
        field("index", $._expression),
        "]",
      )),

    // ── Optional chaining ──────────────────────────────────────────────

    optional_chaining_expression: ($) =>
      prec.left(PREC.member, seq(
        field("object", $._expression),
        "?.",
        field("property", $.identifier),
      )),

    // ── Range ──────────────────────────────────────────────────────────

    range_expression: ($) =>
      prec.left(PREC.comparison, seq(
        field("left", $._expression),
        field("operator", choice("..", "..=")),
        field("right", $._expression),
      )),

    // ── Spread ─────────────────────────────────────────────────────────

    spread_expression: ($) =>
      prec(PREC.unary, seq("...", field("argument", $._expression))),

    // ── Arrow function ─────────────────────────────────────────────────

    arrow_function: ($) =>
      seq(
        field("parameters", choice($.identifier, $.formal_parameters)),
        "=>",
        field("body", choice($._expression, $.block_statement)),
      ),

    // ── Anonymous function ─────────────────────────────────────────────

    anonymous_function: ($) =>
      seq(
        "func",
        field("parameters", $.formal_parameters),
        optional(field("return_type", $.type_annotation)),
        field("body", $.block_statement),
      ),

    // ── Match expression ───────────────────────────────────────────────

    match_expression: ($) =>
      seq(
        "match",
        field("value", $._expression),
        "{",
        repeat($.match_arm),
        "}",
      ),

    match_arm: ($) =>
      seq(
        field("pattern", $._match_pattern_alternation),
        "=>",
        field("body", choice($._expression, $.block_statement)),
        optional(","),
      ),

    // `1 | 2 | 3 => ...` — or-pattern (P1-B). `|` is also the bitwise OR
    // operator in expressions; inside a match arm it separates patterns.
    _match_pattern_alternation: ($) =>
      seq($._match_pattern, repeat(seq("|", $._match_pattern))),

    _match_pattern: ($) =>
      choice(
        $.number,
        $.string,
        $.boolean,
        $.null,
        $.identifier,
        $.array,
        $.dict,
        $.wildcard_pattern,
      ),

    wildcard_pattern: ($) => "_",

    // ── Yield ──────────────────────────────────────────────────────────

    yield_expression: ($) =>
      seq("yield", optional(field("value", $._expression))),

    // ── List comprehension ─────────────────────────────────────────────

    list_comprehension: ($) =>
      seq(
        "[",
        field("result", $._expression),
        "for",
        field("variable", $.identifier),
        "in",
        field("iterable", $._expression),
        optional(seq("if", field("condition", $._expression))),
        "]",
      ),

    // ════════════════════════════════════════════════════════════════════
    // Primary expressions
    // ════════════════════════════════════════════════════════════════════

    _primary_expression: ($) =>
      choice(
        $.number,
        $.string,
        $.template_string,
        $.true,
        $.false,
        $.null,
        $.this,
        $.super,
        $.identifier,
        $.array,
        $.dict,
        $.parenthesized_expression,
      ),

    // ── Numbers ────────────────────────────────────────────────────────

    number: ($) =>
      token(choice(
        /0[xX][0-9a-fA-F][0-9a-fA-F_]*/,
        /0[oO][0-7][0-7_]*/,
        /0[bB][01][01_]*/,
        /[0-9][0-9_]*\.[0-9][0-9_]*([eE][+-]?[0-9][0-9_]*)?/,
        /[0-9][0-9_]*[eE][+-]?[0-9][0-9_]*/,
        /[0-9][0-9_]*/,
      )),

    // ── Strings ────────────────────────────────────────────────────────

    string: ($) =>
      // Single-quoted — no interpolation
      seq(
        "'",
        repeat(choice(
          token.immediate(prec(1, /[^'\\\n]+/)),
          $.escape_sequence,
        )),
        "'",
      ),

    // Double-quoted with ${} interpolation
    template_string: ($) =>
      seq(
        '"',
        repeat(choice(
          $.string_fragment,
          $.escape_sequence,
          "$",
          $.interpolation,
        )),
        '"',
      ),

    string_fragment: ($) =>
      token.immediate(prec(1, /[^"\\$]+/)),

    interpolation: ($) =>
      seq("${", $._expression, "}"),

    escape_sequence: ($) =>
      token(prec(1, seq(
        "\\",
        choice(
          /[\\'"0nrbt$]/,
          /u[0-9a-fA-F]{4}/,
        ),
      ))),

    // ── Booleans / null ────────────────────────────────────────────────

    true: ($) => "true",
    false: ($) => "false",
    null: ($) => "null",
    boolean: ($) => choice($.true, $.false),

    // ── This / Super ───────────────────────────────────────────────────

    this: ($) => "this",
    super: ($) => "super",

    // ── Identifier ─────────────────────────────────────────────────────

    identifier: ($) =>
      /[a-zA-Z_$][a-zA-Z0-9_$]*/,

    // ── Array ──────────────────────────────────────────────────────────

    array: ($) =>
      seq("[", commaSep($._expression), optional(","), "]"),

    // ── Dict ───────────────────────────────────────────────────────────
    // prec(-1): dict loses to block_statement at statement level.
    // Vora's parser: { at statement level is always a block.

    dict: ($) =>
      prec(-1, seq(
        "{",
        commaSep(choice($.pair, $.shorthand_property_identifier)),
        optional(","),
        "}",
      )),

    pair: ($) =>
      seq(
        field("key", choice($.identifier, $.string, $.computed_key)),
        ":",
        field("value", $._expression),
      ),

    computed_key: ($) =>
      seq("[", $._expression, "]"),

    shorthand_property_identifier: ($) =>
      $.identifier,

    // ── Parenthesized ──────────────────────────────────────────────────

    parenthesized_expression: ($) =>
      seq("(", $._expression, ")"),

    // ════════════════════════════════════════════════════════════════════
    // Comments
    // ════════════════════════════════════════════════════════════════════

    comment: ($) =>
      choice($.block_comment, $.line_comment),

    line_comment: ($) =>
      token(seq("//", /.*/)),
  },
});

// ═════════════════════════════════════════════════════════════════════════════
// Helpers
// ═════════════════════════════════════════════════════════════════════════════

function commaSep(rule) {
  return optional(commaSep1(rule));
}

function commaSep1(rule) {
  return seq(rule, repeat(seq(",", rule)));
}
