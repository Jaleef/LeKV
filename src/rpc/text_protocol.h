#ifndef TEXT_PROTOCOL_H_
#define TEXT_PROTOCOL_H_

#include <string>
#include <vector>
#include <optional>
#include <sstream>
#include <algorithm>

struct Command {
    std::string name;
    std::vector<std::string> args;

    std::string arg(size_t i) const {
        return i < args.size() ? args[i] : "";
    }
};

/*
Error Code
暂时使用string，方便传输，而不是enum

BAD_CMD:     命令格式不争取，无法解析
ARGS:        命令参数数量不正确
NOT_IMPL:    命令未实现
UNKNOWN:     未知的命令
NOT_FOUND:   命令的key未找到
 */

class TextProtocol {
public:
    // 解析一行文本命令 (CMD arg1 arg2 ...\r\n)
    static std::optional<Command> Parse(const std::string& line);

    // 编码响应
    static std::string Ok(const std::string& data = "");
    static std::string Err(const std::string& code, const std::string& msg);

    // 分割参数字符串 (处理空格分隔)
    static std::vector<std::string> SplitArgs(const std::string& line);

    // 分布式接口
    static std::string EncodeVoteRequest(uint64_t term, uint64_t candidate_id,
                                        uint64_t last_log_idx, uint64_t last_log_term);
    static std::string EncodeVoteResponse(uint64_t term, bool granted);
    static std::string EncodeAppendRequest(uint64_t term, uint64_t leader_id,
                                        uint64_t prev_idx, uint64_t prev_term,
                                        uint64_t leader_commit);
    static std::string EncodeAppendResponse(uint64_t term, bool success);
};

#endif  // TEXT_PROTOCOL_H_
