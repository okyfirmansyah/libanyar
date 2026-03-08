#include <anyar/command_registry.h>
#include <stdexcept>

namespace anyar {

void CommandRegistry::add(const std::string& name, CommandHandler handler) {
    Entry entry;
    entry.sync_handler = std::move(handler);
    entry.is_async = false;
    commands_[name] = std::move(entry);
}

void CommandRegistry::add_async(const std::string& name, AsyncCommandHandler handler) {
    Entry entry;
    entry.async_handler = std::move(handler);
    entry.is_async = true;
    commands_[name] = std::move(entry);
}

bool CommandRegistry::has(const std::string& name) const {
    return commands_.find(name) != commands_.end();
}

IpcResponse CommandRegistry::dispatch(const IpcRequest& request) {
    IpcResponse response;
    response.id = request.id;

    auto it = commands_.find(request.cmd);
    if (it == commands_.end()) {
        response.error = "Command '" + request.cmd + "' not registered";
        return response;
    }

    auto& entry = it->second;

    try {
        if (entry.is_async) {
            // For async commands dispatched synchronously (via HTTP),
            // we use a fiber-based promise/future to bridge
            bool done = false;
            json result_data;
            std::string result_error;

            entry.async_handler(request.args, [&](const json& data, const std::string& error) {
                result_data = data;
                result_error = error;
                done = true;
            });

            // In fiber context, the async handler should call reply synchronously
            // (since fibers make async look sync). If it hasn't replied yet,
            // that's an error.
            if (!done) {
                response.error = "Async command '" + request.cmd + "' did not complete synchronously";
                return response;
            }

            response.data = result_data;
            response.error = result_error;
        } else {
            response.data = entry.sync_handler(request.args);
        }
    } catch (const std::exception& e) {
        response.error = std::string("Command '") + request.cmd + "' failed: " + e.what();
    } catch (...) {
        response.error = std::string("Command '") + request.cmd + "' failed with unknown error";
    }

    return response;
}

} // namespace anyar
