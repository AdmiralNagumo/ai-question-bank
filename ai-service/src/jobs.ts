import { randomUUID } from 'node:crypto';
import type { AgentMessage } from '@earendil-works/pi-agent-core';
import type { Config } from './config.js';
import { startAgent, type AgentRun, type Completion } from './agent.js';
import { ApiError, type Submission, type JobStatus, type TeachingMode } from './contracts.js';
import type { TeachingContext } from './teaching.js';

interface Conversation {
  id: string;
  owner: number;
  question: number;
  mode: TeachingMode;
  messages: AgentMessage[];
  activeJob?: string;
  touched: number;
}
interface Job {
  id: string;
  owner: number;
  request: Submission;
  teaching: TeachingContext;
  conversation: Conversation;
  status: JobStatus;
  created: number;
  finished?: number;
  run?: AgentRun;
  result?: Completion;
  error?: { error: string; code: string; http_status: number };
}

export class Jobs {
  private jobs = new Map<string, Job>();
  private conversations = new Map<string, Conversation>();
  private queue: Job[] = [];
  private running = 0;
  private stopped = false;
  private housekeeping: NodeJS.Timeout;
  constructor(private config: Config) {
    this.housekeeping = setInterval(() => this.clean(), 60000).unref();
  }
  private clean() {
    const cutoff = Date.now() - this.config.ttlMs;
    for (const [id, job] of this.jobs) {
      if (job.finished !== undefined && job.finished < cutoff) this.jobs.delete(id);
    }
    for (const [id, conversation] of this.conversations) {
      if (!conversation.activeJob && conversation.touched < cutoff) this.conversations.delete(id);
    }
  }
  submit(request: Submission) {
    this.clean();
    if (this.stopped || this.running + this.queue.length >= this.config.concurrency + this.config.queueLimit ||
        this.jobs.size >= this.config.maxConversations * 4) {
      throw new ApiError(503, 'AI_BUSY', '解答任务已满，请稍后重试');
    }
    let conversation: Conversation;
    if (request.conversation_id) {
      const found = this.conversations.get(request.conversation_id);
      if (!found || found.owner !== request.user_id || found.question !== request.question.id) {
        throw new ApiError(404, 'CONVERSATION_NOT_FOUND', '对话不存在或已过期');
      }
      conversation = found;
      if (request.mode !== undefined && request.mode !== conversation.mode) {
        throw new ApiError(409, 'CONVERSATION_MODE_MISMATCH', '切换教学模式时请新建对话');
      }
      if (conversation.activeJob) throw new ApiError(409, 'CONVERSATION_BUSY', '请等待当前解答完成');
      // 保留完整工具消息，达到历史边界时明确要求新对话，不截断工具调用对。
      if (JSON.stringify(conversation.messages).length > 64000 ||
          conversation.messages.filter(message => message.role === 'user').length >= 12) {
        throw new ApiError(409, 'CONVERSATION_LIMIT', '对话较长，请新建对话继续');
      }
    } else {
      if (this.conversations.size >= this.config.maxConversations) {
        throw new ApiError(503, 'AI_BUSY', '解答任务已满，请稍后重试');
      }
      conversation = { id: randomUUID(), owner: request.user_id, question: request.question.id, mode: request.mode ?? 'solve',
                       messages: [], touched: Date.now() };
    }
    if ((conversation.mode === 'review') !== (request.student_answer !== undefined)) {
      throw new ApiError(400, 'INVALID_REQUEST', '错因分析须提供 student_answer；其他模式不接受该字段');
    }
    this.conversations.set(conversation.id, conversation);
    const teaching: TeachingContext = { mode: conversation.mode };
    if (conversation.mode === 'hint') {
      teaching.hintLevel = Math.min(3, 1 + conversation.messages.filter(message => message.role === 'user').length);
    }
    const job: Job = { id: randomUUID(), owner: request.user_id, request, conversation,
                       teaching, status: 'queued', created: performance.now() };
    conversation.activeJob = job.id;
    conversation.touched = Date.now();
    this.jobs.set(job.id, job);
    this.queue.push(job);
    // 返回时仍为 queued，随后事件循环启动运行。
    setImmediate(() => this.pump());
    return this.view(job);
  }
  private find(id: string, owner: number): Job {
    this.clean();
    const job = this.jobs.get(id);
    if (!job || job.owner !== owner) throw new ApiError(404, 'JOB_NOT_FOUND', '任务不存在或已过期');
    return job;
  }
  get(id: string, owner: number) { return this.view(this.find(id, owner)); }
  cancel(id: string, owner: number) {
    const job = this.find(id, owner);
    if (job.status === 'queued') {
      this.queue = this.queue.filter(item => item !== job);
      job.status = 'cancelled'; job.finished = Date.now();
      job.conversation.activeJob = undefined;
    } else if (job.status === 'running') {
      job.status = 'cancelled'; job.finished = Date.now();
      job.run?.cancel(); // 历史只在成功后提交；运行释放在 finally 中完成。
    }
    return this.view(job);
  }
  private view(job: Job) {
    const result: Record<string, unknown> = {
      job_id: job.id, conversation_id: job.conversation.id, question_id: job.request.question.id,
      status: job.status,
      mode: job.teaching.mode,
      ...(job.teaching.hintLevel !== undefined ? { hint_level: job.teaching.hintLevel } : {}),
    };
    if (job.result) { result.reply = job.result.reply; result.model = job.result.model; result.usage = job.result.usage; }
    if (job.error) Object.assign(result, job.error);
    return result;
  }
  private pump() {
    while (!this.stopped && this.running < this.config.concurrency && this.queue.length) {
      const job = this.queue.shift()!;
      job.status = 'running';
      this.running++;
      void this.execute(job);
    }
  }
  private async execute(job: Job) {
    try {
      const remaining = Math.floor(this.config.timeoutMs - (performance.now() - job.created));
      if (remaining <= 0) throw new ApiError(504, 'AI_TIMEOUT', '解答超时，请稍后重试');
      job.run = startAgent({ ...this.config, timeoutMs: remaining }, job.request, job.conversation.messages, job.teaching);
      const result = await job.run.completion;
      if (job.status !== 'cancelled') {
        job.status = 'succeeded'; job.result = { ...result, messages: [] };
        job.conversation.messages = result.messages;
      }
    } catch (error) {
      if (job.status !== 'cancelled') {
        const api = error instanceof ApiError ? error : new ApiError(500, 'AI_INTERNAL_ERROR', '解答服务发生错误');
        job.status = 'failed';
        job.error = { error: api.message, code: api.code, http_status: api.status };
      }
    } finally {
      job.run = undefined; job.finished = Date.now();
      job.conversation.activeJob = undefined; job.conversation.touched = Date.now();
      this.running--;
      this.pump();
    }
  }
  close() {
    this.stopped = true;
    clearInterval(this.housekeeping);
    for (const job of this.jobs.values()) {
      if (job.status === 'queued' || job.status === 'running') this.cancel(job.id, job.owner);
    }
  }
}
