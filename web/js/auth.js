/* ── 登录页 ── */
const AuthPage = (() => {
  const $ = (id) => document.getElementById(id);

  function show() {
    $('page-login').classList.remove('hidden');
    $('navbar').classList.add('hidden');
    $('page-questions').classList.add('hidden');
    $('page-chat').classList.add('hidden');
    $('loginError').classList.add('hidden');
    $('username').value = '';
    $('password').value = '';
    $('username').focus();
  }

  async function handleLogin(e) {
    e.preventDefault();
    const btn = $('loginBtn');
    const errEl = $('loginError');
    btn.disabled = true;
    btn.textContent = '登录中...';
    errEl.classList.add('hidden');

    try {
      const username = $('username').value.trim();
      const password = $('password').value;
      const data = await API.login(username, password);
      // data: { token, user: { id, username, name, role } }
      API.setAuth(data.token, data.user);
      // 跳转到题目列表
      navigate('#questions');
    } catch (e) {
      errEl.textContent = e.message || '登录失败，请重试';
      errEl.classList.remove('hidden');
    } finally {
      btn.disabled = false;
      btn.textContent = '登 录';
    }
  }

  return { show, handleLogin };
})();