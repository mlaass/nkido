# PRD: nkido.cc Project Website

> **Status: SHIPPED (external)** — Implemented in sibling repository `~/workspace/nkido.cc`. This PRD is retained here as the original design doc; all further work and documentation lives in that repo. The live-coding IDE in `web/` moves to `live.nkido.cc`.

---

## 1. Executive Summary

This PRD specifies the **nkido.cc** project website — a marketing / documentation / sub-project hub for the nkido ecosystem. Today, `nkido.cc` does not serve a distinct project site; the current web IDE at `web/` is the only public-facing artifact. That IDE is moving to **`live.nkido.cc`** to free the root domain for a project site that can introduce nkido to first-time visitors, host concept docs, and showcase the Godot addon and ESP32 port.

Key decisions made during planning:

- **New separate repo** named `nkido.cc` (matches the domain).
- **SvelteKit** + `adapter-static`, shared color/type tokens with the live app but a lighter, more spacious marketing layout ("sibling look").
- **Netlify**, separate site from `live.nkido.cc`.
- **Docs strategy for v1**: website hosts fresh concept + getting-started docs; full builtin/opcode reference **deep-links into the live app's docs panel**. No full docs mirror in v1.
- **Landing-page demo**: embedded `<iframe>` of `live.nkido.cc` with a preloaded patch.
- **Sub-project pages** (Godot addon, ESP32 port) ship as full install/usage pages — blocking launch on those MVPs being usable, per the user ("basically working already").
- **Analytics**: GoatCounter (privacy-friendly, no cookie banner).
- **Sponsors**: GitHub Sponsors + Ko-fi (mlaass).
- **License** (featured on landing): MIT.
- Scope includes the **required live-app changes** (iframe-embeddable hosting config, URL-based deep-links into the docs panel) so the project site's demo and docs-linking features actually work.

---

## 2. Current State

| Surface | Today | After this PRD |
|---|---|---|
| `nkido.cc` | Points at current web app (or unconfigured) | **New project site** (this PRD) |
| `live.nkido.cc` | Does not exist | The current `web/` SvelteKit IDE, moved here |
| `docs.nkido.cc` | Does not exist | Still does not exist in v1 (future option) |
| Docs content | Lives in `web/static/docs/`, consumed by IDE only | Same, plus concept docs written fresh in the website repo. Reference docs reached via deep-link into live IDE |
| Godot addon | `git@github.com:mlaass/godot-nkido-addon.git` — MVP nearly complete | Full install/usage page on `nkido.cc/godot` |
| ESP32 port | `https://github.com/mlaass/cedar-esp32` — MVP nearly complete | Full install/usage page on `nkido.cc/esp32` |
| Website repo | Does not exist | **New repo `nkido.cc`** (GitHub) |
| Deployment | Single Netlify site for the web app | Two Netlify sites: `live.nkido.cc` (existing, renamed) and `nkido.cc` (new) |

---

## 3. Goals and Non-Goals

### 3.1 Goals

1. **Introduce nkido** in under 10 seconds of reading to a first-time visitor (hero + feature grid).
2. **Show, don't tell** — embed a live playable IDE on the landing page.
3. **Route people** to the right next step: GitHub, live IDE, Godot addon, ESP32 port, Getting Started.
4. **Host concept + getting-started docs** written for readers who haven't opened the IDE yet.
5. **Deep-link into the live IDE's docs panel** so the full reference has one source of truth (in `web/static/docs/`).
6. **Provide full install/usage pages** for the Godot addon and ESP32 port, each blocking on their respective MVPs being complete.
7. **Blog / news** for launch announcement + 2–4 historical/dev-journal posts + auto-rendered GitHub Releases.
8. **Press kit page** with logos, screenshots, and short project descriptions for external writers.
9. **Sponsor surface** — GitHub Sponsors + Ko-fi.
10. **OSS-appropriate tone** throughout: no "sign up for our newsletter", no aggressive CTAs, no lock-in signals.

### 3.2 Non-Goals (hard cuts — not "later")

These are explicit scope exclusions. Things that are simply *not in v1 but could ship later* are consolidated in §17 Future Work.

1. **Newsletter / email capture** — incompatible with the OSS positioning. Not "later"; just no.
2. **User accounts / login / per-user saved patches** — a different product.
3. **In-browser compiler independent of `live.nkido.cc`** — the project site is marketing + docs, not a second IDE.

### 3.3 Target Experience

Three primary visitor journeys the site is optimized for. Each is a concrete acceptance target for Goal 1 ("under 10 seconds").

**Journey A — First-time visitor from Hacker News / a tweet:**

1. Lands on `nkido.cc/`. Hero headline + one-sentence tagline + two CTAs visible above the fold.
2. Reads the tagline, scrolls past the click-to-activate iframe poster, scans the 6-item feature grid.
3. **At ~8–10s of reading**, can state what nkido is ("a live-coded DSP engine that runs in the browser, on native, in Godot, and on ESP32").
4. Clicks the iframe poster → WASM loads in ≤4s → audio plays on the default patch.
5. Either stays and edits the patch in place, or clicks "Try it in your browser →" for the full IDE.

**Journey B — Godot game developer evaluating the addon:**

1. Lands on `nkido.cc/godot` (likely via a Godot Asset Library link or README).
2. Immediately sees "What it is" + an install command + a 10-line GDScript quickstart.
3. **Within 30s**, has enough to decide whether to `git clone` the addon.
4. Scrolls for limits/status + a link to the addon repo.
5. Outcome: leaves with an install decision, not a "what even is this" question.

**Journey C — Returning user jumping to the live IDE:**

1. Lands on `nkido.cc/` (bookmark or muscle memory from old URL).
2. Clicks `Live` in the top-nav → ends up on `live.nkido.cc` in ≤1 click.
3. Outcome: the project-site never gets in the way of someone who just wants to code.

Verification of these journeys is specified in §14.2.

---

## 4. Target Structure / Information Architecture

### 4.1 Sitemap

```
nkido.cc/
├── /                         Home (hero, features, live iframe, CTAs)
├── /getting-started          Install / first patch / next steps
├── /features                 Deeper feature tour
├── /docs                     Concept & tutorial index (fresh content)
│   ├── /docs/concepts/*      e.g. signals, dags, hot-swap, patterns
│   └── /docs/tutorials/*     0-install → 1-hello-sine → 2-filters → ...
├── /godot                    Godot addon: what/install/quickstart
├── /esp32                    ESP32 port: what/install/quickstart
├── /blog                     Index of posts
│   ├── /blog/<slug>          Hand-written posts
│   └── /blog/releases/<tag>  Auto-generated release-note posts
├── /community                CONTRIBUTING rendered + GitHub links + sponsor
├── /press                    Press kit: logos, screenshots, boilerplate
└── /404
```

Top-nav (6 slots max): **Live** (external, `live.nkido.cc`) · **Docs** · **Godot** · **ESP32** · **Blog** · **GitHub** (external)
Footer: Community · Press · Sponsor · License (MIT) · © notice.

### 4.2 Landing page skeleton

```
┌──────────────────────────────────────────────────────────┐
│  [logo] nkido                  Live  Docs  Godot  ESP32  │
│                                Blog  GitHub             │
├──────────────────────────────────────────────────────────┤
│                                                          │
│        High-performance live-coded audio synthesis.      │
│        A DAG-based DSP engine + pattern DSL.             │
│                                                          │
│        [ Try it in your browser → ]   [ GitHub ]         │
│                                                          │
├──────────────────────────────────────────────────────────┤
│                                                          │
│   ┌────────────────── live iframe ──────────────────┐    │
│   │  live.nkido.cc?patch=hello-sine                 │    │
│   │  (click-to-activate, lazy-loaded)               │    │
│   └─────────────────────────────────────────────────┘    │
│                                                          │
├──────────────────────────────────────────────────────────┤
│   ▲ Live-coding patterns    ▲ 95+ DSP opcodes            │
│   ▲ Runs everywhere         ▲ Lightweight embeddable     │
│   ▲ Hot-swap state preserved  runtime                    │
│   ▲ MIT licensed                                         │
├──────────────────────────────────────────────────────────┤
│  Where it runs:                                          │
│  [Web IDE]  [Native CLI]  [Godot addon]  [ESP32]         │
├──────────────────────────────────────────────────────────┤
│  Getting started / Blog / Community / Sponsor / Footer   │
└──────────────────────────────────────────────────────────┘
```

### 4.3 Headline features (landing feature grid)

1. **Live-coding patterns** — Akkado + Strudel-style mini-notation.
2. **95+ DSP opcodes** — oscillators, filters, reverbs, granular, vocoder, Karplus-Strong, and more.
3. **Runs everywhere** — native, web, Godot, ESP32.
4. **Lightweight embeddable runtime** — Cedar is a standalone C++ library.
5. **Hot-swap with preserved state** — change code while it plays, no glitches.
6. **MIT licensed** — permissive, embeddable in closed-source projects.

---

## 5. Design & Branding

- **Direction**: "Sibling look" — same color tokens + typography as the IDE; lighter, more spacious marketing layout.
- **Theme tokens**: import the same CSS custom-property set used by `web/src/app.css` so a theme tweak in the IDE propagates to `nkido.cc` (possibly via a tiny shared `@nkido/theme-tokens` package or a copy-on-build step — see §9.4).
- **Dark-first** with a light-mode toggle (same pattern as the IDE).
- **Typography**: match the IDE's font stack. Monospace reserved for code samples and signatures.
- **Logo**: extract the SVG paths from `web/src/lib/components/Logo/Logo.svelte` and vendor as a plain `static/logo.svg` in the website repo (referenced via `<img src="/logo.svg">`). Simpler than importing a Svelte component across repos; same "copy + sync manually on change" approach as the design tokens in §9.4.
- **Illustrations**: simple ASCII-diagram style (matching the project's own PRDs) over heavy 3D / stock imagery.
- **No email capture, no popups, no tracking banners.** The only call-outs are "Try it now", "See on GitHub", "Sponsor".

---

## 6. Docs Strategy (v1)

Two-layer approach:

1. **Website-hosted concept + getting-started docs** (`nkido.cc/docs/...`)
   - Written fresh in markdown in the website repo (not synced from `nkido`).
   - Content: "What is nkido?", "Signals & DAGs", "Patterns & mini-notation", "Hot-swap explained", plus a tutorial track mirroring the existing `01-hello-sine` → `05-testing-progression` progression.
   - These pages are SEO-indexable (static HTML) and render without the WASM IDE, so they're accessible from any browser / crawler.

2. **Reference docs via deep-links into the live IDE**
   - The opcode/builtin reference stays in `web/static/docs/reference/` and is served by `live.nkido.cc`.
   - On `nkido.cc`, every "Reference: `osc`" style link routes to `https://live.nkido.cc/?docs=<keyword>` (URL-param driven), which auto-opens the IDE's docs panel at that entry.
   - This requires a small addition to the live app — see §8.

Rationale: no duplication of reference content, single source of truth for the reference. The only build-time fetches the website does are `CONTRIBUTING.md` from nkido master (§13 Phase 2) and the GitHub Releases API for release-note blog posts (§9.2) — no reference-docs sync pipeline. The cost is that reference docs require the WASM IDE to load; that's acceptable for v1 because someone reading reference-level docs is, by definition, already trying to write code.

*Future docs-layer ideas (mirror, sync pipeline, cross-content search) are consolidated in §17.*

---

## 7. Sub-Project Pages

Each sub-project page follows the same structure. Both MVPs are expected to be usable at website launch.

### 7.1 `/godot` — Godot addon

Content sections:

1. **What it is** — embed nkido into Godot 4.6 games for dynamic music + sound design.
2. **Install** — `addons/` directory or Asset Library instructions (per `prd-godot-extension.md`).
3. **Quickstart** — GDScript example showing `NkidoEngine` singleton + `NkidoPlayer` node.
4. **Screenshots** — Godot inspector UI, param binding demo.
5. **Limits / status** — "v0.1, covers MVP feature set from PRD". Link to `prd-godot-extension.md`.
6. **Repo link** — `github.com/mlaass/godot-nkido-addon`.

### 7.2 `/esp32` — ESP32 port

Content sections:

1. **What it is** — Cedar running on ESP32-A1S Audio Kit 2.2 via ESP-IDF + ESP-ADF.
2. **Hardware** — required board + caveats (~146 KB stripped archive, reduced memory limits).
3. **Build & flash** — ESP-IDF commands, UART bytecode loader usage.
4. **Pin / control map** — KEY1–KEY6 hardware controls, ES8388 codec info.
5. **Example patches** — 2–3 small patches that fit the constraints.
6. **Repo link** — `github.com/mlaass/cedar-esp32`.

### 7.3 Content source

Both pages draw from the existing PRDs (`prd-godot-extension.md`, `prd-cedar-esp32.md`) and from the READMEs of those repos. Content is copied into the website repo as markdown and maintained there — no submodule pulls and no automated sync from the sub-project repos. (Unrelated build-time fetches for `CONTRIBUTING.md` and GitHub Releases are documented in §6 and §9.2.)

---

## 8. Live App Changes (scoped into this PRD)

Two features on `live.nkido.cc` are preconditions for the project site. Both are small additions, but they're in scope here so they don't fall through the cracks.

### 8.1 Cross-origin iframe embedding

**Problem**: the live app requires `Cross-Origin-Opener-Policy: same-origin` + `Cross-Origin-Embedder-Policy: require-corp` (for SharedArrayBuffer in AudioWorklet, per `web/netlify.toml`). These headers make the page **un-embeddable** in an iframe from `nkido.cc` unless both sides negotiate correctly.

**Approach**:

- Serve a **lean embed-mode** of the IDE at `live.nkido.cc/embed` — same app, hides chrome (top bar, side panels) and auto-loads a patch from a `?patch=<name>` URL param.
- For the iframe on `nkido.cc`, use **click-to-activate** (poster → real iframe on user click). This avoids loading WASM on every landing-page hit and sidesteps auto-play policy.
- Adjust `netlify.toml` on the live site:
  - Ensure the COEP/COOP headers remain for normal routes (needed for AudioWorklet).
  - Add `Cross-Origin-Resource-Policy: cross-origin` on the `/embed` route's assets so they can be loaded by `nkido.cc`.
  - Add CSP `frame-ancestors` allowing the project-site origins:
    ```
    frame-ancestors 'self' https://nkido.cc https://www.nkido.cc https://*.netlify.app http://localhost:* http://127.0.0.1:*
    ```
    The `*.netlify.app` entry covers Netlify branch previews on the website repo; `localhost:*` / `127.0.0.1:*` cover local dev against the prod live-app. If the `*.netlify.app` wildcard feels too broad, replace with the specific Netlify project slug (e.g. `https://*--nkido-cc.netlify.app`).

Edge cases: Safari has historically been strict about COOP/COEP in iframes. Document fallback: if the iframe fails to boot, show the static poster with a "Open in a new tab →" link.

#### 8.1.1 Named-patch manifest

`?patch=<name>` resolves against a manifest shipped with the live app:

- Location: `web/static/embed-patches/<name>.akk` — the Akkado source for each named demo patch.
- Index: `web/static/embed-patches/index.json` — `{"hello-sine": {"title": "Hello Sine", "description": "..."}}`; the embed boot code uses this to validate names and populate the embed UI's title.
- Initial set shipped with v1: `hello-sine`, `filter-sweep`, `pattern-basics`, `hot-swap-demo`. These double as the "Try it in your browser" targets the project-site landing page and tutorials link to.
- Unknown `<name>`: embed boots with the `hello-sine` default and logs a warning (don't show an error page — the iframe should always produce sound on click).
- Authoring: patches are hand-written in the live-app repo; the project-site repo only references them by name.

### 8.2 Docs deep-link via URL

**Problem**: the live app's docs panel is navigated in-app (no URL-level addressing).

**Approach**:

- Add a URL-param handler in the live app: `live.nkido.cc/?docs=<keyword>` opens the docs panel at the matching lookup entry (reusing the existing F1-help lookup at `web/src/lib/docs/manifest.ts`, which is generated by `scripts/build-docs-index.ts` and exports a `lookup` record).
- Unknown keywords open the docs panel at its index view.
- Also accept `/?doc=<path>` for explicit file paths like `reference/builtins/osc.md`.

Edge cases: keywords with special characters must be URL-encoded; the handler must be race-safe with the initial app boot (don't try to open the panel before the docs module has loaded).

### 8.3 Deliverables on the live-app side

| File | Change |
|---|---|
| `web/netlify.toml` | Add `/embed/*` header rules (CORP, CSP `frame-ancestors`) |
| `web/src/routes/embed/+page.svelte` | New — lean embed-mode route |
| `web/src/lib/components/EmbedChrome.svelte` | New — minimal chrome for embed mode |
| `web/src/lib/stores/audio.svelte.ts` | Minor — accept `?patch=<name>` via query param on boot |
| `web/src/lib/docs/deep-link.ts` | New — URL-param handler for `?docs=<keyword>` / `?doc=<path>` |
| `web/src/routes/+layout.svelte` | Call deep-link handler after docs module ready |

These changes are additive; no existing behavior changes.

---

## 9. Tech Stack & Infrastructure (website repo)

### 9.1 Framework & build

- **SvelteKit** with `@sveltejs/adapter-static` (same stack as `web/`).
- **Svelte 5 runes** for any interactive components (match live app conventions per `CLAUDE.md`).
- **Vite** for dev server; **Bun** as the package manager (consistent with live app).
- **Markdown routing**: **mdsvex** preprocessor — enables the `+page.md` convention used in §11.1's route tree so concept/tutorial/blog content is authored as plain markdown with frontmatter and rendered through SvelteKit's standard routing.
- **Markdown rendering (non-route content)**: `marked` + `gray-matter` for any content loaded outside of mdsvex (e.g. the CONTRIBUTING.md fetch on `/community`). Same libs as live app's docs system.
- **Syntax highlighting**: `shiki` (build-time, zero runtime cost; configured as the mdsvex highlighter).
- **Icons**: `lucide-svelte` (same as live app).

### 9.2 Content model

- **Concept/tutorial docs**: markdown in `src/routes/docs/**/*.md` with frontmatter compatible with the live-app docs convention (`title`, `category`, `keywords`, `order`) so content can later be mirrored if we reverse course on the reference-mirror decision (see §17 item 1).
- **Blog posts**: markdown in `src/routes/blog/*.md` with frontmatter (`title`, `date`, `author`, `slug`, `excerpt`).
- **Release posts**: generated at build time from the GitHub Releases API for the `mlaass/nkido` repo, cached to `src/lib/data/releases.json` during CI.

### 9.3 Deployment

- **Netlify**, new site (separate from `live.nkido.cc`).
- Custom domain: `nkido.cc` apex + `www.nkido.cc` redirect → apex.
- Branch `master` → production; branch previews for PRs.
- No COEP/COOP requirements (site is pure static content); only the embedded iframe loads the live app, which handles its own isolation.

### 9.4 Shared design tokens

Options (implementation detail, decide during build):

- Copy `web/src/app.css`'s token block into the website repo and keep in sync manually.
- Publish a tiny `nkido-theme-tokens` package (npm) from the nkido repo, consumed by both.
- Git submodule of a shared tokens file.

**v1 recommendation**: copy + sync manually. Tokens change rarely; the cost of a build-pipeline is higher than the cost of a periodic copy.

### 9.5 Analytics

- **GoatCounter** (hosted or self-hosted) — cookieless, no banner required, OSS-friendly.
- Add a tiny script in `src/app.html`.

---

## 10. Impact Assessment

| Component | Status | Notes |
|---|---|---|
| `nkido/web/*` (live app) | **Modified** | Adds `/embed` route, docs URL-param handler, netlify header rules (§8) |
| `nkido/web/static/docs/` | **Stays** | Reference docs remain the single source of truth; deep-linked from `nkido.cc` |
| `nkido/docs/*` | **Stays** | Internal PRDs + design docs stay in the nkido repo |
| `nkido/CONTRIBUTING.md` | **Stays** | Rendered on `nkido.cc/community` by fetching raw markdown at build time |
| `nkido/README.md` | **Modified** | Add "Website: nkido.cc" link and "Try it live: live.nkido.cc" update |
| `nkido/netlify.toml` (of live app) | **Modified** | New `/embed/*` rules |
| Godot addon repo | **Stays (separate)** | Content for `/godot` page is written fresh in website repo, linking out |
| ESP32 repo | **Stays (separate)** | Content for `/esp32` page is written fresh in website repo, linking out |
| DNS for `nkido.cc` | **Modified** | Apex → new Netlify site; `live.nkido.cc` CNAME → existing Netlify site |
| `nkido.cc` repo | **New** | New GitHub repo, separate deploy pipeline |

---

## 11. File-Level Changes

### 11.1 New website repo (`nkido.cc`)

```
nkido.cc/
├── README.md
├── LICENSE                      (MIT, matches main project)
├── package.json                 (SvelteKit + bun)
├── bun.lockb
├── svelte.config.js             (adapter-static)
├── vite.config.ts
├── netlify.toml                 (redirects, www→apex, security headers)
├── .github/workflows/deploy.yml (build + push to Netlify on master, preview on PR)
├── static/
│   ├── favicon.svg
│   ├── logo.svg                 (vendored from web/src/lib/components/Logo/Logo.svelte)
│   ├── og-image.png
│   └── press/                   (logos, screenshots for press kit)
├── src/
│   ├── app.html                 (GoatCounter snippet, OG/meta tags)
│   ├── app.css                  (copied design tokens from nkido/web/)
│   ├── lib/
│   │   ├── components/
│   │   │   ├── Layout/Header.svelte
│   │   │   ├── Layout/Footer.svelte
│   │   │   ├── Home/Hero.svelte
│   │   │   ├── Home/LiveEmbed.svelte   (click-to-activate iframe)
│   │   │   ├── Home/FeatureGrid.svelte
│   │   │   ├── Home/RunsEverywhere.svelte
│   │   │   ├── Docs/TutorialCard.svelte
│   │   │   ├── SubProject/InstallBlock.svelte
│   │   │   └── Blog/PostCard.svelte
│   │   ├── data/
│   │   │   └── releases.json    (generated by prebuild script from GitHub API)
│   │   └── markdown/
│   │       └── render.ts        (marked + shiki wrapper)
│   ├── routes/
│   │   ├── +layout.svelte
│   │   ├── +page.svelte                 (/)
│   │   ├── getting-started/+page.md
│   │   ├── features/+page.md
│   │   ├── docs/+page.svelte            (docs index)
│   │   ├── docs/concepts/+page.md
│   │   ├── docs/concepts/[slug]/+page.md
│   │   ├── docs/tutorials/[slug]/+page.md
│   │   ├── godot/+page.md
│   │   ├── esp32/+page.md
│   │   ├── blog/+page.svelte            (index)
│   │   ├── blog/[slug]/+page.md
│   │   ├── blog/releases/[tag]/+page.ts (from releases.json)
│   │   ├── community/+page.svelte       (renders CONTRIBUTING.md)
│   │   ├── press/+page.md
│   │   └── +error.svelte                (404)
│   └── (markdown content lives directly under src/routes/**/ as +page.md,
│        processed by mdsvex — no separate src/content/ tree)
└── scripts/
    ├── fetch-releases.ts                 (GitHub API → releases.json)
    └── fetch-contributing.ts             (fetch CONTRIBUTING.md from nkido master)
```

### 11.2 Changes to the `nkido` repo

| File | Change |
|---|---|
| `web/src/routes/embed/+page.svelte` | **New** — lean embed-mode route |
| `web/src/lib/components/EmbedChrome.svelte` | **New** — minimal chrome for embed mode |
| `web/src/lib/docs/deep-link.ts` | **New** — URL-param handler |
| `web/src/lib/stores/audio.svelte.ts` | **Modified** — accept `?patch=<name>` on boot |
| `web/src/routes/+layout.svelte` | **Modified** — call deep-link handler after docs ready |
| `web/netlify.toml` | **Modified** — add `/embed/*` CORP + CSP `frame-ancestors` rules |
| `README.md` | **Modified** — add "Website: nkido.cc" and update "Try it live" link |

---

## 12. Edge Cases

1. **Iframe fails to load (Safari / COEP quirks)** — show the static poster with an "Open in a new tab →" link pointing to `live.nkido.cc/embed?patch=<name>`.
2. **JavaScript disabled** — landing page must render readable hero + feature list + CTAs without JS (SvelteKit SSR handles this; verify manually).
3. **Slow 3G / mobile** — the iframe is click-to-activate so no WASM on initial load; inline audio samples are lazy-loaded.
4. **GitHub Releases API rate limit in CI** — use a `GITHUB_TOKEN` (available by default in Actions); fall back to the last committed `releases.json` if the fetch fails.
5. **Pre-launch visit to `nkido.cc/godot` or `/esp32` before MVPs are complete** — block the launch until both MVPs are usable (per §3.1 goal 6). If a page absolutely must ship early, add a `Status: pre-alpha` banner at the top.
6. **Deep-link with unknown `?docs=<keyword>`** — open docs panel at index view, don't error.
7. **Concept docs on `nkido.cc` drift from reference on `live.nkido.cc`** — accepted risk for v1. Each concept page footer reads "For reference: open in IDE →" linking to the relevant keyword.
8. **CONTRIBUTING.md fetched from nkido master at build time** — if fetch fails in CI, fail the build loudly (don't silently publish stale content). Cache the last-known-good copy in the repo as a fallback when running `bun run dev` locally.
9. **Blog post slug collisions** between hand-written and release posts — release posts live under `/blog/releases/<tag>` specifically to avoid collisions.
10. **`live.nkido.cc` is down** — landing page's iframe fails gracefully (Edge Case 1); all other content on `nkido.cc` is fully static and unaffected.

---

## 13. Implementation Phases

### Phase 1 — Repo skeleton + landing (1–2 days)

**Goal**: `nkido.cc` serves a static landing page with hero, feature grid, CTAs, and footer.

- Create `nkido.cc` repo on GitHub.
- SvelteKit + adapter-static scaffold; copy design tokens from `nkido/web/`.
- Build header, footer, hero, feature grid, runs-everywhere strip.
- Netlify site + domain DNS (apex + www).
- No iframe yet — use a static poster with "Open live.nkido.cc" button.

**Verification**: `nkido.cc` loads; Lighthouse ≥ 95 in all categories; links resolve.

### Phase 2 — Docs + sub-project pages (2–3 days)

**Goal**: `/getting-started`, `/features`, `/docs`, `/godot`, `/esp32`, `/press`, `/community` all populated.

- Write concept + tutorial markdown (aim: 3 concepts + 5 tutorials at launch).
- Write Godot + ESP32 install/usage pages from the respective PRDs and READMEs.
- Community page fetches `CONTRIBUTING.md` from nkido master at build time.
- Press kit: logo SVGs, screenshots, short/long boilerplate.

**Verification**: all routes render; internal links resolve; external links open in new tabs.

### Phase 3 — Live app changes for embed + docs deep-link (2 days)

**Goal**: `live.nkido.cc/embed?patch=hello-sine` works; `live.nkido.cc/?docs=osc` opens the docs panel at `osc`.

- Add `/embed` route + lean chrome in the nkido repo.
- Add URL-param handler for `?docs=<keyword>` / `?doc=<path>`.
- Update `web/netlify.toml` with `/embed/*` CORP + CSP rules.
- Deploy to `live.nkido.cc` (staging first, then prod).

**Verification**: embed loads in a test iframe on a scratch page; deep-link opens the correct docs entry; normal app still works.

### Phase 4 — Integrate live embed + docs deep-linking on website (1 day)

**Goal**: landing page has click-to-activate live iframe; reference-doc links on `nkido.cc` resolve into `live.nkido.cc/?docs=<keyword>`.

- `Home/LiveEmbed.svelte` component with poster → real iframe on click.
- Concept-doc footers link to `live.nkido.cc/?docs=<keyword>`.
- Safari fallback: "Open in a new tab →" link if iframe fails to boot within N seconds.

**Verification**: iframe activates on click in Chrome/Firefox/Safari; audio plays; editing works inside the iframe.

### Phase 5 — Blog + release automation (1–2 days)

**Goal**: `/blog` has launch post + 2–4 historical/dev-journal posts + auto-rendered GitHub Releases.

- Write launch post: "Introducing nkido".
- Write 2–4 dev-journal posts (e.g. "How hot-swap works", "Designing stereo support", "Bringing Cedar to ESP32").
- `scripts/fetch-releases.ts` pulls GitHub Releases at build time into `src/lib/data/releases.json`.
- Blog index merges hand-written + release posts, sorted by date.

**Verification**: blog index shows all posts; release posts link back to the GitHub release page.

### Phase 6 — Analytics + launch polish (0.5 day)

**Goal**: Site is launch-ready.

- GoatCounter snippet in `src/app.html`.
- OG/meta tags (title, description, og:image per route).
- `robots.txt`, `sitemap.xml` (SvelteKit has helpers).
- Lighthouse pass, accessibility pass, broken-link check.

**Verification**: GoatCounter receives pageviews; OG previews render correctly in Slack/Twitter/Discord link unfurls.

*Post-v1 roadmap items are consolidated in §17 Future Work.*

---

## 14. Testing / Verification Strategy

### 14.1 Automated

- **Link checker** in CI (`lychee` or similar) — fails the build on broken internal links or broken external links to the nkido/godot/esp32 repos.
- **Build check** — `bun run build` must succeed, no type errors (`bun run check`).
- **Lighthouse CI** — Performance ≥ 95, Accessibility ≥ 95, SEO ≥ 95, Best Practices ≥ 95 on `/`, `/getting-started`, `/godot`, `/esp32`.

### 14.2 Manual

- **Iframe cross-browser matrix**: Chrome/Firefox/Safari/Edge — click-to-activate, audio plays, editing works.
- **No-JS pass**: disable JS in browser, verify landing + docs pages are still readable.
- **Mobile pass**: 375px-wide viewport — nav collapses to hamburger, hero readable, iframe gracefully degrades.
- **Deep-link pass**: visit `live.nkido.cc/?docs=osc` in a fresh tab — docs panel opens to `osc`.
- **Deep-link with invalid keyword**: `?docs=doesnotexist` → docs panel opens at index, no error.
- **Sub-project page smoke**: follow the install steps on `/godot` and `/esp32` end-to-end.

**Qualitative / messaging tests** (acceptance for Goal 1 and Goal 4 in §3.1; journeys in §3.3):

- **10-second pitch test**: show the landing page to 3 people who have never heard of nkido, with a 10-second cap on reading. Ask "what is nkido?" afterward. Pass if 2 of 3 can answer substantively (e.g. "a live-coding audio thing that runs in browsers and other places"). Iterate hero copy until pass.
- **Standalone concept-docs readability**: have someone work through the first tutorial on `nkido.cc/docs/tutorials/01-hello-sine` **without opening the live IDE**. Pass if they can describe what a signal / DAG / hot-swap means in their own words afterwards. Validates that concept docs stand on their own (Goal 4).
- **Godot-dev install-decision test**: show `/godot` to a Godot developer unfamiliar with nkido. Pass if they can state within 30 seconds whether they'd try the addon, and (if yes) what the first command to run is. Validates Journey B in §3.3.
- **Top-nav discoverability**: ask a returning user to "get to the live IDE from the home page." Pass if they click `Live` in the top-nav within 5 seconds. Validates Journey C.

### 14.3 Launch checklist

- [ ] `nkido.cc` apex + `www.nkido.cc` redirect resolve.
- [ ] `live.nkido.cc` serves current web app from its new Netlify site.
- [ ] Old URLs (if any) redirect to the right place.
- [ ] Godot addon MVP usable per its PRD's MVP definition.
- [ ] ESP32 MVP usable per its PRD's MVP definition.
- [ ] `README.md` on nkido main repo updated with new URLs.
- [ ] Launch blog post published.
- [ ] Sponsor links working (GitHub Sponsors, Ko-fi).
- [ ] GoatCounter tracking confirmed.
- [ ] All routes in §4.1 sitemap render without errors.

---

## 15. Open Questions

1. **`nkido` GitHub account vs `mlaass` account** — the ESP32 and Godot repos are under `mlaass`; is the main `nkido` repo also under `mlaass` or an `nkido` org? Affects sponsor links and "Edit on GitHub" button targets. *(Default: `mlaass` unless an org is created.)*
2. **Where is `nkido.cc` DNS currently hosted?** (Namecheap / Cloudflare / Netlify DNS?) Needed before Phase 1's Netlify setup.
3. **Is there a project logo SVG already?** Explore agent found a `Logo/` component in `web/src/lib/components/` — confirm it's the canonical logo we want on the marketing site.
4. **Ko-fi username confirmation** — the user wrote `mlaass`; double-check at https://ko-fi.com/mlaass before the sponsor section ships.
5. **`CONTRIBUTING.md` fetch strategy** — fetch at build time (fresh every deploy) or vendor a copy in-repo (simpler, stale risk)? Current recommendation: build-time fetch with in-repo fallback.
6. **Code of Conduct** — does the nkido repo have one? If not, add one before launch. The community page should link to it.

---

## 16. Related Work

- `docs/prd-godot-extension.md` — Godot addon PRD (content source for `/godot` page).
- `docs/prd-cedar-esp32.md` — ESP32 port PRD (content source for `/esp32` page).
- `docs/prd-nkido-web-ide.md` — live IDE PRD (context for `live.nkido.cc`).
- `docs/netlify_deployment.md` — current live app deployment, reference for new Netlify site setup.
- `CONTRIBUTING.md` (repo root) — rendered on `/community`.
- `web/static/docs/DOCUMENTATION_GUIDE.md` — frontmatter / category conventions; website's fresh content reuses them.

---

## 17. Future Work

Items considered and explicitly deferred past v1. Unlike §3.2 (hard cuts), these are *good ideas whose time hasn't come* and are candidates for a v2 PRD once v1 ships and we have usage data.

**Docs layer:**

1. **Full opcode/builtin reference mirror on `nkido.cc`** — currently deep-linked into the live IDE (§6). Mirror would make reference SEO-indexable and crawler-friendly. Depends on a sync pipeline from `web/static/docs/reference/` into the website repo.
2. **`docs.nkido.cc` as a separate site** — only worth the split if reference-docs traffic dwarfs the marketing site.
3. **Submodule / CI pipeline pulling `web/static/docs/`** — prerequisite for (1) and (2).
4. **Search across concept + reference docs on the website** — v1 relies on the live app's search for reference and has no search for concept docs. Acceptable because concept doc volume is small.
5. **Versioned docs** (older-version docs) — not needed until we ship a real release split (e.g. v1 / v2).

**Community & content:**

6. **Real-time chat link** (Discord / Matrix) — ship once a channel actually exists; don't create a dead channel just for the link.
7. **Public roadmap page** — interesting but maintenance-heavy; often drifts from reality. Revisit once there's a steady release cadence.
8. **Internationalization** — English only for v1. Revisit if translators volunteer.

**Tooling:**

9. **Published shared-brand package** (e.g. `@nkido/theme-tokens` and `@nkido/brand`) — currently tokens and logo are copy-on-change (§5, §9.4). Upgrade to a published package if drift becomes painful.
