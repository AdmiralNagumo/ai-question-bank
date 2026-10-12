/* ── 题目列表页 ── */
const QuestionsPage = (() => {
  const $ = (id) => document.getElementById(id);

  /** HTML 转义：题目内容来自后端，拼进 innerHTML 前必须转义，防注入 */
  function escapeHtml(s) {
    return String(s)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#39;');
  }

  function show() {
    $('page-login').classList.add('hidden');
    $('navbar').classList.remove('hidden');
    $('page-questions').classList.remove('hidden');
    $('page-chat').classList.add('hidden');
    $('questionsList').innerHTML = '';
    $('questionsLoading').classList.remove('hidden');
    $('questionsError').classList.add('hidden');
    loadQuestions();
  }

  async function loadQuestions() {
    try {
      const data = await API.questions();    // { questions: [...] }
      renderQuestions(data.questions || []);
    } catch (e) {
      $('questionsLoading').classList.add('hidden');
      $('questionsError').textContent = '加载失败：' + (e.message || '未知错误');
      $('questionsError').classList.remove('hidden');
    }
  }

  function renderQuestions(questions) {
    $('questionsLoading').classList.add('hidden');
    const list = $('questionsList');
    if (questions.length === 0) {
      list.innerHTML = '<p style="text-align:center;color:#999;">暂无题目</p>';
      return;
    }
    list.innerHTML = questions.map(q => {
      const typeMap = { single_choice: '选择题', short_answer: '简答题' };
      const typeLabel = typeMap[q.type] || String(q.type || '');
      const kps = Array.isArray(q.knowledge_points) ? q.knowledge_points : [];
      const tags = kps.map(k => `<span class="q-tag">${escapeHtml(k)}</span>`).join('');
      /* id 会被拼进 onclick 属性，只接受正整数，杜绝属性注入 */
      const safeId = parseInt(q.id, 10);
      if (!isFinite(safeId) || safeId <= 0) return '';
      return `<div class="q-card" onclick="navigate('#chat/${safeId}')">
        <div class="q-card-title">${escapeHtml(q.content)}</div>
        <div class="q-card-meta">
          <span class="q-tag">${escapeHtml(typeLabel)}</span>
          ${tags}
        </div>
      </div>`;
    }).join('');
  }

  return { show };
})();
