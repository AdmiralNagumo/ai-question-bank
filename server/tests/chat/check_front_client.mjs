import assert from 'node:assert/strict';
import { sendPiChat } from '../../../docs/examples/pi-chat-client.mjs';

const baseUrl = 'http://127.0.0.1:' + (process.env.TEST_PORT ?? '8099');
const login = await fetch(baseUrl + '/api/v1/auth/login', {
  method: 'POST', headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify({ username: 'student', password: process.env.DEMO_STUDENT_PW ?? 'student123' }),
});
assert.equal(login.status, 200);
const { token } = await login.json();
const headers = { Authorization: 'Bearer ' + token };
const { questions } = await (await fetch(baseUrl + '/api/v1/questions', { headers })).json();
const states = [];
const first = await sendPiChat({ questionId: questions[0].id, message: '请解释这道题',
  token, baseUrl, pollMs: 20, onStatus: job => states.push(job.status) });
assert.equal(first.status, 'succeeded');
assert(states.includes('queued') && states.includes('running') && states.includes('succeeded'));
const followup = await sendPiChat({ questionId: questions[0].id, message: '再解释一下',
  token, baseUrl, pollMs: 20, conversationId: first.conversation_id });
assert.match(followup.reply, /第 2 轮/);
assert.equal(first.mode, 'solve');
const hinted = await sendPiChat({ questionId: questions[0].id, message: '给我提示',
  mode: 'hint', token, baseUrl, pollMs: 20 });
assert.equal(hinted.mode, 'hint'); assert.equal(hinted.hint_level, 1);
const deeper = await sendPiChat({ questionId: questions[0].id, message: '再提示一下',
  conversationId: hinted.conversation_id, token, baseUrl, pollMs: 20 });
assert.equal(deeper.hint_level, 2);
await assert.rejects(sendPiChat({ questionId: questions[0].id, message: '完整讲解',
  mode: 'solve', conversationId: hinted.conversation_id, token, baseUrl, pollMs: 20 }),
  error => error.code === 'CONVERSATION_MODE_MISMATCH' && error.status === 409);
await assert.rejects(sendPiChat({ questionId: questions[0].id, message: '检查答案',
  mode: 'review', token, baseUrl }), error => error.code === 'INVALID_REQUEST' && error.status === 400);
const reviewed = await sendPiChat({ questionId: questions[0].id, message: '检查答案',
  mode: 'review', studentAnswer: 'x=7', token, baseUrl, pollMs: 20 });
assert.equal(reviewed.mode, 'review');
const reviewedAgain = await sendPiChat({ questionId: questions[0].id, message: '再检查',
  conversationId: reviewed.conversation_id, studentAnswer: 'x=4', token, baseUrl, pollMs: 20 });
assert.equal(reviewedAgain.mode, 'review');
const controller = new AbortController();
await assert.rejects(sendPiChat({ questionId: questions[0].id, message: '测试取消', token,
  baseUrl, pollMs: 20, signal: controller.signal,
  onStatus: () => controller.abort() }), error => error.code === 'CLIENT_CANCELLED');
console.log('PASS: 前端客户端完成讲解、分层提示、检查作答、模式继承和取消，错误与状态契约一致');
