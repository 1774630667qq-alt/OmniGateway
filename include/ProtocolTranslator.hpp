/*
 * @Author: Zhang YuHua 1774630667@qq.com
 * @Date: 2026-04-13 18:48:44
 * @LastEditors: Zhang YuHua 1774630667@qq.com
 * @LastEditTime: 2026-04-13 18:50:48
 * @FilePath: /OmniGateway/include/ProtocolTranslator.hpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置
 * 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#pragma once
#include <string>

namespace MyServer {

/**
 * @brief SSE 流状态跟踪器
 * @details 用于在流式翻译过程中追踪当前处于哪个 content block 阶段
 */
struct StreamState {
    bool messageStarted = false;      ///< message_start 是否已发送
    bool thinkingBlockOpen = false;   ///< 当前是否有 thinking content block 打开
    bool textBlockOpen = false;       ///< 当前是否有 text content block 打开
    bool hasToolCalls = false;        ///< 是否有工具调用 content block 打开
    int nextContentIndex = 0;         ///< 下一个 content block 的序号
};

class ProtocolTranslator {
public:
  /**
   * @brief [请求层] 将 Claude 的请求格式翻译为 OpenAI 格式
   * @param anthropicReq Claude Code 发来的完整 JSON 请求体
   * @param targetModel 目标大模型名称 (如 "GLM-5")
   * @return 翻译后的 OpenAI 格式 JSON 字符串
   */
  static std::string translateRequest(const std::string &anthropicReq,
                                      const std::string &targetModel);

  /**
   * @brief [响应层] 将 OpenAI 的 SSE 数据流切片，翻译为 Claude 期望的 SSE 格式
   * @param openaiSseData 单条 OpenAI JSON 字符串 (或 "[DONE]")
   * @param state 流状态跟踪器，区分 thinking 和 text content block
   * @return 翻译后的 Anthropic SSE 报文
   */
  static std::string translateSseEvent(const std::string &openaiSseData,
                                       StreamState &state);
};

} // namespace MyServer