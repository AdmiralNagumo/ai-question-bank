import test from 'node:test';
import assert from 'node:assert/strict';
import { once } from 'node:events';
import { createServer, type Server, type IncomingMessage, type ServerResponse } from 'node:http';
import { setTimeout as delay } from 'node:timers/promises';
import { loadConfig, type Config } from '../src/config.js';
import { createApp } from '../src/server.js';
import { Jobs } from '../src/jobs.js';
import { parseSubmission } from '../src/contracts.js';

const serviceToken = 'test-only-service-token-1234567890';
const question = { id: 1, content: '质量为 2 kg、加速度为 3 m/s^2，合力是多少？',
                   options: [], knowledge_points: ['牛顿第二定律'], type: 'short_answer' };
const submission = { user_id: 1, question, message: '请解释解法' };
function config(changes: Partial<Config> = {}): Config {
  return { ...loadConfig({ AI_SERVICE_TOKEN: serviceToken, AI_MOCK: '1', AI_SERVICE_PORT: '0',
                          AI_MOCK_DELAY_MS: '0' }), ...changes };
}
async function listening(server: Server): Promise<string> {
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const address = server.address();
  assert(address && typeof address === 'object');
  return `http://127.0.0.1:${address.port}`;
}
async function close(server: Server) {
  await new Promise<void>((resolve, reject) => {
    server.close(error => error ? reject(error) : resolve());
    server.closeAllConnections();
  });
}
async function usingApp(settings: Config, fn: (base: string) => Promise<void>) {
  const server = createApp(settings);
  try { await fn(await listening(server)); }
  finally { await close(server); }
}
async function request(base: string, path: string, method = 'GET', value?: unknown,
                       owner = 1, token: string | null = serviceToken) {
  const headers: Record<string, string> = { 'X-User-Id': String(owner) };
  if (token !== null) headers.Authorization = 'Bearer ' + token;
  if (value !== undefined) headers['Content-Type'] = 'application/json';
  const response = await fetch(base + path, { method, headers,
    body: value === undefined ? undefined : JSON.stringify(value) });
  return { status: response.status, body: await response.json() as Record<string, any>, headers: response.headers };
}
async function finish(base: string, id: string) {
  const until = Date.now() + 5000;
  while (Date.now() < until) {
    const result = await request(base, '/internal/v1/jobs/' + id);
    assert.equal(result.status, 200);
    if (!['queued', 'running'].includes(result.body.status)) return result.body;
    await delay(10);
  }
  throw new Error('Job did not finish');
}

test('configuration rejects missing service credentials and requires a key for real calls', () => {
  assert.throws(() => loadConfig({ AI_MOCK: '1' }), /AI_SERVICE_TOKEN/);
  assert.throws(() => loadConfig({ AI_SERVICE_TOKEN: serviceToken }), /AI_API_KEY/);
  assert.throws(() => loadConfig({ AI_SERVICE_TOKEN: serviceToken, AI_MOCK: '1', AI_TIMEOUT_MS: 'bad' }));
  assert.throws(() => loadConfig({ AI_SERVICE_TOKEN: serviceToken, AI_MOCK: '1',
                                 AI_API_BASE_URL: 'http://external.example/v1' }));
});

test('Pi mock runs, returns text, and keeps follow-up history in the same conversation', async () => {
  await usingApp(config(), async base => {
    const health = await request(base, '/health', 'GET', undefined, 1, null);
    assert.equal(health.status, 200);
    const created = await request(base, '/internal/v1/jobs', 'POST', submission);
    assert.equal(created.status, 202);
    assert.equal(created.body.status, 'queued');
    const first = await finish(base, created.body.job_id);
    assert.equal(first.status, 'succeeded');
    assert.match(first.reply, /第 1 轮/);
    assert.equal(first.model, 'mock');
    assert.equal(first.usage.model_calls, 1);
    const next = await request(base, '/internal/v1/jobs', 'POST', {
      ...submission, conversation_id: first.conversation_id, message: '再解释一下公式',
    });
    const second = await finish(base, next.body.job_id);
    assert.equal(second.status, 'succeeded');
    assert.match(second.reply, /第 2 轮/);
    assert.equal(second.conversation_id, first.conversation_id);
  });
});

test('internal auth, strict input, methods, JSON and body size failures are explicit', async () => {
  await usingApp(config(), async base => {
    assert.equal((await request(base, '/internal/v1/jobs', 'POST', submission, 1, null)).status, 401);
    assert.equal((await request(base, '/internal/v1/jobs', 'POST', submission, 1, 'wrong')).status, 401);
    for (const value of [{ ...submission, api_key: 'unexpected' },
      { ...submission, message: ' ' }, { ...submission, message: 'x'.repeat(4001) },
      { ...submission, question: { ...question, id: 1.5 } },
      { ...submission, conversation_id: null }]) {
      assert.equal((await request(base, '/internal/v1/jobs', 'POST', value)).status, 400);
    }
    const method = await request(base, '/internal/v1/jobs');
    assert.equal(method.status, 405); assert.equal(method.headers.get('allow'), 'POST');
    const invalid = await fetch(base + '/internal/v1/jobs', { method: 'POST',
      headers: { Authorization: 'Bearer ' + serviceToken, 'Content-Type': 'application/json' }, body: '{bad' });
    assert.equal(invalid.status, 400);
    const media = await fetch(base + '/internal/v1/jobs', { method: 'POST',
      headers: { Authorization: 'Bearer ' + serviceToken }, body: '{}' });
    assert.equal(media.status, 415);
    const large = await fetch(base + '/internal/v1/jobs', { method: 'POST',
      headers: { Authorization: 'Bearer ' + serviceToken, 'Content-Type': 'application/json' },
      body: 'x'.repeat(65537) });
    assert.equal(large.status, 413);
  });
});

test('job and conversation access cannot cross user or question boundaries', async () => {
  await usingApp(config(), async base => {
    const created = await request(base, '/internal/v1/jobs', 'POST', submission);
    const completed = await finish(base, created.body.job_id);
    assert.equal((await request(base, '/internal/v1/jobs/' + created.body.job_id, 'GET', undefined, 2)).status, 404);
    assert.equal((await request(base, '/internal/v1/jobs/' + created.body.job_id, 'DELETE', undefined, 2)).status, 404);
    assert.equal((await request(base, '/internal/v1/jobs', 'POST', {
      ...submission, user_id: 2, conversation_id: completed.conversation_id,
    })).status, 404);
    assert.equal((await request(base, '/internal/v1/jobs', 'POST', {
      ...submission, question: { ...question, id: 2 }, conversation_id: completed.conversation_id,
    })).status, 404);
  });
});

test('conversation locking, overload and both queued and active cancellation', async () => {
  await usingApp(config({ mockDelayMs: 200, concurrency: 1, queueLimit: 2 }), async base => {
    const first = await request(base, '/internal/v1/jobs', 'POST', submission);
    assert.equal((await request(base, '/internal/v1/jobs', 'POST', {
      ...submission, conversation_id: first.body.conversation_id,
    })).status, 409);
    const second = await request(base, '/internal/v1/jobs', 'POST', submission);
    const cancelledQueue = await request(base, '/internal/v1/jobs/' + second.body.job_id, 'DELETE');
    assert.equal(cancelledQueue.body.status, 'cancelled');
    const cancelledRunning = await request(base, '/internal/v1/jobs/' + first.body.job_id, 'DELETE');
    assert.equal(cancelledRunning.body.status, 'cancelled');
    assert.equal((await finish(base, first.body.job_id)).status, 'cancelled');
  });
  await usingApp(config({ mockDelayMs: 200, concurrency: 1, queueLimit: 0 }), async base => {
    const first = await request(base, '/internal/v1/jobs', 'POST', submission);
    assert.equal(first.status, 202);
    const second = await request(base, '/internal/v1/jobs', 'POST', submission);
    assert.equal(second.status, 503); assert.equal(second.body.code, 'AI_BUSY');
  });
});

test('timeouts are final job failures and do not publish partial history', async () => {
  await usingApp(config({ mockDelayMs: 200, timeoutMs: 50 }), async base => {
    const created = await request(base, '/internal/v1/jobs', 'POST', submission);
    const completed = await finish(base, created.body.job_id);
    assert.equal(completed.status, 'failed');
    assert.equal(completed.code, 'AI_TIMEOUT');
    assert.equal(completed.http_status, 504);
    assert.equal(completed.reply, undefined);
  });
});

test('the timeout budget includes queue waiting time', async () => {
  await usingApp(config({ mockDelayMs: 160, timeoutMs: 250, concurrency: 1 }), async base => {
    const first = await request(base, '/internal/v1/jobs', 'POST', submission);
    const second = await request(base, '/internal/v1/jobs', 'POST', submission);
    assert.equal((await finish(base, first.body.job_id)).status, 'succeeded');
    const expired = await finish(base, second.body.job_id);
    assert.equal(expired.status, 'failed');
    assert.equal(expired.code, 'AI_TIMEOUT');
  });
});

function stream(response: ServerResponse, delta: unknown, reason = 'stop') {
  response.writeHead(200, { 'Content-Type': 'text/event-stream' });
  const chunk = (change: unknown, finish: string | null) => JSON.stringify({
    id: 'test-completion', object: 'chat.completion.chunk', created: 1, model: 'test-model',
    choices: [{ index: 0, delta: change, finish_reason: finish }],
    ...(finish ? { usage: { prompt_tokens: 10, completion_tokens: 5, total_tokens: 15 } } : {}),
  });
  response.end('data: ' + chunk(delta, null) + '\n\ndata: ' + chunk({}, reason) + '\n\ndata: [DONE]\n\n');
}
async function upstream(fn: (body: Record<string, any>, response: ServerResponse) => void,
                        run: (base: string) => Promise<void>) {
  const server = createServer(async (request: IncomingMessage, response) => {
    const chunks = [];
    for await (const chunk of request) chunks.push(chunk);
    assert.equal(request.url, '/v1/chat/completions');
    assert.equal(request.headers.authorization, 'Bearer test-provider-key');
    fn(JSON.parse(Buffer.concat(chunks).toString()), response);
  });
  try { await run(await listening(server)); }
  finally { await close(server); }
}

test('real Pi provider path executes arithmetic then returns the model answer', async () => {
  let calls = 0;
  await upstream((body, response) => {
    calls++;
    assert.equal(body.stream, true);
    assert.deepEqual(body.tools.map((tool: any) => tool.function.name), ['arithmetic']);
    if (calls === 1) {
      stream(response, { role: 'assistant', tool_calls: [{ index: 0, id: 'calc_1', type: 'function',
        function: { name: 'arithmetic', arguments: JSON.stringify({ operation: 'multiply', a: 2, b: 3 }) } }] }, 'tool_calls');
    } else {
      assert(body.messages.some((message: any) => message.role === 'tool' && message.content === '6'));
      stream(response, { role: 'assistant', content: '根据 F=ma，合力为 6 N。' });
    }
  }, async modelBase => {
    await usingApp(config({ mock: false, baseUrl: modelBase + '/v1', model: 'test-model', apiKey: 'test-provider-key' }), async base => {
      const created = await request(base, '/internal/v1/jobs', 'POST', submission);
      const completed = await finish(base, created.body.job_id);
      assert.equal(completed.status, 'succeeded');
      assert.match(completed.reply, /6 N/);
      assert.equal(completed.usage.model_calls, 2);
      assert.equal(completed.usage.tool_calls, 1);
      assert.equal(completed.usage.input_tokens, 20);
    });
  });
  assert.equal(calls, 2);
});

test('model errors are sanitized; provider 429 is not a user login failure', async () => {
  for (const status of [401, 429, 500]) {
    await upstream((_body, response) => {
      response.writeHead(status, { 'Content-Type': 'application/json' });
      response.end(JSON.stringify({ error: { message: 'PRIVATE PROVIDER DETAILS', type: 'test_error' } }));
    }, async modelBase => {
      await usingApp(config({ mock: false, baseUrl: modelBase + '/v1', model: 'test-model', apiKey: 'test-provider-key' }), async base => {
        const created = await request(base, '/internal/v1/jobs', 'POST', submission);
        const completed = await finish(base, created.body.job_id);
        assert.equal(completed.status, 'failed');
        assert.equal(completed.http_status, status === 429 ? 503 : 502);
        assert(!JSON.stringify(completed).includes('PRIVATE'));
        assert(!JSON.stringify(completed).includes('test-provider-key'));
      });
    });
  }
});

test('model-call budget stops repeated tool requests', async () => {
  let calls = 0;
  await upstream((_body, response) => {
    calls++;
    stream(response, { role: 'assistant', tool_calls: [{ index: 0, id: 'calc_' + calls, type: 'function',
      function: { name: 'arithmetic', arguments: '{"operation":"add","a":1,"b":1}' } }] }, 'tool_calls');
  }, async modelBase => {
    await usingApp(config({ mock: false, baseUrl: modelBase + '/v1', model: 'test-model', apiKey: 'test-provider-key', maxModelCalls: 2 }), async base => {
      const created = await request(base, '/internal/v1/jobs', 'POST', submission);
      const completed = await finish(base, created.body.job_id);
      assert.equal(completed.status, 'failed'); assert.equal(completed.code, 'AI_CALL_LIMIT');
    });
  });
  assert.equal(calls, 2);
});

test('jobs and conversations expire and cannot grow beyond configured bounds', async () => {
  const jobs = new Jobs(config({ ttlMs: 10, maxConversations: 1 }));
  try {
    const created = jobs.submit(parseSubmission(submission));
    assert.throws(() => jobs.submit(parseSubmission(submission)), /任务已满/);
    await delay(80);
    assert.throws(() => jobs.get(String(created.job_id), 1), /任务不存在/);
    const next = jobs.submit(parseSubmission(submission));
    assert.notEqual(next.job_id, created.job_id);
  } finally { jobs.close(); }
});
