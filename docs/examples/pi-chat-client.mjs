/** 浏览器与 Node 均可使用。只向 C 后端发送用户 token，不传模型密钥。 */
export class PiChatError extends Error {
  constructor(message, { code = 'NETWORK_ERROR', status = 0, jobId } = {}) {
    super(message);
    this.name = 'PiChatError';
    this.code = code;
    this.status = status;
    this.jobId = jobId;
  }
}

/** 页面预检查；POST仍会重新核验权限，不能用缓存结果作为授权依据。 */
export async function getPiAiAccess({ token, questionId, baseUrl = '', signal } = {}) {
  let response;
  try { response = await fetch(baseUrl.replace(/\/$/, '') + '/api/v1/ai/access' + (questionId !== undefined ? '?question_id=' + encodeURIComponent(questionId) : ''), {
    headers: { Authorization: 'Bearer ' + token }, signal,
  }); }
  catch { throw new PiChatError('无法查询 AI 使用权限，请重试', { code: 'NETWORK_ERROR' }); }
  let body;
  try { body = await response.json(); }
  catch { throw new PiChatError('权限响应格式不正确', { code: 'INVALID_RESPONSE', status: response.status }); }
  if (!response.ok) throw new PiChatError(body.error ?? '查询权限失败', { code: body.code ?? 'HTTP_ERROR', status: response.status });
  const access = body.ai_access;
  if (!access || !['active','not_granted','revoked','expired'].includes(access.status) ||
      access.allowed !== (access.status === 'active') ||
      !(access.expires_at === null || typeof access.expires_at === 'string')) {
    throw new PiChatError('权限响应格式不正确', { code: 'INVALID_RESPONSE', status: response.status });
  }
  return access;
}

export async function sendPiChat({ questionId, message, conversationId, token,
                                  mode, studentAnswer,
                                  baseUrl = '', onStatus = () => {}, signal,
                                  pollMs = 800, timeoutMs = 45000 }) {
  if (!Number.isInteger(questionId) || questionId <= 0 || questionId > 2147483647 ||
      typeof message !== 'string' || !message.trim() || Array.from(message).length > 4000 ||
      message.includes('\0')) {
    throw new PiChatError('请输入有效题目和 1–4000 字的问题', { code: 'INVALID_REQUEST', status: 400 });
  }
  if ((mode !== undefined && !['solve', 'hint', 'review'].includes(mode)) ||
      (studentAnswer !== undefined && (typeof studentAnswer !== 'string' || !studentAnswer.trim() ||
        Array.from(studentAnswer).length > 4000 || studentAnswer.includes('\0'))) ||
      (mode !== undefined && ((mode === 'review') !== (studentAnswer !== undefined)))) {
    throw new PiChatError('请检查教学模式和学生作答；错因分析需要提供作答', { code: 'INVALID_REQUEST', status: 400 });
  }
  const controller = new AbortController();
  let localTimeout = false;
  let jobId;
  const abort = () => controller.abort();
  signal?.addEventListener('abort', abort, { once: true });
  if (signal?.aborted) controller.abort();
  const timer = setTimeout(() => { localTimeout = true; controller.abort(); }, timeoutMs);
  const headers = { Authorization: 'Bearer ' + token, 'Content-Type': 'application/json' };
  const prefix = baseUrl.replace(/\/$/, '') + '/api/v1/chat';
  async function request(url, method = 'GET', body) {
    let response;
    try {
      response = await fetch(url, { method, headers, signal: controller.signal,
                                   body: body === undefined ? undefined : JSON.stringify(body) });
    } catch (error) {
      if (controller.signal.aborted) {
        throw new PiChatError(localTimeout ? '等待解答超时' : '已取消解答',
          { code: localTimeout ? 'CLIENT_TIMEOUT' : 'CLIENT_CANCELLED', jobId });
      }
      throw new PiChatError('网络连接失败，可以稍后查询该任务', { jobId });
    }
    let result;
    try { result = await response.json(); }
    catch { throw new PiChatError('后端响应格式不正确', { code: 'INVALID_RESPONSE', status: response.status, jobId }); }
    if (!response.ok) {
      throw new PiChatError(result.error ?? '请求失败', { code: result.code ?? 'HTTP_ERROR', status: response.status, jobId });
    }
    return result;
  }
  function wait(ms) {
    return new Promise((resolve, reject) => {
      const cancel = () => { clearTimeout(handle); controller.signal.removeEventListener('abort', cancel);
        reject(new PiChatError(localTimeout ? '等待解答超时' : '已取消解答',
          { code: localTimeout ? 'CLIENT_TIMEOUT' : 'CLIENT_CANCELLED', jobId })); };
      const handle = setTimeout(() => { controller.signal.removeEventListener('abort', cancel); resolve(); }, ms);
      controller.signal.addEventListener('abort', cancel, { once: true });
      if (controller.signal.aborted) cancel();
    });
  }
  try {
    const created = await request(prefix, 'POST', {
      question_id: questionId, message, ...(conversationId ? { conversation_id: conversationId } : {}),
      ...(mode !== undefined ? { mode } : {}),
      ...(studentAnswer !== undefined ? { student_answer: studentAnswer } : {}),
    });
    jobId = created.job_id;
    if (typeof jobId !== 'string' || !/^[0-9a-f-]{36}$/.test(jobId)) {
      throw new PiChatError('后端没有返回任务标识', { code: 'INVALID_RESPONSE' });
    }
    onStatus(created);
    for (;;) {
      const job = await request(prefix + '/' + jobId);
      onStatus(job);
      if (job.status === 'succeeded') return job; // 含 reply 和 conversation_id，可供后续追问。
      if (job.status === 'failed') {
        throw new PiChatError(job.error ?? '解答失败', { code: job.code, status: job.http_status, jobId });
      }
      if (job.status === 'cancelled') throw new PiChatError('已取消解答', { code: 'CLIENT_CANCELLED', jobId });
      if (!['queued', 'running'].includes(job.status)) {
        throw new PiChatError('任务状态不正确', { code: 'INVALID_RESPONSE', jobId });
      }
      await wait(pollMs);
    }
  } catch (error) {
    if (controller.signal.aborted && jobId) {
      // 取消使用独立短超时；原信号已中止，不能继续用它发送 DELETE。
      try { await fetch(prefix + '/' + jobId, { method: 'DELETE', headers, signal: AbortSignal.timeout(1500) }); }
      catch { /* 网络断开时服务端总时限仍会停止任务。 */ }
    }
    throw error;
  } finally {
    clearTimeout(timer);
    signal?.removeEventListener('abort', abort);
  }
}
