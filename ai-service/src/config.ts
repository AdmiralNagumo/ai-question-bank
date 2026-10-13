export interface Config {
  host: string;
  port: number;
  serviceToken: string;
  mock: boolean;
  mockDelayMs: number;
  apiKey: string;
  baseUrl: string;
  model: string;
  timeoutMs: number;
  maxModelCalls: number;
  maxToolCalls: number;
  maxOutputTokens: number;
  concurrency: number;
  queueLimit: number;
  ttlMs: number;
  maxConversations: number;
}

function integer(env: NodeJS.ProcessEnv, name: string, fallback: number,
                 min: number, max: number): number {
  const value = env[name] === undefined ? fallback : Number(env[name]);
  if (!Number.isSafeInteger(value) || value < min || value > max) {
    throw new Error(`${name} must be an integer between ${min} and ${max}`);
  }
  return value;
}

export function loadConfig(env: NodeJS.ProcessEnv = process.env): Config {
  const serviceToken = env.AI_SERVICE_TOKEN ?? '';
  if (!/^[A-Za-z0-9_-]{32,128}$/.test(serviceToken)) {
    throw new Error('AI_SERVICE_TOKEN must contain 32-128 ASCII letters, digits, _ or -');
  }
  const mock = env.AI_MOCK === '1';
  const apiKey = env.AI_API_KEY ?? '';
  if (!mock && !apiKey) throw new Error('AI_API_KEY is required in real mode');
  const baseUrl = env.AI_API_BASE_URL ?? 'https://api.deepseek.com';
  const url = new URL(baseUrl);
  if (!['https:', 'http:'].includes(url.protocol) || url.username || url.password ||
      url.search || url.hash) throw new Error('Invalid AI_API_BASE_URL');
  if (url.protocol === 'http:' && !['127.0.0.1', 'localhost', '[::1]'].includes(url.hostname)) {
    throw new Error('Plain HTTP model endpoints are only allowed on loopback');
  }
  const model = env.AI_MODEL ?? 'deepseek-flash';
  if (!model || model.length > 200) throw new Error('Invalid AI_MODEL');
  return {
    host: env.AI_SERVICE_HOST ?? '127.0.0.1',
    port: integer(env, 'AI_SERVICE_PORT', 8001, 0, 65535),
    serviceToken, mock, apiKey, baseUrl, model,
    mockDelayMs: integer(env, 'AI_MOCK_DELAY_MS', 40, 0, 30000),
    timeoutMs: integer(env, 'AI_TIMEOUT_MS', 30000, 50, 120000),
    maxModelCalls: integer(env, 'AI_MAX_MODEL_CALLS', 3, 1, 8),
    maxToolCalls: integer(env, 'AI_MAX_TOOL_CALLS', 6, 0, 16),
    maxOutputTokens: integer(env, 'AI_MAX_OUTPUT_TOKENS', 2048, 32, 8192),
    concurrency: integer(env, 'AI_CONCURRENCY', 4, 1, 32),
    queueLimit: integer(env, 'AI_QUEUE_LIMIT', 32, 0, 256),
    ttlMs: integer(env, 'AI_STATE_TTL_MS', 3600000, 1000, 86400000),
    maxConversations: integer(env, 'AI_MAX_CONVERSATIONS', 1000, 1, 10000),
  };
}
