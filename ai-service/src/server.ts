import { createServer, type IncomingMessage, type ServerResponse } from 'node:http';
import { timingSafeEqual } from 'node:crypto';
import { pathToFileURL } from 'node:url';
import { loadConfig, type Config } from './config.js';
import { ApiError, parseSubmission, uuidPattern } from './contracts.js';
import { Jobs } from './jobs.js';

function json(response: ServerResponse, status: number, value: unknown) {
  response.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8',
                              'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff' });
  response.end(JSON.stringify(value));
}
function authenticated(request: IncomingMessage, token: string): boolean {
  const expected = Buffer.from('Bearer ' + token);
  const actual = Buffer.from(request.headers.authorization ?? '');
  return actual.length === expected.length && timingSafeEqual(actual, expected);
}
async function body(request: IncomingMessage): Promise<unknown> {
  if (request.headers['content-type']?.split(';')[0]?.trim().toLowerCase() !== 'application/json') {
    throw new ApiError(415, 'UNSUPPORTED_MEDIA_TYPE', '请使用 application/json');
  }
  const chunks: Buffer[] = [];
  let bytes = 0;
  let tooLarge = false;
  for await (const chunk of request) {
    bytes += chunk.length;
    if (bytes > 65536) { tooLarge = true; chunks.length = 0; }
    if (!tooLarge) chunks.push(Buffer.from(chunk));
  }
  if (tooLarge) throw new ApiError(413, 'REQUEST_TOO_LARGE', '请求内容过长');
  try { return JSON.parse(Buffer.concat(chunks).toString('utf8')); }
  catch { throw new ApiError(400, 'INVALID_JSON', 'JSON 格式不正确'); }
}

export function createApp(config: Config) {
  const jobs = new Jobs(config);
  const server = createServer(async (request, response) => {
    try {
      const url = new URL(request.url ?? '/', 'http://localhost');
      if (url.pathname === '/health' && request.method === 'GET') {
        json(response, 200, { status: 'ok', service: 'pi-ai-service', mode: config.mock ? 'mock' : 'real' });
        return;
      }
      if (!authenticated(request, config.serviceToken)) throw new ApiError(401, 'UNAUTHORIZED', '服务凭证无效');
      if (url.pathname === '/internal/v1/jobs') {
        if (request.method !== 'POST') {
          response.setHeader('Allow', 'POST'); throw new ApiError(405, 'METHOD_NOT_ALLOWED', '请求方法不支持');
        }
        json(response, 202, jobs.submit(parseSubmission(await body(request))));
        return;
      }
      const match = /^\/internal\/v1\/jobs\/([^/]+)$/.exec(url.pathname);
      if (!match || !uuidPattern.test(match[1]!)) throw new ApiError(404, 'JOB_NOT_FOUND', '任务不存在或已过期');
      const ownerHeader = request.headers['x-user-id'];
      const owner = typeof ownerHeader === 'string' && /^[1-9][0-9]*$/.test(ownerHeader) ? Number(ownerHeader) : 0;
      if (!Number.isSafeInteger(owner) || owner <= 0 || owner > 2147483647) {
        throw new ApiError(400, 'INVALID_REQUEST', '用户标识不正确');
      }
      if (request.method === 'GET') json(response, 200, jobs.get(match[1]!, owner));
      else if (request.method === 'DELETE') json(response, 200, jobs.cancel(match[1]!, owner));
      else { response.setHeader('Allow', 'GET, DELETE'); throw new ApiError(405, 'METHOD_NOT_ALLOWED', '请求方法不支持'); }
    } catch (error) {
      request.resume();
      const api = error instanceof ApiError ? error : new ApiError(500, 'AI_INTERNAL_ERROR', '解答服务发生错误');
      if (!response.headersSent) json(response, api.status, { error: api.message, code: api.code });
      else response.destroy();
    }
  });
  server.requestTimeout = 10000;
  server.headersTimeout = 10000;
  server.on('close', () => jobs.close());
  return server;
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    const config = loadConfig();
    const server = createApp(config);
    server.listen(config.port, config.host, () => {
      const address = server.address();
      console.log(JSON.stringify({ event: 'listening', port: typeof address === 'object' ? address?.port : config.port }));
    });
    server.on('error', () => { console.error('Pi service failed to listen'); process.exitCode = 1; });
    const shutdown = () => { server.close(); server.closeAllConnections(); };
    process.once('SIGINT', shutdown); process.once('SIGTERM', shutdown);
  } catch (error) {
    console.error(error instanceof Error ? error.message : 'Invalid service configuration');
    process.exitCode = 1;
  }
}
