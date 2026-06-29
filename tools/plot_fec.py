#!/usr/bin/env python3
"""
Render the two presentation graphs from the experiment CSVs as standalone SVGs
(no third-party dependencies — pure stdlib, so it runs anywhere).

  results/graph1_geometry.csv   -> results/graph1_geometry.svg
  results/graph2_loss_sweep.csv -> results/graph2_loss_sweep.svg

Usage (from repo root):  python3 tools/plot_fec.py
"""
import csv, math, os

RESULTS = "results"
COLORS = ["#d62728", "#1f77b4", "#2ca02c", "#9467bd", "#ff7f0e", "#8c564b"]


def read_rows(path):
    with open(path) as f:
        return [{k: v for k, v in row.items()} for row in csv.DictReader(f)]


# ---------------------------------------------------------------- svg helpers
def _esc(s):
    return str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def line_chart(series, xticks, xlabel, ylabel, title, outpath,
               logy=True, ymin=None, ymax=None, x_is_category=True):
    """
    series: list of dicts {label, color, points:[(x,y)]}
    xticks: list of (xvalue, label) — also defines category positions/order.
    """
    W, H = 880, 540
    L, R, T, B = 90, 210, 60, 80          # margins (R wide for legend)
    pw, ph = W - L - R, H - T - B

    xs = [t[0] for t in xticks]
    xpos = {xv: L + (pw * i / max(1, len(xs) - 1)) for i, xv in enumerate(xs)} \
        if x_is_category else None

    def px(xv):
        if x_is_category:
            return xpos[xv]
        return L + pw * (xv - xs[0]) / (xs[-1] - xs[0] or 1)

    ally = [y for s in series for _, y in s["points"] if y > 0]
    lo = ymin if ymin is not None else min(ally)
    hi = ymax if ymax is not None else max(ally)
    if logy:
        lo = 10 ** math.floor(math.log10(lo))
        hi = 10 ** math.ceil(math.log10(hi))

        def py(yv):
            yv = max(yv, lo)
            return T + ph * (1 - (math.log10(yv) - math.log10(lo)) /
                             (math.log10(hi) - math.log10(lo)))
    else:
        def py(yv):
            return T + ph * (1 - (yv - lo) / (hi - lo or 1))

    out = []
    out.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
               f'viewBox="0 0 {W} {H}" font-family="Segoe UI,Arial,sans-serif">')
    out.append(f'<rect width="{W}" height="{H}" fill="white"/>')
    out.append(f'<text x="{W/2}" y="32" text-anchor="middle" '
               f'font-size="20" font-weight="bold">{_esc(title)}</text>')

    # y gridlines + labels
    if logy:
        decades = range(int(math.log10(lo)), int(math.log10(hi)) + 1)
        yvals = [10 ** d for d in decades]
    else:
        yvals = [lo + (hi - lo) * i / 5 for i in range(6)]
    for yv in yvals:
        y = py(yv)
        out.append(f'<line x1="{L}" y1="{y:.1f}" x2="{L+pw}" y2="{y:.1f}" '
                   f'stroke="#e0e0e0" stroke-width="1"/>')
        lab = f"{yv/1000:g}s" if yv >= 1000 else f"{yv:g}ms"
        out.append(f'<text x="{L-10}" y="{y+4:.1f}" text-anchor="end" '
                   f'font-size="12" fill="#444">{lab}</text>')

    # x ticks
    for xv, lab in xticks:
        x = px(xv)
        out.append(f'<line x1="{x:.1f}" y1="{T+ph}" x2="{x:.1f}" y2="{T+ph+5}" '
                   f'stroke="#444"/>')
        out.append(f'<text x="{x:.1f}" y="{T+ph+22:.1f}" text-anchor="middle" '
                   f'font-size="12" fill="#444">{_esc(lab)}</text>')

    # axes
    out.append(f'<line x1="{L}" y1="{T}" x2="{L}" y2="{T+ph}" stroke="#333" stroke-width="1.5"/>')
    out.append(f'<line x1="{L}" y1="{T+ph}" x2="{L+pw}" y2="{T+ph}" stroke="#333" stroke-width="1.5"/>')
    out.append(f'<text x="{L+pw/2}" y="{H-22}" text-anchor="middle" '
               f'font-size="14">{_esc(xlabel)}</text>')
    out.append(f'<text x="28" y="{T+ph/2}" text-anchor="middle" font-size="14" '
               f'transform="rotate(-90 28 {T+ph/2})">{_esc(ylabel)}</text>')

    # series
    for s in series:
        pts = " ".join(f"{px(x):.1f},{py(y):.1f}" for x, y in s["points"])
        out.append(f'<polyline points="{pts}" fill="none" '
                   f'stroke="{s["color"]}" stroke-width="2.5"/>')
        for x, y in s["points"]:
            out.append(f'<circle cx="{px(x):.1f}" cy="{py(y):.1f}" r="3.5" '
                       f'fill="{s["color"]}"/>')

    # legend
    lx, ly = L + pw + 24, T + 6
    for i, s in enumerate(series):
        yy = ly + i * 24
        out.append(f'<rect x="{lx}" y="{yy-10}" width="16" height="4" fill="{s["color"]}"/>')
        out.append(f'<text x="{lx+24}" y="{yy-4}" font-size="13">{_esc(s["label"])}</text>')

    out.append("</svg>")
    with open(outpath, "w") as f:
        f.write("\n".join(out))
    print("wrote", outpath)


# ---------------------------------------------------------------- graph 1
def build_graph1():
    rows = read_rows(os.path.join(RESULTS, "graph1_geometry.csv"))
    ks = sorted({int(r["k"]) for r in rows})
    rs = sorted({int(r["r"]) for r in rows})
    loss = rows[0]["loss_pct"]
    series = []
    for i, rr in enumerate(rs):
        pts = []
        for k in ks:
            m = [x for x in rows if int(x["k"]) == k and int(x["r"]) == rr]
            if m:
                pts.append((k, float(m[0]["t_fec_ms"])))
        series.append({"label": f"r = {rr} parity", "color": COLORS[i + 1], "points": pts})
    # plain-TCP reference (roughly constant across geometry at this loss)
    tcp = sum(float(r["t_tcp_ms"]) for r in rows) / len(rows)
    series.insert(0, {"label": "Plain TCP (no FEC)", "color": COLORS[0],
                      "points": [(k, tcp) for k in ks]})
    xticks = [(k, f"k={k}\n(n={k}+r)") for k in ks]
    # SVG text can't wrap; flatten label
    xticks = [(k, f"k={k}") for k in ks]
    line_chart(series, xticks,
               xlabel="Block size k  (n = k + r symbols on the wire)",
               ylabel="Transfer completion time  (log scale)",
               title=f"Completion time by FEC geometry  (loss = {float(loss):g}%, 256 KiB)",
               outpath=os.path.join(RESULTS, "graph1_geometry.svg"),
               logy=True)


# ---------------------------------------------------------------- graph 2
def build_graph2():
    rows = read_rows(os.path.join(RESULTS, "graph2_loss_sweep.csv"))
    configs = []
    for r in rows:
        key = (int(r["k"]), int(r["r"]))
        if key not in configs:
            configs.append(key)
    losses = sorted({float(r["loss_pct"]) for r in rows})
    series = []
    # TCP baselines (use the first config's run for the t_tcp* columns)
    base = [x for x in rows if int(x["k"]) == configs[0][0] and int(x["r"]) == configs[0][1]]
    base.sort(key=lambda x: float(x["loss_pct"]))
    series.append({"label": "Plain TCP (RTO-only, this stack)", "color": COLORS[0],
                   "points": [(float(x["loss_pct"]), float(x["t_tcp_ms"])) for x in base]})
    series.append({"label": "Idealised TCP (SACK fast-recovery)", "color": "#7f7f7f",
                   "points": [(float(x["loss_pct"]), float(x["t_tcp_fast_ms"])) for x in base]})
    for i, (k, rr) in enumerate(configs):
        m = [x for x in rows if int(x["k"]) == k and int(x["r"]) == rr]
        m.sort(key=lambda x: float(x["loss_pct"]))
        ov = float(m[0]["overhead_pct"])
        series.append({"label": f"FEC k={k},r={rr} ({ov:g}% ovh)",
                       "color": COLORS[i + 1],
                       "points": [(float(x["loss_pct"]), float(x["t_fec_ms"])) for x in m]})
    xticks = [(l, f"{l:g}") for l in losses]
    line_chart(series, xticks,
               xlabel="Packet loss rate  (%)",
               ylabel="Transfer completion time  (log scale)",
               title="Completion time vs loss: FEC recovers locally, TCP waits for RTO",
               outpath=os.path.join(RESULTS, "graph2_loss_sweep.svg"),
               logy=True)


# ---------------------------------------------------------------- graph 3
def build_graph3():
    """Scatter: bandwidth overhead vs the highest loss rate at which FEC still
    beats idealised SACK-TCP, one point per geometry. Upper-left is better
    (more loss tolerated for less overhead)."""
    rows = read_rows(os.path.join(RESULTS, "graph3_winband.csv"))
    pts = [(float(r["overhead_pct"]), float(r["crossover_high_pct"]),
            f'k={r["k"]},r={r["r"]}') for r in rows]

    W, H = 880, 540
    L, R, T, B = 80, 40, 60, 80
    pw, ph = W - L - R, H - T - B
    xmax = max(p[0] for p in pts) * 1.12
    ymax = max(p[1] for p in pts) * 1.18 or 1
    px = lambda x: L + pw * x / xmax
    py = lambda y: T + ph * (1 - y / ymax)

    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
           f'viewBox="0 0 {W} {H}" font-family="Segoe UI,Arial,sans-serif">',
           f'<rect width="{W}" height="{H}" fill="white"/>',
           f'<text x="{W/2}" y="32" text-anchor="middle" font-size="19" '
           f'font-weight="bold">Loss tolerated before real (SACK) TCP wins — by FEC geometry</text>']
    for i in range(0, int(ymax) + 1):
        y = py(i)
        out.append(f'<line x1="{L}" y1="{y:.1f}" x2="{L+pw}" y2="{y:.1f}" stroke="#eee"/>')
        out.append(f'<text x="{L-8}" y="{y+4:.1f}" text-anchor="end" font-size="11" fill="#555">{i}%</text>')
    for xv in range(0, int(xmax) + 1, 10):
        x = px(xv)
        out.append(f'<text x="{x:.1f}" y="{T+ph+20:.1f}" text-anchor="middle" font-size="11" fill="#555">{xv}%</text>')
    out.append(f'<line x1="{L}" y1="{T}" x2="{L}" y2="{T+ph}" stroke="#333"/>')
    out.append(f'<line x1="{L}" y1="{T+ph}" x2="{L+pw}" y2="{T+ph}" stroke="#333"/>')
    out.append(f'<text x="{L+pw/2}" y="{H-30}" text-anchor="middle" font-size="14">bandwidth overhead  r/k  (%)</text>')
    out.append(f'<text x="22" y="{T+ph/2}" text-anchor="middle" font-size="14" '
               f'transform="rotate(-90 22 {T+ph/2})">max loss FEC still beats SACK-TCP (%)</text>')
    for x, y, lab in pts:
        out.append(f'<circle cx="{px(x):.1f}" cy="{py(y):.1f}" r="6" fill="#2ca02c" stroke="#176117"/>')
        out.append(f'<text x="{px(x)+10:.1f}" y="{py(y)+4:.1f}" font-size="12" fill="#222">{_esc(lab)}</text>')
    out.append("</svg>")
    with open(os.path.join(RESULTS, "graph3_winband.svg"), "w") as f:
        f.write("\n".join(out))
    print("wrote", os.path.join(RESULTS, "graph3_winband.svg"))


if __name__ == "__main__":
    build_graph1()
    build_graph2()
    build_graph3()
