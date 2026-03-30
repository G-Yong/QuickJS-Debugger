#pragma once

#include "json.h"
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>

extern "C" {
#include "quickjs.h"
}

struct ScriptInfo {
    std::string id;
    std::string url;        // normalized URL (forward slashes)
    std::string source;
    int end_line = 0;
};

struct FrameInfo {
    std::string filename;
    std::string funcname;
    int line = 0;
    int col = 0;
};

struct Breakpoint {
    int id = 0;
    std::string url;        // normalized path or regex pattern
    int line = 0;           // 1-based (QuickJS convention)
    int column = 0;
    bool enabled = true;
    bool is_regex = false;  // true if url is a regex pattern (from urlRegex)
};

enum class PauseReason {
    Breakpoint,
    Step,
    PauseRequest,
    Entry
};

class DebugSession {
public:
    using SendEventFn = std::function<void(const std::string& method, const json::Value& params)>;

    DebugSession();
    ~DebugSession();

    void set_send_event(SendEventFn fn);

    // --- Script management ---
    std::string add_script(const std::string& url, const std::string& source);
    const ScriptInfo* get_script_by_id(const std::string& id) const;
    const std::vector<ScriptInfo>& scripts() const { return scripts_; }

    // --- Breakpoint management (thread-safe) ---
    // Returns CDP-format result with breakpointId and locations
    json::Value set_breakpoint_by_url(const std::string& url, int line_0based, int column_0based,
                                       bool is_regex = false);
    bool remove_breakpoint(const std::string& bp_id);
    void set_breakpoints_active(bool active);

    // --- Execution control (thread-safe) ---
    void resume();
    void step_over();
    void step_into();
    void step_out();
    void request_pause();

    // --- State ---
    bool is_paused() const { return paused_.load(); }
    bool is_enabled() const { return enabled_.load(); }
    void set_enabled(bool v) { enabled_.store(v); }

    // --- Paused state accessors ---
    json::Value get_call_frames_json() const;
    json::Value get_properties(const std::string& object_id) const;

    // --- Evaluate while paused (cross-thread) ---
    // Called from WS thread; blocks until JS thread evaluates and returns result.
    json::Value evaluate_on_call_frame(const std::string& call_frame_id,
                                       const std::string& expression);

    // --- QuickJS bytecode trace handler ---
    static int bytecode_trace_handler(JSContext *ctx, uint8_t op,
                          const char *filename, const char *funcname,
                          int line, int col, void *opaque);

    // --- Wait for debugger ---
    void set_pause_on_start(bool v) { pause_on_start_ = v; }
    void wait_for_debugger();
    void run_if_waiting();
    void on_disconnect();

    // --- Helpers ---
    static std::string normalize_url(const std::string& path);
    static std::string to_file_url(const std::string& path);

private:
    bool check_breakpoint(const std::string& filename, int line) const;
    void do_pause(JSContext* ctx, const char* filename, const char* funcname,
                  int line, int col, PauseReason reason, int bp_id);
    void capture_frames(JSContext* ctx, const char* filename, const char* funcname,
                        int line, int col);
    json::Value js_value_to_remote_object(JSContext* ctx, JSValue val,
                                           const std::string& group) const;
    std::string store_object(const std::string& group, json::Value props) const;
    std::string find_script_id(const std::string& filename) const;

    SendEventFn send_event_;

    // Scripts
    std::vector<ScriptInfo> scripts_;
    int next_script_id_ = 1;

    // Breakpoints
    mutable std::mutex bp_mutex_;
    std::map<int, Breakpoint> breakpoints_;
    int next_bp_id_ = 1;
    bool bp_active_ = true;

    // Execution state
    std::atomic<bool> enabled_{false};
    std::atomic<bool> paused_{false};

    enum class StepMode { None, Continue, Over, Into, Out, Pause };
    std::atomic<StepMode> step_mode_{StepMode::None};
    std::atomic<bool> pause_requested_{false};
    int step_start_depth_ = 0;
    std::string step_start_file_;
    int step_start_line_ = 0;

    // Frame tracking (updated in OP handler, JS thread only)
    std::vector<FrameInfo> frame_stack_;

    // Pause synchronization
    mutable std::mutex pause_mutex_;
    std::condition_variable pause_cv_;

    // Captured state when paused (written by JS thread, read by WS thread)
    json::Value captured_frames_;
    mutable std::map<std::string, json::Value> object_store_;
    mutable int next_obj_id_ = 1;

    // Wait for debugger
    bool pause_on_start_ = false;
    std::atomic<bool> waiting_{false};
    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;

    // Evaluate-while-paused queue (WS thread -> JS thread)
    struct EvalRequest {
        std::string expression;
        std::string call_frame_id;
        json::Value result;       // filled by JS thread
        bool ready = false;       // set true when JS thread is done
    };
    std::mutex eval_mutex_;
    std::condition_variable eval_request_cv_;  // signals JS thread: new request
    std::condition_variable eval_done_cv_;     // signals WS thread: result ready
    EvalRequest* pending_eval_ = nullptr;
    JSContext* paused_ctx_ = nullptr;  // set during pause for eval access
};
