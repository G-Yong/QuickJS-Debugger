#include "websocket_server.h"
#include "cdp_handler.h"
#include "debug_session.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>
#include <thread>
#include <atomic>
#include <chrono>

#ifdef _WIN32
#include <direct.h>
#include <stdlib.h> // _fullpath
#else
#include <limits.h>
#include <stdlib.h> // realpath
#endif

extern "C" {
#include "quickjs.h"
}

// ============================================================
// Simple console.log implementation
// ============================================================

static DebugSession* g_session = nullptr;
static CDPHandler* g_cdp = nullptr;

static JSValue js_console_log(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    (void)this_val;
    std::string output;
    for (int i = 0; i < argc; i++) {
        if (i > 0) output += " ";
        const char* str = JS_ToCString(ctx, argv[i]);
        if (str) {
            output += str;
            JS_FreeCString(ctx, str);
        }
    }
    printf("%s\n", output.c_str());

    // Send Runtime.consoleAPICalled event if debugger is connected
    if (g_cdp) {
        auto params = json::Value::object();
        params.set("type", "log");
        auto args = json::Value::array();
        for (int i = 0; i < argc; i++) {
            auto arg = json::Value::object();
            const char* str = JS_ToCString(ctx, argv[i]);
            if (str) {
                arg.set("type", "string");
                arg.set("value", str);
                JS_FreeCString(ctx, str);
            } else {
                arg.set("type", "undefined");
            }
            args.push(arg);
        }
        params.set("args", args);
        params.set("executionContextId", 1);

        // Timestamp in milliseconds
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        params.set("timestamp", (double)ms);

        g_cdp->send_event("Runtime.consoleAPICalled", params);
    }

    return JS_UNDEFINED;
}

static void setup_console(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, console, "log",
        JS_NewCFunction(ctx, js_console_log, "log", 1));
    JS_SetPropertyStr(ctx, console, "info",
        JS_NewCFunction(ctx, js_console_log, "info", 1));
    JS_SetPropertyStr(ctx, console, "warn",
        JS_NewCFunction(ctx, js_console_log, "warn", 1));
    JS_SetPropertyStr(ctx, console, "error",
        JS_NewCFunction(ctx, js_console_log, "error", 1));
    JS_SetPropertyStr(ctx, console, "debug",
        JS_NewCFunction(ctx, js_console_log, "debug", 1));

    JS_SetPropertyStr(ctx, global, "console", console);
    JS_FreeValue(ctx, global);
}

// ============================================================
// File reading utility
// ============================================================

static std::string read_file_contents(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ============================================================
// Usage
// ============================================================

static void print_usage(const char* program) {
    fprintf(stderr,
        "Usage: %s [options] <script.js>\n"
        "\n"
        "Options:\n"
        "  --port <port>    WebSocket debug port (default: 9229)\n"
        "  --inspect-brk    Pause on first statement (wait for debugger)\n"
        "  --inspect        Enable debugging without initial pause\n"
        "  --help           Show this help\n"
        "\n"
        "Examples:\n"
        "  %s --inspect-brk test.js\n"
        "  %s --inspect --port 9230 app.js\n"
        "\n"
        "Then open chrome://inspect in Chrome and click 'inspect',\n"
        "or use any CDP-compatible debugger client.\n",
        program, program, program);
}

// ============================================================
// Main
// ============================================================

int main(int argc, char* argv[]) {
    int port = 9229;
    bool inspect_brk = false;
    bool inspect = false;
    std::string script_file;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--inspect-brk") == 0) {
            inspect_brk = true;
            inspect = true;
        } else if (strcmp(argv[i], "--inspect") == 0) {
            inspect = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            script_file = argv[i];
        }
    }

    if (script_file.empty()) {
        fprintf(stderr, "Error: No script file specified.\n\n");
        print_usage(argv[0]);
        return 1;
    }

    // Read script source
    std::string source = read_file_contents(script_file);
    if (source.empty()) {
        fprintf(stderr, "Error: Cannot read file '%s'\n", script_file.c_str());
        return 1;
    }

    // Resolve to absolute path so URLs match between scriptParsed and setBreakpointByUrl
#ifdef _WIN32
    {
        char abs[_MAX_PATH];
        if (_fullpath(abs, script_file.c_str(), _MAX_PATH)) {
            script_file = abs;
        }
    }
#else
    {
        char* abs = realpath(script_file.c_str(), nullptr);
        if (abs) {
            script_file = abs;
            free(abs);
        }
    }
#endif
    // Normalize to forward slashes
    script_file = DebugSession::normalize_url(script_file);
    fprintf(stderr, "[DBG] Script absolute path: %s\n", script_file.c_str());

    // Default to inspect-brk if nothing specified
    if (!inspect && !inspect_brk) {
        inspect = true;
        inspect_brk = true;
    }

    // Create QuickJS runtime and context
    JSRuntime* rt = JS_NewRuntime();
    if (!rt) {
        fprintf(stderr, "Error: Cannot create QuickJS runtime\n");
        return 1;
    }
    JSContext* ctx = JS_NewContext(rt);
    if (!ctx) {
        fprintf(stderr, "Error: Cannot create QuickJS context\n");
        JS_FreeRuntime(rt);
        return 1;
    }

    // Setup console
    setup_console(ctx);

    // Create debug session
    DebugSession session;
    g_session = &session;

    // Add the script
    std::string script_id = session.add_script(script_file, source);

    // Start WebSocket server
    WebSocketServer ws;
    if (!ws.start(port)) {
        fprintf(stderr, "Error: Cannot start WebSocket server on port %d\n", port);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }

    fprintf(stderr, "Debugger listening on ws://127.0.0.1:%d/debug\n", port);
    fprintf(stderr, "Open chrome://inspect in Chrome, or:\n");
    fprintf(stderr, "  devtools://devtools/bundled/js_app.html?experiments=true&v8only=true&ws=127.0.0.1:%d/debug\n", port);

    if (inspect_brk) {
        fprintf(stderr, "Waiting for debugger to connect...\n");
    }

    // Create CDP handler
    CDPHandler cdp(ws, session);
    g_cdp = &cdp;

    // Wire up event sending
    session.set_send_event([&cdp](const std::string& method, const json::Value& params) {
        cdp.send_event(method, params);
    });

    // Wait for DevTools connection
    if (!ws.wait_for_connection(script_file,
            DebugSession::to_file_url(script_file))) {
        fprintf(stderr, "Error: Failed to accept WebSocket connection\n");
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }

    // Setup pause on start
    if (inspect_brk) {
        session.set_pause_on_start(true);
        session.set_enabled(true);
    }

    // Wait for a REAL debugger connection (one that actually sends messages).
    // VS Code may probe with a WebSocket that connects then immediately closes.
    std::atomic<bool> ws_running{true};
    std::thread ws_thread([&] {
        int msg_count = 0;
        while (ws_running.load()) {
            fprintf(stderr, "[WS-Thread] Waiting for messages...\n");
            std::string msg = ws.receive();
            if (msg.empty()) {
                if (!ws.is_connected()) {
                    if (msg_count == 0) {
                        // Connection dropped before any messages — likely a probe.
                        // Re-accept a new connection.
                        fprintf(stderr, "[WS-Thread] Connection dropped before any messages, re-accepting...\n");
                        ws.disconnect();
                        if (!ws.wait_for_connection(
                                session.scripts().empty() ? "" : session.scripts()[0].url,
                                session.scripts().empty() ? "" : DebugSession::to_file_url(session.scripts()[0].url))) {
                            fprintf(stderr, "[WS-Thread] Re-accept failed\n");
                            session.on_disconnect();
                            break;
                        }
                        fprintf(stderr, "[WS-Thread] New connection established, retrying receive...\n");
                        continue; // Try receiving from the new connection
                    }
                    fprintf(stderr, "[WS] DevTools disconnected (after %d messages)\n", msg_count);
                    session.on_disconnect();
                }
                break;
            }
            msg_count++;
            // Log the first ~200 chars of each message
            std::string preview = msg.size() > 200 ? msg.substr(0, 200) + "..." : msg;
            fprintf(stderr, "[WS-Thread] msg #%d: %s\n", msg_count, preview.c_str());
            cdp.handle_message(msg);
        }
        fprintf(stderr, "[WS-Thread] Exiting (received %d messages)\n", msg_count);
    });

    // If inspect-brk, wait for the debugger to send runIfWaitingForDebugger
    if (inspect_brk) {
        session.wait_for_debugger();
    }

    // Register the bytecode trace handler for debugging
    JS_SetBytecodeTraceHandler(ctx, DebugSession::bytecode_trace_handler, &session);

    // Evaluate the script
    fprintf(stderr, "[Engine] Executing %s ...\n", script_file.c_str());
    JSValue result = JS_Eval(ctx, source.c_str(), source.size(),
                              script_file.c_str(), JS_EVAL_TYPE_GLOBAL);

    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        const char* str = JS_ToCString(ctx, exc);
        if (str) {
            fprintf(stderr, "[Engine] Exception: %s\n", str);
            JS_FreeCString(ctx, str);
        }
        // Print stack trace if available
        if (JS_IsObject(exc)) {
            JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
            if (!JS_IsUndefined(stack)) {
                const char* stack_str = JS_ToCString(ctx, stack);
                if (stack_str) {
                    fprintf(stderr, "%s\n", stack_str);
                    JS_FreeCString(ctx, stack_str);
                }
            }
            JS_FreeValue(ctx, stack);
        }
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, result);

    fprintf(stderr, "[Engine] Script execution finished.\n");

    // Cleanup
    ws_running.store(false);
    ws.disconnect();
    if (ws_thread.joinable()) {
        ws_thread.join();
    }

    g_cdp = nullptr;
    g_session = nullptr;

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    return 0;
}
