import { readFileSync } from 'node:fs';
import { mkdir, writeFile, rename } from 'node:fs/promises';
import { randomBytes, createHash } from 'node:crypto';
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
  let fixtures = loadFixtures();
  const args = process.argv.slice(2);
  const option = (name: string) => {
    const index = args.indexOf(name);
    if (index === -1) return undefined;
    const value = args[index + 1];
    if (!value || value.startsWith('--')) throw new Error('评测参数缺少值');
    return value;
  };
  const selected = option('--cases');
  if (selected) {
    const ids = selected.split(',');
    if (ids.some(id => !fixtures.some(fixture => fixture.id === id))) throw new Error('未知案例');
    fixtures = fixtures.filter(fixture => ids.includes(fixture.id));
  }
  if (process.env.AI_EVAL_REAL !== '1' && !args.includes('--real')) {
    console.log(`已检查 ${fixtures.length} 个评测案例；未调用模型。真实评测须显式设置 AI_EVAL_REAL=1 和模型配置。`);
    return;
  }
  const config = loadConfig({ ...process.env, AI_MOCK: '0', AI_ROUTING_MODE: 'platform', AI_SERVICE_TOKEN: randomBytes(24).toString('hex') });
  const hash = (path: string) => createHash('sha256').update(readFileSync(new URL('../../' + path, import.meta.url))).digest('hex');
  const revision = {
    fixtures: hash('eval/fixtures.json'), agent: hash('src/agent.ts'), teaching: hash('src/teaching.ts'), jobs: hash('src/jobs.ts'), math_tools: hash('src/math-tools.ts'), answer_review: hash('src/answer-review.ts'),
    skills: ['solve-question', 'give-hints', 'review-answer'].map(name => ({ name, sha256: hash(`skills/${name}/SKILL.md`) })),
  };
  const directory = new URL('../../eval/reports/', import.meta.url);
  await mkdir(directory, { recursive: true });
  const resume = option('--resume');
  if (resume && !/^evaluation-[a-zA-Z0-9_.-]+\.json$/.test(resume)) throw new Error('恢复文件须是reports目录中的评测报告文件名');
  const output = new URL(resume ?? `evaluation-${new Date().toISOString().replace(/[:.]/g, '-')}.json`, directory);
  const report = resume ? JSON.parse(readFileSync(output, 'utf8')) : {
    model: config.model, base_url: config.baseUrl, revision, started_at: new Date().toISOString(),
    assessed: false, complete: false, note: '结果待逐题核对；任务成功不等于答案正确。', results: [],
  };
  if (report.model !== config.model || report.base_url !== config.baseUrl ||
      JSON.stringify(report.revision) !== JSON.stringify(revision) || !Array.isArray(report.results)) {
    throw new Error('模型或代码已变化，须新建评测，不能混合报告');
  }
  async function checkpoint() {
    report.complete = report.results.length === loadFixtures().length;
    report.updated_at = new Date().toISOString();
    const temporary = new URL(output.href + '.tmp');
    await writeFile(temporary, JSON.stringify(report, null, 2) + '\n', { mode: 0o600 });
    await rename(temporary, output);
  }
  await checkpoint();
  console.log(`评测报告：${fileURLToPath(output)}`);
  const jobs = new Jobs(config);
  try {
    for (const fixture of fixtures) {
      if (report.results.some((entry: { fixture: Fixture }) => entry.fixture.id === fixture.id)) continue;
      const started = performance.now();
      // 标准答案和人工评分规则不进入模型输入，避免答案泄露污染评测。
      const created = jobs.submit(parseSubmission({ user_id: 1, question: fixture.question,
        message: fixture.message, mode: fixture.mode,
        ...(fixture.student_answer !== undefined ? { student_answer: fixture.student_answer } : {}) }));
      let result = jobs.get(String(created.job_id), 1);
      while (result.status === 'queued' || result.status === 'running') {
        await delay(20);
        result = jobs.get(String(created.job_id), 1);
      }
      report.results.push({ fixture, result, duration_ms: Math.round(performance.now() - started),
        human_review: { correctness: null, teaching: null, mode_adherence: null, notes: '' } });
      await checkpoint();
      console.log(`${fixture.id}: ${result.status} (${Math.round(performance.now() - started)} ms)`);
      if (['AI_UPSTREAM_ERROR', 'AI_RATE_LIMITED'].includes(String(result.code))) {
        console.log('模型调用失败，已保存进度；停止后续调用，请先排查服务配置。');
        process.exitCode = 1;
        break;
      }
    }
    console.log(`报告已保存 ${report.results.length} 个案例，待逐题核对：${fileURLToPath(output)}`);
  } finally { jobs.close(); }
}
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  void main().catch(() => { console.error('评测未完成，请检查案例或模型配置。'); process.exitCode = 1; });
}
