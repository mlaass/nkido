"""Flow diagram of the autoresearch setup, in the deck's colours.

usage: uv run --project <repo>/experiments python flow.py   -> flow.png (2400x1350)
Content follows scripts/autoresearch/run.py (loop) and verify.py (_verify gate order).
"""
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch

BG, CARD, FG, MUTED, LINE = "#212121", "#2E2E2E", "#D9D9D9", "#9E9E9E", "#555555"
AMBER, RED = "#F1C232", "#E06C5A"
plt.rcParams["font.family"] = "DejaVu Sans"

fig, ax = plt.subplots(figsize=(16, 9), dpi=150, facecolor=BG)
ax.set_xlim(0, 16)
ax.set_ylim(0, 9)
ax.axis("off")
fig.subplots_adjust(0, 0, 1, 1)


def box(x, y, w, h, title, body="", edge=LINE, title_color=AMBER, fill=CARD, size=17):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0,rounding_size=0.12",
                                fc=fill, ec=edge, lw=2))
    ax.text(x + 0.22, y + h - 0.2, title, color=title_color, fontsize=size, fontweight="bold", va="top")
    if body:
        ax.text(x + 0.22, y + h - 0.72, body, color=FG, fontsize=12.5, va="top", linespacing=1.45)


def arrow(p, q, color=FG, label="", lx=0, ly=0.18, conn="arc3", ha="center"):
    ax.add_patch(FancyArrowPatch(p, q, arrowstyle="-|>", mutation_scale=22, lw=2.2, color=color,
                                 connectionstyle=conn, shrinkA=0, shrinkB=0))
    if label:
        ax.text((p[0] + q[0]) / 2 + lx, (p[1] + q[1]) / 2 + ly, label, color=color, fontsize=12,
                ha=ha, va="bottom", fontweight="bold")


# sweep strip
ax.add_patch(FancyBboxPatch((0.4, 8.15), 15.2, 0.6, boxstyle="round,pad=0,rounding_size=0.1",
                            fc=BG, ec=LINE, lw=1.5))
ax.text(0.62, 8.45, "Sweep", color=AMBER, fontsize=14, fontweight="bold", va="center")
ax.text(1.75, 8.45, "one run = one model on one opcode family  ·  5 families  ·  8 iterations per run  ·  "
        "cost cap per run  ·  one benchmark at a time",
        color=MUTED, fontsize=12, va="center")

# the loop, left to right
box(0.4, 5.35, 3.5, 2.35, "1  Prompt",
    "source of the opcode\ncurrent timings\ndiff accepted so far\nearlier attempts + why rejected")
box(4.6, 5.35, 3.5, 2.35, "2  Agent",
    "edits the C++ in its own\ngit worktree\n30 min, 10 quick checks\n(build, equality, timing)")
box(8.8, 6.0, 1.75, 1.7, "3  Patch", "git diff")
arrow((3.9, 6.85), (4.6, 6.85))
arrow((8.1, 6.85), (8.8, 6.85))
arrow((10.55, 6.85), (11.2, 6.85))

# gates column
GX, GW = 11.2, 4.4
ax.add_patch(FancyBboxPatch((GX, 1.1), GW, 6.6, boxstyle="round,pad=0,rounding_size=0.12",
                            fc=CARD, ec=AMBER, lw=2))
ax.text(GX + 0.22, 7.5, "4  Gates, in this order", color=AMBER, fontsize=17, fontweight="bold", va="top")
GATES = [
    ("Allowed files only", "opcode and SIMD helper files"),
    ("Builds", ""),
    ("Same output", "bit-identical, or below -100 dBFS\nwhere tanh/tan may be approximated;\nfixed + hidden random inputs"),
    ("Unit tests", ""),
    ("No allocation in the audio path", ""),
    ("DSP experiments", "per-opcode Python tests"),
    ("Faster", "at least 5% outside the noise band,\nnothing slower; full benchmark"),
]
y = 6.85
for i, (name, note) in enumerate(GATES):
    h = 0.46 + 0.25 * (note.count("\n") + 1 if note else 0)
    ax.add_patch(FancyBboxPatch((GX + 0.2, y - h), GW - 0.4, h, boxstyle="round,pad=0,rounding_size=0.07",
                                fc=BG, ec=LINE, lw=1.2))
    ax.text(GX + 0.38, y - 0.11, name, color=FG, fontsize=13, fontweight="bold", va="top")
    if note:
        ax.text(GX + 0.38, y - 0.43, note, color=MUTED, fontsize=10.5, va="top", linespacing=1.35)
    y -= h + 0.09

# verdicts
box(4.6, 2.95, 4.6, 1.55, "Rejected", "worktree reset\nthe reason goes into the next prompt",
    edge=RED, title_color=RED)
box(4.6, 1.1, 4.6, 1.5, "Accepted", "committed on the run's branch\nbecomes the baseline to beat",
    edge=AMBER)
arrow((GX, 3.75), (9.2, 3.75), RED, "any gate fails", ly=0.12)
arrow((GX, 1.85), (9.2, 1.85), AMBER, "all pass", ly=0.12)

# back to the prompt
ax.plot([4.6, 2.15], [3.75, 3.75], color=FG, lw=2.2)
ax.plot([4.6, 2.15], [1.85, 1.85], color=FG, lw=2.2)
ax.plot([2.15, 2.15], [1.85, 3.75], color=FG, lw=2.2)
arrow((2.15, 3.75), (2.15, 5.35), FG)
ax.text(2.4, 4.6, "next iteration", color=FG, fontsize=12, fontweight="bold", va="center")

# sources (docs/research/llm-autoresearch-loops.md, adopted items P1-P8, P10)
ax.text(0.4, 0.76, "Ideas from", color=AMBER, fontsize=11.5, fontweight="bold", va="center")
for k, line in enumerate([
    "Karpathy's autoresearch (the loop, equal fixed budget)  ·  AutoKernel (name the bottleneck before editing, "
    "change course on a plateau)",
    "OpenEvolve / AlphaEvolve (attempt history and accepted code in the prompt)  ·  ShinkaEvolve (skip "
    "near-duplicate patches)",
    "METR, CUDA-L1, arXiv 2607.18064 (hidden inputs against gaming the metric)  ·  arXiv 2604.13927 "
    "(compiler vectoriser remarks as feedback)",
]):
    ax.text(1.75, 0.76 - 0.27 * k, line, color=MUTED, fontsize=10.5, va="center")

fig.savefig("flow.png", facecolor=BG)
