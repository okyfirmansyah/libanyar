#pragma once

// LibAnyar tests — pick a free local TCP port.
//
// Random picks from the ephemeral range are unsafe: the port may be in use,
// and Windows reserves whole blocks of 49152-65535 (Hyper-V/WSL).  bind()
// then throws inside the test fiber, svc->stop() is never reached and
// svc->run() blocks forever.  Let the OS choose instead.

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

namespace anyar_test {

/// A port on 127.0.0.1 that was free a moment ago (chosen by the OS).
inline int free_port() {
    namespace ip = boost::asio::ip;
    boost::asio::io_context io;
    ip::tcp::acceptor probe(io, ip::tcp::endpoint(ip::make_address("127.0.0.1"), 0));
    return probe.local_endpoint().port();
}

} // namespace anyar_test
