; Tree-sitter highlight query for Vora
; Scope names follow the Zed / nvim-treesitter convention.
; Keep in sync with grammar.js — regenerate parser after rule changes.

; ── Comments ─────────────────────────────────────────────────────────────

(line_comment) @comment
(block_comment) @comment

; ── Strings ──────────────────────────────────────────────────────────────

(string) @string
(template_string) @string
(string_fragment) @string
(escape_sequence) @string.escape

(interpolation
  "${" @punctuation.special
  "}" @punctuation.special)

; ── Numbers ──────────────────────────────────────────────────────────────

(number) @number

; ── Constants / builtins ─────────────────────────────────────────────────

[(true) (false) (null)] @constant.builtin
(this) @variable.builtin
(super) @variable.builtin

; ── Declaration keywords ─────────────────────────────────────────────────

["let" "const" "func" "Obj"] @keyword

; ── Control flow ─────────────────────────────────────────────────────────

[
  "if" "else" "while" "for" "in" "do" "return" "yield"
  "match" "defer"
] @keyword

(break_statement) @keyword
(continue_statement) @keyword

; ── Exceptions ───────────────────────────────────────────────────────────

["try" "catch" "finally" "throw"] @keyword

; ── Import / export ──────────────────────────────────────────────────────

["import" "export" "from" "as"] @keyword

; ── Operators ────────────────────────────────────────────────────────────

; Assignment
["=" "+=" "-=" "*=" "/=" "%="] @operator

; Logical
["&&" "||" "!"] @operator

; Bitwise (P1-F; "|" doubles as match or-pattern separator)
["&" "|" "^" "~" "<<" ">>"] @operator

; Equality / comparison
["==" "!=" "<" "<=" ">" ">="] @operator

; Arithmetic
["+" "-" "*" "/" "%" "**"] @operator

; Increment / decrement
["++" "--"] @operator

; Ternary / null-coalescing / optional chaining
["?" ":" "??"] @operator
"?." @operator

; Arrow / spread / range
"=>" @operator
"..." @operator
[".." "..="] @operator

; ── Punctuation ──────────────────────────────────────────────────────────

["(" ")" "[" "]" "{" "}"] @punctuation.bracket
["," "." ";"] @punctuation.delimiter

; ── Functions ────────────────────────────────────────────────────────────

(function_declaration
  name: (identifier) @function)

(method_definition
  name: (identifier) @function.method)

(call_expression
  function: (identifier) @function.call)

(call_expression
  function: (member_expression
    property: (identifier) @function.method.call))

; ── Types / classes ──────────────────────────────────────────────────────

(class_declaration
  name: (identifier) @type)

(type_annotation
  type_name: (identifier) @type)

; ── Variables ────────────────────────────────────────────────────────────

(let_declaration
  name: (identifier) @variable)

(const_declaration
  name: (identifier) @variable)

(for_in_statement
  variable: (identifier) @variable)

(formal_parameters
  (identifier) @variable.parameter)

; Match wildcard arm
(wildcard_pattern) @keyword
