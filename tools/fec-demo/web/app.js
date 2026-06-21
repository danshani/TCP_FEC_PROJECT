"use strict";
const $ = (id) => document.getElementById(id);

let fileBuf = null;          // ArrayBuffer of the chosen file, or null = random
let fileName = null;
let animGen = 0;             // cancels a stale animation when a new run starts

// ----------------------------------------------------------------- controls
const drop = $("drop");
drop.addEventListener("click", () => $("file").click());
$("file").addEventListener("change", (e) => { if (e.target.files[0]) takeFile(e.target.files[0]); });
["dragover", "dragenter"].forEach(ev => drop.addEventListener(ev, e => {
  e.preventDefault(); drop.classList.add("hover");
}));
["dragleave", "drop"].forEach(ev => drop.addEventListener(ev, e => {
  e.preventDefault(); drop.classList.remove("hover");
}));
drop.addEventListener("drop", e => { if (e.dataTransfer.files[0]) takeFile(e.dataTransfer.files[0]); });

function takeFile(f) {
  const r = new FileReader();
  r.onload = () => {
    fileBuf = r.result; fileName = f.name;
    $("fileinfo").textContent = `${f.name} — ${fmtBytes(f.size)}`;
  };
  r.readAsArrayBuffer(f);
}

$("loss").addEventListener("input", e => $("lossval").textContent = (+e.target.value).toFixed(2) + "%");
$("speed").addEventListener("input", e => {
  const v = +e.target.value;
  $("spdval").textContent = v > 80 ? "instant" : v > 55 ? "fast" : v > 30 ? "medium" : "slow";
});
$("run").addEventListener("click", run);
$("sweep").addEventListener("click", runSweep);

// ----------------------------------------------------------------- helpers
function fmtBytes(n) {
  if (n >= 1 << 20) return (n / (1 << 20)).toFixed(2) + " MiB";
  if (n >= 1 << 10) return (n / (1 << 10)).toFixed(1) + " KiB";
  return n + " B";
}
function fmtMs(ms) {
  return ms >= 1000 ? (ms / 1000).toFixed(2) + " s" : Math.round(ms) + " ms";
}
function qs() {
  const p = new URLSearchParams({
    k: $("k").value, r: $("r").value, sym: $("sym").value,
    loss: $("loss").value, seed: $("seed").value,
  });
  if (!fileBuf) p.set("size", "262144");
  return p.toString();
}
const sleep = (ms) => new Promise(res => setTimeout(res, ms));

async function post(path) {
  const res = await fetch(path + "?" + qs(), {
    method: "POST",
    headers: { "Content-Type": "application/octet-stream" },
    body: fileBuf || null,
  });
  const j = await res.json();
  if (!res.ok || j.error) throw new Error(j.error || ("HTTP " + res.status));
  return j;
}

// ----------------------------------------------------------------- run + animate
async function run() {
  $("run").disabled = true; $("sweep").disabled = true;
  $("status").textContent = "running real codec…";
  try {
    const data = await post("/api/simulate");
    $("status").textContent =
      `${data.params.blocks} blocks · ${data.params.segments} segments · ` +
      `recover_failures=${data.totals.recover_failures}`;
    await animate(data);
    showResults(data);
  } catch (e) {
    $("status").textContent = "error: " + e.message;
  } finally {
    $("run").disabled = false; $("sweep").disabled = false;
  }
}

async function animate(data) {
  const gen = ++animGen;
  const grid = $("grid");
  grid.innerHTML = "";
  const blocks = data.blocks;
  const k = data.params.k, r = data.params.r;
  const wireTotal = data.totals.wire_packets_fec;

  let sent = 0, lost = 0, rec = 0, bust = 0;
  $("c_over").textContent = data.totals.overhead_pct + "%";

  const speed = +$("speed").value;
  const perTick = Math.max(1, Math.ceil(blocks.length / 400) * (speed > 80 ? 4 : 1));
  const delay = Math.round(120 * Math.pow(1 - speed / 100, 1.6)) + 2;

  for (let bi = 0; bi < blocks.length; bi++) {
    if (gen !== animGen) return;                 // a newer run took over
    const b = blocks[bi];
    const lostSrc = new Set(b.lost_src), lostRed = new Set(b.lost_red);

    const row = document.createElement("div");
    row.className = "blk" + (b.recovered ? "" : " bust");
    const idx = document.createElement("span");
    idx.className = "idx"; idx.textContent = "#" + bi;
    row.appendChild(idx);

    for (let i = 0; i < b.k; i++) {
      const c = document.createElement("div");
      c.className = "cell in " + (lostSrc.has(i) ? (b.recovered ? "rec" : "lost") : "src");
      row.appendChild(c);
    }
    for (let j = 0; j < r; j++) {
      const c = document.createElement("div");
      c.className = "cell in " + (lostRed.has(j) ? "lost" : "par");
      row.appendChild(c);
    }
    grid.appendChild(row);

    sent += b.k + r;
    lost += b.lost_src.length + b.lost_red.length;
    if (b.recovered) rec += b.lost_src.length; else bust++;

    if (bi % perTick === 0 || bi === blocks.length - 1) {
      $("c_sent").textContent = sent;
      $("c_lost").textContent = lost;
      $("c_rec").textContent = rec;
      $("c_bust").textContent = bust;
      const prog = Math.round((bi + 1) / blocks.length * 100);
      $("c_prog").textContent = prog + "%";
      $("progbar").style.width = prog + "%";
      grid.scrollTop = grid.scrollHeight;
      await sleep(delay);
    }
  }
  // final exact totals from the server (authoritative)
  $("c_sent").textContent = wireTotal;
  $("c_lost").textContent = data.totals.lost_packets;
  $("c_rec").textContent = data.totals.recovered_syms;
  $("c_bust").textContent = data.totals.busted_blocks;
}

// ----------------------------------------------------------------- results
function showResults(data) {
  const t = data.timing_ms;
  const max = Math.max(t.tcp_rto, t.tcp_fast, t.fec);
  const rows = [
    ["FEC (k=" + data.params.k + ", r=" + data.params.r + ")", t.fec, "var(--rec)"],
    ["Plain TCP — RTO only (this stack)", t.tcp_rto, "var(--lost)"],
    ["Idealised TCP — fast-retransmit", t.tcp_fast, "#8b949e"],
  ];
  $("bars").innerHTML = rows.map(([lab, ms, col]) => `
    <div class="brow">
      <div>${lab}</div>
      <div class="track"><div class="fill" style="width:${Math.max(1.5, ms / max * 100)}%;background:${col}"></div></div>
      <div class="val">${fmtMs(ms)}</div>
    </div>`).join("");

  const vsRto = t.tcp_rto / t.fec, vsFast = t.tcp_fast / t.fec;
  let verdict = `At <b>${data.params.loss_pct}%</b> loss, FEC delivered ${fmtBytes(data.params.file_bytes)} in ` +
    `<b>${fmtMs(t.fec)}</b> — <b>${vsRto.toFixed(1)}×</b> faster than this stack's RTO-only TCP`;
  verdict += vsFast >= 1
    ? `, and <b>${vsFast.toFixed(1)}×</b> faster than idealised fast-retransmit TCP.`
    : `. Against idealised fast-retransmit TCP it is <b>${(1 / vsFast).toFixed(1)}× slower</b> here — ` +
      `loss has exceeded the block's r-symbol capacity (raise r or lower k).`;
  if (data.totals.recover_failures > 0)
    verdict += ` ⚠ ${data.totals.recover_failures} recovery byte-mismatches (codec bug!).`;
  $("verdict").innerHTML = verdict;
  $("resultPanel").hidden = false;
}

// ----------------------------------------------------------------- sweep chart
async function runSweep() {
  $("run").disabled = true; $("sweep").disabled = true;
  $("status").textContent = "running loss sweep…";
  try {
    const data = await post("/api/sweep");
    drawChart(data);
    $("sweepPanel").hidden = false;
    $("status").textContent = "sweep done";
  } catch (e) {
    $("status").textContent = "error: " + e.message;
  } finally {
    $("run").disabled = false; $("sweep").disabled = false;
  }
}

function drawChart(data) {
  const rows = data.rows;
  const W = 880, H = 460, L = 70, R = 210, T = 30, B = 60, pw = W - L - R, ph = H - T - B;
  const xs = rows.map(r => r.loss_pct);
  const series = [
    ["Plain TCP (RTO-only)", "#f85149", rows.map(r => r.tcp_rto)],
    ["Idealised TCP (fast-rtx)", "#8b949e", rows.map(r => r.tcp_fast)],
    [`FEC k=${data.params.k},r=${data.params.r}`, "#3fb950", rows.map(r => r.fec)],
  ];
  const all = series.flatMap(s => s[2]).filter(v => v > 0);
  const lo = Math.pow(10, Math.floor(Math.log10(Math.min(...all))));
  const hi = Math.pow(10, Math.ceil(Math.log10(Math.max(...all))));
  const px = (i) => L + pw * i / (xs.length - 1);
  const py = (v) => T + ph * (1 - (Math.log10(Math.max(v, lo)) - Math.log10(lo)) / (Math.log10(hi) - Math.log10(lo)));

  let s = `<svg viewBox="0 0 ${W} ${H}" xmlns="http://www.w3.org/2000/svg" font-family="Segoe UI,Arial">`;
  s += `<rect width="${W}" height="${H}" fill="#0e1117"/>`;
  for (let d = Math.log10(lo); d <= Math.log10(hi) + 0.01; d++) {
    const v = Math.pow(10, d), y = py(v);
    s += `<line x1="${L}" y1="${y}" x2="${L + pw}" y2="${y}" stroke="#30363d"/>`;
    s += `<text x="${L - 8}" y="${y + 4}" fill="#8b949e" font-size="11" text-anchor="end">${v >= 1000 ? v / 1000 + "s" : v + "ms"}</text>`;
  }
  xs.forEach((x, i) => {
    s += `<text x="${px(i)}" y="${T + ph + 18}" fill="#8b949e" font-size="11" text-anchor="middle">${x}</text>`;
  });
  s += `<text x="${L + pw / 2}" y="${H - 14}" fill="#e6edf3" font-size="13" text-anchor="middle">packet loss (%)</text>`;
  s += `<text transform="rotate(-90 16 ${T + ph / 2})" x="16" y="${T + ph / 2}" fill="#e6edf3" font-size="13" text-anchor="middle">completion time (log)</text>`;
  series.forEach(([lab, col, ys], si) => {
    const pts = ys.map((v, i) => `${px(i).toFixed(1)},${py(v).toFixed(1)}`).join(" ");
    s += `<polyline points="${pts}" fill="none" stroke="${col}" stroke-width="2.5"/>`;
    ys.forEach((v, i) => s += `<circle cx="${px(i).toFixed(1)}" cy="${py(v).toFixed(1)}" r="3" fill="${col}"/>`);
    const ly = T + 6 + si * 22;
    s += `<rect x="${L + pw + 24}" y="${ly - 9}" width="15" height="4" fill="${col}"/>`;
    s += `<text x="${L + pw + 45}" y="${ly - 3}" fill="#e6edf3" font-size="12">${lab}</text>`;
  });
  s += `</svg>`;
  $("chart").innerHTML = s;
}
