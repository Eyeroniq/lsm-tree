// lsm-tree dashboard: polls engine statistics and drives the REST API.
(() => {
  "use strict";

  const API = "api/v1";
  const POLL_MS = 600;
  const LEVELS = 7;
  const MAX_CHIPS = 48;
  const HUES = [265, 245, 225, 205, 190, 175, 160];  // violet to teal by depth
  const LEVEL_NOTES = ["overlapping", "", "", "", "", "", "deepest"];

  const $ = (id) => document.getElementById(id);
  const number = new Intl.NumberFormat("en");

  // ---- Theme ---------------------------------------------------------------

  const root = document.documentElement;
  const savedTheme = (() => {
    try { return localStorage.getItem("lsm-theme"); } catch { return null; }
  })();
  if (savedTheme === "light" || savedTheme === "dark") {
    root.dataset.theme = savedTheme;
  }
  $("theme-toggle").addEventListener("click", () => {
    root.dataset.theme = root.dataset.theme === "dark" ? "light" : "dark";
    try { localStorage.setItem("lsm-theme", root.dataset.theme); } catch { /* private mode */ }
  });

  // ---- Helpers -------------------------------------------------------------

  function formatBytes(bytes) {
    if (bytes < 1024) return `${bytes} B`;
    const units = ["KB", "MB", "GB", "TB"];
    let value = bytes / 1024;
    let unit = 0;
    while (value >= 1024 && unit < units.length - 1) {
      value /= 1024;
      unit += 1;
    }
    return `${value >= 100 ? value.toFixed(0) : value.toFixed(1)} ${units[unit]}`;
  }

  function percent(part, whole) {
    return whole > 0 ? `${Math.round((part / whole) * 100)}%` : "—";
  }

  function setStat(id, text) {
    const element = $(id);
    if (element.textContent === text) return;
    element.textContent = text;
    element.classList.add("bump");
    setTimeout(() => element.classList.remove("bump"), 400);
  }

  function toast(message, isError = false) {
    const element = document.createElement("div");
    element.className = `toast${isError ? " error" : ""}`;
    element.textContent = message;
    $("toasts").appendChild(element);
    setTimeout(() => element.remove(), 3400);
  }

  async function request(path, options = {}) {
    const response = await fetch(path, options);
    if (!response.ok && response.status !== 404) {
      let detail = `${response.status} ${response.statusText}`;
      try {
        const problem = await response.json();
        if (problem.detail) detail = problem.detail;
      } catch { /* not JSON */ }
      throw new Error(detail);
    }
    return response;
  }

  // ---- Activity feed -------------------------------------------------------

  const feed = $("feed");

  function log(kind, message) {
    const empty = feed.querySelector(".empty");
    if (empty) empty.remove();
    const item = document.createElement("li");
    item.className = kind;
    const dot = document.createElement("span");
    dot.className = "dot";
    const text = document.createElement("span");
    text.textContent = message;
    const time = document.createElement("time");
    time.textContent = new Date().toLocaleTimeString([], { hour12: false });
    item.append(dot, text, time);
    feed.prepend(item);
    while (feed.children.length > 60) feed.lastElementChild.remove();
  }

  const placeholder = document.createElement("li");
  placeholder.className = "empty";
  placeholder.textContent = "Nothing yet. Write some keys to wake the engine up.";
  feed.appendChild(placeholder);

  // ---- The tree ------------------------------------------------------------

  const levelsElement = $("levels");
  const levelRows = [];
  for (let level = 0; level < LEVELS; level += 1) {
    const row = document.createElement("div");
    row.className = "level";
    const name = document.createElement("span");
    name.className = "level-name";
    name.textContent = `L${level}`;
    name.title = LEVEL_NOTES[level] || `level ${level}`;
    const chips = document.createElement("div");
    chips.className = "chips";
    const count = document.createElement("span");
    count.className = "level-count";
    count.textContent = "0";
    row.append(name, chips, count);
    levelsElement.appendChild(row);
    levelRows.push({ row, chips, count, shown: -1 });
  }

  function renderLevel(level, tables) {
    const view = levelRows[level];
    if (view.shown === tables) return;
    const previous = Math.max(view.shown, 0);
    view.chips.replaceChildren();
    if (tables === 0) {
      const empty = document.createElement("span");
      empty.className = "chips-empty";
      empty.textContent = level === 0 ? "empty: next flush lands here" : "empty";
      view.chips.appendChild(empty);
    } else {
      const visible = Math.min(tables, MAX_CHIPS);
      for (let index = 0; index < visible; index += 1) {
        const chip = document.createElement("span");
        chip.className = "chip";
        chip.style.setProperty("--hue", HUES[level]);
        if (view.shown >= 0 && index >= previous) chip.classList.add("new");
        view.chips.appendChild(chip);
      }
      if (tables > visible) {
        const more = document.createElement("span");
        more.className = "chips-more";
        more.textContent = `+${tables - visible}`;
        view.chips.appendChild(more);
      }
    }
    view.count.textContent = number.format(tables);
    if (view.shown >= 0) {
      view.row.classList.add("flash");
      setTimeout(() => view.row.classList.remove("flash"), 600);
    }
    view.shown = tables;
  }

  // ---- Throughput sparkline -------------------------------------------------

  const samples = new Array(60).fill(0);

  function renderSparkline() {
    const width = 600;
    const height = 56;
    const peak = Math.max(...samples, 1);
    const step = width / (samples.length - 1);
    const points = samples.map((value, index) => [
      index * step,
      height - 4 - (value / peak) * (height - 10),
    ]);
    const line = points.map(([x, y], index) => `${index ? "L" : "M"}${x.toFixed(1)} ${y.toFixed(1)}`).join(" ");
    $("spark-line").setAttribute("d", line);
    $("spark-area").setAttribute("d", `${line} L${width} ${height} L0 ${height} Z`);
  }

  // ---- Polling --------------------------------------------------------------

  let last = null;
  let lastTime = 0;
  let memtablePeak = 1;

  function describeLevels(before, after) {
    const changes = [];
    for (let level = 0; level < LEVELS; level += 1) {
      if (before[level] !== after[level]) changes.push(`L${level} ${before[level]}→${after[level]}`);
    }
    return changes.join(", ");
  }

  function narrate(previous, current) {
    const before = previous.tablesPerLevel;
    const after = current.tablesPerLevel;
    const rewritten = current.compactionBytesWritten - previous.compactionBytesWritten;
    const levelsChanged = before.some((count, level) => count !== after[level]);
    if (rewritten > 0) {
      log("compact", `Compaction rewrote ${formatBytes(rewritten)} · ${describeLevels(before, after)}`);
    } else if (levelsChanged) {
      const total = (levels) => levels.reduce((sum, count) => sum + count, 0);
      if (after[0] > before[0] && total(after) > total(before)) {
        const flushed = after[0] - before[0];
        log("flush", `Memtable flushed → ${flushed} new L0 table${flushed > 1 ? "s" : ""}`);
      } else if (total(after) === total(before)) {
        log("move", `Table moved down without a rewrite · ${describeLevels(before, after)}`);
      } else {
        log("flush", describeLevels(before, after));
      }
    }
  }

  async function poll() {
    try {
      const response = await request(`${API}/admin/stats`);
      const stats = await response.json();
      const now = performance.now();
      if (last) {
        const seconds = (now - lastTime) / 1000;
        const rate = Math.max(0, (stats.sequence - last.sequence) / seconds);
        samples.push(rate);
        samples.shift();
        $("throughput").textContent = number.format(Math.round(rate));
        narrate(last, stats);
      }
      renderSparkline();

      setStat("stat-writes", number.format(stats.sequence));
      setStat("stat-tables", number.format(stats.sstableCount));
      const deepest = stats.tablesPerLevel.reduce((deep, count, level) => (count ? level : deep), -1);
      $("stat-tables-hint").textContent = deepest >= 0 ? `levels L0–L${deepest}` : "none flushed yet";
      setStat("stat-memtable", number.format(stats.memtableEntries));
      setStat("stat-bloom", percent(stats.bloomNegativeHits, stats.bloomChecks));
      setStat("stat-cache", percent(stats.blockCacheHits, stats.blockCacheHits + stats.blockReads));
      setStat("stat-compacted", formatBytes(stats.compactionBytesWritten));

      memtablePeak = Math.max(memtablePeak, stats.memtableEntries);
      $("memtable-fill").style.width = `${Math.min(100, (stats.memtableEntries / memtablePeak) * 100)}%`;
      $("memtable-count").textContent = number.format(stats.memtableEntries);
      stats.tablesPerLevel.forEach((tables, level) => renderLevel(level, tables));

      last = stats;
      lastTime = now;
      setStatus("up", "Live");
    } catch {
      setStatus("down", "Offline");
    }
  }

  function setStatus(state, text) {
    const status = $("status");
    if (status.dataset.state === state) return;
    status.dataset.state = state;
    $("status-text").textContent = text;
  }

  // ---- Actions ---------------------------------------------------------------

  async function busy(button, work) {
    button.disabled = true;
    button.classList.add("busy");
    try {
      await work();
    } catch (error) {
      toast(error.message, true);
      log("error", error.message);
    } finally {
      button.disabled = false;
      button.classList.remove("busy");
    }
  }

  $("load").addEventListener("click", (event) =>
    busy(event.currentTarget, async () => {
      log("write", "Writing 5,000 keys…");
      const response = await request(`${API}/demo/load?count=5000`, { method: "POST" });
      const result = await response.json();
      const seconds = (result.millis / 1000).toFixed(2);
      toast(`Wrote ${number.format(result.written)} keys in ${seconds} s`);
      log("write", `Wrote ${number.format(result.written)} keys in ${seconds} s`);
    }));

  $("read").addEventListener("click", (event) =>
    busy(event.currentTarget, async () => {
      log("read", "Reading 5,000 random keys…");
      const response = await request(`${API}/demo/read?count=5000`, { method: "POST" });
      const result = await response.json();
      const rate = Math.round(result.read / Math.max(result.millis, 1) * 1000);
      const message = `Read ${number.format(result.read)} keys (${number.format(result.found)} found) · ${number.format(rate)} reads/s`;
      toast(message);
      log("read", message);
    }));

  $("flush").addEventListener("click", (event) =>
    busy(event.currentTarget, async () => {
      await request(`${API}/admin/flush`, { method: "POST" });
      toast("Memtable flushed");
    }));

  $("compact").addEventListener("click", (event) =>
    busy(event.currentTarget, async () => {
      await request(`${API}/admin/compact`, { method: "POST" });
      toast("Full compaction finished");
    }));

  // ---- Key explorer -----------------------------------------------------------

  const keyInput = $("kv-key");
  const valueInput = $("kv-value");
  const resultBox = $("kv-result");

  function showResult(tagClass, tagText, text) {
    resultBox.hidden = false;
    resultBox.replaceChildren();
    const tag = document.createElement("span");
    tag.className = `tag ${tagClass}`;
    tag.textContent = tagText;
    resultBox.append(tag, document.createTextNode(text));
  }

  function keyPath() {
    const key = keyInput.value.trim();
    if (!key) {
      keyInput.focus();
      throw new Error("Enter a key first");
    }
    return { key, path: `${API}/kv/${encodeURIComponent(key)}` };
  }

  async function explorer(action) {
    try {
      const { key, path } = keyPath();
      if (action === "put") {
        await request(path, {
          method: "PUT",
          headers: { "Content-Type": "text/plain;charset=UTF-8" },
          body: valueInput.value,
        });
        showResult("ok", "saved", `${key} = ${valueInput.value}`);
        log("write", `put ${key}`);
      } else if (action === "get") {
        const response = await request(path);
        if (response.status === 404) {
          showResult("miss", "not found", key);
        } else {
          const value = await response.text();
          valueInput.value = value;
          showResult("ok", "found", `${key} = ${value}`);
        }
      } else if (action === "delete") {
        await request(path, { method: "DELETE" });
        showResult("gone", "deleted", key);
        log("write", `delete ${key}`);
      }
    } catch (error) {
      toast(error.message, true);
    }
  }

  $("kv-form").addEventListener("submit", (event) => {
    event.preventDefault();
    explorer("put");
  });
  document.querySelectorAll("#kv-form [data-action]").forEach((button) => {
    if (button.type === "button") {
      button.addEventListener("click", () => explorer(button.dataset.action));
    }
  });

  // ---- Range scan ---------------------------------------------------------------

  $("scan-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    const prefix = $("scan-prefix").value;
    // The prefix range ends just after the prefix's last character.
    const end = prefix ? prefix.slice(0, -1) + String.fromCharCode(prefix.charCodeAt(prefix.length - 1) + 1) : "";
    const params = new URLSearchParams({ from: prefix, to: end, limit: "100" });
    try {
      const response = await request(`${API}/kv?${params}`);
      const page = await response.json();
      const list = $("scan-results");
      list.replaceChildren();
      if (page.items.length === 0) {
        const empty = document.createElement("li");
        empty.className = "empty";
        empty.textContent = "No keys with that prefix.";
        list.appendChild(empty);
      }
      page.items.forEach((item, index) => {
        const row = document.createElement("li");
        row.style.animationDelay = `${Math.min(index, 20) * 12}ms`;
        const key = document.createElement("span");
        key.className = "scan-key";
        key.textContent = item.key;
        const value = document.createElement("span");
        value.className = "scan-value";
        value.textContent = item.value;
        row.append(key, value);
        list.appendChild(row);
      });
      const count = $("scan-count");
      count.hidden = false;
      count.textContent = page.truncated ? `first ${page.items.length}` : `${page.items.length} keys`;
    } catch (error) {
      toast(error.message, true);
    }
  });

  poll();
  setInterval(poll, POLL_MS);
})();
