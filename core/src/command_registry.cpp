#include <anyar/command_registry.h>
#include <boost/fiber/future.hpp>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <utility>

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
            // The handler may call `reply` synchronously, or later from any
            // fiber or thread (e.g. after run_blocking() or a worker job).
            // We suspend this fiber until then.  If every copy of `reply` is
            // destroyed without being called, the handler can never answer:
            // ReplyState's destructor resolves the wait with an error.
            struct ReplyState {
                boost::fibers::promise<std::pair<json, std::string>> promise;
                std::atomic<bool> replied{false};
                std::string cmd;
                void resolve(json data, std::string error) {
                    if (!replied.exchange(true)) {
                        promise.set_value({std::move(data), std::move(error)});
                    }
                }
                ~ReplyState() {
                    resolve(nullptr, "Async command '" + cmd +
                        "' did not complete: reply callback dropped without being called");
                }
            };
            auto state = std::make_shared<ReplyState>();
            state->cmd = request.cmd;
            auto future = state->promise.get_future();

            // Hand the handler the only strong reference (inside the reply
            // closure) so that dropping `reply` destroys ReplyState.
            {
                CommandReply reply = [s = std::move(state)](const json& data,
                                                             const std::string& error) {
                    s->resolve(data, error);
                };
                entry.async_handler(request.args, std::move(reply));
            }

            auto [data, error] = future.get();
            response.data  = std::move(data);
            response.error = std::move(error);
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
