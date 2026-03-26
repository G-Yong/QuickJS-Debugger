#include "cdp_handler.h"
#include <cstdio>

CDPHandler::CDPHandler(WebSocketServer& ws, DebugSession& session)
    : ws_(ws), session_(session) {}

void CDPHandler::send_response(int id, const json::Value& result) {
    auto msg = json::Value::object();
    msg.set("id", id);
    msg.set("result", result);
    ws_.send(msg.serialize());
}

void CDPHandler::send_error(int id, int code, const std::string& message) {
    auto err = json::Value::object();
    err.set("code", code);
    err.set("message", message);
    auto msg = json::Value::object();
    msg.set("id", id);
    msg.set("error", err);
    ws_.send(msg.serialize());
}

void CDPHandler::send_event(const std::string& method, const json::Value& params) {
    auto msg = json::Value::object();
    msg.set("method", method);
    msg.set("params", params);
    ws_.send(msg.serialize());
}

void CDPHandler::handle_message(const std::string& message) {
    auto msg = json::Value::parse(message);
    if (!msg.is_object()) return;

    int id = msg.has("id") ? msg["id"].get_int() : 0;
    std::string method = msg.has("method") ? msg["method"].get_string() : "";
    json::Value params = msg.has("params") ? msg["params"] : json::Value::object();

    fprintf(stderr, "[CDP] <- %s (id=%d)\n", method.c_str(), id);

    json::Value result;
    bool handled = true;

    // Debugger domain
    if (method == "Debugger.enable") {
        result = on_debugger_enable(params);
    } else if (method == "Debugger.disable") {
        result = on_debugger_disable(params);
    } else if (method == "Debugger.setBreakpointByUrl") {
        result = on_debugger_set_breakpoint_by_url(params);
    } else if (method == "Debugger.removeBreakpoint") {
        result = on_debugger_remove_breakpoint(params);
    } else if (method == "Debugger.resume") {
        result = on_debugger_resume(params);
    } else if (method == "Debugger.stepOver") {
        result = on_debugger_step_over(params);
    } else if (method == "Debugger.stepInto") {
        result = on_debugger_step_into(params);
    } else if (method == "Debugger.stepOut") {
        result = on_debugger_step_out(params);
    } else if (method == "Debugger.pause") {
        result = on_debugger_pause(params);
    } else if (method == "Debugger.getScriptSource") {
        result = on_debugger_get_script_source(params);
    } else if (method == "Debugger.setBreakpointsActive") {
        result = on_debugger_set_breakpoints_active(params);
    } else if (method == "Debugger.getPossibleBreakpoints") {
        result = on_debugger_get_possible_breakpoints(params);
    } else if (method == "Debugger.evaluateOnCallFrame") {
        result = on_debugger_evaluate_on_call_frame(params);
    } else if (method == "Debugger.setPauseOnExceptions") {
        result = on_debugger_set_pause_on_exceptions(params);
    }
    // Runtime domain
    else if (method == "Runtime.enable") {
        result = on_runtime_enable(params);
    } else if (method == "Runtime.disable") {
        result = on_runtime_disable(params);
    } else if (method == "Runtime.getProperties") {
        result = on_runtime_get_properties(params);
    } else if (method == "Runtime.runIfWaitingForDebugger") {
        result = on_runtime_run_if_waiting(params);
    } else if (method == "Runtime.evaluate") {
        result = on_runtime_evaluate(params);
    } else if (method == "Runtime.callFunctionOn") {
        result = on_runtime_call_function_on(params);
    } else if (method == "Runtime.releaseObjectGroup") {
        result = on_runtime_release_object_group(params);
    } else if (method == "Runtime.getIsolateId") {
        result = on_runtime_get_isolate_id(params);
    }
    // Profiler domain (stub)
    else if (method == "Profiler.enable") {
        result = on_profiler_enable(params);
    } else if (method == "Profiler.disable") {
        result = on_profiler_disable(params);
    }
    // Unknown methods - return empty result to avoid errors
    else {
        fprintf(stderr, "[CDP] Unhandled method: %s\n", method.c_str());
        result = json::Value::object();
    }

    if (id > 0) {
        send_response(id, result);
    }
}

// ============================================================
// Debugger domain handlers
// ============================================================

json::Value CDPHandler::on_debugger_enable(const json::Value& /*params*/) {
    debugger_enabled_ = true;
    session_.set_enabled(true);

    // Send scriptParsed for all known scripts
    for (const auto& script : session_.scripts()) {
        auto params = json::Value::object();
        params.set("scriptId", script.id);
        params.set("url", DebugSession::to_file_url(script.url));
        params.set("startLine", 0);
        params.set("startColumn", 0);
        params.set("endLine", script.end_line);
        params.set("endColumn", 0);
        params.set("executionContextId", 1);
        params.set("hash", "");
        params.set("isLiveEdit", false);
        params.set("sourceMapURL", "");
        params.set("hasSourceURL", false);
        params.set("isModule", false);
        params.set("length", (int)script.source.size());
        send_event("Debugger.scriptParsed", params);
    }

    auto result = json::Value::object();
    result.set("debuggerId", "quickjs-debugger-1");
    return result;
}

json::Value CDPHandler::on_debugger_disable(const json::Value& /*params*/) {
    debugger_enabled_ = false;
    session_.set_enabled(false);
    return json::Value::object();
}

json::Value CDPHandler::on_debugger_set_breakpoint_by_url(const json::Value& params) {
    int line = params.has("lineNumber") ? params["lineNumber"].get_int() : 0;
    int col = params.has("columnNumber") ? params["columnNumber"].get_int() : 0;
    std::string url;
    bool is_regex = false;
    if (params.has("url")) {
        url = params["url"].get_string();
    } else if (params.has("urlRegex")) {
        url = params["urlRegex"].get_string();
        is_regex = true;
    }
    // Strip file:// prefix for plain URLs (not regexes)
    if (!is_regex) {
        if (url.find("file:///") == 0) {
            url = url.substr(8); // Remove "file:///" (keep drive letter)
        } else if (url.find("file://") == 0) {
            url = url.substr(7);
        }
    }
    fprintf(stderr, "[CDP] setBreakpointByUrl: %s='%s' line=%d\n",
            is_regex ? "regex" : "url", url.c_str(), line);
    return session_.set_breakpoint_by_url(url, line, col, is_regex);
}

json::Value CDPHandler::on_debugger_remove_breakpoint(const json::Value& params) {
    std::string bp_id = params.has("breakpointId") ? params["breakpointId"].get_string() : "";
    session_.remove_breakpoint(bp_id);
    return json::Value::object();
}

json::Value CDPHandler::on_debugger_resume(const json::Value& /*params*/) {
    session_.resume();
    return json::Value::object();
}

json::Value CDPHandler::on_debugger_step_over(const json::Value& /*params*/) {
    session_.step_over();
    return json::Value::object();
}

json::Value CDPHandler::on_debugger_step_into(const json::Value& /*params*/) {
    session_.step_into();
    return json::Value::object();
}

json::Value CDPHandler::on_debugger_step_out(const json::Value& /*params*/) {
    session_.step_out();
    return json::Value::object();
}

json::Value CDPHandler::on_debugger_pause(const json::Value& /*params*/) {
    session_.request_pause();
    return json::Value::object();
}

json::Value CDPHandler::on_debugger_get_script_source(const json::Value& params) {
    std::string script_id = params.has("scriptId") ? params["scriptId"].get_string() : "";
    const ScriptInfo* si = session_.get_script_by_id(script_id);
    auto result = json::Value::object();
    if (si) {
        result.set("scriptSource", si->source);
    } else {
        result.set("scriptSource", "");
    }
    return result;
}

json::Value CDPHandler::on_debugger_set_breakpoints_active(const json::Value& params) {
    bool active = params.has("active") ? params["active"].get_bool() : true;
    session_.set_breakpoints_active(active);
    return json::Value::object();
}

json::Value CDPHandler::on_debugger_get_possible_breakpoints(const json::Value& params) {
    // Return the requested range as possible breakpoints
    auto result = json::Value::object();
    auto locations = json::Value::array();

    if (params.has("start")) {
        const auto& start = params["start"];
        std::string sid = start.has("scriptId") ? start["scriptId"].get_string() : "";
        int start_line = start.has("lineNumber") ? start["lineNumber"].get_int() : 0;
        int end_line = start_line + 1;
        if (params.has("end")) {
            end_line = params["end"]["lineNumber"].get_int();
        }
        for (int l = start_line; l <= end_line; l++) {
            auto loc = json::Value::object();
            loc.set("scriptId", sid);
            loc.set("lineNumber", l);
            loc.set("columnNumber", 0);
            locations.push(loc);
        }
    }

    result.set("locations", locations);
    return result;
}

json::Value CDPHandler::on_debugger_evaluate_on_call_frame(const json::Value& params) {
    std::string call_frame_id = params.has("callFrameId") ? params["callFrameId"].get_string() : "0";
    std::string expression = params.has("expression") ? params["expression"].get_string() : "";
    return session_.evaluate_on_call_frame(call_frame_id, expression);
}

json::Value CDPHandler::on_debugger_set_pause_on_exceptions(const json::Value& /*params*/) {
    // Stub - always returns success
    return json::Value::object();
}

// ============================================================
// Runtime domain handlers
// ============================================================

json::Value CDPHandler::on_runtime_enable(const json::Value& /*params*/) {
    runtime_enabled_ = true;

    // Send executionContextCreated event
    auto ctx_desc = json::Value::object();
    ctx_desc.set("id", 1);
    ctx_desc.set("origin", "");
    ctx_desc.set("name", "QuickJS");
    auto aux = json::Value::object();
    aux.set("isDefault", true);
    ctx_desc.set("auxData", aux);

    auto params = json::Value::object();
    params.set("context", ctx_desc);
    send_event("Runtime.executionContextCreated", params);

    return json::Value::object();
}

json::Value CDPHandler::on_runtime_disable(const json::Value& /*params*/) {
    runtime_enabled_ = false;
    return json::Value::object();
}

json::Value CDPHandler::on_runtime_get_properties(const json::Value& params) {
    std::string object_id = params.has("objectId") ? params["objectId"].get_string() : "";
    return session_.get_properties(object_id);
}

json::Value CDPHandler::on_runtime_run_if_waiting(const json::Value& /*params*/) {
    session_.run_if_waiting();
    return json::Value::object();
}

json::Value CDPHandler::on_runtime_evaluate(const json::Value& params) {
    // Limited: return undefined
    (void)params;
    auto result = json::Value::object();
    auto ro = json::Value::object();
    ro.set("type", "undefined");
    result.set("result", ro);
    return result;
}

json::Value CDPHandler::on_runtime_call_function_on(const json::Value& /*params*/) {
    auto result = json::Value::object();
    auto ro = json::Value::object();
    ro.set("type", "undefined");
    result.set("result", ro);
    return result;
}

json::Value CDPHandler::on_runtime_release_object_group(const json::Value& /*params*/) {
    return json::Value::object();
}

json::Value CDPHandler::on_runtime_get_isolate_id(const json::Value& /*params*/) {
    auto result = json::Value::object();
    result.set("id", "quickjs-isolate-1");
    return result;
}

// ============================================================
// Profiler domain (stubs)
// ============================================================

json::Value CDPHandler::on_profiler_enable(const json::Value& /*params*/) {
    return json::Value::object();
}

json::Value CDPHandler::on_profiler_disable(const json::Value& /*params*/) {
    return json::Value::object();
}
