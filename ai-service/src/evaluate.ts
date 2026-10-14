import { readFileSync } from 'node:fs';
import { mkdir, writeFile } from 'node:fs/promises';
import { randomBytes } from 'node:crypto';
import { setTimeout as delay } from 'node:timers/promises';
import { pathToFileURL, fileURLToPath } from 'node:url';
import { loadConfig } from './config.js';
import { parseSubmission, type Question, type TeachingMode } from './contracts.js';
import { Jobs } from './jobs.js';

interface Fixture {
  id: string;
  mode: TeachingMode;
  question: Question;
  message: string;
  student_answer?: string;
  rubric: string[];
}
export function loadFixtures(): Fixture[] {
  const values: unknown = JSON.parse(readFileSync(new URL('../../eval/fixtures.json', import.meta.url), 'utf8'));
  if (!Array.isArray(values) || values.length !== 30) throw new Error('评测集须含 30 个案例');
  const ids = new Set<string>();
  for (const value of values) {
    if (!value || typeof value !== 'object' || typeof value.id !== 'string' || ids.has(value.id) ||
        !['solve', 'hint', 'review'].includes(value.mode) || !Array.isArray(value.rubric) || value.rubric.length === 0 ||
        !value.rubric.every((item: unknown) => typeof item === 'string' && item.trim()) ||
        ((value.mode === 'review') !== (value.student_answer !== undefined))) throw new Error('评测案例格式不正确');
    parseSubmission({ user_id: 1, question: value.question, message: value.message, mode: value.mode,
      ...(value.student_answer !== undefined ? { student_answer: value.student_answer } : {}) });
    ids.add(value.id);
  }
  return values as Fixture[];
}

async function main() {
  const fixtures = loadFixtures();
  if (process.env.AI_EVAL_REAL !== '1') {
    console.log(`已检查 ${fixtures.length} 个评测案例；未调用模型。真实评测须显式设置 AI_EVAL_REAL=1 和模型配置。`);
    return;
  }
  const config = loadConfig({ ...process.env, AI_MOCK: '0', AI_SERVICE_TOKEN: randomBytes(24).toString('hex') });
  const jobs = new Jobs(config);
  const results = [];
  try {
    for (const fixture of fixtures) {
      const started = Date.now();
      // 标准答案和人工评分规则不进入模型输入，避免答案泄露污染评测。
      const created = jobs.submit(parseSubmission({ user_id: 1, question: fixture.question,
        message: fixture.message, mode: fixture.mode,
        ...(fixture.student_answer !== undefined ? { student_answer: fixture.student_answer } : {}) }));
      let result = jobs.get(String(created.job_id), 1);
      while (result.status === 'queued' || result.status === 'running') {
        await delay(20);
        result = jobs.get(String(created.job_id), 1);
      }
      results.push({ fixture, result, duration_ms: Date.now() - started,
        human_review: { correctness: null, teaching: null, mode_adherence: null, notes: '' } });
    }
    const directory = new URL('../../eval/reports/', import.meta.url);
    await mkdir(directory, { recursive: true });
    const output = new URL(`evaluation-${new Date().toISOString().replace(/[:.]/g, '-')}.json`, directory);
    await writeFile(output, JSON.stringify({ model: config.model, assessed: false,
      note: '结果待人工评分；任务成功不等于答案正确。', results }, null, 2) + '\n', { mode: 0o600, flag: 'wx' });
    console.log(`已完成 ${results.length} 个案例，报告待人工核对：${fileURLToPath(output)}`);
  } finally { jobs.close(); }
}
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  void main().catch(() => { console.error('评测未完成，请检查案例或模型配置。'); process.exitCode = 1; });
}
