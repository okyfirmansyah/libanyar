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

    /// Register an async command handler
    void add_async(const std::string& name, AsyncCommandHandler handler);

    /// Check if a command exists
    bool has(const std::string& name) const;

    /// Dispatch a command synchronously. Returns the result JSON.
    /// Throws std::runtime_error if command not found.
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
