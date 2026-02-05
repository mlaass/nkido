/**
 * Piano Roll Visualization Renderer
 *
 * Displays pattern events as a scrolling piano roll with playhead.
 * Refactored from the old pattern-preview.ts to use the new visualization system.
 */

import type { VisualizationRenderer } from './registry';
import { registerRenderer } from './registry';
import { VizType, type VizDecl, type PatternEvent } from '$lib/stores/audio.svelte';
import { patternHighlightStore } from '$lib/stores/pattern-highlight.svelte';

/**
 * Get pattern events for a piano roll visualization.
 * Uses stateId to find the pattern directly from the store.
 */
function getPatternEvents(viz: VizDecl): { events: PatternEvent[]; cycleLength: number } {
	// Primary: look up by stateId directly (now populated from C++ codegen)
	if (viz.stateId) {
		const pattern = patternHighlightStore.getPattern(viz.stateId);
		if (pattern) {
			return {
				events: pattern.events,
				cycleLength: pattern.info.cycleLength
			};
		}
	}

	// Fallback: find the nearest pattern by source location
	const patterns = patternHighlightStore.getAllPatterns();
	let closestPattern = null;
	let closestDistance = Infinity;

	for (const pattern of patterns) {
		const patternEnd = pattern.info.docOffset + pattern.info.docLength;
		// Pattern should be before or overlapping the pianoroll call
		if (patternEnd <= viz.sourceOffset + 100) {
			const distance = viz.sourceOffset - patternEnd;
			if (distance >= 0 && distance < closestDistance) {
				closestDistance = distance;
				closestPattern = pattern;
			}
		}
	}

	if (closestPattern) {
		return {
			events: closestPattern.events,
			cycleLength: closestPattern.info.cycleLength
		};
	}

	// Last fallback: use first pattern if any exist
	if (patterns.length > 0) {
		return {
			events: patterns[0].events,
			cycleLength: patterns[0].info.cycleLength
		};
	}

	return { events: [], cycleLength: 4 };
}

/**
 * Convert frequency to Y position using MIDI-based pitch mapping.
 * Maps MIDI range 36-84 (C2-C6) to canvas height.
 */
function freqToY(freq: number, height: number): number {
	const midi = 12 * Math.log2(freq / 440) + 69;
	const normalized = Math.max(0, Math.min(1, (midi - 36) / 48));
	return height - 4 - normalized * (height - 8);
}

/**
 * Render the piano roll on a canvas
 */
function render(
	canvas: HTMLCanvasElement,
	events: PatternEvent[],
	cycleLength: number,
	beatPos: number,
	isPlaying: boolean
): void {
	const ctx = canvas.getContext('2d');
	if (!ctx) return;

	const w = canvas.width;
	const h = canvas.height;

	// Calculate visible window (1+ cycle centered on playhead when playing)
	const windowBeats = Math.max(4, cycleLength);
	const windowStart = isPlaying ? Math.max(0, beatPos - windowBeats * 0.25) : 0;
	const windowEnd = windowStart + windowBeats;

	// Get computed styles
	const computedStyle = getComputedStyle(canvas);
	const bgColor = computedStyle.getPropertyValue('--bg-secondary').trim() || '#1a1a1a';
	const borderColor = computedStyle.getPropertyValue('--border-primary').trim() || '#333';
	const accentColor = computedStyle.getPropertyValue('--accent-primary').trim() || '#4ade80';
	const dimColor = computedStyle.getPropertyValue('--text-tertiary').trim() || '#666';
	const playheadColor = computedStyle.getPropertyValue('--accent-secondary').trim() || '#fb923c';

	// Clear with background
	ctx.fillStyle = bgColor;
	ctx.fillRect(0, 0, w, h);

	// Draw beat grid
	ctx.strokeStyle = borderColor;
	ctx.lineWidth = 1;
	for (let beat = Math.ceil(windowStart); beat < windowEnd; beat++) {
		const x = ((beat - windowStart) / (windowEnd - windowStart)) * w;
		ctx.beginPath();
		ctx.moveTo(x, 0);
		ctx.lineTo(x, h);
		ctx.stroke();
	}

	// Draw events
	for (const evt of events) {
		const cycleTime = evt.time % cycleLength;
		const maxCycle = Math.ceil(windowEnd / cycleLength) + 1;

		for (let cycle = -1; cycle <= maxCycle; cycle++) {
			const evtTime = cycleTime + cycle * cycleLength;
			if (evtTime >= windowStart && evtTime < windowEnd) {
				const x = ((evtTime - windowStart) / (windowEnd - windowStart)) * w;
				const evtW = Math.max((evt.duration / (windowEnd - windowStart)) * w, 3);
				const y = freqToY(evt.value, h);

				ctx.fillStyle = evtTime < beatPos ? dimColor : accentColor;
				ctx.fillRect(x, y, evtW, 4);
			}
		}
	}

	// Draw playhead when playing
	if (isPlaying) {
		const playheadBeat = beatPos % cycleLength + Math.floor(windowStart / cycleLength) * cycleLength;
		for (let offset = -cycleLength; offset <= cycleLength; offset += cycleLength) {
			const ph = playheadBeat + offset;
			if (ph >= windowStart && ph < windowEnd) {
				const x = ((ph - windowStart) / (windowEnd - windowStart)) * w;
				ctx.strokeStyle = playheadColor;
				ctx.lineWidth = 2;
				ctx.beginPath();
				ctx.moveTo(x, 0);
				ctx.lineTo(x, h);
				ctx.stroke();
				break;
			}
		}
	}
}

/**
 * Piano Roll Renderer
 */
const pianoRollRenderer: VisualizationRenderer = {
	create(viz: VizDecl): HTMLElement {
		const container = document.createElement('div');
		container.className = 'viz-pianoroll';
		container.style.cssText = `
			display: inline-block;
			border-radius: 4px;
			overflow: hidden;
			background: var(--bg-secondary, #1a1a1a);
			border: 1px solid var(--border-primary, #333);
			width: 200px;
			height: 50px;
			vertical-align: top;
		`;

		// Add label
		const label = document.createElement('div');
		label.className = 'viz-label';
		label.textContent = viz.name;
		label.style.cssText = `
			font-size: 10px;
			padding: 2px 6px;
			color: var(--text-secondary, #888);
			border-bottom: 1px solid var(--border-primary, #333);
		`;
		container.appendChild(label);

		// Add canvas
		const canvas = document.createElement('canvas');
		canvas.width = 400;
		canvas.height = 64;
		canvas.style.cssText = 'display: block; width: 200px; height: 32px;';
		container.appendChild(canvas);

		// Store viz data on element for updates
		(container as unknown as { _vizDecl: VizDecl })._vizDecl = viz;

		return container;
	},

	update(element: HTMLElement, viz: VizDecl, beatPos: number, isPlaying: boolean): void {
		const canvas = element.querySelector('canvas');
		if (!canvas) return;

		const { events, cycleLength } = getPatternEvents(viz);
		render(canvas, events, cycleLength, beatPos, isPlaying);
	},

	destroy(_element: HTMLElement): void {
		// Nothing to clean up for piano roll
	}
};

// Register the piano roll renderer
registerRenderer(VizType.PianoRoll, pianoRollRenderer);

export { pianoRollRenderer };
