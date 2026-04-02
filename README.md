**English** | [中文](./README.zh-CN.md)

# QuickJS CDP Debugger

A QuickJS script debugger based on the Chrome DevTools Protocol (CDP), enabling breakpoint debugging of a custom QuickJS engine in VS Code.

![Debug in VSCode](doc/images/debugInVSCode.png)

## Project Structure

```
jsTest/
├── CMakeLists.txt              # Top-level build file
├── README.md
├── test.js                     # Test script
├── quickjs/                    # QuickJS engine (modified, with debug API)
│   ├── quickjs.h/c             #   Core engine
│   └── ...
├── debugger/                   # CDP debugger
│   ├── CMakeLists.txt          #   Debugger build file
│   └── src/
│       ├── main.cpp            #   Entry: creates QuickJS runtime + starts debug service
│       ├── websocket_server.h/cpp  # WebSocket server (port 9229)
│       ├── cdp_handler.h/cpp   #   CDP message dispatch & handling
│       ├── debug_session.h/cpp #   Debug core: breakpoints, stepping, pause, variable capture
│       ├── json.h              #   Lightweight JSON parse/serialize
│       └── platform.h          #   Cross-platform socket abstraction
├── compat/
│   └── msvc/
│       └── stdatomic.h         # C11 stdatomic shim for MSVC < 17.5
└── .vscode/
    └── launch.json             # VS Code debug attach configuration
```

## Architecture Overview

```
  VS Code (DevTools Frontend)
       │
       │  WebSocket (ws://127.0.0.1:9229/debug)
       ▼
  ┌─────────────────┐
  │ WebSocketServer │  Accept connections, WebSocket handshake, frame encode/decode
  └────────┬────────┘
           │
  ┌────────▼────────┐
  │   CDPHandler    │  Parse CDP JSON messages, dispatch to domain handlers
  └────────┬────────┘
           │
  ┌────────▼────────┐
  │  DebugSession   │  Core debug logic
  │  - Breakpoints  │  set/remove breakpoint
  │  - Stepping     │  step over / into / out / continue
  │  - Pause/Resume │  pause / resume
  │  - Call Stack   │  capture call frames + scope variables
  │  - debug_break  │  OP_debug callback (check breakpoint/step)
  └────────┬────────┘
           │
  ┌────────▼────────┐
  │  QuickJS Engine │  Execute JS scripts, trigger debug callbacks
  │  - JS_SetDebugBreakHandler()      set/clear debug break callback
  │  - OP_debug opcode                fires callback at statement boundaries
  │  - JS_GetStackDepth()             get call stack depth
  │  - JS_GetLocalVariablesAtLevel()  get locals at a given frame level
  │  - JS_FreeLocalVariables()        free variable array
  └─────────────────┘
```

### Debug Flow

1. `qjs_debug` starts → loads script → starts WebSocket server on port 9229, waits for debugger connection
2. VS Code connects to `ws://127.0.0.1:9229/debug` via the attach configuration in `launch.json`
3. VS Code sends CDP commands such as `Debugger.enable` and `Debugger.setBreakpointByUrl`
4. `CDPHandler` parses messages and calls `DebugSession` to set breakpoints
5. `Runtime.runIfWaitingForDebugger` unblocks execution, script starts running
6. QuickJS hits `OP_debug` opcodes at statement boundaries → invokes the debug break callback → checks breakpoint/step conditions
7. When a breakpoint is hit, `do_pause()` captures the call stack and variables, sends a `Debugger.paused` event, and blocks
8. VS Code receives the paused event and displays the breakpoint location and variables; user actions (continue/step) send corresponding CDP commands
9. `DebugSession` receives the command and unblocks, script resumes execution

## Building

### Requirements

| Platform | Compiler                      | Other       |
|----------|-------------------------------|-------------|
| Windows  | MSVC (VS2019+) or MinGW-w64  | CMake ≥ 3.10 |
| Linux    | GCC or Clang                  | CMake ≥ 3.10 |

### Windows (MSVC)

Open a **Developer Command Prompt for VS** (or initialize the environment manually), then:

```cmd
mkdir build
cd build
cmake .. -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Debug
nmake
```

The output binary is `build/debugger/qjs_debug.exe`.

### Windows (MinGW)

```bash
mkdir build && cd build
cmake .. -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug
mingw32-make
```

### Linux

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

### Key Build Notes

- **MSVC < 17.5**: Automatically injects the `compat/msvc/stdatomic.h` shim to resolve the missing C11 `<stdatomic.h>`.
- **No compile-time flags required**: The debug interface uses a dedicated `OP_debug` opcode that is always emitted at statement boundaries. The runtime cost is virtually zero when no handler is set (a single `unlikely` branch). Attach a handler at any time via `JS_SetDebugBreakHandler()`. `DIRECT_DISPATCH` (computed goto) remains fully enabled.

## Usage

### Basic Usage

```bash
# Start debugger, wait for VS Code to connect before executing the script
./build/debugger/qjs_debug --inspect-brk test.js

# Start debugger, script runs immediately (only breakpoints are active)
./build/debugger/qjs_debug --inspect test.js
```

Output on startup:
```
Debugger listening on ws://127.0.0.1:9229/debug
Waiting for debugger to connect...
```

### Debugging in VS Code

1. Start `qjs_debug`:
   ```bash
   ./build/debugger/qjs_debug --inspect-brk test.js
   ```

2. Open the workspace in VS Code and ensure `.vscode/launch.json` contains:
   ```json
   {
       "configurations": [{
           "type": "node",
           "request": "attach",
           "name": "Attach to QuickJS",
           "address": "127.0.0.1",
           "port": 9229,
           "localRoot": "${workspaceFolder}",
           "remoteRoot": "${workspaceFolder}",
           "skipFiles": ["<node_internals>/**"]
       }]
   }
   ```

3. Set breakpoints in `test.js`

4. Press `F5` and select "Attach to QuickJS"

5. Once VS Code connects, the script begins execution. It pauses at breakpoints automatically, allowing you to inspect variables, view the call stack, and step through code.

### Command-Line Arguments

| Argument          | Description                                                        |
|-------------------|--------------------------------------------------------------------|
| `--inspect`       | Start debug service; script runs immediately                       |
| `--inspect-brk`   | Start debug service; wait for debugger to connect and resume       |
| `--port <N>`      | Specify WebSocket listen port (default: 9229)                      |

### Debugging with Chrome DevTools

In addition to VS Code, you can also debug with Chrome:

1. Start `qjs_debug --inspect-brk test.js`
2. Open Chrome and navigate to `chrome://inspect`
3. Find the target under "Remote Target" and click "inspect"

## Supported CDP Commands

| Domain    | Methods                           |
|-----------|-----------------------------------|
| Debugger  | `enable` / `disable`              |
| Debugger  | `setBreakpointByUrl`              |
| Debugger  | `removeBreakpoint`                |
| Debugger  | `setBreakpointsActive`            |
| Debugger  | `getScriptSource`                 |
| Debugger  | `resume`                          |
| Debugger  | `stepOver` / `stepInto` / `stepOut` |
| Debugger  | `pause`                           |
| Debugger  | `setPauseOnExceptions`            |
| Runtime   | `enable`                          |
| Runtime   | `runIfWaitingForDebugger`         |
| Runtime   | `getProperties`                   |
| Runtime   | `evaluate`                        |
| Profiler  | `enable` / `disable`              |

## QuickJS Debug API

Extended QuickJS APIs this project depends on (declared in `quickjs.h`):

All debug APIs are always available — no compile-time flags required.

```c
// Debug break callback — invoked at statement boundaries when the interpreter
// hits an OP_debug opcode. Return 0 to continue, non-zero to raise exception.
typedef int JSDebugBreakFunc(JSContext *ctx,
                             const char *filename, const char *funcname,
                             int line, int col);

// Set (or clear) the debug break handler. When the interpreter hits an
// OP_debug opcode and a handler is set, it is called. Pass NULL to disable.
// Works with any context (JS_NewContext, JS_NewContextRaw, etc.).
void JS_SetDebugBreakHandler(JSContext *ctx, JSDebugBreakFunc *cb);

// Get current call stack depth
int JS_GetStackDepth(JSContext *ctx);

// Get local variables at a given stack frame level
JSDebugLocalVar *JS_GetLocalVariablesAtLevel(JSContext *ctx, int level, int *pcount);

// Free the variable array returned by JS_GetLocalVariablesAtLevel
void JS_FreeLocalVariables(JSContext *ctx, JSDebugLocalVar *vars, int count);
```

## Notes

- The `quickjs/` directory contains a modified fork of [quickjs-ng/quickjs](https://github.com/quickjs-ng/quickjs) with the debug interface additions ([PR #1421](https://github.com/quickjs-ng/quickjs/pull/1421)). `OP_debug` opcodes are always emitted; the runtime cost is virtually zero when no handler is set.
- On Windows, file paths are case-insensitive; the debugger handles normalization internally.
- In `--inspect-brk` mode, the program blocks until a debugger connects and sends `runIfWaitingForDebugger`.
