/* ── 对话页（题目详情 + AI 对话） ── */
const ChatPage = (() => {
  const $ = (id) => document.getElementById(id);
  let currentQuestionId = null;
  let chatMockNoticeShown = false;

  /** HTML 转义：AI 回复属于不可信内容，插入 innerHTML 前必须先转义，
   *  否则后端返回的 <script> / <img onerror=...> 会被浏览器执行（XSS）。
   *  这也是 CONTRIBUTING.md 审查清单中「注入」一项的要求。 */
  function escapeHtml(s) {
    return String(s)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#39;');
  }

  function show(questionId) {
    currentQuestionId = parseInt(questionId, 10);
    $('page-login').classList.add('hidden');
    $('navbar').classList.remove('hidden');
    $('page-questions').classList.add('hidden');
    $('page-chat').classList.remove('hidden');

    // 重置
    $('chatMessages').innerHTML = '';
    chatMockNoticeShown = false;
    $('chatInput').value = '';
    $('detailLoading').classList.remove('hidden');
    $('detailError').classList.add('hidden');
    $('detailContent').classList.add('hidden');

    // 更新用户信息
    updateUserInfo();

    loadQuestionDetail(currentQuestionId);

    // 加个欢迎消息
    addMessage('system', '请在下方输入你的问题，AI 将为你解答这道题。');

    /* 离线演示时明确告知，避免把前端内置演示数据误当成真实后端 / 真实 AI 展示 */
    if (typeof API !== 'undefined' && API.isOfflineDemo && API.isOfflineDemo()) {
      addMessage('system', '⚠ 离线演示模式：数据来自前端内置演示集，未连接后端，回复不是真实 AI 生成。');
    }
  }

  function updateUserInfo() {
    const user = API.getUser();
    $('userInfo').textContent = user ? (user.name || user.username) + ' (' + (user.role === 'admin' ? '管理员' : user.role === 'teacher' ? '老师' : '学生') + ')' : '';
  }

  async function loadQuestionDetail(id) {
    try {
      const data = await API.questionDetail(id);  // { question: {...} }
      const q = data.question;
      renderDetail(q);
      $('chatQuestionTitle').textContent = '题目 #' + q.id;
    } catch (e) {
      $('detailLoading').classList.add('hidden');
      $('detailError').textContent = '加载题目失败：' + (e.message || '未知错误');
      $('detailError').classList.remove('hidden');
    }
  }

  function renderDetail(q) {
    $('detailLoading').classList.add('hidden');
    $('detailContent').classList.remove('hidden');

    $('qContent').textContent = q.content;

    // 选项
    const optsEl = $('qOptions');
    if (Array.isArray(q.options) && q.options.length > 0) {
      optsEl.innerHTML = q.options.map(opt =>
        `<div class="q-opt-item"><span class="q-opt-label">${opt.label}.</span>${opt.text}</div>`
      ).join('');
    } else {
      optsEl.innerHTML = '<p style="color:#999;font-size:13px;">（本题为简答/填空类型，无选项）</p>';
    }

    // 类型标签
    const typeMap = { 'single_choice': '选择题', 'short_answer': '简答题' };
    $('qType').textContent = typeMap[q.type] || q.type;

    // 知识点
    const kps = Array.isArray(q.knowledge_points) ? q.knowledge_points : [];
    $('qKnowledge').textContent = kps.length > 0 ? kps.join('、') : '';

    // 答案 & 解析
    $('qAnswer').textContent = q.answer || '（暂无）';
    $('qAnalysis').textContent = q.analysis || '（暂无）';
  }

  async function sendMessage() {
    const input = $('chatInput');
    const text = input.value.trim();
    if (!text || !currentQuestionId) return;

    input.value = '';
    addMessage('user', text);

    // 显示加载中
    const loadingId = addMessage('loading', 'AI 思考中...');

    try {
      const data = await API.chat(currentQuestionId, text);
      // 移除 loading，添加回复
      removeMessage(loadingId);
      addMessage('ai', data.reply || '（无回复）');

      /* 无论 file:// 还是服务器上的 http://，只要后端 chat 接口没就绪就会
       * 静默回退到内置数据。这里补一条提示，避免把演示数据当成真实 AI 输出。 */
      if (!chatMockNoticeShown && API.isChatMocked && API.isChatMocked()) {
        chatMockNoticeShown = true;
        addMessage('system', '⚠ 上面这条回复来自前端内置演示数据：后端 /api/v1/chat 尚未实现，不是真实 AI 生成。');
      }
    } catch (e) {
      removeMessage(loadingId);
      addMessage('system', '对话失败：' + (e.message || '未知错误'));
    }
  }

  /* ── 消息管理 ── */
  let msgCounter = 0;
  function addMessage(type, text) {
    const id = 'msg-' + (++msgCounter);
    const el = document.createElement('div');
    el.id = id;
    el.className = 'msg msg-' + type;
    /* textContent 由浏览器自动转义，本身安全 */
    el.textContent = text;
    /* AI 回复需要保留换行：必须先转义、再把 \n 换成 <br>，顺序不能颠倒 */
    if (type === 'ai') {
      el.innerHTML = escapeHtml(text).replace(/\n/g, '<br>');
    }
    $('chatMessages').appendChild(el);
    el.scrollIntoView({ behavior: 'smooth', block: 'end' });
    return id;
  }

  function removeMessage(id) {
    const el = document.getElementById(id);
    if (el) el.remove();
  }

  return { show, sendMessage };
})();
