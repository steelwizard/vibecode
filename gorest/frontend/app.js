(function () {
  "use strict";

  function apiBase() {
    const { protocol, hostname } = window.location;
    if (protocol === "http:" || protocol === "https:") {
      const host =
        hostname.includes(":") && !hostname.startsWith("[")
          ? `[${hostname}]`
          : hostname;
      return `${protocol}//${host}:9080`;
    }
    // file:// or other — cannot infer host; default loopback API
    return "http://127.0.0.1:9080";
  }

  const API_BASE = apiBase();
  const api = (path) => API_BASE + path;

  let apiReachable = false;

  const form = document.getElementById("todo-form");
  const titleInput = document.getElementById("todo-title");
  const submitBtn = document.getElementById("submit-btn");
  const listEl = document.getElementById("todo-list");
  const emptyEl = document.getElementById("empty-state");
  const countEl = document.getElementById("todo-count");
  const errorBanner = document.getElementById("error-banner");
  const apiStatus = document.getElementById("api-status");
  const statusText = apiStatus.querySelector(".status-text");

  const dtFmt = new Intl.DateTimeFormat(undefined, {
    dateStyle: "medium",
    timeStyle: "short",
  });

  function setError(msg) {
    if (!msg) {
      errorBanner.hidden = true;
      errorBanner.textContent = "";
      return;
    }
    errorBanner.textContent = msg;
    errorBanner.hidden = false;
  }

  function setApiState(state, text) {
    apiStatus.dataset.state = state;
    statusText.textContent = text;
  }

  async function checkHealth() {
    try {
      // No custom headers: keeps GET as a "simple" request (no CORS preflight) in browsers.
      const res = await fetch(api("/health"));
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const data = await res.json();
      if (data.status !== "ok") throw new Error("bad health payload");
      apiReachable = true;
      setApiState("ok", "API ready");
    } catch (e) {
      if (!apiReachable) {
        setApiState("error", "API unreachable");
      }
      if (typeof console !== "undefined" && console.debug) {
        console.debug("gorest /health failed:", e);
      }
    }
  }

  function renderTodos(todos) {
    listEl.innerHTML = "";
    const n = todos.length;
    countEl.textContent = n === 0 ? "" : n === 1 ? "1 item" : `${n} items`;
    emptyEl.hidden = n > 0;

    for (const todo of todos) {
      const li = document.createElement("li");
      li.className = "todo-item" + (todo.completed ? " is-done" : "");
      const when = dtFmt.format(new Date(todo.created_at));
      li.innerHTML = `
        <label class="todo-check-label">
          <input type="checkbox" class="todo-check" data-todo-id="${todo.id}" ${
        todo.completed ? "checked" : ""
      } />
          <span class="sr-only">Mark todo ${todo.id} as done or not done</span>
        </label>
        <span class="todo-id" title="Todo id">#${todo.id}</span>
        <div class="todo-body">
          <p class="todo-title"></p>
          <p class="todo-meta"></p>
        </div>
        <button type="button" class="btn remove" data-delete-id="${todo.id}" aria-label="Remove todo">Remove</button>
      `;
      li.querySelector(".todo-title").textContent = todo.title;
      li.querySelector(".todo-meta").textContent = when;
      listEl.appendChild(li);
    }
  }

  async function loadTodos() {
    setError("");
    try {
      const res = await fetch(api("/todos"));
      if (!res.ok) throw new Error(`Could not load todos (${res.status})`);
      const todos = await res.json();
      if (!Array.isArray(todos)) throw new Error("Invalid response from server");
      renderTodos(todos);
      apiReachable = true;
      setApiState("ok", "API ready");
    } catch (e) {
      setError(e.message || "Failed to load todos");
      renderTodos([]);
    }
  }

  listEl.addEventListener("change", async (ev) => {
    const input = ev.target;
    if (
      !input ||
      input.type !== "checkbox" ||
      !input.classList.contains("todo-check") ||
      !listEl.contains(input)
    ) {
      return;
    }

    const id = input.getAttribute("data-todo-id");
    if (!id) return;

    const wasChecked = !input.checked;
    const li = input.closest("li.todo-item");
    setError("");
    input.disabled = true;
    try {
      const res = await fetch(api(`/todos/${encodeURIComponent(id)}`), {
        method: "PATCH",
        headers: {
          "Content-Type": "application/json",
          Accept: "application/json",
        },
        body: JSON.stringify({ completed: input.checked }),
      });
      const data = await res.json().catch(() => ({}));
      if (res.status === 404) {
        throw new Error(data.error || "Todo not found");
      }
      if (!res.ok) {
        throw new Error(data.error || `Could not update todo (${res.status})`);
      }
      if (li) {
        li.classList.toggle("is-done", Boolean(data.completed));
      }
    } catch (e) {
      input.checked = wasChecked;
      setError(e.message || "Failed to update todo");
    } finally {
      input.disabled = false;
    }
  });

  listEl.addEventListener("click", async (ev) => {
    const btn = ev.target.closest("button.remove[data-delete-id]");
    if (!btn || !listEl.contains(btn)) return;

    const id = btn.getAttribute("data-delete-id");
    if (!id) return;

    setError("");
    btn.disabled = true;
    try {
      const res = await fetch(api(`/todos/${encodeURIComponent(id)}`), {
        method: "DELETE",
      });
      if (res.status === 404) {
        const data = await res.json().catch(() => ({}));
        throw new Error(data.error || "Todo not found");
      }
      if (!res.ok) {
        const data = await res.json().catch(() => ({}));
        throw new Error(data.error || `Could not remove todo (${res.status})`);
      }
      await loadTodos();
    } catch (e) {
      setError(e.message || "Failed to remove todo");
      btn.disabled = false;
    }
  });

  form.addEventListener("submit", async (ev) => {
    ev.preventDefault();
    const title = titleInput.value.trim();
    if (!title) return;

    setError("");
    submitBtn.disabled = true;
    try {
      const res = await fetch(api("/todos"), {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          Accept: "application/json",
        },
        body: JSON.stringify({ title }),
      });
      const data = await res.json().catch(() => ({}));
      if (!res.ok) {
        const msg = data.error || `Could not create todo (${res.status})`;
        throw new Error(msg);
      }
      titleInput.value = "";
      titleInput.focus();
      await loadTodos();
    } catch (e) {
      setError(e.message || "Failed to create todo");
    } finally {
      submitBtn.disabled = false;
    }
  });

  loadTodos();
  checkHealth();
})();
