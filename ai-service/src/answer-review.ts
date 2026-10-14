import type { Submission } from './contracts.js';
import { verifyLinear } from './math-tools.js';

// 仅识别单个简单数值答案与显式 ax+b=c；不使用标准答案、不推测学生过程。
export function answerOnlyFeedback(request: Submission): string | undefined {
  const value = /^\s*(?:x\s*=\s*)?([+-]?(?:\d+(?:\.\d+)?|\.\d+))\s*[。.!！]?\s*$/u.exec(request.student_answer ?? '');
  if (!value) return undefined;
  const runs = request.question.content.replace(/\s/g, '').replaceAll('−', '-').match(/[+\-\d.x*/^()=]+/g) ?? [];
  const equations = runs.filter(run => run.includes('='));
  if (equations.length !== 1) return undefined;
  const match = /^([+-]?(?:\d+(?:\.\d+)?|\.\d+)?)x([+-](?:\d+(?:\.\d+)?|\.\d+))?=([+-]?(?:\d+(?:\.\d+)?|\.\d+))$/.exec(equations[0]!);
  if (!match) return undefined;
  const a = match[1] === '' || match[1] === '+' ? 1 : match[1] === '-' ? -1 : Number(match[1]);
  const b = Number(match[2] ?? 0);
  const c = Number(match[3]);
  const x = Number(value[1]);
  try {
    const checked = verifyLinear({ a, b, c, x });
    const verdict = checked.holds
      ? '该候选值代入这一等式，在数值容差内成立；题目的其他限制仍需检查。'
      : '两边不相等，因此这个候选值不满足题中的等式。';
    return `你提交的候选答案是 x=${x}。\n\n代入题中的等式：左边 = ${a}×(${x}) + (${b}) = ${checked.lhs}，右边 = ${c}。${verdict}\n\n你只提供了最终答案，没有中间步骤，因此目前无法确定具体错因。请补充列式、变形和计算过程，我再逐步检查。`;
  } catch { return undefined; }
}
