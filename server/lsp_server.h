/**
 * Vora Language Server Protocol (LSP) server.
 *
 * Implements the LSP specification on top of JSON-RPC 2.0 over stdio.
 * Handles the full lifecycle (initialize → running → shutdown → exit),
 * document synchronization, and language features.
 *
 * Dependencies (from the main Vora project):
 *   json_rpc/json_rpc.h, json_rpc/transport.h, json_rpc/message_router.h,
 *   common/error_reporter.h, lexer/lexer.h, parser/parser.h,
 *   formatter/formatter.h
 *
 * Roadmap #3.1 — LSP server implementation.
 */

#ifndef VORA_LSP_SERVER_H
#define VORA_LSP_SERVER_H

#include "json_rpc/json_rpc.h"
#include "json_rpc/message_router.h"
#include "json_rpc/transport.h"
#include "common/error_reporter.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace vora::lsp {

/// Per-document state tracked by the LSP server.
struct DocumentState {
    std::string uri;
    std::string languageId = "vora";
    int version = 0;
    std::string text;
    bool open = false;
};

/// Diagnostic collector — implements ErrorReporter, stores diagnostics
/// for later publishing via textDocument/publishDiagnostics.
class DiagnosticCollector : public ErrorReporter {
public:
    void report(int line, int column, int length,
                const std::string& message, Severity severity) override;
    bool hadError() const override { return !diagnostics_.empty(); }

    /// Move out the collected diagnostics and reset for the next parse.
    std::vector<Diagnostic> takeDiagnostics();

private:
    std::vector<Diagnostic> diagnostics_;
};

/// Vora LSP server — the full language server implementation.
///
/// Lifecycle:
///   LspServer server;
///   server.run();  // enters the main loop, blocks until shutdown+exit
///
/// Protocol flow:
///   initialize → initialized → didOpen/didChange/... → shutdown → exit
class LspServer {
public:
    LspServer();
    ~LspServer();

    // Non-copyable, non-movable.
    LspServer(const LspServer&) = delete;
    LspServer& operator=(const LspServer&) = delete;
    LspServer(LspServer&&) = delete;
    LspServer& operator=(LspServer&&) = delete;

    /// Enter the main loop — reads messages from stdin, dispatches to
    /// registered handlers, and writes responses to stdout.  Blocks until
    /// the client sends an "exit" notification (after "shutdown").
    void run();

private:
    // ── Transport ───────────────────────────────────────────────────────
    StdioTransport transport_;
    MessageRouter router_;

    // ── Lifecycle state ─────────────────────────────────────────────────
    bool initialized_ = false;
    bool shutdown_ = false;

    // ── Server info ─────────────────────────────────────────────────────
    std::string serverName_ = "Vora LSP";
    std::string serverVersion_ = "0.1.0";

    // ── Document store ──────────────────────────────────────────────────
    std::unordered_map<std::string, DocumentState> documents_;

    // ── Handlers ────────────────────────────────────────────────────────

    // Lifecycle
    nlohmann::json handleInitialize(const nlohmann::json& params);
    void handleInitialized(const nlohmann::json& params);
    nlohmann::json handleShutdown(const nlohmann::json& params);
    void handleExit(const nlohmann::json& params);

    // Document synchronization
    void handleDidOpen(const nlohmann::json& params);
    void handleDidChange(const nlohmann::json& params);
    void handleDidClose(const nlohmann::json& params);

    // Language features
    nlohmann::json handleCompletion(const nlohmann::json& params);
    nlohmann::json handleDefinition(const nlohmann::json& params);
    nlohmann::json handleHover(const nlohmann::json& params);
    nlohmann::json handleFormatting(const nlohmann::json& params);
    nlohmann::json handleDocumentSymbol(const nlohmann::json& params);

    // ── Diagnostics ─────────────────────────────────────────────────────
    void publishDiagnostics(const std::string& uri);

    // ── Helpers ─────────────────────────────────────────────────────────
    void log(const std::string& message);
    DocumentState* getDocument(const std::string& uri);
    nlohmann::json positionToLsp(int line, int column);
    int lspPositionToOffset(const std::string& text, int line, int character);
};

}  // namespace vora::lsp

#endif  // VORA_LSP_SERVER_H
