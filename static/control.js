(() => {
  const $ = (selector) => document.querySelector(selector);
  const list = $("#queueList");
  const resultBox = $("#result");
  const commandInput = $("#command");
  let latestState = null;
  let fontTimer = null;

  function safeText(value) { return document.createTextNode(String(value)); }

  function render(state) {
    latestState = state;
    $("#roomId").textContent = state.room_id;
    $("#queueCount").textContent = state.queue.length;
    $("#pendingCount").textContent = state.pending_priority.length;
    $("#keyword").textContent = state.keyword;
    $("#fontSize").value = state.font_size;
    $("#fontOutput").textContent = `${state.font_size}px`;
    $("#overlayUrl").textContent = `${location.origin}/console`;

    const connection = $("#connection");
    connection.className = `connection ${state.live_status.state}`;
    connection.querySelector("span").textContent = state.live_status.message;

    if (!state.queue.length) {
      list.innerHTML = '<p class="empty">暂无排队用户</p>';
    } else {
      list.replaceChildren(...state.queue.map((item) => {
        const row = document.createElement("div");
        row.className = "queue-item";
        const rank = document.createElement("span");
        rank.className = "queue-rank";
        rank.textContent = String(item.position).padStart(2, "0");
        const info = document.createElement("div");
        const name = document.createElement("div");
        name.className = "queue-name";
        name.append(safeText(item.name));
        const meta = document.createElement("div");
        meta.className = "queue-meta";
        meta.textContent = item.priority ? "灯牌优先" : (item.source === "manual" ? "手动加入" : "弹幕加入");
        info.append(name, meta);
        const remove = document.createElement("button");
        remove.className = "remove";
        remove.type = "button";
        remove.title = `删除第 ${item.position} 位`;
        remove.textContent = "×";
        remove.addEventListener("click", () => runCommand(`del-${item.position}`));
        row.append(rank, info, remove);
        return row;
      }));
    }

    const pending = $("#pendingList");
    if (!state.pending_priority.length) {
      pending.innerHTML = '<p class="empty small">暂无记录</p>';
    } else {
      pending.replaceChildren(...state.pending_priority.map((item) => {
        const row = document.createElement("div");
        row.className = "pending-user";
        const name = document.createElement("span");
        name.textContent = item.name;
        const credits = document.createElement("span");
        credits.textContent = `${item.credits} 次优先`;
        row.append(name, credits);
        return row;
      }));
    }
  }

  function showResult(ok, message) {
    resultBox.className = `result ${ok ? "ok" : "error"}`;
    resultBox.textContent = message;
  }

  async function runCommand(command) {
    if (!command.trim()) return;
    try {
      const response = await fetch("/api/command", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ command }),
      });
      const payload = await response.json();
      showResult(payload.ok, payload.message);
      if (!payload.changed) await refresh();
    } catch (error) {
      showResult(false, `连接失败：${error.message}`);
    }
  }

  async function refresh() {
    try {
      const response = await fetch("/api/state");
      render(await response.json());
    } catch (error) {
      showResult(false, `刷新失败：${error.message}`);
    }
  }

  $("#commandForm").addEventListener("submit", (event) => {
    event.preventDefault();
    const command = commandInput.value.trim();
    runCommand(command);
    commandInput.select();
  });

  document.querySelectorAll("[data-fill]").forEach((button) => {
    button.addEventListener("click", () => {
      commandInput.value = button.dataset.fill;
      commandInput.focus();
      commandInput.setSelectionRange(commandInput.value.length, commandInput.value.length);
    });
  });
  document.querySelectorAll("[data-command]").forEach((button) => {
    button.addEventListener("click", () => {
      if (button.dataset.command === "clear" && latestState?.queue.length && !confirm("确定清空整个队列吗？")) return;
      runCommand(button.dataset.command);
    });
  });
  $("#refresh").addEventListener("click", refresh);
  $("#testQueue").addEventListener("click", () => runCommand(`test-queue-${$("#testName").value.trim()}`));
  $("#testGift").addEventListener("click", () => runCommand(`test-gift-${$("#testName").value.trim()}`));
  $("#fontSize").addEventListener("input", (event) => {
    const size = event.target.value;
    $("#fontOutput").textContent = `${size}px`;
    clearTimeout(fontTimer);
    fontTimer = setTimeout(() => runCommand(`font-${size}`), 110);
  });

  refresh();
  setInterval(refresh, 800);
})();
