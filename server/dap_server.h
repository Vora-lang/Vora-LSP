/**
 * Vora Debug Adapter Protocol (DAP) server.
 *
 * Implements the DAP specification on top of JSON-RPC 2.0 over stdio.
 * Reuses the same JSON-RPC infrastructure (StdioTransport, MessageRouter)
 * as the LSP server — DAP is also JSON-RPC 2.0 with Content-Length framing.
 */
#ifndef VORA_DAP_SERVER_H
#define VORA_DAP_SERVER_H

#include "json_rpc/json_rpc.h"
#include "json_rpc/message_router.h"
#include "json_rpc/transport.h"
#include "vm/vm.h"
#include "vm/compiler.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace vora::dap {

/// DAP server — debug adapter speaking the VS Code Debug Adapter Protocol.
///
/// Lifecycle:
///   DapServer server;
///   server.run();  // enters the main loop, blocks until disconnect
class DapServer {
public:
    DapServer();
    ~DapServer() = default;

    DapServer(const DapServer&) = delete;
    DapServer& operator=(const DapServer&) = delete;

    void run();

private:
    vora::lsp::StdioTransport transport_;
    vora::lsp::MessageRouter router_;

    // VM state
    vora::VM vm_;
    std::unique_ptr<vora::Chunk> chunk_;
    bool running_ = false;
    int seq_ = 0;  // DAP sequence counter

    // Breakpoint storage
    struct BreakpointInfo {
        int line;
        std::vector<size_t> offsets;  // bytecode offsets for this line
        bool verified = false;
    };
    std::string sourcePath_;
    std::string sourceText_;
    std::unordered_map<std::string, std::vector<BreakpointInfo>> breakpoints_;

    // ── Core execution ──────────────────────────────────────────────────
    void compileAndRun(const std::string& path);
    void resumeExecution();
    void applyBreakpoints();

    // ── DAP request handlers ────────────────────────────────────────────
    nlohmann::json handleInitialize(const nlohmann::json& params);
    nlohmann::json handleLaunch(const nlohmann::json& params);
    nlohmann::json handleSetBreakpoints(const nlohmann::json& params);
    nlohmann::json handleSetExceptionBreakpoints(const nlohmann::json& params);
    nlohmann::json handleThreads(const nlohmann::json& params);
    nlohmann::json handleStackTrace(const nlohmann::json& params);
    nlohmann::json handleScopes(const nlohmann::json& params);
    nlohmann::json handleVariables(const nlohmann::json& params);
    nlohmann::json handleContinue(const nlohmann::json& params);
    nlohmann::json handleNext(const nlohmann::json& params);
    nlohmann::json handleStepIn(const nlohmann::json& params);
    nlohmann::json handleStepOut(const nlohmann::json& params);
    nlohmann::json handlePause(const nlohmann::json& params);
    nlohmann::json handleEvaluate(const nlohmann::json& params);
    nlohmann::json handleDisconnect(const nlohmann::json& params);

    // ── Event emitters ──────────────────────────────────────────────────
    void sendEvent(const std::string& event, const nlohmann::json& body = {});
    void sendStoppedEvent(const std::string& reason);
    void sendOutputEvent(const std::string& output);
    void sendTerminatedEvent();

    // ── Helpers ─────────────────────────────────────────────────────────
    static int lineToOffset(const std::vector<int>& lines, int targetLine);
    int getCurrentLine() const;
    static std::string valueToDisplayString(const Value& v);
    static int valueToVariablesRef(const Value& v);
};

} // namespace vora::dap

#endif // VORA_DAP_SERVER_H
