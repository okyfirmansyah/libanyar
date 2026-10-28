// TelemetryPlugin — polls the device and keeps per-dashboard sessions with
// a SharedBuffer each. (Customer code; crashes on exit sometimes.)

#include <anyar/plugin.h>
#include <anyar/shared_buffer.h>
#include <anyar/task.h>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <string>

#include "device.h"   // read_device_block(): blocking, ~5 ms

class TelemetryPlugin : public anyar::IAnyarPlugin {
public:
    std::string name() const override { return "telemetry"; }

    void initialize(anyar::PluginContext& ctx) override {
        service_ = ctx.service;
        events_ = &ctx.events;

        // Poll the device forever and push samples to every dashboard.
        service_->execute([this] {
            while (running_) {
                auto block = anyar::run_blocking(service_, [] { return read_device_block(); });
                for (auto& [id, buf] : sessions_) {
                    std::memcpy(buf->data(), block.data(), std::min(buf->size(), block.size()));
                    events_->emit("telemetry:frame", {{"buffer", buf->name()}, {"id", buf->id()}});
                }
            }
        });

        ctx.commands.add("telemetry:open", [this](const anyar::json& args) -> anyar::json {
            const std::string id = args.at("dashboard").get<std::string>();
            auto buf = anyar::SharedBuffer::create("telemetry-" + id, 1 << 20);
            sessions_[id] = buf;
            return {{"buffer", buf->name()}};
        });

        // The dashboard calls this (fire-and-forget) when a widget unmounts.
        ctx.commands.add("telemetry:release", [this](const anyar::json& args) -> anyar::json {
            const std::string id = args.at("dashboard").get<std::string>();
            auto it = sessions_.find(id);
            if (it != sessions_.end()) {
                anyar::SharedBufferRegistry::instance().remove(it->second->name());
                sessions_.erase(it);
            }
            return {{"ok", true}};
        });
    }

    void shutdown() override {
        running_ = false;
        for (auto& [id, buf] : sessions_) anyar::SharedBufferRegistry::instance().remove(buf->name());
        sessions_.clear();
    }

private:
    asyik::service_ptr service_;
    anyar::EventBus* events_ = nullptr;
    std::atomic<bool> running_{true};
    std::map<std::string, std::shared_ptr<anyar::SharedBuffer>> sessions_;
};
