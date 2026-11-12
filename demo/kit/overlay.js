// Recording overlays, injected into every page by Playwright (add_init_script):
// a visible cursor that pulses on click, a chapter pill, and full-screen
// intro/outro cards. Exposed as window.__demo.
(() => {
  if (window.__demo) return;

  const css = `
    #__demo-cursor {
      position: fixed; left: 0; top: 0; z-index: 2147483647;
      width: 22px; height: 22px; margin: -11px 0 0 -11px; border-radius: 50%;
      background: radial-gradient(circle, rgba(255, 221, 87, .95) 0 38%, rgba(255, 214, 10, .35) 60%, rgba(255, 214, 10, 0) 72%);
      box-shadow: 0 0 18px 4px rgba(255, 214, 10, .35);
      pointer-events: none; transition: opacity .2s; opacity: 0;
    }
    #__demo-cursor.pulse::after {
      content: ""; position: absolute; inset: -14px; border-radius: 50%;
      border: 2px solid rgba(255, 214, 10, .85);
      animation: __demo-pulse .5s ease-out forwards;
    }
    @keyframes __demo-pulse { from { transform: scale(.35); opacity: 1; } to { transform: scale(1.25); opacity: 0; } }

    #__demo-chapter {
      position: fixed; top: 16px; right: 16px; z-index: 2147483646;
      display: flex; align-items: center; gap: 10px;
      padding: 7px 16px 7px 7px; border-radius: 999px;
      background: rgba(18, 20, 34, .62); border: 1px solid rgba(255, 255, 255, .16);
      backdrop-filter: blur(14px) saturate(140%); -webkit-backdrop-filter: blur(14px) saturate(140%);
      box-shadow: 0 10px 30px -12px rgba(0, 0, 0, .6);
      color: #f4f5fa; font: 600 14px/1 "Segoe UI", system-ui, sans-serif; letter-spacing: .01em;
      pointer-events: none; opacity: 0; transform: translateY(-8px);
      transition: opacity .35s, transform .35s;
    }
    #__demo-chapter.show { opacity: 1; transform: none; }
    #__demo-chapter b {
      display: grid; place-items: center; min-width: 30px; height: 26px; padding: 0 8px; border-radius: 999px;
      background: linear-gradient(135deg, #8b5cf6, #22d3ee); color: #0b0d14; font-weight: 800; font-size: 13px;
    }

    #__demo-card {
      position: fixed; inset: 0; z-index: 2147483645;
      display: grid; place-items: center; padding: 48px;
      background:
        radial-gradient(800px 500px at 15% 10%, rgba(139, 92, 246, .35), transparent 60%),
        radial-gradient(700px 500px at 90% 95%, rgba(34, 211, 238, .25), transparent 60%),
        #080a14;
      color: #eef0f7; font-family: "Segoe UI", system-ui, sans-serif;
      opacity: 0; transition: opacity .45s; pointer-events: none;
    }
    #__demo-card.show { opacity: 1; }
    #__demo-card .inner { width: min(980px, 100%); }
    #__demo-card .eyebrow {
      display: inline-flex; align-items: center; gap: 10px; margin-bottom: 22px;
      font: 700 13px/1 "Segoe UI", sans-serif; letter-spacing: .18em; text-transform: uppercase; color: #a5b4fc;
    }
    #__demo-card .eyebrow svg { width: 26px; height: 26px; }
    #__demo-card h1 {
      margin: 0 0 16px; font-size: 76px; font-weight: 800; letter-spacing: -.035em; line-height: 1;
      background: linear-gradient(120deg, #ffffff 30%, #c4b5fd 60%, #67e8f9); -webkit-background-clip: text; background-clip: text; color: transparent;
    }
    #__demo-card p.pitch { margin: 0 0 30px; font-size: 25px; line-height: 1.4; color: #c9cede; max-width: 860px; }
    #__demo-card .chips { display: flex; flex-wrap: wrap; gap: 10px; }
    #__demo-card .chips span {
      padding: 8px 14px; border-radius: 999px; font-size: 15px; font-weight: 600; color: #dfe3f1;
      background: rgba(255, 255, 255, .07); border: 1px solid rgba(255, 255, 255, .13);
    }
    #__demo-card h2 { margin: 0 0 26px; font-size: 44px; font-weight: 800; letter-spacing: -.02em; }
    #__demo-card ul { display: grid; grid-template-columns: 1fr 1fr; gap: 14px 40px; margin: 0 0 30px; padding: 0; list-style: none; }
    #__demo-card li { display: flex; gap: 12px; align-items: baseline; font-size: 20px; color: #dfe3f1; }
    #__demo-card li::before {
      content: ""; flex: none; width: 9px; height: 9px; border-radius: 50%; transform: translateY(-2px);
      background: linear-gradient(135deg, #8b5cf6, #22d3ee);
    }
    #__demo-card .foot { font-size: 18px; color: #9aa3ba; }
    #__demo-card .foot strong { color: #eef0f7; }
  `;

  const logo = `<svg viewBox="0 0 32 32"><defs><linearGradient id="__dg" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#8b5cf6"/><stop offset="1" stop-color="#22d3ee"/></linearGradient></defs><rect x="4" y="5" width="24" height="5" rx="2.5" fill="url(#__dg)"/><rect x="7" y="13.5" width="18" height="5" rx="2.5" fill="url(#__dg)" opacity=".75"/><rect x="10" y="22" width="12" height="5" rx="2.5" fill="url(#__dg)" opacity=".5"/></svg>`;

  function mount() {
    if (document.getElementById("__demo-cursor")) return;
    const style = document.createElement("style");
    style.textContent = css;
    document.documentElement.appendChild(style);
    for (const id of ["__demo-cursor", "__demo-chapter", "__demo-card"]) {
      const element = document.createElement("div");
      element.id = id;
      document.documentElement.appendChild(element);
    }
  }

  const escape = (text) => String(text).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);

  window.addEventListener("mousemove", (event) => {
    const cursor = document.getElementById("__demo-cursor");
    if (!cursor) return;
    cursor.style.opacity = "1";
    cursor.style.transform = `translate(${event.clientX}px, ${event.clientY}px)`;
  }, true);
  window.addEventListener("mousedown", () => {
    const cursor = document.getElementById("__demo-cursor");
    if (!cursor) return;
    cursor.classList.remove("pulse");
    void cursor.offsetWidth;
    cursor.classList.add("pulse");
  }, true);

  // The cursor hides behind full-screen cards until the mouse moves again.
  function hideCursor() {
    const cursor = document.getElementById("__demo-cursor");
    if (cursor) cursor.style.opacity = "0";
  }

  window.__demo = {
    chapter(number, text) {
      mount();
      const pill = document.getElementById("__demo-chapter");
      pill.innerHTML = `<b>${String(number).padStart(2, "0")}</b><span>${escape(text)}</span>`;
      pill.classList.add("show");
    },
    hideChapter() {
      mount();
      document.getElementById("__demo-chapter").classList.remove("show");
    },
    intro({ eyebrow, title, pitch, uses }) {
      mount();
      const card = document.getElementById("__demo-card");
      card.innerHTML = `<div class="inner">
        <div class="eyebrow">${logo}<span>${escape(eyebrow)}</span></div>
        <h1>${escape(title)}</h1>
        <p class="pitch">${escape(pitch)}</p>
        <div class="chips">${uses.map((use) => `<span>${escape(use)}</span>`).join("")}</div>
      </div>`;
      card.classList.add("show");
      hideCursor();
    },
    outro({ title, features, foot }) {
      mount();
      const card = document.getElementById("__demo-card");
      card.innerHTML = `<div class="inner">
        <div class="eyebrow">${logo}<span>lsm-tree</span></div>
        <h2>${escape(title)}</h2>
        <ul>${features.map((feature) => `<li>${escape(feature)}</li>`).join("")}</ul>
        <div class="foot">${foot}</div>
      </div>`;
      card.classList.add("show");
      hideCursor();
    },
    hideCard() {
      mount();
      document.getElementById("__demo-card").classList.remove("show");
    },
  };

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", mount);
  } else {
    mount();
  }
})();
