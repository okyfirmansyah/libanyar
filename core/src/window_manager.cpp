#include <anyar/window_manager.h>
#include <anyar/main_thread.h>

#include <algorithm>
#include <mutex>
#include <stdexcept>

namespace anyar {

std::string WindowManager::create(const WindowCreateOptions& opts, int port) {
    {
        std::lock_guard<boost::fibers::mutex> lock(mutex_);
        if (windows_.count(opts.label)) {
            throw std::runtime_error(
                "Window label already exists: " + opts.label);
        }
    }

    // Create the window (must be on main thread)
    auto window = std::make_shared<Window>(opts, port);

    // Set up parent/child relationship
    if (!opts.parent.empty()) {
        Window* parent = get(opts.parent);
        if (parent) {
            window->set_parent(*parent);
            if (opts.modal) {
                window->set_modal(true);
            }
        }
    }

    // Install on_close handler to auto-remove from map
    std::string label = opts.label;
    window->set_on_close([this, label]() {
        // Schedule removal on next idle to avoid re-entry during signal
        // handling.  The shared_ptr in the map keeps the Window alive.
        std::string lbl = label;
        WindowClosedCallback notify;
        {
            std::lock_guard<boost::fibers::mutex> lock(mutex_);
            windows_.erase(lbl);
            notify = on_closed_;
        }
        if (notify) {
            notify(lbl);
        }
    });

    // Register in map
    {
        std::lock_guard<boost::fibers::mutex> lock(mutex_);
        windows_[opts.label] = window;
    }

    // Notify creation callback (setup_native_ipc binds IPC + scripts here)
    if (on_created_) {
        on_created_(*window, opts);
    }

    // NOW show the window — all setup (parent, modal, IPC, scripts) is done.
    // This calls webview_set_size → window_show() → gtk_container_add +
    // gtk_widget_show, making the window visible with all configuration applied.
    window->show();

    return opts.label;
}

Window* WindowManager::get(const std::string& label) {
    std::lock_guard<boost::fibers::mutex> lock(mutex_);
    auto it = windows_.find(label);
    if (it != windows_.end()) {
        return it->second.get();
    }
    return nullptr;
}

std::vector<std::string> WindowManager::labels() const {
    std::lock_guard<boost::fibers::mutex> lock(mutex_);
    std::vector<std::string> result;
    result.reserve(windows_.size());
    for (auto& [label, _] : windows_) {
        result.push_back(label);
    }
    return result;
}

size_t WindowManager::count() const {
    std::lock_guard<boost::fibers::mutex> lock(mutex_);
    return windows_.size();
}

void WindowManager::close(const std::string& label) {
    std::shared_ptr<Window> window;
    {
        std::lock_guard<boost::fibers::mutex> lock(mutex_);
        auto it = windows_.find(label);
        if (it == windows_.end()) return;
        window = it->second;
    }

    if (window && !window->is_destroyed()) {
        window->destroy();
    }
}

void WindowManager::close_all() {
    std::vector<std::shared_ptr<Window>> to_close;
    {
        std::lock_guard<boost::fibers::mutex> lock(mutex_);
        for (auto& [_, win] : windows_) {
            to_close.push_back(win);
        }
    }

    for (auto& win : to_close) {
        if (!win->is_destroyed()) {
            win->destroy();
        }
    }
}

void WindowManager::remove(const std::string& label) {
    WindowClosedCallback notify;
    {
        std::lock_guard<boost::fibers::mutex> lock(mutex_);
        windows_.erase(label);
        notify = on_closed_;
    }
    if (notify) {
        notify(label);
    }
}

void WindowManager::set_on_window_closed(WindowClosedCallback cb) {
    std::lock_guard<boost::fibers::mutex> lock(mutex_);
    on_closed_ = std::move(cb);
}

void WindowManager::set_on_window_created(WindowCreatedCallback cb) {
    std::lock_guard<boost::fibers::mutex> lock(mutex_);
    on_created_ = std::move(cb);
}

} // namespace anyar
