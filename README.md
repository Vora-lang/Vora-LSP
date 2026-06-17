# Vora LSP — Language Server + VS Code Extension

Language Server Protocol (LSP) implementation for the [Vora](https://github.com/Vora-lang/Vora) programming language, plus the official VS Code extension.

## Structure

```
Vora-LSP/
├── server/                  # C++ LSP server (builds as part of Vora)
│   ├── lsp_server.h         # LspServer class
│   ├── lsp_server.cpp       # Protocol handlers + main loop
│   └── main.cpp             # Entry point
├── src/
│   └── extension.ts         # VS Code language client
├── syntaxes/
│   └── vora.tmLanguage.json # TextMate grammar for .va files
├── package.json             # VS Code extension manifest
└── language-configuration.json
```

## Features

- **Diagnostics**: Real-time parse error reporting via `textDocument/publishDiagnostics`
- **Formatting**: Document formatting via `textDocument/formatting` (uses Vora's `SourceFormatter`)
- **Completion, go-to-definition, hover, document symbols**: Stubs returning empty — need semantic analysis (roadmap #4)

## Building

The LSP server is built as part of the main [Vora](https://github.com/Vora-lang/Vora) project:

```bash
cd Vora
cmake -B build
cmake --build build --target vora-lsp
```

The binary `vora-lsp` (or `vora-lsp.exe` on Windows) is produced in the build output directory.

## VS Code Extension

### Development

```bash
cd Vora-LSP
npm install
npm run compile
```

Press F5 in VS Code to launch an Extension Development Host with Vora support.

### Packaging

```bash
npm install -g @vscode/vsce
vsce package
```

This produces `vora-lang-0.1.0.vsix` which can be installed via `code --install-extension`.

## License

MIT — see the main [Vora](https://github.com/Vora-lang/Vora) repository.
