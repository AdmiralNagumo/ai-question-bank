import { Agent, type AgentMessage, type AgentTool } from '@earendil-works/pi-agent-core';
import { createModels, fauxAssistantMessage, fauxProvider, Type,
         type AssistantMessage, type Model } from '@earendil-works/pi-ai';
import { deepseekProvider } from '@earendil-works/pi-ai/providers/deepseek';
import { setTimeout as delay } from 'node:timers/promises';
import type { Config } from './config.js';
import { ApiError, type Submission } from './contracts.js';
import { teachingInstructions, type TeachingContext } from './teaching.js';
import { linearSolver, linearVerifier } from './math-tools.js';
import { answerOnlyFeedback } from './answer-review.js';

const calculatorParameters = Type.Object({
  operation: Type.Union(['add', 'subtract', 'multiply', 'divide', 'power'].map(value => Type.Literal(value))),
  a: Type.Number({ minimum: -1e12, maximum: 1e12 }),
  b: Type.Number({ minimum: -1e12, maximum: 1e12 }),
}, { additionalProperties: false });

export const calculator: AgentTool<typeof calculatorParameters, { value: number }> = {
  name: 'arithmetic', label: '算术计算', description: '对两个数执行一次算术计算。',
  parameters: calculatorParameters,
  async execute(_id, { operation, a, b }) {
    let value: number;
    switch (operation) {
      case 'add': value = a + b; break;
      case 'subtract': value = a - b; break;
      case 'multiply': value = a * b; break;
      case 'divide': if (b === 0) throw new Error('除数不能为零'); value = a / b; break;
      case 'power': value = a ** b; break;
      default: throw new Error('不支持的运算');
    }
    if (!Number.isFinite(value)) throw new Error('计算结果超出范围');
    return { content: [{ type: 'text', text: String(value) }], details: { value } };
  },
};

export interface Completion {
  reply: string;
  model: string;
  messages: AgentMessage[];
  usage: { input_tokens: number; output_tokens: number; model_calls: number; tool_calls: number };
}
export interface AgentRun {
  completion: Promise<Completion>;
  cancel(): void;
}

export function startAgent(config: Config, request: Submission, history: AgentMessage[], teaching: TeachingContext): AgentRun {
  const models = createModels(); // 默认仅内存凭证，不读取个人 Pi 的配置或扩展。
  let model: Model<string>;
  if (config.mock) {
    const mock = fauxProvider({ provider: 'question-bank-mock', models: [{ id: 'mock' }] });
    mock.setResponses([async (context, options) => {
      await delay(config.mockDelayMs, undefined, { signal: options?.signal });
      const round = context.messages.filter(message => message.role === 'user').length;
      return fauxAssistantMessage(`Mock 解答（第 ${round} 轮；模式 ${teaching.mode}${teaching.hintLevel ? `；提示 ${teaching.hintLevel} 级` : ''}）\n这是接口演示，未执行真实解题。\n题目：${request.question.content}\n提问：${request.message}`);
    }]);
    models.setProvider(mock.provider);
    model = mock.getModel();
  } else {
    const provider = deepseekProvider();
    models.setProvider(provider);
    // 显式模型定义允许接入兼容供应商和测试服务，不依赖全局 CLI 模型目录。
    model = {
      id: config.model, name: config.model, provider: provider.id,
      api: 'openai-completions', baseUrl: config.baseUrl, reasoning: false,
      input: ['text'], contextWindow: 128000, maxTokens: config.maxOutputTokens,
      cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0 },
    };
  }
  let modelCalls = 0;
  let toolCalls = 0;
  let inputTokens = 0;
  let outputTokens = 0;
  const visibleSteps: string[] = [];
  let lastStatus = 0;
  let limited = false;
  let timedOut = false;
  const tools: AgentTool[] = teaching.mode === 'hint' ? [calculator] : [calculator, linearSolver, linearVerifier];
  const agent = new Agent({
    initialState: { systemPrompt: teachingInstructions(teaching, tools.map(tool => tool.name)), model, tools,
                    messages: structuredClone(history), thinkingLevel: 'off' },
    streamFn: (selectedModel, context, options) => models.streamSimple(selectedModel, context, {
      ...options, apiKey: config.mock ? 'mock' : config.apiKey,
      maxTokens: config.maxOutputTokens, timeoutMs: config.timeoutMs,
      maxRetries: 0, maxRetryDelayMs: 1000,
      fetch: async (input, init) => {
        const response = await fetch(input, init);
        lastStatus = response.status; // SDK 的 onResponse 不覆盖抛出异常的 4xx/5xx。
        return response;
      },
    }),
    beforeToolCall: async () => {
      toolCalls++;
      if (toolCalls > config.maxToolCalls) {
        limited = true;
        return { block: true, reason: '工具调用次数已达到上限', terminate: true };
      }
      return undefined;
    },
    shouldStopAfterTurn: ({ message }) => {
      if (message.content.some(item => item.type === 'toolCall') && modelCalls >= config.maxModelCalls) {
        limited = true;
      }
      return limited;
    },
    toolExecution: 'sequential',
  });
  const unsubscribe = agent.subscribe(event => {
    if (event.type === 'turn_start') modelCalls++;
    if (event.type === 'turn_end' && event.message.role === 'assistant') {
      inputTokens += event.message.usage.input + event.message.usage.cacheRead + event.message.usage.cacheWrite;
      outputTokens += event.message.usage.output;
      const text = event.message.content.filter(item => item.type === 'text').map(item => item.text).join('\n').trim();
      if (text) visibleSteps.push(text); // 保留本轮工具调用前后的教学文字；不收集thinking或历史轮次。
    }
  });
  const completion = (async (): Promise<Completion> => {
    const timer = setTimeout(() => { timedOut = true; agent.abort(); }, config.timeoutMs);
    try {
      await agent.prompt(JSON.stringify({ question: request.question, message: request.message,
        ...(request.student_answer !== undefined ? { student_answer: request.student_answer } : {}) }));
      if (timedOut) throw new ApiError(504, 'AI_TIMEOUT', '解答超时，请稍后重试');
      if (limited) throw new ApiError(502, 'AI_CALL_LIMIT', '解答达到执行次数上限');
      const last = agent.state.messages.findLast(message => message.role === 'assistant') as AssistantMessage | undefined;
      if (!last || last.stopReason === 'error' || last.stopReason === 'aborted') {
        if (lastStatus === 429) throw new ApiError(503, 'AI_RATE_LIMITED', '模型服务繁忙，请稍后重试');
        throw new ApiError(502, 'AI_UPSTREAM_ERROR', '模型服务暂时不可用');
      }
      if (last.stopReason === 'length') throw new ApiError(502, 'AI_OUTPUT_LIMIT', '解答超过输出长度限制');
      const finalText = last.content.filter(item => item.type === 'text').map(item => item.text).join('\n');
      if (!finalText.trim()) throw new ApiError(502, 'AI_INVALID_RESPONSE', '模型未返回可用解答');
      const checkedAnswer = teaching.mode === 'review' ? answerOnlyFeedback(request) : undefined;
      const reply = checkedAnswer ?? visibleSteps.join('\n\n');
      const messages = structuredClone(agent.state.messages);
      if (checkedAnswer) {
        // 工具调用对保持完整；当前轮次不可靠的文字不进入后续追问历史。
        for (const message of messages.slice(history.length)) {
          if (message.role === 'assistant') message.content = message.content.filter(item => item.type === 'toolCall');
        }
        const final = messages.findLast(message => message.role === 'assistant') as AssistantMessage;
        final.content.push({ type: 'text', text: reply });
      }
      return { reply, model: config.mock ? 'mock' : config.model,
               messages,
               usage: { input_tokens: inputTokens, output_tokens: outputTokens,
                        model_calls: modelCalls, tool_calls: toolCalls } };
    } catch (error) {
      if (timedOut) throw new ApiError(504, 'AI_TIMEOUT', '解答超时，请稍后重试');
      if (error instanceof ApiError) throw error;
      throw new ApiError(502, 'AI_UPSTREAM_ERROR', '模型服务暂时不可用');
    } finally {
      clearTimeout(timer);
      unsubscribe();
    }
  })();
  return { completion, cancel: () => agent.abort() };
}
