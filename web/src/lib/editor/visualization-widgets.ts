/**
 * CodeMirror extension for block-level visualization widgets
 *
 * Creates block decorations (below source lines) for visualization declarations.
 * When multiple visualizations are on the same line, they are grouped into a single
 * container widget to prevent line number misalignment.
 */

import {
	EditorView,
	Decoration,
	WidgetType,
	ViewPlugin
} from '@codemirror/view';
import type { DecorationSet } from '@codemirror/view';
import { StateField, StateEffect, RangeSet } from '@codemirror/state';
import { audioEngine, type VizDecl } from '$lib/stores/audio.svelte';
import { getRenderer } from '$lib/visualizations/registry';

// Import visualization renderers to register them
import '$lib/visualizations';

/**
 * State effect to update visualization decorations
 */
export const setVizDecls = StateEffect.define<VizDecl[]>();

/**
 * Visualization container widget
 * Groups multiple visualizations at the same line position into a single block widget.
 * This prevents CodeMirror from allocating vertical space for each widget separately.
 */
class VisualizationContainerWidget extends WidgetType {
	private vizList: VizDecl[];
	private elements: HTMLElement[] = [];
	private rafId = 0;

	constructor(vizList: VizDecl[]) {
		super();
		this.vizList = vizList;
	}

	eq(other: VisualizationContainerWidget): boolean {
		if (this.vizList.length !== other.vizList.length) return false;
		return this.vizList.every((v, i) =>
			v.stateId === other.vizList[i].stateId &&
			v.type === other.vizList[i].type &&
			v.sourceOffset === other.vizList[i].sourceOffset
		);
	}

	toDOM(): HTMLElement {
		// Outer wrapper with display:block for proper CodeMirror measurement
		const wrapper = document.createElement('div');
		wrapper.className = 'viz-wrapper';
		wrapper.style.cssText = `
			display: block;
			padding: 8px 0;
			line-height: 0;
		`;

		// Inner flex container for horizontal layout
		const container = document.createElement('div');
		container.className = 'viz-container';
		container.style.cssText = `
			display: inline-flex;
			flex-wrap: wrap;
			gap: 8px;
			vertical-align: top;
		`;
		wrapper.appendChild(container);

		for (const viz of this.vizList) {
			const renderer = getRenderer(viz.type);
			if (renderer) {
				const el = renderer.create(viz);
				this.elements.push(el);
				container.appendChild(el);
			} else {
				// Fallback for unknown visualization types
				const fallback = document.createElement('div');
				fallback.className = 'viz-unknown';
				fallback.style.cssText = `
					display: inline-block;
					padding: 8px;
					background: var(--bg-secondary, #1a1a1a);
					border: 1px solid var(--border-primary, #333);
					border-radius: 4px;
					color: var(--text-tertiary, #666);
					font-size: 11px;
					vertical-align: top;
				`;
				fallback.textContent = `Unknown visualization type: ${viz.type}`;
				this.elements.push(fallback);
				container.appendChild(fallback);
			}
		}

		this.startRenderLoop();
		return wrapper;
	}

	destroy(): void {
		if (this.rafId) {
			cancelAnimationFrame(this.rafId);
			this.rafId = 0;
		}

		for (let i = 0; i < this.elements.length; i++) {
			const renderer = getRenderer(this.vizList[i].type);
			if (renderer) {
				renderer.destroy(this.elements[i]);
			}
		}
		this.elements = [];
	}

	private startRenderLoop(): void {
		const render = async () => {
			if (this.elements.length === 0) return;

			const isPlaying = audioEngine.isPlaying;
			const beatPos = isPlaying ? await audioEngine.getCurrentBeatPosition() : 0;

			for (let i = 0; i < this.elements.length; i++) {
				const renderer = getRenderer(this.vizList[i].type);
				if (renderer) {
					renderer.update(this.elements[i], this.vizList[i], beatPos, isPlaying);
				}
			}

			// Always continue animation loop (visualizations handle not-playing state)
			this.rafId = requestAnimationFrame(render);
		};

		render();
	}
}

/**
 * Find the line end position for a source offset
 */
function getLineEndForOffset(doc: { lineAt: (pos: number) => { to: number }; length: number }, offset: number): number {
	// Ensure offset is within document bounds
	const safeOffset = Math.max(0, Math.min(offset, doc.length));

	// Get the line containing this offset
	const line = doc.lineAt(safeOffset);

	// Return the end of the line
	return line.to;
}

/**
 * Build decorations for visualization widgets
 * Groups visualizations at the same line position into a single container widget
 */
function buildDecorations(vizDecls: VizDecl[], doc: { lineAt: (pos: number) => { to: number }; length: number }): DecorationSet {
	// Group widgets by their line end position
	const widgetsByLine = new Map<number, VizDecl[]>();

	for (const viz of vizDecls) {
		const lineEnd = getLineEndForOffset(doc, viz.sourceOffset);
		if (!widgetsByLine.has(lineEnd)) {
			widgetsByLine.set(lineEnd, []);
		}
		widgetsByLine.get(lineEnd)!.push(viz);
	}

	// Create ONE block decoration per line (containing all widgets for that line)
	const decorations: Array<{ pos: number; decoration: Decoration }> = [];

	for (const [pos, vizList] of widgetsByLine) {
		decorations.push({
			pos,
			decoration: Decoration.widget({
				widget: new VisualizationContainerWidget(vizList),
				block: true,  // Render as block below the line
				side: 1       // After the line
			})
		});
	}

	// Sort by position (required for RangeSet)
	decorations.sort((a, b) => a.pos - b.pos);

	return RangeSet.of(
		decorations.map((d) => d.decoration.range(d.pos))
	);
}

/**
 * State field that manages visualization decorations
 */
const visualizationField = StateField.define<DecorationSet>({
	create() {
		return Decoration.none;
	},

	update(decorations, tr) {
		// Check for vizDecls effect
		for (const effect of tr.effects) {
			if (effect.is(setVizDecls)) {
				return buildDecorations(effect.value, tr.state.doc);
			}
		}
		// Map decorations through document changes
		return decorations.map(tr.changes);
	},

	provide: (f) => EditorView.decorations.from(f)
});

/**
 * ViewPlugin that polls for vizDecls changes
 */
class VizWidgetsPlugin {
	private view: EditorView;
	private rafId = 0;
	private isDestroyed = false;
	private lastVizDeclsHash = '';

	constructor(view: EditorView) {
		this.view = view;
		// Start polling for vizDecls changes
		this.startPolling();
	}

	destroy() {
		this.isDestroyed = true;
		if (this.rafId) {
			cancelAnimationFrame(this.rafId);
			this.rafId = 0;
		}
	}

	private computeHash(vizDecls: VizDecl[]): string {
		return vizDecls.map(v => `${v.stateId}:${v.type}:${v.sourceOffset}`).join(',');
	}

	private startPolling(): void {
		const poll = () => {
			if (this.isDestroyed) return;

			const vizDecls = audioEngine.vizDecls;
			const currentHash = this.computeHash(vizDecls);

			if (currentHash !== this.lastVizDeclsHash) {
				this.lastVizDeclsHash = currentHash;
				// Dispatch effect to update decorations
				this.view.dispatch({
					effects: setVizDecls.of(vizDecls)
				});
				// Force CodeMirror to re-measure widget heights
				this.view.requestMeasure();
			}

			// Poll every 100ms
			this.rafId = window.setTimeout(() => {
				requestAnimationFrame(poll);
			}, 100) as unknown as number;
		};

		poll();
	}
}

const vizWidgetsPlugin = ViewPlugin.fromClass(VizWidgetsPlugin);

/**
 * Export the extension (StateField + ViewPlugin)
 */
export function visualizationWidgets() {
	return [visualizationField, vizWidgetsPlugin];
}
