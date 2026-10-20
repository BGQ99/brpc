#pragma once
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace echo_proto {
class Arguments {
public:
    Arguments(int argc, char** argv) {
        const std::unordered_set<std::string> allowed = {
            "server", "transport", "size", "concurrency", "warmup", "seconds",
            "timeout-ms", "capacity", "port", "device", "polling", "group"
        };
        for (int i = 1; i < argc; ++i) {
            std::string arg(argv[i]);
            auto equals = arg.find('=');
            if (arg.substr(0, 2) != "--" || equals == std::string::npos) {
                throw std::invalid_argument("use --key=value arguments");
            }
            std::string key = arg.substr(2, equals - 2);
            if (!allowed.count(key) || !values_.emplace(key, arg.substr(equals + 1)).second) {
                throw std::invalid_argument("unknown or duplicate argument: " + key);
            }
        }
    }
    std::string Text(const std::string& key, const std::string& fallback) const {
        auto it = values_.find(key); return it == values_.end() ? fallback : it->second;
    }
    int Int(const std::string& key, int fallback, int low, int high) const {
        std::string text = Text(key, std::to_string(fallback));
        size_t end = 0;
        long value = std::stol(text, &end);
        if (end != text.size() || value < low || value > high) throw std::invalid_argument("invalid " + key);
        return static_cast<int>(value);
    }
    bool Polling() const {
        std::string text = Text("polling", "false");
        if (text != "true" && text != "false") throw std::invalid_argument("polling must be true or false");
        return text == "true";
    }
private:
    std::unordered_map<std::string, std::string> values_;
};
}
