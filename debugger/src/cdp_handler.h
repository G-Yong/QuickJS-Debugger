#pragma once

#include "json.h"
#include "websocket_server.h"
#include "debug_session.h"
#include <string>

class CDPHandler {
public:
    CDPHandler(WebSocketServer& ws, DebugSession& session);

    // Handle an incoming CDP JSON message
    void handle_message(const std::string& message);

    // Send a CDP event (thread-safe via WebSocket)
    void send_event(const std::string& method, const json::Value& params);

private:
    void send_response(int id, const json::Value& result);
    void send_error(int id, int code, const std::string& message);

    // Debugger domain
    json::Value on_debugger_enable(const json::Value& params);
    json::Value on_debugger_disable(const json::Value& params);
    json::Value on_debugger_set_breakpoint_by_url(const json::Value& params);
    json::Value on_debugger_remove_breakpoint(const json::Value& params);
    json::Value on_debugger_resume(const json::Value& params);
    json::Value on_debugger_step_over(const json::Value& params);
    json::Value on_debugger_step_into(const json::Value& params);
    json::Value on_debugger_step_out(const json::Value& params);
    json::Value on_debugger_pause(const json::Value& params);
    json::Value on_debugger_get_script_source(const json::Value& params);
    json::Value on_debugger_set_breakpoints_active(const json::Value& params);
    json::Value on_debugger_get_possible_breakpoints(const json::Value& params);
    json::Value on_debugger_evaluate_on_call_frame(const json::Value& params);
    json::Value on_debugger_set_pause_on_exceptions(const json::Value& params);

    // Runtime domain
    json::Value on_runtime_enable(const json::Value& params);
    json::Value on_runtime_disable(const json::Value& params);
    json::Value on_runtime_get_properties(const json::Value& params);
    json::Value on_runtime_run_if_waiting(const json::Value& params);
    json::Value on_runtime_evaluate(const json::Value& params);
    json::Value on_runtime_call_function_on(const json::Value& params);
    json::Value on_runtime_release_object_group(const json::Value& params);
    json::Value on_runtime_get_isolate_id(const json::Value& params);

    // Profiler domain (minimal)
    json::Value on_profiler_enable(const json::Value& params);
    json::Value on_profiler_disable(const json::Value& params);

    WebSocketServer& ws_;
    DebugSession& session_;
    bool runtime_enabled_ = false;
    bool debugger_enabled_ = false;
};
