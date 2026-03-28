#include "debug_session.h"
#include <algorithm>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>
#include <regex>

// ============================================================
// URL / path helpers
// ============================================================

std::string DebugSession::normalize_url(const std::string& path) {
    std::string r = path;
    std::replace(r.begin(), r.end(), '\\', '/');
    return r;
}

std::string DebugSession::to_file_url(const std::string& path) {
    std::string norm = normalize_url(path);
    if (norm.find("file://") == 0) return norm;
    // If it looks like an absolute path (starts with / or X:/)
    if (!norm.empty() && (norm[0] == '/' || (norm.size() >= 3 && norm[1] == ':'))) {
        if (norm[0] != '/') {
            norm = "/" + norm;
        }
        return "file://" + norm;
    }
    return norm;
}

static std::string to_lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return (char)tolower(c); });
    return r;
}

static bool paths_match(const std::string& a, const std::string& b) {
#ifdef _WIN32
    return to_lower(a) == to_lower(b);
#else
    return a == b;
#endif
}

// ============================================================
// Constructor / Destructor
// ============================================================

DebugSession::DebugSession() = default;
DebugSession::~DebugSession() = default;

void DebugSession::set_send_event(SendEventFn fn) {
    send_event_ = std::move(fn);
}

// ============================================================
// Script management
// ============================================================

std::string DebugSession::add_script(const std::string& url, const std::string& source) {
    ScriptInfo info;
    info.id = std::to_string(next_script_id_++);
    info.url = normalize_url(url);
    info.source = source;

    // Count lines
    int lines = 1;
    for (char c : source) {
        if (c == '\n') lines++;
    }
    info.end_line = lines;

    scripts_.push_back(std::move(info));

    // If debugger is enabled, send scriptParsed event
    if (enabled_.load() && send_event_) {
        const auto& si = scripts_.back();
        auto params = json::Value::object();
        params.set("scriptId", si.id);
        params.set("url", to_file_url(si.url));
        params.set("startLine", 0);
        params.set("startColumn", 0);
        params.set("endLine", si.end_line);
        params.set("endColumn", 0);
        params.set("executionContextId", 1);
        params.set("hash", "");
        params.set("isLiveEdit", false);
        params.set("sourceMapURL", "");
        params.set("hasSourceURL", false);
        params.set("isModule", false);
        params.set("length", (int)si.source.size());
        send_event_("Debugger.scriptParsed", params);
    }

    return scripts_.back().id;
}

const ScriptInfo* DebugSession::get_script_by_id(const std::string& id) const {
    for (const auto& s : scripts_) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

std::string DebugSession::find_script_id(const std::string& filename) const {
    std::string norm = normalize_url(filename);
    for (const auto& s : scripts_) {
        if (paths_match(s.url, norm)) return s.id;
        // Also match by basename
        std::string s_base = s.url;
        auto pos = s_base.rfind('/');
        if (pos != std::string::npos) s_base = s_base.substr(pos + 1);
        std::string f_base = norm;
        pos = f_base.rfind('/');
        if (pos != std::string::npos) f_base = f_base.substr(pos + 1);
        if (paths_match(s_base, f_base) && !s_base.empty()) return s.id;
    }
    return "0";
}

// ============================================================
// Breakpoint management
// ============================================================

json::Value DebugSession::set_breakpoint_by_url(const std::string& url,
                                                  int line_0based, int column_0based,
                                                  bool is_regex) {
    std::lock_guard<std::mutex> lock(bp_mutex_);

    Breakpoint bp;
    bp.id = next_bp_id_++;
    bp.url = is_regex ? url : normalize_url(url);
    bp.line = line_0based + 1; // Convert to 1-based for QuickJS
    bp.column = column_0based;
    bp.enabled = true;
    bp.is_regex = is_regex;

    breakpoints_[bp.id] = bp;

    fprintf(stderr, "[DBG] Set breakpoint #%d: url='%s' line=%d (1-based)\n",
            bp.id, bp.url.c_str(), bp.line);

    // Build CDP result
    std::string bp_id_str = std::to_string(bp.id) + ":" + std::to_string(line_0based) + ":0";

    // Find matching script for resolved location
    std::string script_id;
    for (const auto& s : scripts_) {
        if (is_regex) {
            // Match regex against script URL (as file:// URL and raw path)
            try {
                std::regex re(bp.url, std::regex_constants::ECMAScript | std::regex_constants::icase);
                std::string surl = to_file_url(s.url);
                if (std::regex_search(surl, re) || std::regex_search(s.url, re)) {
                    script_id = s.id;
                    break;
                }
            } catch (...) {}
        }
        if (paths_match(s.url, bp.url) ||
            to_lower(s.url).find(to_lower(bp.url)) != std::string::npos ||
            to_lower(bp.url).find(to_lower(s.url)) != std::string::npos) {
            script_id = s.id;
            break;
        }
        // Match by basename
        std::string s_base = s.url;
        auto pos = s_base.rfind('/');
        if (pos != std::string::npos) s_base = s_base.substr(pos + 1);
        std::string b_base = bp.url;
        pos = b_base.rfind('/');
        if (pos != std::string::npos) b_base = b_base.substr(pos + 1);
        if (paths_match(s_base, b_base) && !s_base.empty()) {
            script_id = s.id;
            break;
        }
    }

    auto location = json::Value::object();
    location.set("scriptId", script_id.empty() ? "0" : script_id);
    location.set("lineNumber", line_0based);
    location.set("columnNumber", column_0based);

    auto locations = json::Value::array();
    locations.push(location);

    auto result = json::Value::object();
    result.set("breakpointId", bp_id_str);
    result.set("locations", locations);

    return result;
}

bool DebugSession::remove_breakpoint(const std::string& bp_id_str) {
    std::lock_guard<std::mutex> lock(bp_mutex_);
    // bp_id_str format: "N:line:col"
    int bp_id = 0;
    try {
        bp_id = std::stoi(bp_id_str);
    } catch (...) {
        return false;
    }
    return breakpoints_.erase(bp_id) > 0;
}

void DebugSession::set_breakpoints_active(bool active) {
    std::lock_guard<std::mutex> lock(bp_mutex_);
    bp_active_ = active;
}

bool DebugSession::check_breakpoint(const std::string& filename, int line) const {
    std::lock_guard<std::mutex> lock(bp_mutex_);
    if (!bp_active_) return false;

    std::string norm_file = normalize_url(filename);
    std::string file_url = to_file_url(norm_file);
    std::string file_base = norm_file;
    auto pos = file_base.rfind('/');
    if (pos != std::string::npos) file_base = file_base.substr(pos + 1);

    for (const auto& [id, bp] : breakpoints_) {
        if (!bp.enabled) continue;
        if (bp.line != line) continue;

        if (bp.is_regex) {
            // VS Code sends urlRegex — match against file URL and raw path
            try {
                std::regex re(bp.url, std::regex_constants::ECMAScript | std::regex_constants::icase);
                if (std::regex_search(file_url, re) || std::regex_search(norm_file, re)) {
                    return true;
                }
            } catch (...) {
                // Malformed regex — fall through to plain matching
            }
        }

        // Plain path matching (for url-based breakpoints or regex fallback)
        std::string bp_base = bp.url;
        auto bpos = bp_base.rfind('/');
        if (bpos != std::string::npos) bp_base = bp_base.substr(bpos + 1);

        if (paths_match(norm_file, bp.url) || paths_match(file_base, bp_base) ||
            to_lower(norm_file).find(to_lower(bp.url)) != std::string::npos ||
            to_lower(bp.url).find(to_lower(norm_file)) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// ============================================================
// Execution control
// ============================================================

void DebugSession::resume() {
    step_mode_.store(StepMode::Continue);
    pause_requested_.store(false);
    paused_.store(false);
    pause_cv_.notify_all();
}

void DebugSession::step_over() {
    // Capture current position so op_handler knows when we've moved
    step_start_depth_ = (int)frame_stack_.size();
    step_start_line_ = frame_stack_.empty() ? 0 : frame_stack_.back().line;
    step_start_file_ = frame_stack_.empty() ? "" : frame_stack_.back().filename;
    step_mode_.store(StepMode::Over);
    pause_requested_.store(false);
    paused_.store(false);
    pause_cv_.notify_all();
}

void DebugSession::step_into() {
    step_start_depth_ = (int)frame_stack_.size();
    step_start_line_ = frame_stack_.empty() ? 0 : frame_stack_.back().line;
    step_start_file_ = frame_stack_.empty() ? "" : frame_stack_.back().filename;
    step_mode_.store(StepMode::Into);
    pause_requested_.store(false);
    paused_.store(false);
    pause_cv_.notify_all();
}

void DebugSession::step_out() {
    step_start_depth_ = (int)frame_stack_.size();
    step_start_line_ = frame_stack_.empty() ? 0 : frame_stack_.back().line;
    step_start_file_ = frame_stack_.empty() ? "" : frame_stack_.back().filename;
    step_mode_.store(StepMode::Out);
    pause_requested_.store(false);
    paused_.store(false);
    pause_cv_.notify_all();
}

void DebugSession::request_pause() {
    pause_requested_.store(true);
}

// ============================================================
// Wait for debugger
// ============================================================

void DebugSession::wait_for_debugger() {
    if (!pause_on_start_) return;
    waiting_.store(true);
    std::unique_lock<std::mutex> lock(wait_mutex_);
    wait_cv_.wait(lock, [this] { return !waiting_.load(); });
}

void DebugSession::run_if_waiting() {
    // Set Continue mode so breakpoints are checked from the start
    step_mode_.store(StepMode::Continue);
    waiting_.store(false);
    wait_cv_.notify_all();
}

void DebugSession::on_disconnect() {
    // Unblock everything on disconnect
    pause_requested_.store(false);
    step_mode_.store(StepMode::Continue);
    paused_.store(false);
    pause_cv_.notify_all();
    waiting_.store(false);
    wait_cv_.notify_all();
}

// ============================================================
// JSValue → CDP RemoteObject
// ============================================================

json::Value DebugSession::js_value_to_remote_object(JSContext* ctx, JSValue val,
                                                      const std::string& group) const {
    auto obj = json::Value::object();

    if (JS_IsUndefined(val)) {
        obj.set("type", "undefined");
    } else if (JS_IsNull(val)) {
        obj.set("type", "object");
        obj.set("subtype", "null");
        obj.set("value", nullptr);
    } else if (JS_IsBool(val)) {
        bool b = JS_ToBool(ctx, val) != 0;
        obj.set("type", "boolean");
        obj.set("value", b);
        obj.set("description", b ? "true" : "false");
    } else if (JS_VALUE_GET_TAG(val) == JS_TAG_INT) {
        int32_t v;
        JS_ToInt32(ctx, &v, val);
        obj.set("type", "number");
        obj.set("value", v);
        obj.set("description", std::to_string(v));
    } else if (JS_IsNumber(val)) {
        double v;
        JS_ToFloat64(ctx, &v, val);
        obj.set("type", "number");
        obj.set("value", v);
        char buf[64];
        snprintf(buf, sizeof(buf), "%.17g", v);
        obj.set("description", std::string(buf));
    } else if (JS_IsString(val)) {
        const char* s = JS_ToCString(ctx, val);
        if (s) {
            obj.set("type", "string");
            obj.set("value", s);
            obj.set("description", std::string("\"") + s + "\"");
            JS_FreeCString(ctx, s);
        }
    } else if (JS_IsFunction(ctx, val)) {
        obj.set("type", "function");
        obj.set("className", "Function");
        obj.set("description", "function()");
        // Store for later property access
        auto props = json::Value::array();
        std::string oid = store_object(group, props);
        obj.set("objectId", oid);
    } else if (JS_IsArray(val)) {
        obj.set("type", "object");
        obj.set("subtype", "array");
        obj.set("className", "Array");

        // Get array length and elements
        JSValue len_val = JS_GetPropertyStr(ctx, val, "length");
        int32_t len = 0;
        JS_ToInt32(ctx, &len, len_val);
        JS_FreeValue(ctx, len_val);

        obj.set("description", std::string("Array(") + std::to_string(len) + ")");

        // Store array properties
        auto props = json::Value::array();
        for (int32_t i = 0; i < len && i < 100; i++) {
            JSValue elem = JS_GetPropertyUint32(ctx, val, i);
            auto prop = json::Value::object();
            prop.set("name", std::to_string(i));
            prop.set("value", js_value_to_remote_object(ctx, elem, group));
            prop.set("writable", true);
            prop.set("configurable", true);
            prop.set("enumerable", true);
            prop.set("isOwn", true);
            props.push(prop);
            JS_FreeValue(ctx, elem);
        }
        // Add length property
        auto len_prop = json::Value::object();
        len_prop.set("name", "length");
        auto len_ro = json::Value::object();
        len_ro.set("type", "number");
        len_ro.set("value", (int)len);
        len_ro.set("description", std::to_string(len));
        len_prop.set("value", len_ro);
        len_prop.set("writable", true);
        len_prop.set("configurable", false);
        len_prop.set("enumerable", false);
        len_prop.set("isOwn", true);
        props.push(len_prop);

        std::string oid = store_object(group, props);
        obj.set("objectId", oid);
    } else if (JS_IsObject(val)) {
        obj.set("type", "object");
        obj.set("className", "Object");

        // Get own property names
        JSPropertyEnum* props_enum = nullptr;
        uint32_t prop_count = 0;
        auto props = json::Value::array();

        if (JS_GetOwnPropertyNames(ctx, &props_enum, &prop_count, val,
                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < prop_count && i < 100; i++) {
                JSValue pval = JS_GetProperty(ctx, val, props_enum[i].atom);
                const char* pname = JS_AtomToCString(ctx, props_enum[i].atom);
                if (pname) {
                    auto prop = json::Value::object();
                    prop.set("name", pname);
                    prop.set("value", js_value_to_remote_object(ctx, pval, group));
                    prop.set("writable", true);
                    prop.set("configurable", true);
                    prop.set("enumerable", true);
                    prop.set("isOwn", true);
                    props.push(prop);
                    JS_FreeCString(ctx, pname);
                }
                JS_FreeValue(ctx, pval);
            }
            for (uint32_t i = 0; i < prop_count; i++)
                JS_FreeAtom(ctx, props_enum[i].atom);
            js_free(ctx, props_enum);
        }

        obj.set("description", "Object");
        std::string oid = store_object(group, props);
        obj.set("objectId", oid);
    } else {
        obj.set("type", "undefined");
    }

    return obj;
}

std::string DebugSession::store_object(const std::string& group, json::Value props) const {
    std::string oid = "{\"ordinal\":" + std::to_string(next_obj_id_++) + ",\"injectedScriptId\":1}";
    object_store_[oid] = std::move(props);
    return oid;
}

// ============================================================
// State capture (when pausing)
// ============================================================

void DebugSession::capture_frames(JSContext* ctx, const char* filename,
                                    const char* funcname, int line, int col) {
    int depth = JS_GetStackDepth(ctx);

    // Update frame_stack_ from depth tracking
    while ((int)frame_stack_.size() > depth) frame_stack_.pop_back();
    while ((int)frame_stack_.size() < depth) frame_stack_.push_back(FrameInfo{});

    if (!frame_stack_.empty()) {
        auto& top = frame_stack_.back();
        top.filename = filename ? filename : "";
        top.funcname = funcname ? funcname : "";
        top.line = line;
        top.col = col;
    }

    // Clear old object store
    object_store_.clear();
    next_obj_id_ = 1;

    // Build CDP call frames (innermost first)
    auto frames = json::Value::array();
    for (int i = (int)frame_stack_.size() - 1; i >= 0; i--) {
        int level = (int)frame_stack_.size() - 1 - i; // level for JS_GetLocalVariablesAtLevel
        const auto& fi = frame_stack_[i];

        auto frame = json::Value::object();
        frame.set("callFrameId", std::to_string(level));
        frame.set("functionName", fi.funcname.empty() ? "(anonymous)" : fi.funcname);

        // Location
        std::string sid = find_script_id(fi.filename);
        auto location = json::Value::object();
        location.set("scriptId", sid);
        location.set("lineNumber", fi.line > 0 ? fi.line - 1 : 0); // 0-based for CDP
        location.set("columnNumber", fi.col > 0 ? fi.col - 1 : 0);
        frame.set("location", location);
        frame.set("url", to_file_url(fi.filename));

        // Scope chain
        auto scope_chain = json::Value::array();

        // Local scope - capture variables using QuickJS API
        int var_count = 0;
        JSDebugLocalVar* vars = JS_GetLocalVariablesAtLevel(ctx, level, &var_count);
        if (vars && var_count > 0) {
            auto local_props = json::Value::array();
            for (int v = 0; v < var_count; v++) {
                auto prop = json::Value::object();
                prop.set("name", vars[v].name ? vars[v].name : "");
                prop.set("value", js_value_to_remote_object(ctx, vars[v].value, "scope"));
                prop.set("writable", true);
                prop.set("configurable", true);
                prop.set("enumerable", true);
                prop.set("isOwn", true);
                local_props.push(prop);
            }
            JS_FreeLocalVariables(ctx, vars, var_count);

            std::string scope_oid = store_object("scope", local_props);

            auto local_scope = json::Value::object();
            local_scope.set("type", "local");
            auto scope_obj = json::Value::object();
            scope_obj.set("type", "object");
            scope_obj.set("className", "Object");
            scope_obj.set("objectId", scope_oid);
            local_scope.set("object", scope_obj);
            local_scope.set("name", fi.funcname.empty() ? "(anonymous)" : fi.funcname);
            scope_chain.push(local_scope);
        }

        // Global scope
        {
            JSValue global = JS_GetGlobalObject(ctx);
            auto global_props = json::Value::array();

            JSPropertyEnum* gprops = nullptr;
            uint32_t gprop_count = 0;
            if (JS_GetOwnPropertyNames(ctx, &gprops, &gprop_count, global,
                    JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                for (uint32_t g = 0; g < gprop_count && g < 50; g++) {
                    JSValue gval = JS_GetProperty(ctx, global, gprops[g].atom);
                    const char* gname = JS_AtomToCString(ctx, gprops[g].atom);
                    if (gname) {
                        auto gprop = json::Value::object();
                        gprop.set("name", gname);
                        gprop.set("value", js_value_to_remote_object(ctx, gval, "global"));
                        gprop.set("writable", true);
                        gprop.set("configurable", true);
                        gprop.set("enumerable", true);
                        gprop.set("isOwn", true);
                        global_props.push(gprop);
                        JS_FreeCString(ctx, gname);
                    }
                    JS_FreeValue(ctx, gval);
                }
                for (uint32_t g = 0; g < gprop_count; g++)
                    JS_FreeAtom(ctx, gprops[g].atom);
                js_free(ctx, gprops);
            }
            JS_FreeValue(ctx, global);

            std::string global_oid = store_object("global", global_props);

            auto global_scope = json::Value::object();
            global_scope.set("type", "global");
            auto gobj = json::Value::object();
            gobj.set("type", "object");
            gobj.set("className", "Object");
            gobj.set("objectId", global_oid);
            global_scope.set("object", gobj);
            scope_chain.push(global_scope);
        }

        frame.set("scopeChain", scope_chain);

        // this
        auto this_obj = json::Value::object();
        this_obj.set("type", "object");
        this_obj.set("className", "global");
        frame.set("this", this_obj);

        frames.push(frame);
    }

    captured_frames_ = frames;
}

// ============================================================
// Pause logic
// ============================================================

void DebugSession::do_pause(JSContext* ctx, const char* filename,
                              const char* funcname, int line, int col,
                              PauseReason reason, int bp_id) {
    // Capture state
    capture_frames(ctx, filename, funcname, line, col);

    paused_.store(true);

    // Build Debugger.paused event
    auto params = json::Value::object();
    params.set("callFrames", captured_frames_);

    switch (reason) {
    case PauseReason::Breakpoint: {
        params.set("reason", "breakpoint");
        auto hit_bps = json::Value::array();
        if (bp_id > 0) {
            hit_bps.push(std::to_string(bp_id) + ":" + std::to_string(line - 1) + ":0");
        }
        params.set("hitBreakpoints", hit_bps);
        break;
    }
    case PauseReason::Step:
        params.set("reason", "step");
        params.set("hitBreakpoints", json::Value::array());
        break;
    case PauseReason::PauseRequest:
        params.set("reason", "pause");
        params.set("hitBreakpoints", json::Value::array());
        break;
    case PauseReason::Entry:
        params.set("reason", "Break on start");
        params.set("hitBreakpoints", json::Value::array());
        break;
    }

    if (send_event_) {
        send_event_("Debugger.paused", params);
    }

    // Store JSContext for evaluate-while-paused
    paused_ctx_ = ctx;

    // Block until resumed, but wake up to process eval requests
    {
        std::unique_lock<std::mutex> lock(pause_mutex_);
        while (paused_.load()) {
            pause_cv_.wait(lock, [this] {
                return !paused_.load() || pending_eval_ != nullptr;
            });

            // Process pending eval request if any
            if (paused_.load() && pending_eval_ != nullptr) {
                std::lock_guard<std::mutex> eval_lock(eval_mutex_);
                if (pending_eval_) {
                    // Evaluate expression on the JS thread
                    int frame_level = 0;
                    try { frame_level = std::stoi(pending_eval_->call_frame_id); } catch (...) {}

                    // Try local variable lookup first
                    bool found = false;
                    int var_count = 0;
                    JSDebugLocalVar* vars = JS_GetLocalVariablesAtLevel(ctx, frame_level, &var_count);
                    if (vars && var_count > 0) {
                        for (int v = 0; v < var_count; v++) {
                            if (vars[v].name && pending_eval_->expression == vars[v].name) {
                                pending_eval_->result = json::Value::object();
                                pending_eval_->result.set("result",
                                    js_value_to_remote_object(ctx, vars[v].value, "watch"));
                                found = true;
                                break;
                            }
                        }
                        JS_FreeLocalVariables(ctx, vars, var_count);
                    }

                    if (!found) {
                        // Try global eval as fallback
                        JSValue global = JS_GetGlobalObject(ctx);
                        JSValue eval_val = JS_Eval(ctx,
                            pending_eval_->expression.c_str(),
                            pending_eval_->expression.size(),
                            "<debugger-eval>", JS_EVAL_TYPE_GLOBAL);
                        if (JS_IsException(eval_val)) {
                            JSValue exc = JS_GetException(ctx);
                            const char* exc_str = JS_ToCString(ctx, exc);
                            auto err_result = json::Value::object();
                            auto err_obj = json::Value::object();
                            err_obj.set("type", "object");
                            err_obj.set("subtype", "error");
                            err_obj.set("className", "Error");
                            err_obj.set("description", exc_str ? exc_str : "Error");
                            if (exc_str) JS_FreeCString(ctx, exc_str);
                            JS_FreeValue(ctx, exc);
                            err_result.set("result", err_obj);
                            err_result.set("exceptionDetails", json::Value::object());
                            pending_eval_->result = err_result;
                        } else {
                            pending_eval_->result = json::Value::object();
                            pending_eval_->result.set("result",
                                js_value_to_remote_object(ctx, eval_val, "console"));
                        }
                        JS_FreeValue(ctx, eval_val);
                        JS_FreeValue(ctx, global);
                    }

                    pending_eval_->ready = true;
                    eval_done_cv_.notify_all();
                }
            }
        }
    }

    paused_ctx_ = nullptr;

    // Send resumed event
    if (send_event_) {
        send_event_("Debugger.resumed", json::Value::object());
    }
}

json::Value DebugSession::get_call_frames_json() const {
    return captured_frames_;
}

json::Value DebugSession::get_properties(const std::string& object_id) const {
    auto it = object_store_.find(object_id);
    if (it != object_store_.end()) {
        auto result = json::Value::object();
        result.set("result", it->second);
        return result;
    }
    // Return empty result
    auto result = json::Value::object();
    result.set("result", json::Value::array());
    return result;
}

json::Value DebugSession::evaluate_on_call_frame(const std::string& call_frame_id,
                                                   const std::string& expression) {
    if (!paused_.load()) {
        auto r = json::Value::object();
        auto ro = json::Value::object();
        ro.set("type", "undefined");
        r.set("result", ro);
        return r;
    }

    // Post request to JS thread
    EvalRequest req;
    req.expression = expression;
    req.call_frame_id = call_frame_id;
    req.ready = false;

    {
        std::lock_guard<std::mutex> lock(eval_mutex_);
        pending_eval_ = &req;
    }

    // Wake up the JS thread (which is waiting in do_pause)
    pause_cv_.notify_all();

    // Wait for JS thread to fill in the result
    {
        std::unique_lock<std::mutex> lock(eval_mutex_);
        eval_done_cv_.wait(lock, [&req] { return req.ready; });
        pending_eval_ = nullptr;
    }

    return req.result;
}

// ============================================================
// OP Handler
// ============================================================

int DebugSession::op_handler(JSContext *ctx, uint8_t op,
                               const char *filename, const char *funcname,
                               int line, int col, void *opaque) {

    auto* self = static_cast<DebugSession*>(opaque);
    if (!self->enabled_.load()) return 0;
    if (!filename || line <= 0) return 0;

    // 只有当行号变化时才更新调用栈信息，这样可以避免在同一行内多次调用时重复更新调用栈，提高性能
    if (!self->frame_stack_.empty() && self->frame_stack_.back().line == line) {
        return 0;
    }

    int depth = JS_GetStackDepth(ctx);

    // Update frame stack tracking
    while ((int)self->frame_stack_.size() > depth) self->frame_stack_.pop_back();
    while ((int)self->frame_stack_.size() < depth) self->frame_stack_.push_back(FrameInfo{});
    if (!self->frame_stack_.empty()) {
        auto& top = self->frame_stack_.back();
        top.filename = filename ? filename : "";
        top.funcname = funcname ? funcname : "";
        top.line = line;
        top.col = col;
    }

    StepMode mode = self->step_mode_.load();

    // Check for pause request
    if (self->pause_requested_.load()) {
        self->pause_requested_.store(false);
        self->step_mode_.store(StepMode::None);
        self->do_pause(ctx, filename, funcname, line, col, PauseReason::PauseRequest, 0);
        return 0;
    }

    // Check for entry breakpoint (first instruction)
    if (self->pause_on_start_) {
        self->pause_on_start_ = false;
        self->step_mode_.store(StepMode::None);
        self->do_pause(ctx, filename, funcname, line, col, PauseReason::Entry, 0);
        return 0;
    }

    // Check breakpoints (in all modes)
    if (self->check_breakpoint(filename, line)) {
        // Find which breakpoint
        int bp_id = 0;
        {
            std::lock_guard<std::mutex> lock(self->bp_mutex_);
            std::string norm = normalize_url(filename);
            std::string furl = to_file_url(norm);
            std::string base = norm;
            auto p = base.rfind('/');
            if (p != std::string::npos) base = base.substr(p + 1);
            for (const auto& [id, bp] : self->breakpoints_) {
                if (bp.line == line && bp.enabled) {
                    if (bp.is_regex) {
                        try {
                            std::regex re(bp.url, std::regex_constants::ECMAScript | std::regex_constants::icase);
                            if (std::regex_search(furl, re) || std::regex_search(norm, re)) {
                                bp_id = id;
                                break;
                            }
                        } catch (...) {}
                    }
                    std::string bp_base = bp.url;
                    auto bpos = bp_base.rfind('/');
                    if (bpos != std::string::npos) bp_base = bp_base.substr(bpos + 1);
                    if (paths_match(norm, bp.url) || paths_match(base, bp_base) ||
                        to_lower(norm).find(to_lower(bp.url)) != std::string::npos ||
                        to_lower(bp.url).find(to_lower(norm)) != std::string::npos) {
                        bp_id = id;
                        break;
                    }
                }
            }
        }
        self->step_mode_.store(StepMode::None);
        self->do_pause(ctx, filename, funcname, line, col, PauseReason::Breakpoint, bp_id);
        return 0;
    }

    // Handle step modes
    switch (mode) {
    case StepMode::None:
    case StepMode::Continue:
        break;

    case StepMode::Into:
        // Pause on the next line/statement
        if (line != self->step_start_line_ ||
            (filename && self->step_start_file_ != filename)) {
            self->step_mode_.store(StepMode::None);
            self->do_pause(ctx, filename, funcname, line, col, PauseReason::Step, 0);
        }
        break;

    case StepMode::Over:
        // Pause on next line at the same or lower depth
        if (depth <= self->step_start_depth_) {
            if (line != self->step_start_line_ ||
                (filename && self->step_start_file_ != filename)) {
                self->step_mode_.store(StepMode::None);
                self->do_pause(ctx, filename, funcname, line, col, PauseReason::Step, 0);
            }
        }
        break;

    case StepMode::Out:
        // Pause when we return to a lower depth
        if (depth < self->step_start_depth_) {
            self->step_mode_.store(StepMode::None);
            self->do_pause(ctx, filename, funcname, line, col, PauseReason::Step, 0);
        }
        break;

    default:
        break;
    }

    return 0;
}
