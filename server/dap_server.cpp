/**
 * Vora DAP server — implementation.
 */
#include "dap_server.h"

#include "lexer/lexer.h"
#include "parser/parser.h"
#include "common/error_reporter.h"
#include "gc/gc_heap.h"

#include <fstream>
#include <sstream>

namespace vora::dap {

// =========================================================================
// Construction
// =========================================================================

DapServer::DapServer() {
    using namespace std::placeholders;

    router_.registerRequest("initialize",   [this](auto& p) { return handleInitialize(p); });
    router_.registerRequest("launch",       [this](auto& p) { return handleLaunch(p); });
    router_.registerRequest("setBreakpoints", [this](auto& p) { return handleSetBreakpoints(p); });
    router_.registerRequest("setExceptionBreakpoints", [this](auto& p) { return handleSetExceptionBreakpoints(p); });
    router_.registerRequest("threads",      [this](auto& p) { return handleThreads(p); });
    router_.registerRequest("stackTrace",   [this](auto& p) { return handleStackTrace(p); });
    router_.registerRequest("scopes",       [this](auto& p) { return handleScopes(p); });
    router_.registerRequest("variables",    [this](auto& p) { return handleVariables(p); });
    router_.registerRequest("continue",     [this](auto& p) { return handleContinue(p); });
    router_.registerRequest("next",         [this](auto& p) { return handleNext(p); });
    router_.registerRequest("stepIn",       [this](auto& p) { return handleStepIn(p); });
    router_.registerRequest("stepOut",      [this](auto& p) { return handleStepOut(p); });
    router_.registerRequest("pause",        [this](auto& p) { return handlePause(p); });
    router_.registerRequest("evaluate",     [this](auto& p) { return handleEvaluate(p); });
    router_.registerRequest("disconnect",   [this](auto& p) { return handleDisconnect(p); });
}

// =========================================================================
// Main loop
// =========================================================================

void DapServer::run() {
    for (;;) {
        auto raw = transport_.readMessage();
        if (!raw.has_value()) break;
        auto response = router_.handleMessage(*raw);
        if (response.has_value()) {
            transport_.sendMessage(*response);
        }
    }
}

// =========================================================================
// Event emitters
// =========================================================================

void DapServer::sendEvent(const std::string& event, const nlohmann::json& body) {
    nlohmann::json msg;
    msg["type"] = "event";
    msg["seq"] = ++seq_;
    msg["event"] = event;
    if (!body.empty()) msg["body"] = body;
    transport_.sendMessage(msg.dump());
}

void DapServer::sendStoppedEvent(const std::string& reason) {
    nlohmann::json body;
    body["reason"] = reason;
    body["threadId"] = 0;
    body["allThreadsStopped"] = true;
    sendEvent("stopped", body);
}

void DapServer::sendOutputEvent(const std::string& output) {
    nlohmann::json body;
    body["category"] = "stdout";
    body["output"] = output + "\n";
    sendEvent("output", body);
}

void DapServer::sendTerminatedEvent() {
    sendEvent("terminated");
}

// =========================================================================
// Helpers
// =========================================================================

int DapServer::lineToOffset(const std::vector<int>& lines, int targetLine) {
    // RLE decode: find first bytecode offset at targetLine
    size_t pos = 0;
    for (size_t i = 0; i + 1 < lines.size(); i += 2) {
        int runLen = lines[i];
        int line = lines[i + 1];
        if (line == targetLine) return static_cast<int>(pos);
        pos += static_cast<size_t>(runLen);
    }
    return -1;
}

int DapServer::getCurrentLine() const {
    if (!vm_.currentChunk) return 1;
    size_t offset = vm_.debugCurrentOffset();
    if (offset >= vm_.currentChunk->code.size()) offset = 0;
    size_t pos = 0;
    for (size_t i = 0; i + 1 < vm_.currentChunk->lines.size(); i += 2) {
        int runLen = vm_.currentChunk->lines[i];
        if (offset < pos + static_cast<size_t>(runLen))
            return vm_.currentChunk->lines[i + 1];
        pos += static_cast<size_t>(runLen);
    }
    return 0;
}

std::string DapServer::valueToDisplayString(const Value& v) {
    if (v.isNull())   return "null";
    if (v.isBool())   return v.asBool() ? "true" : "false";
    if (v.isInt())    return std::to_string(v.asInt());
    if (v.isDouble()) return std::to_string(v.asDouble());
    if (v.isGcString()) return "\"" + v.asGcString()->value + "\"";
    if (v.isArray())  return "Array[" + std::to_string(v.asArray()->elements.size()) + "]";
    if (v.isDict())   return "Dict{" + std::to_string(v.asDict()->pairs.size()) + "}";
    if (v.isSet())    return "Set{" + std::to_string(v.asSet()->elements.size()) + "}";
    if (v.isMap())    return "Map{" + std::to_string(v.asMap()->pairs.size()) + "}";
    if (v.isCallable()) return "<function>";
    if (v.isObjectInstance()) return "<" + v.asObjectInstance()->className + " object>";
    return "<value>";
}

int DapServer::valueToVariablesRef(const Value& v) {
    if (v.isArray() || v.isDict() || v.isSet() || v.isMap() || v.isObjectInstance())
        return 1;  // expandable — handled by variables request
    return 0;
}

// =========================================================================
// Core execution
// =========================================================================

void DapServer::compileAndRun(const std::string& path) {
    sourcePath_ = path;

    // Read file
    std::ifstream file(path);
    if (!file.is_open()) {
        sendOutputEvent("Error: Cannot open file: " + path);
        sendTerminatedEvent();
        return;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    sourceText_ = ss.str();

    // Compile
    StderrErrorReporter reporter(sourceText_);
    Lexer lexer(sourceText_, reporter);
    auto tokens = lexer.scanTokens();
    Parser parser(std::move(tokens), reporter);
    auto prog = parser.parse();
    if (parser.hasError()) {
        sendOutputEvent("Error: Parse errors detected");
        sendTerminatedEvent();
        return;
    }

    Compiler compiler(reporter);
    compiler.seedGlobals(vm_.getGlobalNames());
    chunk_ = std::make_unique<Chunk>(compiler.compile(prog.get()));
    if (compiler.hadError) {
        sendOutputEvent("Error: Compilation errors detected");
        sendTerminatedEvent();
        return;
    }

    vm_.initGlobals(compiler.getGlobalNames());
    applyBreakpoints();
    running_ = true;
    resumeExecution();
}

void DapServer::applyBreakpoints() {
    vm_.debugClearBreakpoints();
    auto it = breakpoints_.find(sourcePath_);
    if (it == breakpoints_.end()) return;

    for (auto& bp : it->second) {
        bp.offsets.clear();
        bp.verified = false;
        if (vm_.currentChunk) {
            int offset = lineToOffset(vm_.currentChunk->lines, bp.line);
            if (offset >= 0) {
                bp.offsets.push_back(static_cast<size_t>(offset));
                bp.verified = true;
                vm_.debugSetBreakpoint(static_cast<size_t>(offset));
            }
        }
    }
}

void DapServer::resumeExecution() {
    if (!chunk_ || !running_) return;
    InterpretResult result = vm_.interpret(*chunk_);
    if (result == InterpretResult::DEBUG_STOPPED) {
        sendStoppedEvent("breakpoint");
    } else if (result == InterpretResult::OK) {
        running_ = false;
        sendTerminatedEvent();
    } else if (result == InterpretResult::COMPILE_ERROR) {
        running_ = false;
        sendOutputEvent("Error: Compilation failed");
        sendTerminatedEvent();
    } else {
        running_ = false;
        sendOutputEvent("Error: Runtime error");
        sendTerminatedEvent();
    }
}

// =========================================================================
// Protocol handlers
// =========================================================================

nlohmann::json DapServer::handleInitialize(const nlohmann::json& params) {
    nlohmann::json body;
    body["supportsConfigurationDoneRequest"] = false;
    body["supportsEvaluateForHovers"] = true;
    body["supportsStepInTargetsRequest"] = false;
    body["supportsConditionalBreakpoints"] = false;
    body["supportsHitConditionalBreakpoints"] = false;
    body["supportsLogPoints"] = false;
    body["supportsSetVariable"] = false;
    body["supportsFunctionBreakpoints"] = false;
    body["supportsExceptionFilterOptions"] = false;
    body["exceptionBreakpointFilters"] = nlohmann::json::array();

    sendEvent("initialized");
    return body;
}

nlohmann::json DapServer::handleLaunch(const nlohmann::json& params) {
    std::string program = params.value("program", "");
    if (program.empty()) {
        sendOutputEvent("Error: No program specified");
        sendTerminatedEvent();
        return nlohmann::json::object();
    }
    compileAndRun(program);
    return nlohmann::json::object();
}

nlohmann::json DapServer::handleSetBreakpoints(const nlohmann::json& params) {
    nlohmann::json body;
    body["breakpoints"] = nlohmann::json::array();

    std::string path;
    if (params.contains("source") && params["source"].is_object())
        path = params["source"].value("path", "");
    if (path.empty()) return body;

    auto& bps = breakpoints_[path];
    bps.clear();

    auto lines = params.value("breakpoints", nlohmann::json::array());
    for (const auto& bp : lines) {
        BreakpointInfo info;
        info.line = bp.value("line", 0);
        bps.push_back(info);
    }

    // If already running (hot reload), re-apply
    if (running_ && path == sourcePath_) {
        applyBreakpoints();
    }

    // Report back verified state
    for (const auto& bp : bps) {
        nlohmann::json out;
        out["verified"] = bp.verified;
        out["line"] = bp.line;
        if (!bp.verified) out["message"] = "No code at this line";
        body["breakpoints"].push_back(out);
    }

    return body;
}

nlohmann::json DapServer::handleSetExceptionBreakpoints(const nlohmann::json&) {
    return nlohmann::json::object();  // not supported yet
}

nlohmann::json DapServer::handleThreads(const nlohmann::json&) {
    nlohmann::json body;
    body["threads"] = nlohmann::json::array();
    nlohmann::json thread;
    thread["id"] = 0;
    thread["name"] = "main";
    body["threads"].push_back(thread);
    return body;
}

nlohmann::json DapServer::handleStackTrace(const nlohmann::json& params) {
    nlohmann::json body;
    body["stackFrames"] = nlohmann::json::array();

    int startFrame = params.value("startFrame", 0);
    int levels = params.value("levels", 20);
    int totalFrames = vm_.debugFrameCount();

    int endFrame = std::min(startFrame + levels, totalFrames);
    for (int i = startFrame; i < endFrame; i++) {
        nlohmann::json frame;
        frame["id"] = i;
        frame["name"] = vm_.debugFrameName(i);
        frame["line"] = vm_.debugFrameLine(i);
        frame["column"] = 1;

        // Source info
        nlohmann::json source;
        source["name"] = sourcePath_.empty() ? "<script>" :
            sourcePath_.substr(sourcePath_.find_last_of("/\\") + 1);
        source["path"] = sourcePath_;
        frame["source"] = source;

        body["stackFrames"].push_back(frame);
    }
    body["totalFrames"] = totalFrames;
    return body;
}

nlohmann::json DapServer::handleScopes(const nlohmann::json& params) {
    int frameId = params.value("frameId", 0);
    nlohmann::json body;
    body["scopes"] = nlohmann::json::array();

    // Locals scope
    nlohmann::json locals;
    locals["name"] = "Locals";
    locals["variablesReference"] = 1000 + frameId;  // encode frame in ref
    locals["expensive"] = false;
    body["scopes"].push_back(locals);

    // Globals scope
    nlohmann::json globals;
    globals["name"] = "Globals";
    globals["variablesReference"] = 2000;  // fixed ref for globals
    globals["expensive"] = false;
    body["scopes"].push_back(globals);

    return body;
}

nlohmann::json DapServer::handleVariables(const nlohmann::json& params) {
    int varRef = params.value("variablesReference", 0);
    nlohmann::json body;
    body["variables"] = nlohmann::json::array();

    auto addVar = [&](const std::string& name, const Value& v) {
        nlohmann::json var;
        var["name"] = name;
        var["value"] = valueToDisplayString(v);
        var["variablesReference"] = valueToVariablesRef(v);
        body["variables"].push_back(var);
    };

    if (varRef >= 2000) {
        // Globals
        auto names = vm_.getGlobalNames();
        for (size_t i = 0; i < names.size(); i++) {
            Value val = vm_.getGlobal(names[i]);
            if (!val.isNull()) {
                addVar(names[i], val);
            }
        }
    } else if (varRef >= 1000) {
        // Locals for a frame
        int frameId = varRef - 1000;
        auto localNames = vm_.debugLocalNames(frameId);
        for (size_t i = 0; i < localNames.size(); i++) {
            Value val = vm_.debugLocalValue(frameId, static_cast<int>(i));
            addVar(localNames[i], val);
        }
    } else if (varRef == 1) {
        // Children of an expandable value — simplified: just show as string
        // Full implementation would enumerate array elements / dict keys
        addVar("(expandable)", nullptr);
    }

    return body;
}

nlohmann::json DapServer::handleContinue(const nlohmann::json&) {
    vm_.debugResume();
    resumeExecution();
    nlohmann::json body;
    body["allThreadsContinued"] = true;
    return body;
}

nlohmann::json DapServer::handleNext(const nlohmann::json&) {
    vm_.debugSetStepMode(StepMode::StepOver);
    vm_.debugResume();
    resumeExecution();
    return nlohmann::json::object();
}

nlohmann::json DapServer::handleStepIn(const nlohmann::json&) {
    vm_.debugSetStepMode(StepMode::StepIn);
    vm_.debugResume();
    resumeExecution();
    return nlohmann::json::object();
}

nlohmann::json DapServer::handleStepOut(const nlohmann::json&) {
    vm_.debugSetStepMode(StepMode::StepOut);
    vm_.debugResume();
    resumeExecution();
    return nlohmann::json::object();
}

nlohmann::json DapServer::handlePause(const nlohmann::json&) {
    vm_.debugPause();
    return nlohmann::json::object();
}

nlohmann::json DapServer::handleEvaluate(const nlohmann::json& params) {
    std::string expr = params.value("expression", "");
    Value result;
    InterpretResult res = vm_.debugEvaluate(expr, result);

    nlohmann::json body;
    if (res == InterpretResult::OK) {
        body["result"] = valueToDisplayString(result);
        body["variablesReference"] = valueToVariablesRef(result);
    } else {
        body["result"] = "<evaluation error>";
        body["variablesReference"] = 0;
    }
    return body;
}

nlohmann::json DapServer::handleDisconnect(const nlohmann::json&) {
    running_ = false;
    return nlohmann::json::object();
}

} // namespace vora::dap
