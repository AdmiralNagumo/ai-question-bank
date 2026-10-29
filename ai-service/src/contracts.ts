export type JobStatus = 'queued' | 'running' | 'succeeded' | 'failed' | 'cancelled';
export type TeachingMode = 'solve' | 'hint' | 'review';
export interface Question {
  id: number;
  content: string;
  options: { label: string; text: string }[];
  knowledge_points: string[];
  type: string;
}
export interface Execution {
  teacher_id: number;
  version: number;
  api_key: string;
  model: string;
}
export interface Submission {
  execution?: Execution; // 仅可信C可发送；永不进入prompt/view/工具。

  user_id: number;
  question: Question;
  message: string;
  conversation_id?: string;
  mode?: TeachingMode;
  student_answer?: string;
}
export class ApiError extends Error {
  constructor(public status: number, public code: string, message: string) {
    super(message);
  }
}
export const uuidPattern = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;

function record(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}
function text(value: unknown, max: number): value is string {
  return typeof value === 'string' && value.trim().length > 0 &&
    Array.from(value).length <= max && !value.includes('\0');
}
function positive(value: unknown): value is number {
  return typeof value === 'number' && Number.isSafeInteger(value) && value > 0 && value <= 2147483647;
}
function onlyKeys(value: Record<string, unknown>, allowed: string[]): boolean {
  return Object.keys(value).every(key => allowed.includes(key));
}

export function parseSubmission(value: unknown): Submission {
  const fail = () => { throw new ApiError(400, 'INVALID_REQUEST', '请求字段或格式不正确'); };
  if (!record(value) || !onlyKeys(value, ['user_id', 'question', 'message', 'conversation_id', 'mode', 'student_answer', 'execution']) ||
      !positive(value.user_id) || !text(value.message, 4000)) return fail();
  if (value.mode !== undefined && (typeof value.mode !== 'string' || !['solve', 'hint', 'review'].includes(value.mode))) return fail();
  if (value.student_answer !== undefined && !text(value.student_answer, 4000)) return fail();
  if (value.execution !== undefined) {
    const e = value.execution;
    if (!record(e) || !onlyKeys(e, ['teacher_id', 'version', 'api_key', 'model']) ||
        !positive(e.teacher_id) || !positive(e.version) || typeof e.api_key !== 'string' ||
        !/^[\x21-\x7e]{16,512}$/.test(e.api_key) || typeof e.model !== 'string' ||
        !/^[A-Za-z0-9_.:/-]{1,128}$/.test(e.model)) return fail();
  }
  const question = value.question;
  if (!record(question) || !onlyKeys(question, ['id', 'content', 'options', 'knowledge_points', 'type']) ||
      !positive(question.id) || !text(question.content, 8000) || !text(question.type, 64) ||
      !Array.isArray(question.options) || question.options.length > 20 ||
      !question.options.every(option => record(option) && onlyKeys(option, ['label', 'text']) &&
        text(option.label, 32) && text(option.text, 2000)) ||
      !Array.isArray(question.knowledge_points) || question.knowledge_points.length > 32 ||
      !question.knowledge_points.every(point => text(point, 200))) return fail();
  if (value.conversation_id !== undefined &&
      (typeof value.conversation_id !== 'string' || !uuidPattern.test(value.conversation_id))) return fail();
  return structuredClone(value) as unknown as Submission;
}
