#include <anyar/window.h>
#include "webview/webview.h"

#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace anyar {

struct Window::Impl {
    webview_t wv = nullptr;
    int server_port;

    // Pointers to bound callback closures — must outlive the webview
    std::vector<std::unique_ptr<Window::BindCallback>> bind_cbs;

    Impl(const WindowConfig& config, int port)
        : server_port(port)
    {
        wv = webview_create(config.debug ? 1 : 0, nullptr);
        if (!wv) {
            throw std::runtime_error("Failed to create webview instance");
        }
        webview_set_title(wv, config.title.c_str());
        webview_set_size(wv, config.width, config.height,
                         config.resizable ? WEBVIEW_HINT_NONE : WEBVIEW_HINT_FIXED);
    }

    ~Impl() {
        if (wv) {
            webview_destroy(wv);
            wv = nullptr;
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
};

Window::Window(const WindowConfig& config, int server_port)
    : impl_(std::make_unique<Impl>(config, server_port))
{
    // Navigate to the LibAsyik HTTP server
    std::ostringstream url;
    url << "http://127.0.0.1:" << server_port << "/";
    webview_navigate(impl_->wv, url.str().c_str());

    // Inject the port number so the JS bridge can find the IPC endpoint
    std::ostringstream init_js;
    init_js << "window.__LIBANYAR_PORT__ = " << server_port << ";";
    webview_init(impl_->wv, init_js.str().c_str());
}

Window::~Window() = default;

void Window::run() {
    webview_run(impl_->wv);
}

void Window::terminate() {
    webview_terminate(impl_->wv);
}

void Window::eval(const std::string& js) {
    webview_eval(impl_->wv, js.c_str());
}

void Window::navigate(const std::string& url) {
    webview_navigate(impl_->wv, url.c_str());
}

void Window::dispatch(std::function<void()> fn) {
    // Pack the std::function into a heap-allocated copy so we can pass it through void*
    auto* f = new std::function<void()>(std::move(fn));
    webview_dispatch(impl_->wv,
        [](webview_t /*w*/, void* arg) {
            auto* func = static_cast<std::function<void()>*>(arg);
            (*func)();
            delete func;
        },
        f);
}

void Window::set_title(const std::string& title) {
    webview_set_title(impl_->wv, title.c_str());
}

void Window::bind(const std::string& name, BindCallback callback) {
    auto cb = std::make_unique<BindCallback>(std::move(callback));
    auto* raw_ptr = cb.get();
    impl_->bind_cbs.push_back(std::move(cb));

    webview_bind(impl_->wv, name.c_str(),
        [](const char* seq, const char* req, void* arg) {
            auto* fn = static_cast<BindCallback*>(arg);
            (*fn)(std::string(seq), std::string(req));
        },
        raw_ptr);
}

void Window::return_result(const std::string& seq, int status,
                           const std::string& result) {
    webview_return(impl_->wv, seq.c_str(), status, result.c_str());
}

void Window::init(const std::string& js) {
    webview_init(impl_->wv, js.c_str());
}

} // namespace anyar
