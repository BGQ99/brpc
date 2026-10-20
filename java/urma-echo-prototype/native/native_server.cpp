#include "arguments.h"
#include "core.h"

#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

namespace {
volatile std::sig_atomic_t stopping = 0;
void Signal(int) { stopping = 1; }
}

int main(int argc, char** argv) {
    try {
        echo_proto::Arguments args(argc, argv);
        const int port = args.Int("port", 8000, 1, 65535);
        echo_proto::ConfigureUrma(args.Text("device", ""), args.Polling());
        echo_proto::ServerCore server(false, args.Int("capacity", 128, 1, 65536));
        server.Start(port, args.Text("transport", "tcp"));
        std::signal(SIGTERM, Signal);
        std::signal(SIGINT, Signal);
        std::cout << "SERVER_READY native port=" << port
                  << " transport_requested=" << args.Text("transport", "tcp")
                  << " path_verified=false" << std::endl;
        while (!stopping) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        server.Shutdown();
        std::cout << "SERVER_CLOSED=PASS" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "NATIVE_SERVER=FAIL " << error.what() << std::endl;
        return 1;
    }
}
