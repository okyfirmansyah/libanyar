#pragma once

// LibAnyar — Command Registry
// Maps command names to C++ handler functions

#include <anyar/types.h>
#include <mutex>
#include <string>
#include <unordered_map>

namespace anyar {

class CommandRegistry {
public:
    CommandRegistry() = default;

    /// Register a synchronous command handler
    void add(const std::string& name, CommandHandler handler);

    /// Register an async command handler.
    /// The handler receives a `reply(data, error)` callback that it may call
    /// synchronously or later — from any fiber or thread (e.g. when a
    /// worker-pool job finishes).  The IPC caller waits (fiber-suspended)
    /// until reply is called.  If every copy of `reply` is destroyed without
    /// being called, the caller gets a "did not complete" error.  Calls after
    /// the first are ignored.
    void add_async(const std::string& name, AsyncCommandHandler handler);

    /// Check if a command exists
    bool has(const std::string& name) const;

    /// Dispatch a command and wait for its result.  Unknown commands and
    /// handler exceptions are reported in IpcResponse::error (never thrown).
    /// For async handlers this suspends the calling fiber (or blocks a plain
    /// thread) until the handler replies.
    IpcResponse dispatch(const IpcRequest& request);

private:
    struct Entry {
        CommandHandler sync_handler;
        AsyncCommandHandler async_handler;
        bool is_async = false;
    };

    std::unordered_map<std::string, Entry> commands_;
};

} // namespace anyar
