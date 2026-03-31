#include "text_protocol.h"


std::optional<Command> TextProtocol::Parse(const std::string& line) {
    if (line.empty()) {
        return std::nullopt;
    }

    // 去掉 \r\n
    std::string trimmed = line;
    if (!trimmed.empty() && trimmed.back() == '\n') trimmed.pop_back();
    if (!trimmed.empty() && trimmed.back() == '\r') trimmed.pop_back();
    if (trimmed.empty()) return std::nullopt;

    std::istringstream iss(trimmed);
    Command cmd;
    if (!(iss >> cmd.name)) return std::nullopt;

    // 统一转大写处理
    std::transform(cmd.name.begin(), cmd.name.end(), cmd.name.begin(), ::toupper);

    std::string arg;
    while (iss >> arg) {
        cmd.args.push_back(arg);
    }

    return cmd;
}

std::string TextProtocol::Ok(const std::string& data) {
    if (data.empty()) return "+OK\r\n";
    return "+OK " + data + "\r\n";
}

std::string TextProtocol::Err(const std::string& code, const std::string& msg) {
    return "-ERR " + code + " " + msg + "\r\n";
}