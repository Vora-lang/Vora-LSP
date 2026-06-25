/**
 * Vora LSP server implementation.
 *
 * Protocol handlers for the full LSP lifecycle + language features.
 * Diagnostics use the Vora lexer + error-tolerant parser to surface
 * parse errors. Formatting delegates to SourceFormatter.
 *
 * Completion, go-to-definition, hover, and document symbols are now
 * backed by the SemanticAnalyzer for scope-aware resolution.
 * References and signature help are also implemented.
 */

#include "lsp_server.h"

#include "lexer/lexer.h"
#include "parser/parser.h"
#include "formatter/formatter.h"
#include "lsp/semantic_analyzer.h"
#include "ast/stmt.h"
#include "ast/expr.h"
#include "ast/program.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace vora::lsp {

// ═══════════════════════════════════════════════════════════════════════════
// DiagnosticCollector
// ═══════════════════════════════════════════════════════════════════════════

void DiagnosticCollector::report(int line, int column, int length,
                                  const std::string& message, Severity severity) {
    diagnostics_.push_back({line, column, length, message, severity});
}

std::vector<Diagnostic> DiagnosticCollector::takeDiagnostics() {
    std::vector<Diagnostic> result;
    result.swap(diagnostics_);
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// LspServer constructor — register all handlers
// ═══════════════════════════════════════════════════════════════════════════

LspServer::LspServer() {
    // Set server version from build system (VORA_VERSION) or fallback.
#ifdef VORA_VERSION
    serverVersion_ = VORA_VERSION;
#else
    serverVersion_ = "0.1.0";
#endif

    // ── Lifecycle ───────────────────────────────────────────────────────
    router_.registerRequest("initialize",
        [this](const nlohmann::json& p) { return handleInitialize(p); });
    router_.registerNotification("initialized",
        [this](const nlohmann::json& p) { handleInitialized(p); });
    router_.registerRequest("shutdown",
        [this](const nlohmann::json& p) { return handleShutdown(p); });
    router_.registerNotification("exit",
        [this](const nlohmann::json& p) { handleExit(p); });

    // ── Document sync ───────────────────────────────────────────────────
    router_.registerNotification("textDocument/didOpen",
        [this](const nlohmann::json& p) { handleDidOpen(p); });
    router_.registerNotification("textDocument/didChange",
        [this](const nlohmann::json& p) { handleDidChange(p); });
    router_.registerNotification("textDocument/didClose",
        [this](const nlohmann::json& p) { handleDidClose(p); });
    router_.registerNotification("textDocument/didSave",
        [this](const nlohmann::json& p) { handleDidSave(p); });

    // ── Language features ───────────────────────────────────────────────
    router_.registerRequest("textDocument/completion",
        [this](const nlohmann::json& p) { return handleCompletion(p); });
    router_.registerRequest("textDocument/definition",
        [this](const nlohmann::json& p) { return handleDefinition(p); });
    router_.registerRequest("textDocument/hover",
        [this](const nlohmann::json& p) { return handleHover(p); });
    router_.registerRequest("textDocument/formatting",
        [this](const nlohmann::json& p) { return handleFormatting(p); });
    router_.registerRequest("textDocument/documentSymbol",
        [this](const nlohmann::json& p) { return handleDocumentSymbol(p); });
    router_.registerRequest("textDocument/references",
        [this](const nlohmann::json& p) { return handleReferences(p); });
    router_.registerRequest("textDocument/signatureHelp",
        [this](const nlohmann::json& p) { return handleSignatureHelp(p); });

    log("Vora LSP server " + serverVersion_ + " ready");
}

LspServer::~LspServer() = default;

// ═══════════════════════════════════════════════════════════════════════════
// Main loop
// ═══════════════════════════════════════════════════════════════════════════

void LspServer::run() {
    for (;;) {
        auto raw = transport_.readMessage();
        if (!raw.has_value()) {
            // EOF on stdin — client disconnected.
            log("stdin closed, exiting");
            break;
        }

        auto response = router_.handleMessage(*raw);
        if (response.has_value()) {
            if (!transport_.sendMessage(*response)) {
                log("Failed to write response, exiting");
                break;
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Lifecycle handlers
// ═══════════════════════════════════════════════════════════════════════════

nlohmann::json LspServer::handleInitialize(const nlohmann::json& params) {
    initialized_ = true;

    // Capture workspace root for import resolution.
    if (params.contains("rootUri") && params["rootUri"].is_string()) {
        workspaceRoot_ = params["rootUri"].get<std::string>();
        // Strip file:// prefix if present.
        if (workspaceRoot_.rfind("file://", 0) == 0) {
            workspaceRoot_ = workspaceRoot_.substr(7);
        }
    } else if (params.contains("rootPath") && params["rootPath"].is_string()) {
        workspaceRoot_ = params["rootPath"].get<std::string>();
    }

    nlohmann::json result;
    result["capabilities"]["textDocumentSync"]["openClose"] = true;
    result["capabilities"]["textDocumentSync"]["change"] = 1;  // full sync
    result["capabilities"]["textDocumentSync"]["save"] = nlohmann::json::object();

    result["capabilities"]["completionProvider"]["triggerCharacters"] =
        nlohmann::json::array({".", ":"});
    result["capabilities"]["completionProvider"]["resolveProvider"] = false;

    result["capabilities"]["definitionProvider"] = true;
    result["capabilities"]["hoverProvider"] = true;
    result["capabilities"]["documentFormattingProvider"] = true;
    result["capabilities"]["documentSymbolProvider"] = true;
    result["capabilities"]["referencesProvider"] = true;
    result["capabilities"]["signatureHelpProvider"]["triggerCharacters"] =
        nlohmann::json::array({"(", ","});
    result["capabilities"]["signatureHelpProvider"]["retriggerCharacters"] =
        nlohmann::json::array({")"});

    result["serverInfo"]["name"] = serverName_;
    result["serverInfo"]["version"] = serverVersion_;

    log("initialize → " + serverName_ + " " + serverVersion_);
    return result;
}

void LspServer::handleInitialized(const nlohmann::json& /*params*/) {
    log("initialized");
}

nlohmann::json LspServer::handleShutdown(const nlohmann::json& /*params*/) {
    shutdown_ = true;
    log("shutdown");
    return nullptr;
}

void LspServer::handleExit(const nlohmann::json& /*params*/) {
    log("exit");
    // Per LSP spec: exit notification after shutdown → terminate.
    // Use std::exit() (not std::_Exit) so that destructors, atexit handlers,
    // and I/O buffers are cleaned up properly during normal shutdown.
    std::exit(shutdown_ ? 0 : 1);
}

// ═══════════════════════════════════════════════════════════════════════════
// Document synchronization
// ═══════════════════════════════════════════════════════════════════════════

void LspServer::handleDidOpen(const nlohmann::json& params) {
    auto td = params["textDocument"];
    std::string uri = td["uri"].get<std::string>();

    DocumentState doc;
    doc.uri = uri;
    doc.languageId = td.value("languageId", "vora");
    doc.version = td.value("version", 0);
    doc.text = td["text"].get<std::string>();
    doc.open = true;
    documents_[uri] = std::move(doc);

    log("didOpen: " + uri);
    publishDiagnostics(uri);
}

void LspServer::handleDidChange(const nlohmann::json& params) {
    auto td = params["textDocument"];
    std::string uri = td["uri"].get<std::string>();

    auto* doc = getDocument(uri);
    if (!doc) {
        log("didChange for unknown document: " + uri);
        return;
    }

    doc->version = td.value("version", doc->version + 1);

    // Full-text sync: the last change contains the entire new text.
    auto& changes = params["contentChanges"];
    if (!changes.empty()) {
        doc->text = changes.back()["text"].get<std::string>();
    }

    // Invalidate cached analysis.
    doc->cacheValid = false;
    doc->cachedProgram.reset();
    doc->cachedAnalyzer.reset();

    publishDiagnostics(uri);
}

void LspServer::handleDidClose(const nlohmann::json& params) {
    auto td = params["textDocument"];
    std::string uri = td["uri"].get<std::string>();

    documents_.erase(uri);

    // Clear diagnostics for this document.
    nlohmann::json diagParams;
    diagParams["uri"] = uri;
    diagParams["diagnostics"] = nlohmann::json::array();
    sendNotification("textDocument/publishDiagnostics", diagParams);

    log("didClose: " + uri);
}

void LspServer::handleDidSave(const nlohmann::json& /*params*/) {
    // No-op: full text sync on didChange already handles content updates.
    // The server re-parses on every didChange, so didSave requires no action.
}

// ═══════════════════════════════════════════════════════════════════════════
// Language features
// ═══════════════════════════════════════════════════════════════════════════

nlohmann::json LspServer::handleCompletion(const nlohmann::json& params) {
    auto td = params["textDocument"];
    std::string uri = td["uri"].get<std::string>();
    auto* doc = getDocument(uri);
    if (!doc) {
        return nlohmann::json::object();
    }

    int line = params["position"]["line"].get<int>();
    int character = params["position"]["character"].get<int>();

    nlohmann::json result;
    result["isIncomplete"] = false;
    auto& items = result["items"] = nlohmann::json::array();

    int offset = lspPositionToOffset(doc->text, line, character);
    auto context = detectCompletionContext(doc->text, offset);

    // ── Semantic analysis (lazy, cached) ──────────────────────────────────
    parseAndAnalyze(uri);
    vora::SemanticAnalyzer* analyzer = (doc->cacheValid) ? doc->cachedAnalyzer.get() : nullptr;

    // ═══════════════════════════════════════════════════════════════════════
    // Context dispatch
    // ═══════════════════════════════════════════════════════════════════════

    if (context == CompletionContext::PROPERTY_ACCESS) {
        // ── After '.' → only methods, filtered by type if possible ────────
        int dotOffset = offset - 1;  // position of the '.' character
        std::string typeName = detectPrecedingType(doc->text, dotOffset, analyzer);

        if (!typeName.empty()) {
            auto typedMethods = getTypeMethods(typeName);
            for (auto& m : typedMethods) items.push_back(std::move(m));
        } else {
            // Fallback: all methods (type unknown).
            auto allMethods = getAllMethods();
            for (auto& m : allMethods) items.push_back(std::move(m));
        }

    } else if (context == CompletionContext::STRING_LITERAL) {
        // ── Inside "..." → no completions (defer to import-path handling later) ──
        // For now, return empty; import-path completion can be added later.

    } else if (context == CompletionContext::CALL_ARGUMENT) {
        // ── Inside (...) → expression-level keywords + builtins + symbols ──
        auto exprKeys = getExpressionKeywords();
        for (auto& kw : exprKeys) items.push_back(std::move(kw));

        auto builtins = getBuiltinCompletions();
        for (auto& b : builtins) items.push_back(std::move(b));

        // Visible symbols.
        if (analyzer) {
            int voraLine = line + 1;
            int voraCol = character + 1;
            auto visibleSymbols = analyzer->getVisibleSymbols(voraLine, voraCol);

            for (auto* sym : visibleSymbols) {
                nlohmann::json item;
                item["label"] = sym->name;

                switch (sym->kind) {
                    case SymbolKind::Variable:  item["kind"] = 6;  break;
                    case SymbolKind::Constant:  item["kind"] = 14; break;
                    case SymbolKind::Function:  item["kind"] = 3;  break;
                    case SymbolKind::Parameter: item["kind"] = 6;  break;
                    case SymbolKind::Object:    item["kind"] = 7;  break;
                    case SymbolKind::Method:    item["kind"] = 2;  break;
                    case SymbolKind::Import:    item["kind"] = 9;  break;
                    case SymbolKind::ForVar:    item["kind"] = 6;  break;
                    case SymbolKind::CatchVar:  item["kind"] = 6;  break;
                }

                std::string detail;
                switch (sym->kind) {
                    case SymbolKind::Variable:  detail = "let " + sym->name; break;
                    case SymbolKind::Constant:  detail = "const " + sym->name; break;
                    case SymbolKind::Function:
                        detail = "func " + sym->name + "(";
                        for (size_t pi = 0; pi < sym->paramNames.size(); pi++) {
                            if (pi > 0) detail += ", ";
                            detail += sym->paramNames[pi];
                        }
                        detail += ")";
                        break;
                    case SymbolKind::Object:
                        detail = "Obj " + sym->name;
                        if (!sym->parentNames.empty()) detail += " : " + sym->parentNames[0];
                        break;
                    case SymbolKind::Method:   detail = "method " + sym->name; break;
                    case SymbolKind::Import:   detail = "import \"" + sym->importPath + "\""; break;
                    case SymbolKind::Parameter: detail = "param " + sym->name; break;
                    default: detail = sym->name; break;
                }
                if (!sym->typeHint.empty()) detail += ": " + sym->typeHint;
                item["detail"] = detail;

                // Sort: same-scope items first.
                item["sortText"] = std::to_string(sym->scopeLevel) + "_" + sym->name;
                items.push_back(std::move(item));
            }
        }

    } else {
        // ── GENERAL context: keywords + builtins + symbols ─────────────────
        auto keywords = getKeywordCompletions();
        for (auto& kw : keywords) items.push_back(std::move(kw));

        auto builtins = getBuiltinCompletions();
        for (auto& b : builtins) items.push_back(std::move(b));

        // Visible symbols.
        if (analyzer) {
            int voraLine = line + 1;
            int voraCol = character + 1;
            auto visibleSymbols = analyzer->getVisibleSymbols(voraLine, voraCol);

            for (auto* sym : visibleSymbols) {
                nlohmann::json item;
                item["label"] = sym->name;

                switch (sym->kind) {
                    case SymbolKind::Variable:  item["kind"] = 6;  break;
                    case SymbolKind::Constant:  item["kind"] = 14; break;
                    case SymbolKind::Function:  item["kind"] = 3;  break;
                    case SymbolKind::Parameter: item["kind"] = 6;  break;
                    case SymbolKind::Object:    item["kind"] = 7;  break;
                    case SymbolKind::Method:    item["kind"] = 2;  break;
                    case SymbolKind::Import:    item["kind"] = 9;  break;
                    case SymbolKind::ForVar:    item["kind"] = 6;  break;
                    case SymbolKind::CatchVar:  item["kind"] = 6;  break;
                }

                std::string detail;
                switch (sym->kind) {
                    case SymbolKind::Variable:  detail = "let " + sym->name; break;
                    case SymbolKind::Constant:  detail = "const " + sym->name; break;
                    case SymbolKind::Function:
                        detail = "func " + sym->name + "(";
                        for (size_t pi = 0; pi < sym->paramNames.size(); pi++) {
                            if (pi > 0) detail += ", ";
                            detail += sym->paramNames[pi];
                        }
                        detail += ")";
                        break;
                    case SymbolKind::Object:
                        detail = "Obj " + sym->name;
                        if (!sym->parentNames.empty()) detail += " : " + sym->parentNames[0];
                        break;
                    case SymbolKind::Method:   detail = "method " + sym->name; break;
                    case SymbolKind::Import:   detail = "import \"" + sym->importPath + "\""; break;
                    case SymbolKind::Parameter: detail = "param " + sym->name; break;
                    default: detail = sym->name; break;
                }
                if (!sym->typeHint.empty()) detail += ": " + sym->typeHint;
                item["detail"] = detail;

                item["sortText"] = std::to_string(sym->scopeLevel) + "_" + sym->name;
                items.push_back(std::move(item));
            }
        }
    }

    return result;
}

nlohmann::json LspServer::handleDefinition(const nlohmann::json& params) {
    auto td = params["textDocument"];
    std::string uri = td["uri"].get<std::string>();
    auto* doc = getDocument(uri);
    if (!doc) return nullptr;

    int line = params["position"]["line"].get<int>();
    int character = params["position"]["character"].get<int>();

    parseAndAnalyze(uri);
    if (!doc->cacheValid || !doc->cachedAnalyzer) return nullptr;

    int voraLine = line + 1;
    int voraCol = character + 1;

    const auto* symbol = doc->cachedAnalyzer->findDeclarationAt(voraLine, voraCol);
    if (!symbol) return nullptr;

    // Convert to LSP location.
    nlohmann::json location;
    location["uri"] = uri;
    location["range"] = tokenToLspRange(symbol->declToken);

    // If it's an import, try cross-file resolution.
    if (symbol->kind == SymbolKind::Import && !symbol->importPath.empty()) {
        std::string importFilePath = resolveImportPath(uri, symbol->importPath);
        auto* importedAnalyzer = analyzeImportedFile(importFilePath);
        if (importedAnalyzer) {
            // Return all exported symbols from the imported file as a list.
            auto& exported = importedAnalyzer->getExportedSymbols();
            if (exported.size() == 1) {
                // Single export — point directly to it.
                std::string fileUri = "file:///" + importFilePath;
                location["uri"] = fileUri;
                location["range"] = tokenToLspRange(exported[0]->declToken);
            } else if (!exported.empty()) {
                // Multiple exports — return the first one (client can show list).
                std::string fileUri = "file:///" + importFilePath;
                location["uri"] = fileUri;
                location["range"] = tokenToLspRange(exported[0]->declToken);
            }
        }
    }

    return location;
}

nlohmann::json LspServer::handleHover(const nlohmann::json& params) {
    auto td = params["textDocument"];
    std::string uri = td["uri"].get<std::string>();
    auto* doc = getDocument(uri);
    if (!doc) return nullptr;

    int line = params["position"]["line"].get<int>();
    int character = params["position"]["character"].get<int>();

    // Get the word at the cursor position.
    int offset = lspPositionToOffset(doc->text, line, character);
    const std::string& text = doc->text;

    // Extract word boundaries at offset.
    int start = offset;
    while (start > 0 && (std::isalnum(static_cast<unsigned char>(text[start - 1])) || text[start - 1] == '_' || text[start - 1] == '$')) {
        start--;
    }
    int end = offset;
    while (end < static_cast<int>(text.size()) && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_' || text[end] == '$')) {
        end++;
    }
    std::string word = text.substr(start, end - start);
    if (word.empty()) return nullptr;

    // ── Check for keywords ──────────────────────────────────────────────
    static const std::unordered_map<std::string, std::string> keywordHovers = {
        {"let", "**let** — declares a mutable variable.\n\n`let name = value` or `let name: Type = value`"},
        {"const", "**const** — declares an immutable binding.\n\n`const name = value` — must be initialized, cannot be reassigned."},
        {"func", "**func** — declares a named function or anonymous lambda.\n\n`func name(params) { body }` or `func(params) { body }`"},
        {"return", "**return** — exits the current function, optionally with a value.\n\n`return` or `return value`"},
        {"if", "**if** — conditional branch.\n\n`if (condition) { ... }` or `if condition { ... }`"},
        {"else", "**else** — alternative branch for if.\n\n`else { ... }` or `else if (condition) { ... }`"},
        {"while", "**while** — loop while condition is truthy.\n\n`while (condition) { body }`"},
        {"for", "**for** — C-style or for-in loop.\n\n`for (init; cond; inc) { body }` or `for item in iterable { body }`"},
        {"in", "**in** — used in for-in loops.\n\n`for item in iterable { body }`"},
        {"break", "**break** — exits the innermost loop."},
        {"continue", "**continue** — skips to the next iteration of the innermost loop."},
        {"Obj", "**Obj** — declares a class-like object.\n\n`Obj Name : Parent(params) { this.prop = prop; func method() { ... } }`"},
        {"this", "**this** — refers to the current object instance inside a method or constructor."},
        {"super", "**super** — refers to the parent class, used to call parent methods."},
        {"try", "**try** — begins an exception-handling block.\n\n`try { ... } catch (e) { ... } finally { ... }`"},
        {"catch", "**catch** — handles an exception thrown in a try block.\n\n`catch (variable) { ... }`"},
        {"finally", "**finally** — cleanup block that always executes after try/catch."},
        {"throw", "**throw** — throws an exception value.\n\n`throw \"error message\"` or `throw expression`"},
        {"import", "**import** — imports a module.\n\n`import \"path\"` or `import \"path\" as alias`"},
        {"export", "**export** — exports a declaration from the current module.\n\n`export func`, `export let`, `export const`, `export Obj`"},
        {"from", "**from** — selective import.\n\n`from \"path\" import name1, name2`"},
        {"as", "**as** — aliases an import.\n\n`import \"path\" as alias`"},
        {"yield", "**yield** — suspends a generator function, returning a value.\n\n`yield value`"},
    };

    auto kwIt = keywordHovers.find(word);
    if (kwIt != keywordHovers.end()) {
        nlohmann::json result;
        result["contents"]["kind"] = "markdown";
        result["contents"]["value"] = kwIt->second;
        return result;
    }

    // ── Check for builtin functions ─────────────────────────────────────
    static const std::unordered_map<std::string, std::string> builtinHovers = {
        {"print", "**print**(...values)\n\nPrints values separated by spaces, followed by a newline."},
        {"type", "**type**(value) → string\n\nReturns the type name of a value: `\"int\"`, `\"float\"`, `\"string\"`, `\"array\"`, `\"dict\"`, `\"boolean\"`, `\"null\"`, `\"function\"`, `\"object\"`."},
        {"len", "**len**(value) → int\n\nReturns the length of an array, string, or dict."},
        {"clock", "**clock**() → float\n\nReturns the current Unix timestamp in seconds."},
        {"assert", "**assert**(condition, message?)\n\nThrows an error if condition is falsy."},
        {"int", "**int**(value) → int\n\nConverts a value to integer (truncates floats)."},
        {"float", "**float**(value) → float\n\nConverts a value to a 64-bit float."},
        {"toString", "**toString**(value) → string\n\nConverts any value to its string representation."},
        {"input", "**input**(prompt?) → string\n\nReads a line from stdin, returns null on EOF."},
        {"range", "**range**(stop) or **range**(start, stop, step?) → array\n\nGenerates an array of integers."},
        {"bin", "**bin**(n) → string\n\nConverts an integer to a binary string (e.g. `\"0b101010\"`)."},
        {"oct", "**oct**(n) → string\n\nConverts an integer to an octal string (e.g. `\"0o77\"`)."},
        {"hex", "**hex**(n) → string\n\nConverts an integer to a hex string (e.g. `\"0xff\"`)."},
        {"iter", "**iter**(collection) → iterator\n\nCreates an iterator over an array, string, dict, or generator."},
        {"next", "**next**(iterator) → value\n\nAdvances an iterator and returns the next element. Throws StopIteration at end."},
        {"abs", "**abs**(x) → number\n\nReturns the absolute value."},
        {"sqrt", "**sqrt**(x) → float\n\nReturns the square root."},
        {"sin", "**sin**(x) → float\n\nReturns the sine of x (radians)."},
        {"cos", "**cos**(x) → float\n\nReturns the cosine of x (radians)."},
        {"min", "**min**(array) → number\n\nReturns the minimum value in an array."},
        {"max", "**max**(array) → number\n\nReturns the maximum value in an array."},
        {"jsonParse", "**jsonParse**(string) → value\n\nParses a JSON string into a Vora value."},
        {"jsonStringify", "**jsonStringify**(value, indent?) → string\n\nSerializes a Vora value to a JSON string."},
        {"random_int", "**random_int**(min, max) → int\n\nReturns a random integer in the range [min, max] (inclusive)."},
        {"random_float", "**random_float**(min, max, decimals?) → float\n\nReturns a random float in the range [min, max). Optional `decimals` controls decimal places."},
    };

    auto biIt = builtinHovers.find(word);
    if (biIt != builtinHovers.end()) {
        nlohmann::json result;
        result["contents"]["kind"] = "markdown";
        result["contents"]["value"] = biIt->second;
        return result;
    }

    // ── Check for function / variable declarations via semantic analysis ─
    parseAndAnalyze(uri);
    if (doc->cacheValid && doc->cachedAnalyzer) {
        int voraLine = line + 1;
        int voraCol = character + 1;

        const auto* symbol = doc->cachedAnalyzer->findSymbolAt(voraLine, voraCol);
        if (symbol) {
            std::string hoverText;
            switch (symbol->kind) {
                case SymbolKind::Function: {
                    hoverText = "**func " + symbol->name + "(";
                    for (size_t i = 0; i < symbol->paramNames.size(); i++) {
                        if (i > 0) hoverText += ", ";
                        hoverText += symbol->paramNames[i];
                    }
                    if (symbol->hasRestParam) hoverText += "...";
                    hoverText += ")**";
                    break;
                }
                case SymbolKind::Object: {
                    hoverText = "**Obj " + symbol->name;
                    if (!symbol->parentNames.empty()) {
                        hoverText += " : ";
                        for (size_t i = 0; i < symbol->parentNames.size(); i++) {
                            if (i > 0) hoverText += ", ";
                            hoverText += symbol->parentNames[i];
                        }
                    }
                    hoverText += "(";
                    for (size_t i = 0; i < symbol->paramNames.size(); i++) {
                        if (i > 0) hoverText += ", ";
                        hoverText += symbol->paramNames[i];
                    }
                    hoverText += ")**";
                    if (!symbol->methodNames.empty()) {
                        hoverText += "\n\nMethods: ";
                        for (size_t i = 0; i < symbol->methodNames.size(); i++) {
                            if (i > 0) hoverText += ", ";
                            hoverText += "`" + symbol->methodNames[i] + "`";
                        }
                    }
                    break;
                }
                case SymbolKind::Method: {
                    hoverText = "**method " + symbol->name + "(";
                    for (size_t i = 0; i < symbol->paramNames.size(); i++) {
                        if (i > 0) hoverText += ", ";
                        hoverText += symbol->paramNames[i];
                    }
                    hoverText += ")**";
                    break;
                }
                case SymbolKind::Variable:
                case SymbolKind::Constant: {
                    hoverText = symbol->kind == SymbolKind::Constant
                        ? "**const " + symbol->name + "**"
                        : "**let " + symbol->name + "**";
                    if (!symbol->typeHint.empty()) {
                        hoverText += ": " + symbol->typeHint;
                    }
                    break;
                }
                case SymbolKind::Parameter: {
                    hoverText = "**param " + symbol->name + "**";
                    break;
                }
                case SymbolKind::Import: {
                    hoverText = "**import \"" + symbol->importPath + "\"**";
                    break;
                }
                default: {
                    hoverText = "**" + symbol->name + "**";
                    break;
                }
            }

            // Add reference count if > 1.
            auto refs = doc->cachedAnalyzer->findReferencesTo(
                symbol->declToken.line, symbol->declToken.column);
            int refCount = static_cast<int>(refs.size()) - 1;  // minus the decl itself
            if (refCount > 0) {
                hoverText += "\n\nReferenced " + std::to_string(refCount) +
                             (refCount == 1 ? " time" : " times");
            }

            nlohmann::json result;
            result["contents"]["kind"] = "markdown";
            result["contents"]["value"] = hoverText;
            return result;
        }
    }

    return nullptr;
}

nlohmann::json LspServer::handleFormatting(const nlohmann::json& params) {
    auto td = params["textDocument"];
    std::string uri = td["uri"].get<std::string>();

    auto* doc = getDocument(uri);
    if (!doc) {
        return nlohmann::json::array();  // LSP spec: return TextEdit[], empty = no changes
    }

    // Lex → Parse → Format
    DiagnosticCollector collector;
    Lexer lexer(doc->text, collector);
    auto tokens = lexer.scanTokens();

    Parser parser(std::move(tokens), collector);
    parser.setSource(doc->text);
    auto program = parser.parse();

    if (!program || program->statements.empty()) {
        // Empty or broken — return empty TextEdit[] (no formatting changes).
        return nlohmann::json::array();
    }

    SourceFormatter formatter;
    std::string formatted = formatter.format(program.get());

    // Compute the edit range (entire document).
    int totalLines = 0;
    int lastLineLength = 0;
    for (char c : doc->text) {
        if (c == '\n') {
            totalLines++;
            lastLineLength = 0;
        } else {
            lastLineLength++;
        }
    }

    nlohmann::json edit;
    edit["range"]["start"]["line"] = 0;
    edit["range"]["start"]["character"] = 0;
    edit["range"]["end"]["line"] = totalLines;
    edit["range"]["end"]["character"] = lastLineLength;
    edit["newText"] = formatted;

    return nlohmann::json::array({edit});
}

nlohmann::json LspServer::handleDocumentSymbol(const nlohmann::json& params) {
    auto td = params["textDocument"];
    std::string uri = td["uri"].get<std::string>();
    auto* doc = getDocument(uri);
    if (!doc) return nlohmann::json::array();

    parseAndAnalyze(uri);

    // Use semantic analyzer if available; fall back to AST walk.
    if (doc->cacheValid && doc->cachedAnalyzer) {
        auto& analyzer = *doc->cachedAnalyzer;
        nlohmann::json symbols = nlohmann::json::array();

        // Collect all symbols, organized by scope.
        // Symbols at scope level 0 are top-level; higher levels are nested.
        // We build a flat list with children populated.
        const auto& allSyms = analyzer.getVisibleSymbols(1, 1);

        // Filter to symbols that have position info and are "declaration-worthy".
        for (auto* sym : allSyms) {
            if (sym->declToken.line == 0) continue;  // no position info (catch vars, etc.)
            if (sym->kind == SymbolKind::Parameter) continue;  // shown in function signature
            if (sym->kind == SymbolKind::ForVar) continue;     // too minor
            if (sym->kind == SymbolKind::CatchVar) continue;   // too minor

            nlohmann::json symJson;
            symJson["name"] = sym->name;

            // Map to LSP SymbolKind.
            switch (sym->kind) {
                case SymbolKind::Variable:  symJson["kind"] = 13; break; // Variable
                case SymbolKind::Constant:  symJson["kind"] = 14; break; // Constant
                case SymbolKind::Function:  symJson["kind"] = 12; break; // Function
                case SymbolKind::Object:    symJson["kind"] = 5;  break; // Class
                case SymbolKind::Method:    symJson["kind"] = 6;  break; // Method
                case SymbolKind::Import:    symJson["kind"] = 17; break; // Module
                default:                   symJson["kind"] = 13; break; // Variable
            }

            symJson["range"] = tokenToLspRange(sym->declToken);
            symJson["selectionRange"] = tokenToLspRange(sym->declToken);

            // Build detail string.
            std::string detail =
                std::string(symbolKindToString(sym->kind)) + " " + sym->name;
            if (!sym->typeHint.empty()) detail += ": " + sym->typeHint;
            symJson["detail"] = detail;

            // Add method children for objects.
            if (!sym->methodNames.empty()) {
                nlohmann::json children = nlohmann::json::array();
                for (auto& methodName : sym->methodNames) {
                    nlohmann::json child;
                    child["name"] = methodName;
                    child["kind"] = 6;  // Method
                    child["detail"] = "method " + methodName;
                    // We don't have position info for methods in SymbolInfo
                    // without extra tracking — skip range for now.
                    child["range"] = symJson["range"];
                    child["selectionRange"] = symJson["selectionRange"];
                    children.push_back(std::move(child));
                }
                if (!children.empty()) {
                    symJson["children"] = std::move(children);
                }
            }

            symbols.push_back(std::move(symJson));
        }
        return symbols;
    }

    // Fallback: reuse cached program from parseAndAnalyze if available.
    // Avoid re-parsing when the original parse already failed (lex errors, etc.).
    if (doc->cachedProgram) {
        nlohmann::json symbols = nlohmann::json::array();
        collectSymbolsFromProgram(*doc->cachedProgram, symbols);
        return symbols;
    }
    return nlohmann::json::array();
}

// ═══════════════════════════════════════════════════════════════════════════
// Diagnostics
// ═══════════════════════════════════════════════════════════════════════════

void LspServer::publishDiagnostics(const std::string& uri) {
    auto* doc = getDocument(uri);
    if (!doc) return;

    DiagnosticCollector collector;
    std::vector<Diagnostic> diagnostics;

    // ── Lex ─────────────────────────────────────────────────────────────
    Lexer lexer(doc->text, collector);
    auto tokens = lexer.scanTokens();

    // Save error state BEFORE taking diagnostics (takeDiagnostics empties
    // the vector, which makes hadError() return false).
    bool lexHadError = lexer.hasError();

    // Collect lexer errors.
    auto lexDiags = collector.takeDiagnostics();

    // ── Parse ────────────────────────────────────────────────────────────
    std::unique_ptr<vora::Program> program;
    bool parseHadError = true;
    if (!lexHadError) {
        // Only parse if lexing succeeded (avoids cascading parse errors
        // from garbage tokens).
        Parser parser(std::move(tokens), collector);
        parser.setSource(doc->text);
        program = parser.parse();
        parseHadError = parser.hasError();
    }

    auto parseDiags = collector.takeDiagnostics();

    // Merge lexer and parser diagnostics.
    diagnostics.insert(diagnostics.end(),
                       std::make_move_iterator(lexDiags.begin()),
                       std::make_move_iterator(lexDiags.end()));
    diagnostics.insert(diagnostics.end(),
                       std::make_move_iterator(parseDiags.begin()),
                       std::make_move_iterator(parseDiags.end()));

    // ── Semantic diagnostics (warnings/hints) ────────────────────────────
    // Run only if lex + parse succeeded (we have a valid AST).
    // Cache the analysis results so subsequent language feature requests
    // (completion, hover, goto-def) can reuse them via parseAndAnalyze().
    if (!lexHadError && !parseHadError && program) {
        auto semAnalyzer = std::make_unique<vora::SemanticAnalyzer>();
        semAnalyzer->analyze(*program);

        // Cache for later reuse (parseAndAnalyze checks cacheValid first).
        doc->cachedProgram = std::move(program);
        doc->cachedAnalyzer = std::move(semAnalyzer);
        doc->cacheValid = true;

        // Unused variables → warning.
        for (auto* sym : doc->cachedAnalyzer->getUnusedSymbols()) {
            diagnostics.push_back({
                sym->declToken.line,
                sym->declToken.column,
                static_cast<int>(sym->declToken.lexeme.size()),
                "Unused variable '" + sym->name + "'",
                Severity::Warning
            });
        }

        // Unreachable code → warning.
        for (auto& tok : doc->cachedAnalyzer->getUnreachableTokens()) {
            diagnostics.push_back({
                tok.line, tok.column,
                static_cast<int>(tok.lexeme.size()),
                "Unreachable code",
                Severity::Warning
            });
        }

        // Shadowed variables → hint.
        for (auto& [inner, outer] : doc->cachedAnalyzer->getShadowedSymbols()) {
            diagnostics.push_back({
                inner->declToken.line,
                inner->declToken.column,
                static_cast<int>(inner->declToken.lexeme.size()),
                "Variable '" + inner->name + "' shadows declaration at line " +
                    std::to_string(outer->declToken.line),
                Severity::Hint
            });
        }
    }

    // ── Convert to LSP format ────────────────────────────────────────────
    nlohmann::json lspDiags = nlohmann::json::array();
    for (const auto& d : diagnostics) {
        // Vora uses 1-based line/column; LSP uses 0-based.
        int lspLine = (d.line > 0) ? d.line - 1 : 0;
        int lspCol  = (d.column > 0) ? d.column - 1 : 0;

        int severity = 1;  // Error by default
        switch (d.severity) {
            case Severity::Error:   severity = 1; break;
            case Severity::Warning: severity = 2; break;
            case Severity::Hint:    severity = 3; break;
        }

        nlohmann::json lspDiag;
        lspDiag["range"]["start"]["line"] = lspLine;
        lspDiag["range"]["start"]["character"] = lspCol;
        lspDiag["range"]["end"]["line"] = lspLine;
        lspDiag["range"]["end"]["character"] = lspCol + d.length;
        lspDiag["severity"] = severity;
        lspDiag["source"] = "vora";
        lspDiag["message"] = d.message;

        lspDiags.push_back(std::move(lspDiag));
    }

    // ── Publish ──────────────────────────────────────────────────────────
    nlohmann::json params;
    params["uri"] = uri;
    params["diagnostics"] = std::move(lspDiags);

    sendNotification("textDocument/publishDiagnostics", params);
}

// ═══════════════════════════════════════════════════════════════════════════
// Cache management
// ═══════════════════════════════════════════════════════════════════════════

void LspServer::parseAndAnalyze(const std::string& uri) {
    auto* doc = getDocument(uri);
    if (!doc) return;

    if (doc->cacheValid) return;

    DiagnosticCollector collector;
    Lexer lexer(doc->text, collector);
    auto tokens = lexer.scanTokens();

    if (lexer.hasError()) {
        // Can't analyze — parse would produce garbage.
        doc->cacheValid = false;
        return;
    }

    Parser parser(std::move(tokens), collector);
    parser.setSource(doc->text);
    auto program = parser.parse();

    if (!program) {
        doc->cacheValid = false;
        return;
    }

    // Run semantic analysis.
    auto analyzer = std::make_unique<vora::SemanticAnalyzer>();
    analyzer->analyze(*program);

    doc->cachedProgram = std::move(program);
    doc->cachedAnalyzer = std::move(analyzer);
    doc->cacheValid = true;
}

void LspServer::invalidateCache(const std::string& uri) {
    auto* doc = getDocument(uri);
    if (doc) {
        doc->cacheValid = false;
        doc->cachedProgram.reset();
        doc->cachedAnalyzer.reset();
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Cross-file resolution
// ═══════════════════════════════════════════════════════════════════════════

std::string LspServer::resolveImportPath(const std::string& currentUri,
                                          const std::string& importPath) {
    // Strip file:// prefix.
    std::string currentPath = currentUri;
    if (currentPath.rfind("file:///", 0) == 0) {
        currentPath = currentPath.substr(8);  // "file:///" on Windows = 8 chars
    } else if (currentPath.rfind("file://", 0) == 0) {
        currentPath = currentPath.substr(7);
    }

    // Resolve relative paths.
    std::string dir;
    auto slashPos = currentPath.find_last_of("/\\");
    if (slashPos != std::string::npos) {
        dir = currentPath.substr(0, slashPos);
    }

    std::string resolved;

    if (importPath.rfind("./", 0) == 0 || importPath.rfind("../", 0) == 0) {
        // Relative path — resolve against the current file's directory.
        resolved = dir + "/" + importPath;
    } else {
        // Bare path — search workspace root first, then std/.
        if (!workspaceRoot_.empty()) {
            resolved = workspaceRoot_ + "/" + importPath;
        } else {
            resolved = dir + "/" + importPath;
        }
    }

    // Normalize: replace backslashes with forward slashes.
    for (auto& c : resolved) {
        if (c == '\\') c = '/';
    }

    // Append .va if no extension.
    if (resolved.size() < 3 || resolved.rfind(".va") != resolved.size() - 3) {
        resolved += ".va";
    }

    // Remove duplicate slashes (simple approach).
    std::string normalized;
    for (size_t i = 0; i < resolved.size(); i++) {
        if (resolved[i] == '/' && i + 1 < resolved.size() && resolved[i + 1] == '/') {
            continue;
        }
        normalized += resolved[i];
    }

    return normalized;
}

vora::SemanticAnalyzer* LspServer::analyzeImportedFile(const std::string& filePath) {
    // Check cache.
    auto cacheIt = importedAnalyzers_.find(filePath);
    if (cacheIt != importedAnalyzers_.end()) {
        return cacheIt->second.get();
    }

    // Read file from disk.
    std::ifstream file(filePath);
    if (!file.is_open()) {
        // Try std/ directory adjacent to the workspace.
        // Extract basename from filePath (which may be an absolute path).
        auto lastSlash = filePath.find_last_of("/\\");
        std::string fileName = (lastSlash != std::string::npos)
            ? filePath.substr(lastSlash + 1) : filePath;
        std::string altPath = workspaceRoot_ + "/std/" + fileName;
        file.open(altPath);
        if (!file.is_open()) return nullptr;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string source = buffer.str();

    // Lex + parse.
    DiagnosticCollector collector;
    Lexer lexer(source, collector);
    auto tokens = lexer.scanTokens();
    if (lexer.hasError()) return nullptr;

    Parser parser(std::move(tokens), collector);
    parser.setSource(source);
    auto program = parser.parse();
    if (!program) return nullptr;

    // Analyze.
    auto analyzer = std::make_unique<vora::SemanticAnalyzer>();
    analyzer->analyze(*program);

    auto* result = analyzer.get();
    importedPrograms_[filePath] = std::move(program);
    importedAnalyzers_[filePath] = std::move(analyzer);
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// References
// ═══════════════════════════════════════════════════════════════════════════

nlohmann::json LspServer::handleReferences(const nlohmann::json& params) {
    auto td = params["textDocument"];
    std::string uri = td["uri"].get<std::string>();
    auto* doc = getDocument(uri);
    if (!doc) return nlohmann::json::array();

    parseAndAnalyze(uri);
    if (!doc->cacheValid || !doc->cachedAnalyzer) return nlohmann::json::array();

    int line = params["position"]["line"].get<int>();
    int character = params["position"]["character"].get<int>();

    // Convert LSP 0-based to Vora 1-based.
    int voraLine = line + 1;
    int voraCol = character + 1;

    auto refs = doc->cachedAnalyzer->findReferencesTo(voraLine, voraCol);

    nlohmann::json locations = nlohmann::json::array();
    for (auto& ref : refs) {
        nlohmann::json loc;
        loc["uri"] = uri;
        loc["range"] = tokenToLspRange(ref.token);
        locations.push_back(std::move(loc));
    }

    // Also search in importing files (cross-file references).
    // For exported symbols, check all open documents that import this module.
    // (Simplified: only same-file references for now; cross-file is a future
    // enhancement requiring import graph traversal.)

    return locations;
}

// ═══════════════════════════════════════════════════════════════════════════
// Signature help
// ═══════════════════════════════════════════════════════════════════════════

nlohmann::json LspServer::handleSignatureHelp(const nlohmann::json& params) {
    auto td = params["textDocument"];
    std::string uri = td["uri"].get<std::string>();
    auto* doc = getDocument(uri);
    if (!doc) return nullptr;

    parseAndAnalyze(uri);
    if (!doc->cacheValid || !doc->cachedAnalyzer) return nullptr;

    int line = params["position"]["line"].get<int>();
    int character = params["position"]["character"].get<int>();

    int voraLine = line + 1;
    int voraCol = character + 1;

    // Strategy: find the innermost CallExpr that contains the cursor position.
    // Since we don't have AST node ranges, we use a text-based approach:
    // walk backward from the cursor to find the opening '(' of a call.
    int offset = lspPositionToOffset(doc->text, line, character);

    // Walk backward to find the function name.
    int parenNesting = 0;
    int callStart = -1;
    for (int i = offset; i >= 0; i--) {
        char c = doc->text[i];
        if (c == ')') parenNesting++;
        else if (c == '(') {
            if (parenNesting == 0) {
                callStart = i;
                break;
            }
            parenNesting--;
        }
    }

    if (callStart < 0) return nullptr;

    // Find the callee name before the '('.
    int calleeEnd = callStart;
    while (calleeEnd > 0 && doc->text[calleeEnd - 1] == ' ') calleeEnd--;
    int calleeStart = calleeEnd;
    while (calleeStart > 0 && (std::isalnum(static_cast<unsigned char>(doc->text[calleeStart - 1]))
                               || doc->text[calleeStart - 1] == '_'
                               || doc->text[calleeStart - 1] == '$')) {
        calleeStart--;
    }
    std::string calleeName = doc->text.substr(calleeStart, calleeEnd - calleeStart);

    // Count commas between callStart and offset to determine active parameter.
    int activeParam = 0;
    int parenDepth = 0;
    for (int i = callStart + 1; i < offset && i < static_cast<int>(doc->text.size()); i++) {
        char c = doc->text[i];
        if (c == '(' || c == '[' || c == '{') parenDepth++;
        else if (c == ')' || c == ']' || c == '}') parenDepth--;
        else if (c == ',' && parenDepth == 0) activeParam++;
    }

    // Find the callee declaration.
    const auto* symbol = doc->cachedAnalyzer->findSymbolAt(voraLine, voraCol);
    if (!symbol && !calleeName.empty()) {
        // Try resolving by name.
        // Guard rfind against npos (calleeStart == 0 or no preceding newline).
        auto rfindResult = doc->text.rfind('\n', calleeStart > 0 ? static_cast<size_t>(calleeStart - 1) : 0);
        int calleeCol = calleeStart - static_cast<int>(rfindResult == std::string::npos ? 0 : rfindResult);
        symbol = doc->cachedAnalyzer->findDeclarationAt(
            static_cast<int>(std::count(doc->text.begin(), doc->text.begin() + calleeStart, '\n')) + 1,
            calleeCol);
        if (!symbol) {
            // Try the analyzer's table directly.
            const auto& allSyms = doc->cachedAnalyzer->getVisibleSymbols(voraLine, voraCol);
            for (auto* s : allSyms) {
                if (s->name == calleeName &&
                    (s->kind == SymbolKind::Function || s->kind == SymbolKind::Method ||
                     s->kind == SymbolKind::Object)) {
                    symbol = s;
                    break;
                }
            }
        }
    }

    if (!symbol) return nullptr;

    // Build signature information.
    nlohmann::json sigInfo;
    std::string label = symbol->name + "(";
    nlohmann::json params_array = nlohmann::json::array();

    for (size_t pi = 0; pi < symbol->paramNames.size(); pi++) {
        if (pi > 0) label += ", ";
        label += symbol->paramNames[pi];

        nlohmann::json paramInfo;
        paramInfo["label"] = symbol->paramNames[pi];
        params_array.push_back(std::move(paramInfo));
    }
    if (symbol->hasRestParam && !symbol->paramNames.empty()) {
        label += "...";
    }
    label += ")";

    sigInfo["label"] = label;
    if (!params_array.empty()) {
        sigInfo["parameters"] = std::move(params_array);
    }

    nlohmann::json result;
    result["signatures"] = nlohmann::json::array({std::move(sigInfo)});
    result["activeSignature"] = 0;
    result["activeParameter"] = activeParam;

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// Helpers
// ═══════════════════════════════════════════════════════════════════════════

void LspServer::log(const std::string& message) {
    transport_.log("[Vora LSP] " + message);
}

void LspServer::sendNotification(const std::string& method,
                                  const nlohmann::json& params) {
    transport_.sendMessage(buildNotification(method, params));
}

DocumentState* LspServer::getDocument(const std::string& uri) {
    auto it = documents_.find(uri);
    return (it != documents_.end()) ? &it->second : nullptr;
}

nlohmann::json LspServer::tokenToLspRange(const Token& token) {
    nlohmann::json range;
    // Token line/column are 1-based; LSP uses 0-based.
    int lspLine = (token.line > 0) ? token.line - 1 : 0;
    int lspCol  = (token.column > 0) ? token.column - 1 : 0;
    range["start"]["line"] = lspLine;
    range["start"]["character"] = lspCol;
    range["end"]["line"] = lspLine;
    range["end"]["character"] = lspCol + static_cast<int>(token.lexeme.size());
    return range;
}

int LspServer::lspPositionToOffset(const std::string& text, int line, int character) {
    // Defensive: negative positions or empty text → clamp to start.
    if (text.empty() || line < 0 || character < 0) {
        return 0;
    }
    // Defensive: if line is beyond document bounds, clamp to end of text.
    int totalLines = static_cast<int>(std::count(text.begin(), text.end(), '\n'));
    if (line > totalLines) {
        return static_cast<int>(text.size());
    }
    int currentLine = 0;
    int currentChar = 0;
    for (size_t i = 0; i < text.size(); i++) {
        if (currentLine == line && currentChar == character) {
            return static_cast<int>(i);
        }
        if (text[i] == '\n') {
            currentLine++;
            currentChar = 0;
        } else {
            // Skip UTF-8 continuation bytes (10xxxxxx) — they don't
            // advance the LSP character position. Without this, multi-byte
            // characters (Unicode, Chinese, emoji, etc.) cause the offset
            // to drift, producing garbled substrings that fail JSON
            // serialization with "invalid UTF-8 byte".
            if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
                currentChar++;
            }
        }
    }
    // Cursor at end of text or beyond — return last valid offset.
    return static_cast<int>(text.size());
}

nlohmann::json LspServer::offsetToLspPosition(const std::string& text, int offset) {
    int line = 0;
    int character = 0;
    for (int i = 0; i < offset && i < static_cast<int>(text.size()); i++) {
        if (text[i] == '\n') {
            line++;
            character = 0;
        } else if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
            // Count only UTF-8 start bytes (not continuation bytes) as characters.
            character++;
        }
    }
    nlohmann::json pos;
    pos["line"] = line;
    pos["character"] = character;
    return pos;
}

// ── Symbol collection (Document Symbols) ────────────────────────────────────

void LspServer::collectSymbols(const Stmt* stmt, nlohmann::json& symbols) {
    if (!stmt) return;

    // ── FuncStmt / exported func ─────────────────────────────────────────
    if (auto* func = dynamic_cast<const FuncStmt*>(stmt)) {
        nlohmann::json sym;
        sym["name"] = func->name;
        sym["kind"] = 12;  // Function
        sym["range"] = tokenToLspRange(func->nameToken);
        sym["selectionRange"] = tokenToLspRange(func->nameToken);
        // Build detail: signature
        std::string detail = "func " + func->name + "(";
        for (size_t i = 0; i < func->params.size(); i++) {
            if (i > 0) detail += ", ";
            if (func->params[i].isRest) detail += "...";
            detail += func->params[i].name;
        }
        detail += ")";
        sym["detail"] = detail;
        symbols.push_back(std::move(sym));
        // Recurse into body for nested functions
        if (func->body) {
            for (auto& s : func->body->statements) {
                collectSymbols(s.get(), symbols);
            }
        }
        return;
    }

    // ── ObjStmt / exported Obj ───────────────────────────────────────────
    if (auto* obj = dynamic_cast<const ObjStmt*>(stmt)) {
        nlohmann::json sym;
        sym["name"] = obj->name;
        sym["kind"] = 5;  // Class
        sym["range"] = tokenToLspRange(obj->nameToken);
        sym["selectionRange"] = tokenToLspRange(obj->nameToken);
        std::string detail = "Obj " + obj->name;
        if (!obj->parentNames.empty()) {
            detail += " : ";
            for (size_t i = 0; i < obj->parentNames.size(); i++) {
                if (i > 0) detail += ", ";
                detail += obj->parentNames[i];
            }
        }
        sym["detail"] = detail;

        // Collect methods as children
        nlohmann::json children = nlohmann::json::array();
        for (auto& m : obj->methods) {
            if (auto* mf = dynamic_cast<const FuncStmt*>(m.get())) {
                nlohmann::json child;
                child["name"] = mf->name;
                child["kind"] = 6;  // Method
                child["range"] = tokenToLspRange(mf->nameToken);
                child["selectionRange"] = tokenToLspRange(mf->nameToken);
                child["detail"] = "method " + mf->name;
                children.push_back(std::move(child));
            }
        }
        if (!children.empty()) {
            sym["children"] = std::move(children);
        }
        symbols.push_back(std::move(sym));
        return;
    }

    // ── LetStmt / const ──────────────────────────────────────────────────
    if (auto* let = dynamic_cast<const LetStmt*>(stmt)) {
        nlohmann::json sym;
        sym["name"] = let->name;
        sym["kind"] = let->isConst ? 14 : 13;  // Constant or Variable
        sym["range"] = tokenToLspRange(let->nameToken);
        sym["selectionRange"] = tokenToLspRange(let->nameToken);
        std::string detail = let->isConst ? "const " : "let ";
        detail += let->name;
        if (!let->typeAnnotation.empty()) detail += ": " + let->typeAnnotation;
        sym["detail"] = detail;
        symbols.push_back(std::move(sym));
        return;
    }

    // ── ExportStmt — unwrap and delegate ─────────────────────────────────
    if (auto* exp = dynamic_cast<const ExportStmt*>(stmt)) {
        if (exp->declaration) {
            collectSymbols(exp->declaration.get(), symbols);
        }
        return;
    }

    // ── ImportStmt ───────────────────────────────────────────────────────
    if (auto* imp = dynamic_cast<const ImportStmt*>(stmt)) {
        nlohmann::json sym;
        // Use alias if available, else derive name from module path
        std::string displayName = imp->alias.empty()
            ? imp->modulePath
            : imp->alias;
        sym["name"] = displayName;
        sym["kind"] = 17;  // Module
        sym["range"] = tokenToLspRange(imp->keyword);
        sym["selectionRange"] = tokenToLspRange(imp->keyword);
        std::string detail = "import \"" + imp->modulePath + "\"";
        if (!imp->alias.empty()) detail += " as " + imp->alias;
        sym["detail"] = detail;
        symbols.push_back(std::move(sym));
        return;
    }

    // ── Recurse into block-like containers ───────────────────────────────
    if (auto* block = dynamic_cast<const BlockStmt*>(stmt)) {
        for (auto& s : block->statements) {
            collectSymbols(s.get(), symbols);
        }
        return;
    }
    if (auto* ifStmt = dynamic_cast<const IfStmt*>(stmt)) {
        if (ifStmt->thenBranch) collectSymbols(ifStmt->thenBranch.get(), symbols);
        if (ifStmt->elseBranch) collectSymbols(ifStmt->elseBranch.get(), symbols);
        return;
    }
    if (auto* whileStmt = dynamic_cast<const WhileStmt*>(stmt)) {
        if (whileStmt->body) collectSymbols(whileStmt->body.get(), symbols);
        return;
    }
    if (auto* forStmt = dynamic_cast<const ForStmt*>(stmt)) {
        if (forStmt->body) collectSymbols(forStmt->body.get(), symbols);
        return;
    }
    if (auto* cforStmt = dynamic_cast<const CForStmt*>(stmt)) {
        if (cforStmt->body) collectSymbols(cforStmt->body.get(), symbols);
        return;
    }
    if (auto* tryStmt = dynamic_cast<const TryStmt*>(stmt)) {
        if (tryStmt->tryBlock) collectSymbols(tryStmt->tryBlock.get(), symbols);
        if (tryStmt->catchBlock) collectSymbols(tryStmt->catchBlock.get(), symbols);
        if (tryStmt->finallyBlock) collectSymbols(tryStmt->finallyBlock.get(), symbols);
        return;
    }
}

void LspServer::collectSymbolsFromProgram(const Program& program, nlohmann::json& symbols) {
    for (auto& stmt : program.statements) {
        collectSymbols(stmt.get(), symbols);
    }
}

// ── Completion Data ─────────────────────────────────────────────────────────

nlohmann::json LspServer::getKeywordCompletions() const {
    static const std::vector<std::pair<const char*, const char*>> keywords = {
        {"let", "Declare a mutable variable"},
        {"const", "Declare an immutable constant"},
        {"func", "Declare a function"},
        {"return", "Return from a function"},
        {"if", "Conditional branch"},
        {"else", "Alternative branch"},
        {"while", "While loop"},
        {"for", "For-in or C-style for loop"},
        {"in", "For-in loop iterator"},
        {"break", "Exit loop"},
        {"continue", "Skip to next iteration"},
        {"Obj", "Declare a class-like object"},
        {"this", "Current object instance"},
        {"super", "Parent class reference"},
        {"try", "Begin exception handling block"},
        {"catch", "Catch exception"},
        {"finally", "Finally cleanup block"},
        {"throw", "Throw an exception"},
        {"import", "Import a module"},
        {"export", "Export a declaration"},
        {"from", "Selective import"},
        {"as", "Import alias"},
        {"yield", "Yield from generator"},
        {"true", "Boolean true"},
        {"false", "Boolean false"},
        {"null", "Null value"},
    };

    nlohmann::json items = nlohmann::json::array();
    for (auto& [word, desc] : keywords) {
        nlohmann::json item;
        item["label"] = word;
        item["kind"] = 14;  // Keyword
        item["detail"] = desc;
        item["insertText"] = word;
        items.push_back(std::move(item));
    }
    return items;
}

nlohmann::json LspServer::getBuiltinCompletions() const {
    static const std::vector<std::pair<const char*, const char*>> builtins = {
        {"print", "Print values to stdout"},
        {"type", "Get type name of a value"},
        {"len", "Get length of array/string/dict"},
        {"clock", "Current Unix timestamp"},
        {"assert", "Throw if condition is falsy"},
        {"int", "Convert to integer"},
        {"float", "Convert to float"},
        {"toString", "Convert to string"},
        {"input", "Read a line from stdin"},
        {"range", "Generate a range of numbers"},
        {"bin", "Integer to binary string"},
        {"oct", "Integer to octal string"},
        {"hex", "Integer to hex string"},
        {"iter", "Create an iterator"},
        {"next", "Advance an iterator"},
        {"abs", "Absolute value"},
        {"sqrt", "Square root"},
        {"sin", "Sine (radians)"},
        {"cos", "Cosine (radians)"},
        {"min", "Minimum of array"},
        {"max", "Maximum of array"},
        {"random_int", "Random integer in range"},
        {"random_float", "Random float in range"},
        {"jsonParse", "Parse JSON string"},
        {"jsonStringify", "Serialize to JSON string"},
    };

    nlohmann::json items = nlohmann::json::array();
    for (auto& [word, desc] : builtins) {
        nlohmann::json item;
        item["label"] = word;
        item["kind"] = 3;  // Function
        item["detail"] = desc;
        item["insertText"] = word;
        items.push_back(std::move(item));
    }
    return items;
}

// ── Expression-level keyword completions (values, not declarations) ──────────

nlohmann::json LspServer::getExpressionKeywords() const {
    static const std::vector<std::pair<const char*, const char*>> exprKeywords = {
        {"true", "Boolean true"},
        {"false", "Boolean false"},
        {"null", "Null value"},
        {"this", "Current object instance"},
        {"super", "Parent class reference"},
    };

    nlohmann::json items = nlohmann::json::array();
    for (auto& [word, desc] : exprKeywords) {
        nlohmann::json item;
        item["label"] = word;
        item["kind"] = 14;  // Keyword
        item["detail"] = desc;
        item["insertText"] = word;
        items.push_back(std::move(item));
    }
    return items;
}

// ── Type-specific method completions ─────────────────────────────────────────

nlohmann::json LspServer::getTypeMethods(const std::string& typeName) const {
    // Method definitions grouped by runtime type name.
    // Keys match the strings returned by Vora's type() builtin.
    struct MethodDef { const char* name; const char* desc; };
    static const std::unordered_map<std::string, std::vector<MethodDef>> typeMethods = {
        {"string", {
            {"substring", "String method: extract substring"},
            {"include", "String method: check if contains"},
            {"startsWith", "String method: check prefix"},
            {"endsWith", "String method: check suffix"},
            {"upper", "String method: to uppercase"},
            {"lower", "String method: to lowercase"},
            {"trim", "String method: trim whitespace"},
            {"replace", "String method: replace first occurrence"},
            {"replaceAll", "String method: replace all occurrences"},
            {"split", "String method: split by delimiter"},
        }},
        {"array", {
            {"add", "Array method: add element"},
            {"pop", "Array method: remove last element"},
            {"insert", "Array method: insert at index"},
            {"remove", "Array method: remove at index"},
            {"indexOf", "Array method: find index of value"},
            {"clear", "Array method: remove all elements"},
            {"length", "Array length property"},
        }},
        {"dict", {
            {"keys", "Dict method: get all keys"},
            {"values", "Dict method: get all values"},
            {"has", "Dict method: check if key exists"},
        }},
        {"set", {
            {"add", "Set method: add element"},
            {"has", "Set method: check if element exists"},
            {"delete", "Set method: remove element"},
            {"clear", "Set method: remove all elements"},
            {"values", "Set method: get all values as array"},
            {"size", "Set property: number of elements"},
        }},
        {"map", {
            {"set", "Map method: set key-value pair"},
            {"get", "Map method: get value by key"},
            {"has", "Map method: check if key exists"},
            {"delete", "Map method: remove key-value pair"},
            {"clear", "Map method: remove all entries"},
            {"keys", "Map method: get all keys"},
            {"values", "Map method: get all values"},
            {"size", "Map property: number of entries"},
        }},
    };

    nlohmann::json items = nlohmann::json::array();
    auto it = typeMethods.find(typeName);
    if (it == typeMethods.end()) return items;

    for (auto& [name, desc] : it->second) {
        nlohmann::json item;
        item["label"] = name;
        item["kind"] = 2;  // Method
        item["detail"] = desc;
        item["insertText"] = name;
        item["sortText"] = "0_" + std::string(name);  // highest priority
        items.push_back(std::move(item));
    }
    return items;
}

nlohmann::json LspServer::getAllMethods() const {
    // Fallback: return all methods for all types (current behavior).
    nlohmann::json items = nlohmann::json::array();
    static const std::vector<std::string> allTypes = {"string", "array", "dict", "set", "map"};
    for (auto& t : allTypes) {
        auto typeItems = getTypeMethods(t);
        for (auto& item : typeItems) {
            items.push_back(std::move(item));
        }
    }
    return items;
}

// ── Context Detection ────────────────────────────────────────────────────────

CompletionContext LspServer::detectCompletionContext(const std::string& text,
                                                      int offset) const {
    if (offset <= 0) return CompletionContext::GENERAL;

    // ── Property access: cursor is right after '.' ────────────────────────
    if (text[offset - 1] == '.') {
        return CompletionContext::PROPERTY_ACCESS;
    }

    // ── String literal: cursor is inside "..." ────────────────────────────
    // Walk backward to see if we're inside an unclosed string.
    bool insideString = false;
    for (int i = offset - 1; i >= 0; i--) {
        char c = text[i];
        if (c == '"') {
            // Check if this quote is escaped.
            int backslashes = 0;
            int j = i - 1;
            while (j >= 0 && text[j] == '\\') { backslashes++; j--; }
            if (backslashes % 2 == 0) {
                insideString = true;
            }
            break;
        }
        if (c == '\n') break;
    }
    if (insideString) return CompletionContext::STRING_LITERAL;

    // ── Call argument: cursor is inside '(' ... ')' of a function call ────
    // Walk backward from cursor, tracking paren balance.
    int parenDepth = 0;
    for (int i = offset - 1; i >= 0; i--) {
        char c = text[i];
        if (c == ')') parenDepth++;
        else if (c == '(') {
            if (parenDepth == 0) {
                // Found opening paren. Check if it's a function call.
                int j = i - 1;
                while (j >= 0 && (text[j] == ' ' || text[j] == '\t')) j--;
                if (j >= 0 && (std::isalnum(static_cast<unsigned char>(text[j]))
                               || text[j] == '_' || text[j] == '$'
                               || text[j] == ')' || text[j] == ']')) {
                    return CompletionContext::CALL_ARGUMENT;
                }
                break;
            }
            parenDepth--;
        }
        else if (c == '\n' || c == '{' || c == '}' || c == ';') {
            // Statement boundary — not inside a call on this line.
            break;
        }
    }

    return CompletionContext::GENERAL;
}

std::string LspServer::detectPrecedingType(const std::string& text, int dotOffset,
                                            vora::SemanticAnalyzer* analyzer) const {
    // dotOffset points to the '.' character (offset - 1 from cursor).
    // Scan backward to find the start of the expression before '.'.

    if (dotOffset <= 0) return "";

    int end = dotOffset;
    // Skip whitespace between identifier and dot (should be none, but be safe).
    while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\t')) end--;
    if (end <= 0) return "";

    int start = end;

    // ── 1. Detect string literal: "..." . ─────────────────────────────────
    if (text[start - 1] == '"') {
        // Walk backward to verify it's a complete string literal.
        int i = start - 2;
        bool closed = false;
        while (i >= 0) {
            if (text[i] == '"') {
                int backslashes = 0;
                int j = i - 1;
                while (j >= 0 && text[j] == '\\') { backslashes++; j--; }
                if (backslashes % 2 == 0) {
                    closed = true;
                    break;
                }
            }
            if (text[i] == '\n') break;
            i--;
        }
        if (closed) return "string";
    }

    // ── 2. Detect array literal: [...] . ──────────────────────────────────
    if (text[start - 1] == ']') {
        return "array";
    }

    // ── 3. Detect dict literal: {...} . ───────────────────────────────────
    if (text[start - 1] == '}') {
        return "dict";
    }

    // ── 4. Extract preceding identifier ───────────────────────────────────
    if (!std::isalnum(static_cast<unsigned char>(text[start - 1]))
        && text[start - 1] != '_' && text[start - 1] != '$') {
        return "";  // not an identifier
    }

    while (start > 0 && (std::isalnum(static_cast<unsigned char>(text[start - 1]))
                         || text[start - 1] == '_' || text[start - 1] == '$')) {
        start--;
    }

    std::string ident = text.substr(start, end - start);
    if (ident.empty()) return "";

    // ── 5. Look up typeHint from semantic analyzer ────────────────────────
    if (analyzer) {
        const auto& allSyms = analyzer->getVisibleSymbols(1, 0);  // get all symbols
        // Actually, better to search the full symbol table directly.
        // The analyzer has access to the symbol table.
        // We'll use findSymbolAt or a direct lookup.
        // For now, iterate visible symbols to find matching name.
        for (auto* sym : allSyms) {
            if (sym->name == ident && !sym->typeHint.empty()) {
                // Normalize typeHint: Vora's type() returns lowercase.
                std::string hint = sym->typeHint;
                // Map common type annotations to runtime type names.
                if (hint == "string" || hint == "str") return "string";
                if (hint == "array" || hint == "Array" || hint == "list") return "array";
                if (hint == "dict" || hint == "Dict" || hint == "object") return "dict";
                if (hint == "set" || hint == "Set") return "set";
                if (hint == "map" || hint == "Map") return "map";
                return hint;  // return as-is for other types
            }
        }
    }

    return "";  // unknown type → fall back to all methods
}

// ── Go-to-Definition ────────────────────────────────────────────────────────

}  // namespace vora::lsp
