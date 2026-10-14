import type { AgentTool } from '@earendil-works/pi-agent-core';
import { Type } from '@earendil-works/pi-ai';

const bounded = () => Type.Number({ minimum: -1e12, maximum: 1e12 });
const coefficients = Type.Object({ a: bounded(), b: bounded(), c: bounded() }, { additionalProperties: false });
const candidate = Type.Object({ a: bounded(), b: bounded(), c: bounded(), x: bounded() }, { additionalProperties: false });

function check(...values: number[]) {
  if (values.some(value => !Number.isFinite(value) || Math.abs(value) > 1e12)) throw new Error('数值必须有限且绝对值不超过 1e12');
}
export function solveLinear({ a, b, c }: { a: number; b: number; c: number }) {
  check(a, b, c);
  if (a === 0) return { kind: b === c ? 'all_real' : 'no_solution' } as const;
  const x = (c - b) / a;
  check(x);
  return { kind: 'unique', x } as const;
}
export function verifyLinear({ a, b, c, x }: { a: number; b: number; c: number; x: number }) {
  check(a, b, c, x);
  const lhs = a * x + b;
  const residual = lhs - c;
  // IEEE-754 运算容差；不是模型置信度或严格符号证明。
  const tolerance = 8 * Number.EPSILON * Math.max(1, Math.abs(a * x), Math.abs(b), Math.abs(c));
  const holds = a === 0 ? b === c : Math.abs(residual) <= tolerance;
  return { holds, lhs, rhs: c, residual, tolerance, method: 'numeric_substitution' };
}
export const linearSolver: AgentTool<typeof coefficients, ReturnType<typeof solveLinear>> = {
  name: 'solve_linear_equation', label: '一元一次方程求解',
  description: '只解 a*x+b=c。模型须先正确整理系数；返回唯一实数解、无解或任意实数。数值结果不是严格符号证明。',
  parameters: coefficients,
  async execute(_id, args) {
    const details = solveLinear(args);
    return { content: [{ type: 'text', text: JSON.stringify(details) }], details };
  },
};
export const linearVerifier: AgentTool<typeof candidate, ReturnType<typeof verifyLinear>> = {
  name: 'verify_linear_solution', label: '候选解代入校验',
  description: '将候选解 x 代入 a*x+b=c，返回残差及是否在数值容差内成立；不核验原题、单位和定义域。',
  parameters: candidate,
  async execute(_id, args) {
    const details = verifyLinear(args);
    return { content: [{ type: 'text', text: JSON.stringify(details) }], details };
  },
};
