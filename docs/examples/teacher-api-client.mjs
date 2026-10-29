import { PiChatError } from './pi-chat-client.mjs';

async function request(path, { token, baseUrl = '', signal, method = 'GET', value } = {}) {
  let response;
  try {
    response = await fetch(baseUrl.replace(/\/$/, '') + path, {
      method, signal, cache: 'no-store', headers: { Authorization: 'Bearer ' + token,
        ...(value === undefined ? {} : { 'Content-Type': 'application/json' }) },
      ...(value === undefined ? {} : { body: JSON.stringify(value) }),
    });
  } catch { throw new PiChatError('无法连接后端，请检查网络', { code: 'NETWORK_ERROR' }); }
  let body;
  try { body = await response.json(); }
  catch { throw new PiChatError('接口响应格式不正确', { code: 'INVALID_RESPONSE', status: response.status }); }
  if (!response.ok) {
    const error = new PiChatError(body?.error ?? '请求失败', { code: body?.code ?? 'HTTP_ERROR', status: response.status });
    error.quota = body?.quota;
    throw error;
  }
  return body;
}

/** Key只作为本次请求参数使用，不写localStorage、不输出日志；成功后清空页面输入框。 */
export function replaceTeacherApi({ apiKey, model = 'deepseek-flash', ...options }) {
  return request('/api/v1/teacher/ai-api', { ...options, method: 'PUT', value: { api_key: apiKey, model } });
}
export function getTeacherApi(options) { return request('/api/v1/teacher/ai-api', options); }
export function setTeacherApiEnabled({ enabled, ...options }) {
  return request('/api/v1/teacher/ai-api', { ...options, method: 'PATCH', value: { enabled } });
}
export function getQuestionAiAccess({ questionId, ...options }) {
  if (!Number.isInteger(questionId) || questionId < 1 || questionId > 2147483647) {
    throw new PiChatError('请选择有效题目', { code: 'INVALID_REQUEST', status: 400 });
  }
  return request('/api/v1/ai/access?question_id=' + encodeURIComponent(questionId), options);
}
