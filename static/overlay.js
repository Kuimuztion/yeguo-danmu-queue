(() => {
  const module = document.querySelector(".queue-module");
  const queue = document.querySelector("#queue");
  let lastRevision = null;
  let failedRefreshes = 0;

  function setBackendOnline(online) {
    if (online) {
      failedRefreshes = 0;
      module.classList.remove("backend-offline");
      module.removeAttribute("aria-hidden");
      return;
    }

    failedRefreshes += 1;
    // OBS keeps the last rendered browser frame after the local server exits.
    // Two failed heartbeats mean the backend is gone, so erase that frame.
    if (failedRefreshes >= 2) {
      lastRevision = null;
      queue.replaceChildren();
      module.classList.add("backend-offline");
      module.setAttribute("aria-hidden", "true");
    }
  }

  function render(state) {
    document.documentElement.style.setProperty("--font-size", `${state.font_size}px`);
    // Polling returns the same state every 500ms. Rebuilding unchanged rows would
    // restart their entrance animation and make names bounce continuously.
    if (state.revision === lastRevision) return;
    lastRevision = state.revision;
    const maxRows = Number(state.max_rows || 50);
    queue.replaceChildren(...state.queue.slice(0, maxRows).map((item) => {
      const row = document.createElement("div");
      row.className = `queue-row${item.priority ? " priority" : ""}`;
      row.dataset.id = item.id;

      const rank = document.createElement("span");
      rank.className = "rank";
      rank.textContent = item.position;
      const arrow = document.createElement("span");
      arrow.className = "arrow";
      arrow.textContent = "→";
      const name = document.createElement("span");
      name.className = "name";
      name.textContent = item.name;
      row.append(rank, arrow, name);
      return row;
    }));
  }

  async function refresh() {
    try {
      const response = await fetch("/api/state", { cache: "no-store" });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const state = await response.json();
      setBackendOnline(true);
      render(state);
    } catch (_) {
      setBackendOnline(false);
    }
  }

  refresh();
  setInterval(refresh, 500);
})();
