/**
 * Vora LSP server implementation.
 *
 * Protocol handlers for the full LSP lifecycle + language features.
 * Diagnostics use the Vora lexer + error-tolerant parser to surface
 * parse errors. Formatting delegates to SourceFormatter.
 *
 * Completion, go-to-definition, hover, and document symbols return
 * empty/not-implemented responses — they require semantic analysis
 * (roadmap #4).
 */

#include "lsp_server.h"

#include "lexer/lexer.h"
#include "parser/parser.h"
#include "formatter/formatter.h"

#include <cstdlib>
#include <sstream>

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

    // Also register the shorthand forms that some clients send.
    router_.registerRequest("shutdown",
        [this](const nlohmann::json& p) { return handleShutdown(p); });

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

nlohmann::json LspServer::handleInitialize(const nlohmann::json& /*params*/) {
    initialized_ = true;

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
    // Use _Exit to avoid static destructors that might hang.
    std::_Exit(shutdown_ ? 0 : 1);
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
    transport_.sendMessage(buildNotification("textDocument/publishDiagnostics", diagParams));

    log("didClose: " + uri);
}

// ═══════════════════════════════════════════════════════════════════════════
// Language features
// ═══════════════════════════════════════════════════════════════════════════

nlohmann::json LspServer::handleCompletion(const nlohmann::json& /*params*/) {
    // Completion requires semantic analysis (roadmap #4).
    // Return empty list for now.
    nlohmann::json result;
    result["isIncomplete"] = false;
    result["items"] = nlohmann::json::array();
    return result;
}

nlohmann::json LspServer::handleDefinition(const nlohmann::json& /*params*/) {
    // Go-to-definition requires semantic analysis (roadmap #4).
    return nullptr;
}

nlohmann::json LspServer::handleHover(const nlohmann::json& /*params*/) {
    // Hover requires semantic analysis (roadmap #4).
    return nullptr;
}

nlohmann::json LspServer::handleFormatting(const nlohmann::json& params) {
    auto td = params["textDocument"];
    std::string uri = td["uri"].get<std::string>();

    auto* doc = getDocument(uri);
    if (!doc) {
        nlohmann::json result;
        result["error"] = "Document not found";
        return result;
    }

    // Lex → Parse → Format
    DiagnosticCollector collector;
    Lexer lexer(doc->text, collector);
    auto tokens = lexer.scanTokens();

    Parser parser(std::move(tokens), collector);
    parser.setSource(doc->text);
    auto program = parser.parse();

    if (!program || program->statements.empty()) {
        // Empty or completely broken — return original text unchanged.
        nlohmann::json result;
        result["error"] = "Could not parse document";
        return result;
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

nlohmann::json LspServer::handleDocumentSymbol(const nlohmann::json& /*params*/) {
    // Document symbols require semantic analysis (roadmap #4).
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
    if (!lexHadError) {
        // Only parse if lexing succeeded (avoids cascading parse errors
        // from garbage tokens).
        Parser parser(std::move(tokens), collector);
        parser.setSource(doc->text);
        parser.parse();  // result ignored — errors go to collector
    }

    auto parseDiags = collector.takeDiagnostics();

    // Merge lexer and parser diagnostics.
    diagnostics.insert(diagnostics.end(),
                       std::make_move_iterator(lexDiags.begin()),
                       std::make_move_iterator(lexDiags.end()));
    diagnostics.insert(diagnostics.end(),
                       std::make_move_iterator(parseDiags.begin()),
                       std::make_move_iterator(parseDiags.end()));

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

    transport_.sendMessage(buildNotification("textDocument/publishDiagnostics", params));
}

// ═══════════════════════════════════════════════════════════════════════════
// Helpers
// ═══════════════════════════════════════════════════════════════════════════

void LspServer::log(const std::string& message) {
    transport_.log("[Vora LSP] " + message);
}

DocumentState* LspServer::getDocument(const std::string& uri) {
    auto it = documents_.find(uri);
    return (it != documents_.end()) ? &it->second : nullptr;
}

nlohmann::json LspServer::positionToLsp(int /*line*/, int /*column*/) {
    // Stub — full UTF-16 position mapping is roadmap #5.
    return nlohmann::json::object();
}

int LspServer::lspPositionToOffset(const std::string& /*text*/,
                                    int /*line*/, int /*character*/) {
    // Stub — full UTF-16 position mapping is roadmap #5.
    return 0;
}

}  // namespace vora::lsp
