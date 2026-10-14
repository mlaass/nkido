"""The flow diagram (flow.py / flow.png) as native Google Slides shapes.

usage: python3 flow_slide.py REVISION INDEX > flow_slide.json   (batchUpdate body)
Same layout and wording as flow.py: 16x9 diagram units mapped onto the 10 x 5.625 in slide.
"""
import json, sys

EMU = 914400
S = 0.625  # inches per diagram unit
HEX = {"bg": "#212121", "card": "#2E2E2E", "fg": "#D9D9D9", "muted": "#9E9E9E", "line": "#555555",
       "amber": "#F1C232", "red": "#E06C5A"}


def rgb(name):
    h = HEX[name]
    return {"red": round(int(h[1:3], 16) / 255, 3), "green": round(int(h[3:5], 16) / 255, 3),
            "blue": round(int(h[5:7], 16) / 255, 3)}


def props(x, y, w, h):  # diagram units, y = bottom edge (matplotlib convention)
    return {"pageObjectId": "flow_s",
            "size": {"width": {"magnitude": max(round(w * S * EMU), 1), "unit": "EMU"},
                     "height": {"magnitude": max(round(h * S * EMU), 1), "unit": "EMU"}},
            "transform": {"scaleX": 1, "scaleY": 1, "translateX": round(x * S * EMU),
                          "translateY": round((9 - y - h) * S * EMU), "unit": "EMU"}}


reqs, n = [], 0


def shape(x, y, w, h, paras, fill="card", edge="line", weight=1.5, kind="ROUND_RECTANGLE", align="TOP"):
    """paras: [(text, size_pt, colour, bold)]"""
    global n
    n += 1
    oid = f"flow_{n}"
    reqs.append({"createShape": {"objectId": oid, "shapeType": kind, "elementProperties": props(x, y, w, h)}})
    sp = {"contentAlignment": align}
    fields = "contentAlignment,outline"
    if fill:
        sp["shapeBackgroundFill"] = {"solidFill": {"color": {"rgbColor": rgb(fill)}}}
        fields += ",shapeBackgroundFill.solidFill.color"
    if edge:
        sp["outline"] = {"outlineFill": {"solidFill": {"color": {"rgbColor": rgb(edge)}}},
                         "weight": {"magnitude": weight, "unit": "PT"}}
    else:
        sp["outline"] = {"propertyState": "NOT_RENDERED"}
    reqs.append({"updateShapeProperties": {"objectId": oid, "shapeProperties": sp, "fields": fields}})
    reqs.append({"insertText": {"objectId": oid, "text": "\n".join(p[0] for p in paras)}})
    # body style on everything, then the first paragraph's style if it differs (fewer requests)
    def style(rng, size, colour, bold):
        reqs.append({"updateTextStyle": {"objectId": oid, "textRange": rng,
                     "style": {"fontSize": {"magnitude": size, "unit": "PT"}, "bold": bold,
                               "foregroundColor": {"opaqueColor": {"rgbColor": rgb(colour)}}},
                     "fields": "fontSize,bold,foregroundColor"}})
    style({"type": "ALL"}, *paras[-1][1:])
    if len(paras) > 1:
        style({"type": "FIXED_RANGE", "startIndex": 0, "endIndex": len(paras[0][0])}, *paras[0][1:])
    return oid


def label(x, y, w, h, text, size, colour, bold=False):
    return shape(x, y, w, h, [(text, size, colour, bold)], fill=None, edge=None, kind="TEXT_BOX", align="MIDDLE")


def line(x0, y0, x1, y1, colour="fg", head=None):
    """Horizontal or vertical. head: 'start' | 'end' | None, relative to (x0,y0)->(x1,y1)."""
    global n
    n += 1
    oid = f"flow_{n}"
    lx, hx, ly, hy = min(x0, x1), max(x0, x1), min(y0, y1), max(y0, y1)
    reqs.append({"createLine": {"objectId": oid, "lineCategory": "STRAIGHT",
                                "elementProperties": props(lx, ly, hx - lx, hy - ly)}})
    lp = {"lineFill": {"solidFill": {"color": {"rgbColor": rgb(colour)}}}, "weight": {"magnitude": 1.5, "unit": "PT"}}
    fields = "lineFill.solidFill.color,weight"
    if head:
        # the API line runs left->right / top->bottom; pick the matching end
        forward = (x1 > x0) or (y1 < y0)
        lp["endArrow" if forward else "startArrow"] = "FILL_ARROW"
        fields += ",endArrow" if forward else ",startArrow"
    reqs.append({"updateLineProperties": {"objectId": oid, "lineProperties": lp, "fields": fields}})


T, B = 10.5, 8  # title / body pt
# sweep strip (kept clear of the logo)
shape(0.4, 8.15, 13.9, 0.6, [("Sweep    one run = one model on one opcode family  ·  5 families  ·  8 iterations per run"
                              "  ·  cost cap per run  ·  one benchmark at a time", 7.5, "muted", False)],
      fill="bg", weight=1, align="MIDDLE")
reqs.append({"updateTextStyle": {"objectId": f"flow_{n}", "textRange": {"type": "FIXED_RANGE", "startIndex": 0,
             "endIndex": 5}, "style": {"bold": True, "foregroundColor": {"opaqueColor": {"rgbColor": rgb("amber")}}},
             "fields": "bold,foregroundColor"}})
# loop
shape(0.4, 5.35, 3.5, 2.35, [("1  Prompt", T, "amber", True), ("source of the opcode", B, "fg", False),
      ("current timings", B, "fg", False), ("diff accepted so far", B, "fg", False),
      ("earlier attempts + why rejected", B, "fg", False)])
shape(4.6, 5.35, 3.5, 2.35, [("2  Agent", T, "amber", True), ("edits the C++ in its own git worktree", B, "fg", False),
      ("30 min, 10 quick checks", B, "fg", False), ("(build, equality, timing)", B, "fg", False)])
shape(8.8, 6.0, 1.75, 1.7, [("3  Patch", T, "amber", True), ("git diff", B, "fg", False)])
line(3.9, 6.85, 4.6, 6.85, head="end")
line(8.1, 6.85, 8.8, 6.85, head="end")
line(10.55, 6.85, 11.2, 6.85, head="end")
# gates
GX, GW = 11.2, 4.4
shape(GX, 1.1, GW, 6.6, [("4  Gates, in this order", T, "amber", True)], edge="amber")
GATES = [("Allowed files only", "opcode and SIMD helper files"), ("Builds", ""),
         ("Same output", "bit-identical, or below -100 dBFS where tanh/tan may be approximated; fixed + hidden random inputs"),
         ("Unit tests", ""), ("No allocation in the audio path", ""),
         ("DSP experiments", "per-opcode Python tests"),
         ("Faster", "at least 5% outside the noise band, nothing slower; full benchmark")]
LINES = [1, 0, 3, 0, 0, 1, 2]
y = 6.85
for (name, note), k in zip(GATES, LINES):
    h = 0.46 + 0.25 * k
    shape(GX + 0.2, y - h, GW - 0.4, h, [(name, 8, "fg", True)] + ([(note, 6.5, "muted", False)] if note else []),
          fill="bg", weight=0.75, kind="RECTANGLE", align="MIDDLE")
    y -= h + 0.09
# verdicts
shape(4.6, 2.95, 4.6, 1.55, [("Rejected", T, "red", True), ("worktree reset", B, "fg", False),
      ("the reason goes into the next prompt", B, "fg", False)], edge="red")
shape(4.6, 1.1, 4.6, 1.5, [("Accepted", T, "amber", True), ("committed on the run's branch", B, "fg", False),
      ("becomes the baseline to beat", B, "fg", False)], edge="amber")
line(GX, 3.75, 9.2, 3.75, "red", head="end")
line(GX, 1.85, 9.2, 1.85, "amber", head="end")
label(9.2, 3.8, 2.0, 0.5, "any gate fails", 7.5, "red", True)
label(9.2, 1.9, 2.0, 0.5, "all pass", 7.5, "amber", True)
for oid in (f"flow_{n - 1}", f"flow_{n}"):
    reqs.append({"updateParagraphStyle": {"objectId": oid, "textRange": {"type": "ALL"},
                 "style": {"alignment": "CENTER"}, "fields": "alignment"}})
# back to the prompt
line(4.6, 3.75, 2.15, 3.75)
line(4.6, 1.85, 2.15, 1.85)
line(2.15, 1.85, 2.15, 3.75)
line(2.15, 3.75, 2.15, 5.35, head="end")
label(2.25, 4.3, 2.4, 0.5, "next iteration", 7.5, "fg", True)
# sources
label(0.25, 0.1, 15.5, 0.95,
      "Ideas from  Karpathy's autoresearch (the loop, equal fixed budget)  ·  AutoKernel (name the bottleneck before "
      "editing, change course on a plateau)  ·  OpenEvolve / AlphaEvolve (attempt history and accepted code in the "
      "prompt)  ·  ShinkaEvolve (skip near-duplicate patches)  ·  METR, CUDA-L1, arXiv 2607.18064 (hidden inputs "
      "against gaming the metric)  ·  arXiv 2604.13927 (compiler vectoriser remarks as feedback)", 6.5, "muted")
reqs.append({"updateTextStyle": {"objectId": f"flow_{n}", "textRange": {"type": "FIXED_RANGE", "startIndex": 0,
             "endIndex": 10}, "style": {"bold": True, "foregroundColor": {"opaqueColor": {"rgbColor": rgb("amber")}}},
             "fields": "bold,foregroundColor"}})

head = [{"createSlide": {"objectId": "flow_s", "insertionIndex": int(sys.argv[2]),
                         "slideLayoutReference": {"layoutId": "g3a8b88a45b2_0_42"},
                         "placeholderIdMappings": [{"layoutPlaceholder": {"type": "TITLE", "index": 0},
                                                    "objectId": "flow_t"}]}},
        {"deleteObject": {"objectId": "flow_t"}}]
print(json.dumps({"requests": head + reqs, "writeControl": {"requiredRevisionId": sys.argv[1]}},
                 ensure_ascii=False, separators=(",", ":")))
