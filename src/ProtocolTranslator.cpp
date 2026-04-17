#include "ProtocolTranslator.hpp"
#include "json.hpp"
#include "Logger.hpp"

using json = nlohmann::json;

namespace MyServer {

std::string ProtocolTranslator::translateRequest(const std::string& anthropicReq, const std::string& targetModel) {
    try {
        json anthropicJson = json::parse(anthropicReq);
        json openaiJson;
        
        // 1. 注入目标模型
        openaiJson["model"] = targetModel;
        openaiJson["messages"] = json::array();
        
        // 2. 转换 System Prompt
        // Anthropic 格式：顶级 "system" 字段，可以是字符串或数组
        // OpenAI 格式：messages 数组的第一项 role=system
        if (anthropicJson.contains("system")) {
            std::string systemText;
            if (anthropicJson["system"].is_string()) {
                systemText = anthropicJson["system"].get<std::string>();
            } else if (anthropicJson["system"].is_array()) {
                for (const auto& item : anthropicJson["system"]) {
                    if (item.contains("text") && item["text"].is_string()) {
                        if (!systemText.empty()) systemText += "\n";
                        systemText += item["text"].get<std::string>();
                    }
                }
            }
            if (!systemText.empty()) {
                openaiJson["messages"].push_back({
                    {"role", "system"},
                    {"content", systemText}
                });
            }
        }
        
        // 3. 转换消息历史
        // Claude 的 content 可以是字符串或结构化数组（含 text/tool_use/tool_result/thinking）
        // 需要精确映射到 OpenAI 格式
        if (anthropicJson.contains("messages")) {
            for (const auto& msg : anthropicJson["messages"]) {
                std::string role = msg.value("role", "");
                
                // 情况 A：content 是纯字符串 → 原样透传
                if (msg["content"].is_string()) {
                    openaiJson["messages"].push_back({
                        {"role", role},
                        {"content", msg["content"]}
                    });
                    continue;
                }
                
                // 情况 B：content 是结构化数组 → 需要逐项分析
                if (msg["content"].is_array()) {
                    if (role == "assistant") {
                        // 提取文本和工具调用
                        std::string textContent;
                        json toolCalls = json::array();
                        
                        for (const auto& item : msg["content"]) {
                            std::string type = item.value("type", "");
                            if (type == "text") {
                                textContent += item.value("text", "");
                            } else if (type == "tool_use") {
                                json tc = {
                                    {"id", item.value("id", "")},
                                    {"type", "function"},
                                    {"function", {
                                        {"name", item.value("name", "")},
                                        {"arguments", item.contains("input") ? item["input"].dump() : "{}"}
                                    }}
                                };
                                toolCalls.push_back(tc);
                            }
                            // thinking 类型直接跳过，不传给后端
                        }
                        
                        json assistantMsg = {{"role", "assistant"}};
                        if (!textContent.empty()) {
                            assistantMsg["content"] = textContent;
                        } else {
                            assistantMsg["content"] = nullptr;
                        }
                        if (!toolCalls.empty()) {
                            assistantMsg["tool_calls"] = toolCalls;
                        }
                        openaiJson["messages"].push_back(assistantMsg);
                        
                    } else if (role == "user") {
                        // 用户消息可能包含 text 和 tool_result
                        std::string textContent;
                        
                        for (const auto& item : msg["content"]) {
                            std::string type = item.value("type", "");
                            if (type == "tool_result") {
                                // tool_result → OpenAI 的 role=tool 消息
                                std::string resultContent;
                                if (item.contains("content")) {
                                    if (item["content"].is_string()) {
                                        resultContent = item["content"].get<std::string>();
                                    } else if (item["content"].is_array()) {
                                        for (const auto& ci : item["content"]) {
                                            if (ci.value("type", "") == "text") {
                                                resultContent += ci.value("text", "");
                                            }
                                        }
                                    }
                                }
                                openaiJson["messages"].push_back({
                                    {"role", "tool"},
                                    {"tool_call_id", item.value("tool_use_id", "")},
                                    {"content", resultContent}
                                });
                            } else if (type == "text") {
                                textContent += item.value("text", "");
                            }
                        }
                        
                        // 如果有纯文本内容，单独作为 user 消息
                        if (!textContent.empty()) {
                            openaiJson["messages"].push_back({
                                {"role", "user"},
                                {"content", textContent}
                            });
                        }
                    }
                }
            }
        }
        
        // 4. 转换工具定义
        // Claude: tools[].input_schema → OpenAI: tools[].function.parameters
        if (anthropicJson.contains("tools") && anthropicJson["tools"].is_array()) {
            json openaiTools = json::array();
            for (const auto& tool : anthropicJson["tools"]) {
                json openaiTool = {
                    {"type", "function"},
                    {"function", {
                        {"name", tool.value("name", "")},
                        {"description", tool.value("description", "")},
                        {"parameters", tool.value("input_schema", json::object())}
                    }}
                };
                openaiTools.push_back(openaiTool);
            }
            openaiJson["tools"] = openaiTools;
        }
        
        // 5. 强制开启流式输出
        openaiJson["stream"] = true;
        
        return openaiJson.dump(); 
    } catch (const std::exception& e) {
        LOG_ERROR << "Request JSON 解析失败: " << e.what();
        return ""; 
    }
}

std::string ProtocolTranslator::translateSseEvent(const std::string& openaiSseData, StreamState& state) {
    // 去掉 SSE "data: " 前缀
    std::string jsonStr = openaiSseData;
    if (jsonStr.substr(0, 6) == "data: ") {
        jsonStr = jsonStr.substr(6);
    }
    // 去掉首尾空白
    while (!jsonStr.empty() && (jsonStr.front() == '\n' || jsonStr.front() == '\r' || jsonStr.front() == ' ')) jsonStr.erase(jsonStr.begin());
    while (!jsonStr.empty() && (jsonStr.back() == '\n' || jsonStr.back() == '\r' || jsonStr.back() == ' ')) jsonStr.pop_back();

    // 【边界情况】：流式传输结束标志
    if (jsonStr == "[DONE]") {
        std::string result;
        // 关闭所有还在打开的 content block
        if (state.thinkingBlockOpen) {
            json stop = {{"type", "content_block_stop"}, {"index", state.nextContentIndex - 1}};
            result += "event: content_block_stop\ndata: " + stop.dump() + "\n\n";
            state.thinkingBlockOpen = false;
        }
        if (state.textBlockOpen) {
            json stop = {{"type", "content_block_stop"}, {"index", state.nextContentIndex - 1}};
            result += "event: content_block_stop\ndata: " + stop.dump() + "\n\n";
            state.textBlockOpen = false;
        }
        json messageDelta = {
            {"type", "message_delta"},
            {"delta", {{"stop_reason", "end_turn"}}},
            {"usage", {{"input_tokens", 0}, {"output_tokens", 0}}}
        };
        result += "event: message_delta\ndata: " + messageDelta.dump() + "\n\n";
        result += "event: message_stop\ndata: {\"type\": \"message_stop\"}\n\n";
        return result;
    }

    if (jsonStr.empty()) return "";

    try {
        json openaiJson = json::parse(jsonStr);
        if (!openaiJson.contains("choices") || openaiJson["choices"].empty()) {
            return ""; // 忽略心跳包或空数据
        }

        auto& choice = openaiJson["choices"][0];
        auto& delta = choice["delta"];
        std::string result;

        // ============================================================
        // 【步骤 1】：如果还没发过 message_start，现在发
        // ============================================================
        if (!state.messageStarted) {
            state.messageStarted = true;

            std::string role = "assistant";
            if (delta.contains("role") && delta["role"].is_string()) {
                role = delta["role"].get<std::string>();
            }

            json messageStart = {
                {"type", "message_start"},
                {"message", {
                    {"id", openaiJson.value("id", "msg_proxy_001")},
                    {"type", "message"},
                    {"role", role},
                    {"content", json::array()},
                    {"model", openaiJson.value("model", "unknown")},
                    {"usage", {
                        {"input_tokens", 0},
                        {"output_tokens", 0},
                        {"cache_creation_input_tokens", 0},
                        {"cache_read_input_tokens", 0}
                    }}
                }}
            };
            result += "event: message_start\ndata: " + messageStart.dump() + "\n\n";
        }

        // ============================================================
        // 【步骤 2】：处理 reasoning_content（思考文本）
        // ============================================================
        if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string()) {
            std::string thinking = delta["reasoning_content"].get<std::string>();
            if (!thinking.empty()) {
                // 如果 thinking block 还没打开，打开它
                if (!state.thinkingBlockOpen) {
                    json blockStart = {
                        {"type", "content_block_start"},
                        {"index", state.nextContentIndex},
                        {"content_block", {{"type", "thinking"}, {"thinking", ""}}}
                    };
                    result += "event: content_block_start\ndata: " + blockStart.dump() + "\n\n";
                    state.thinkingBlockOpen = true;
                    state.nextContentIndex++;
                }
                // 发送 thinking delta
                json thinkingDelta = {
                    {"type", "content_block_delta"},
                    {"index", state.nextContentIndex - 1},
                    {"delta", {{"type", "thinking_delta"}, {"thinking", thinking}}}
                };
                result += "event: content_block_delta\ndata: " + thinkingDelta.dump() + "\n\n";
            }
        }

        // ============================================================
        // 【步骤 3】：处理 content（正式输出文本）
        // ============================================================
        if (delta.contains("content") && delta["content"].is_string()) {
            std::string text = delta["content"].get<std::string>();
            if (!text.empty()) {
                // 如果 thinking block 还开着，先关闭它
                if (state.thinkingBlockOpen) {
                    json stop = {{"type", "content_block_stop"}, {"index", state.nextContentIndex - 1}};
                    result += "event: content_block_stop\ndata: " + stop.dump() + "\n\n";
                    state.thinkingBlockOpen = false;
                }
                // 如果 text block 还没打开，打开它
                if (!state.textBlockOpen) {
                    json blockStart = {
                        {"type", "content_block_start"},
                        {"index", state.nextContentIndex},
                        {"content_block", {{"type", "text"}, {"text", ""}}}
                    };
                    result += "event: content_block_start\ndata: " + blockStart.dump() + "\n\n";
                    state.textBlockOpen = true;
                    state.nextContentIndex++;
                }
                // 发送 text delta
                json textDelta = {
                    {"type", "content_block_delta"},
                    {"index", state.nextContentIndex - 1},
                    {"delta", {{"type", "text_delta"}, {"text", text}}}
                };
                result += "event: content_block_delta\ndata: " + textDelta.dump() + "\n\n";
            }
        }

        // ============================================================
        // 【步骤 3.5】：处理 tool_calls（工具调用）
        // OpenAI: delta.tool_calls[{index, id, function:{name, arguments}}]
        // Claude: content_block type=tool_use + input_json_delta
        // ============================================================
        if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
            for (const auto& tc : delta["tool_calls"]) {
                int tcIndex = tc.value("index", 0);

                // 新工具调用开始（带 id 字段表示是第一个 chunk）
                if (tc.contains("id")) {
                    // 先关闭之前打开的 block
                    if (state.thinkingBlockOpen) {
                        json stop = {{"type", "content_block_stop"}, {"index", state.nextContentIndex - 1}};
                        result += "event: content_block_stop\ndata: " + stop.dump() + "\n\n";
                        state.thinkingBlockOpen = false;
                    }
                    if (state.textBlockOpen) {
                        json stop = {{"type", "content_block_stop"}, {"index", state.nextContentIndex - 1}};
                        result += "event: content_block_stop\ndata: " + stop.dump() + "\n\n";
                        state.textBlockOpen = false;
                    }

                    // 开新的 tool_use content block
                    std::string toolName;
                    if (tc.contains("function") && tc["function"].contains("name")) {
                        toolName = tc["function"]["name"].get<std::string>();
                    }
                    json blockStart = {
                        {"type", "content_block_start"},
                        {"index", state.nextContentIndex},
                        {"content_block", {
                            {"type", "tool_use"},
                            {"id", tc["id"]},
                            {"name", toolName},
                            {"input", json::object()}
                        }}
                    };
                    result += "event: content_block_start\ndata: " + blockStart.dump() + "\n\n";
                    state.hasToolCalls = true;
                    state.nextContentIndex++;
                }

                // 发送参数增量
                if (tc.contains("function") && tc["function"].contains("arguments")) {
                    std::string args = tc["function"]["arguments"].get<std::string>();
                    if (!args.empty()) {
                        json argDelta = {
                            {"type", "content_block_delta"},
                            {"index", state.nextContentIndex - 1},
                            {"delta", {
                                {"type", "input_json_delta"},
                                {"partial_json", args}
                            }}
                        };
                        result += "event: content_block_delta\ndata: " + argDelta.dump() + "\n\n";
                    }
                }
            }
        }

        // ============================================================
        // 【步骤 4】：处理 finish_reason（生成结束）
        // ============================================================
        if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
            std::string finishReason = choice["finish_reason"].get<std::string>();

            // 关闭所有打开的 block
            if (state.thinkingBlockOpen) {
                json stop = {{"type", "content_block_stop"}, {"index", state.nextContentIndex - 1}};
                result += "event: content_block_stop\ndata: " + stop.dump() + "\n\n";
                state.thinkingBlockOpen = false;
            }
            if (state.textBlockOpen) {
                json stop = {{"type", "content_block_stop"}, {"index", state.nextContentIndex - 1}};
                result += "event: content_block_stop\ndata: " + stop.dump() + "\n\n";
                state.textBlockOpen = false;
            }
            // tool_use block 也需要关闭
            if (state.hasToolCalls) {
                json stop = {{"type", "content_block_stop"}, {"index", state.nextContentIndex - 1}};
                result += "event: content_block_stop\ndata: " + stop.dump() + "\n\n";
            }

            // 映射 stop_reason：tool_calls → tool_use
            std::string stopReason = (finishReason == "tool_calls") ? "tool_use" : "end_turn";
            json messageDelta = {
                {"type", "message_delta"},
                {"delta", {{"stop_reason", stopReason}}},
                {"usage", {{"input_tokens", 0}, {"output_tokens", 0}}}
            };
            result += "event: message_delta\ndata: " + messageDelta.dump() + "\n\n";
        }

        return result;

    } catch (const std::exception& e) {
        LOG_ERROR << "SSE JSON 翻译失败: " << e.what() << " | 原始数据: " << openaiSseData;
    }

    return "";
}

} // namespace MyServer