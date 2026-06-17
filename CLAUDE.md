# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Description

**Vora-LSP** is the Language Server Protocol (LSP) implementation for the Vora programming language, plus the official VS Code extension.

## Cross-repo references

| Repository | Path | Purpose |
|-----------|------|---------|
| **Vora-LSP** (this repo) | `D:\Vora-LSP` | LSP server (C++) + VS Code extension (TypeScript) |
| **Vora** | `D:\Vora` | Language core: lexer, parser, AST, compiler, VM, runtime |
| **Vora website** | `D:\Vora-lang.github.io` | Generated user documentation site |

**The Vora core is the source of truth** for all language behavior. The LSP server is a thin wrapper — it delegates all analysis to the Vora lexer, parser, and formatter. When implementing LSP features, always start by understanding the corresponding Vora subsystem.

## Repository structure

```
Vora-LSP/
├── server/                  # C++ LSP server
│   ├── CMakeLists.txt       # (removed — built via Vora's CMakeLists.txt)
│   ├── lsp_server.h         # LspServer class + DiagnosticCollector
│   ├── lsp_server.cpp       # Protocol handlers + main loop
│   └── main.cpp             # Entry point
├── src/
│   └── extension.ts         # VS Code language client
├── syntaxes/
│   └── vora.tmLanguage.json # TextMate grammar for .va files
├── package.json             # VS Code extension manifest
├── tsconfig.json
├── language-configuration.json
└── .vscodeignore
```

## Build

### LSP server (C++)

The LSP server is built as part of the main Vora project. It links against `vora_lib` and uses headers from `D:\Vora\src\`.

```bash
cd D:\Vora
cmake -B build
cmake --build build --target vora-lsp
```

Binary: `D:\Vora\build\<preset>\Debug\vora-lsp.exe` (or `vora-lsp` on Linux/macOS)

The build target is defined in `D:\Vora\CMakeLists.txt` (search for "LSP server executable"). Source files are referenced via the `VORA_LSP_SOURCE_DIR` cache variable (default: `../Vora-LSP/server` relative to the Vora repo).

### VS Code extension (TypeScript)

```bash
cd D:\Vora-LSP
npm install
npm run compile        # one-shot build
npm run watch          # watch mode for development
```

Press F5 in VS Code to launch an Extension Development Host.

### Packaging

```bash
npm install -g @vscode/vsce
vsce package           # → vora-lang-0.1.0.vsix
code --install-extension vora-lang-0.1.0.vsix
```

## LSP server architecture

```
stdin → StdioTransport (Content-Length framing)
     → MessageRouter (JSON-RPC dispatch)
       → LspServer handlers
         → Vora Lexer + Parser + SourceFormatter
       ← JSON-RPC response
     ← StdioTransport
stdout
```

### Handler → Vora dependency map

| LSP method | Vora subsystem used | Status |
|-----------|-------------------|--------|
| `textDocument/publishDiagnostics` | `Lexer` + `Parser` + `ErrorReporter` | ✅ Working |
| `textDocument/formatting` | `Lexer` + `Parser` + `SourceFormatter` | ✅ Working |
| `textDocument/completion` | Needs semantic analysis (#4) | ⏳ Empty |
| `textDocument/definition` | Needs semantic analysis (#4) | ⏳ Empty |
| `textDocument/hover` | Needs semantic analysis (#4) | ⏳ Empty |
| `textDocument/documentSymbol` | Needs semantic analysis (#4) | ⏳ Empty |

### Key classes

- **`DiagnosticCollector`** (`lsp_server.h`): Implements `ErrorReporter`, collects `Diagnostic` structs during lexing/parsing. Diagnostics are converted to LSP format (0-based lines) and published via `textDocument/publishDiagnostics`.
- **`LspServer`** (`lsp_server.h`): Owns the `StdioTransport`, `MessageRouter`, and document store. The constructor registers all handlers. `run()` enters the blocking main loop.

## Adding LSP features

When implementing a new LSP feature (e.g. completion):

1. **Understand the Vora subsystem** first — read the relevant code in `D:\Vora\src\`.
2. **Add the handler** to `lsp_server.cpp` (register in constructor, implement method).
3. **Update server capabilities** in `handleInitialize()` if the feature needs client-side enablement.
4. **Test manually**: pipe a JSON-RPC request via stdin and verify the response.
5. **Update syntax highlighting** (`syntaxes/vora.tmLanguage.json`) if the feature changes the language grammar (new keywords, operators, etc.).

## When syntax changes

If the Vora language adds new keywords, operators, or literal syntax:
1. Update `syntaxes/vora.tmLanguage.json` in this repo
2. Update `USER_GUIDE.md` in `D:\Vora`
3. Update `D:\Vora\docs\` design docs
4. Regenerate `D:\Vora-lang.github.io` via `scripts/build-website-docs.py`
