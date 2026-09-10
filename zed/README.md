# Vora Zed Extension

[Vora](https://github.com/Vora-lang/Vora) language support for the [Zed](https://zed.dev) editor.

## Current status

| Feature | Status | Notes |
|---------|--------|-------|
| Language recognition (`.va`) | ✅ | `languages/vora/config.toml` |
| Syntax highlighting | ✅ | `grammars/vora.wasm` built (incl. P1-F bitwise `& \| ^ ~ << >>`); query in `tree-sitter-vora/queries/highlights.scm` |
| Bracket matching / auto-close | ✅ | Fallback regex in config.toml |
| Auto-indentation | ✅ | Fallback regex in config.toml |
| LSP (diagnostics, formatting) | 🔜 | Deferred — `vora-lsp` binary exists, needs wiring |

## Installation

### Manual install

Copy this directory into Zed's extensions folder:

| Platform | Path |
|----------|------|
| macOS | `~/Library/Application Support/Zed/extensions/vora/` |
| Linux | `~/.local/share/zed/extensions/vora/` |
| Windows | `%LOCALAPPDATA%\Zed\extensions\vora\` |

### Build tree-sitter WASM

```bash
cd tree-sitter-vora
npm install
npx tree-sitter generate        # ✅ already done (rerun after grammar.js changes)
npx tree-sitter build --wasm    # needs emcc (emsdk) or docker on PATH
# → output: tree-sitter-vora.wasm, copy to ../grammars/vora.wasm
```

> Windows note: tree-sitter-cli < 0.25 spawns `emcc.bat` specifically; current
> emsdk ships only `emcc.exe`, so a one-line `emcc.bat` shim at the emsdk root
> (`@"%~dp0upstream\emscripten\emcc.exe" %*`) makes `npx tree-sitter build
> --wasm` work.

## File structure

```
vora/
├── extension.toml              # Extension manifest
├── languages/
│   └── vora/
│       └── config.toml         # Language configuration (grammar, brackets, indents)
├── grammars/
│   └── vora.wasm               # Compiled grammar (from tree-sitter-vora build)
├── tree-sitter-vora/           # Tree-sitter grammar project
│   ├── grammar.js              # Grammar definition (precedence mirrors parser.cpp)
│   ├── queries/
│   │   └── highlights.scm      # Highlight query (declared in tree-sitter.json)
│   ├── src/
│   │   ├── scanner.c           # External scanner (block comments)
│   │   ├── parser.c            # Generated parser
│   │   └── grammar.json        # Generated grammar JSON
│   └── package.json
└── README.md
```

## License

MIT — see the main [Vora](https://github.com/Vora-lang/Vora) repository.
