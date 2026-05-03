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

// Scale interval sets (semitones from root C)
const SCALE_INTERVALS: Record<string, Set<number>> = {
	chromatic: new Set([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]),
	pentatonic: new Set([0, 2, 4, 7, 9]),
	octave: new Set([0])
};

/**
 * Render the piano roll on a canvas
 */
function render(
	canvas: HTMLCanvasElement,
	events: PatternEvent[],
	cycleLength: number,
	beatPos: number,
	isPlaying: boolean,
	opts: { beats?: number; showGrid: boolean; scale: string }
): void {
	const ctx = canvas.getContext('2d');
	if (!ctx) return;

	const w = canvas.width;
	const h = canvas.height;

	// Calculate visible window (1+ cycle centered on playhead when playing)
	const windowBeats = opts.beats ?? Math.max(4, cycleLength);
	const windowStart = isPlaying ? Math.max(0, beatPos - windowBeats * 0.25) : 0;
	const windowEnd = windowStart + windowBeats;

	// Get computed styles
	const computedStyle = getComputedStyle(canvas);
	const bgColor = computedStyle.getPropertyValue('--bg-secondary').trim() || '#1a1a1a';
	const borderColor = computedStyle.getPropertyValue('--border-primary').trim() || '#333';
	const accentColor = computedStyle.getPropertyValue('--accent-primary').trim() || '#4ade80';
	const dimColor = computedStyle.getPropertyValue('--text-tertiary').trim() || '#666';
	const playheadColor = computedStyle.getPropertyValue('--accent-secondary').trim() || '#fb923c';

	const scaleSet = SCALE_INTERVALS[opts.scale] ?? SCALE_INTERVALS.chromatic;

	// Clear with background
	ctx.fillStyle = bgColor;
	ctx.fillRect(0, 0, w, h);

	// Draw beat grid
	if (opts.showGrid) {
		ctx.strokeStyle = borderColor;
		ctx.lineWidth = 1;
		for (let beat = Math.ceil(windowStart); beat < windowEnd; beat++) {
			const x = ((beat - windowStart) / (windowEnd - windowStart)) * w;
			ctx.beginPath();
			ctx.moveTo(x, 0);
			ctx.lineTo(x, h);
			ctx.stroke();
		}
	}

	// Draw events
	for (const evt of events) {
		const cycleTime = evt.time % cycleLength;
		const maxCycle = Math.ceil(windowEnd / cycleLength) + 1;

		// Check if event's note is in the selected scale
		const midi = Math.round(12 * Math.log2(evt.value / 440) + 69);
		const inScale = scaleSet.has(((midi % 12) + 12) % 12);

		for (let cycle = -1; cycle <= maxCycle; cycle++) {
			const evtTime = cycleTime + cycle * cycleLength;
			if (evtTime >= windowStart && evtTime < windowEnd) {
				const x = ((evtTime - windowStart) / (windowEnd - windowStart)) * w;
				const evtW = Math.max((evt.duration / (windowEnd - windowStart)) * w, 3);
				const y = freqToY(evt.value, h);

				if (evtTime < beatPos || !inScale) {
					ctx.fillStyle = dimColor;
				} else {
					ctx.fillStyle = accentColor;
				}
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

// Default dimensions for visualization widgets
const DEFAULT_WIDTH = 200;
const DEFAULT_HEIGHT = 50;
const LABEL_HEIGHT = 18;

/**
 * Piano Roll Renderer
 */
const pianoRollRenderer: VisualizationRenderer = {
	create(viz: VizDecl): HTMLElement {
		// Extract dimensions and parameters from options
		const opts = viz.options || {};
		const isRelativeWidth = typeof opts.width === 'string';
		const isRelativeHeight = typeof opts.height === 'string';
		const width = isRelativeWidth ? DEFAULT_WIDTH : ((opts.width as number) ?? DEFAULT_WIDTH);
		const height = isRelativeHeight ? DEFAULT_HEIGHT : ((opts.height as number) ?? DEFAULT_HEIGHT);
		const canvasHeight = height - LABEL_HEIGHT;

		const container = document.createElement('div');
		container.className = 'viz-pianoroll';
		container.style.width = isRelativeWidth ? '100%' : `${width}px`;
		container.style.height = isRelativeHeight ? '100%' : `${height}px`;

		// Add label
		const label = document.createElement('div');
		label.className = 'viz-label';
		label.textContent = viz.name;
		container.appendChild(label);

		// Add canvas (2x for retina)
		const canvas = document.createElement('canvas');
		canvas.width = width * 2;
		canvas.height = canvasHeight * 2;
		canvas.style.width = `${width}px`;
		canvas.style.height = `${canvasHeight}px`;
		container.appendChild(canvas);

		// Attach ResizeObserver for relative sizing
		let resizeObserver: ResizeObserver | null = null;
		if (isRelativeWidth || isRelativeHeight) {
			resizeObserver = new ResizeObserver(entries => {
				for (const entry of entries) {
					const rect = entry.contentRect;
					const newWidth = isRelativeWidth ? rect.width : width;
					const newHeight = isRelativeHeight ? rect.height : height;
					const newCanvasHeight = newHeight - LABEL_HEIGHT;
					if (newCanvasHeight <= 0) return;
					canvas.width = Math.round(newWidth * 2);
					canvas.height = Math.round(newCanvasHeight * 2);
					canvas.style.width = `${newWidth}px`;
					canvas.style.height = `${newCanvasHeight}px`;
				}
			});
			resizeObserver.observe(container);
		}

		// Store viz data and parsed options on element for updates
		(container as unknown as { _vizDecl: VizDecl; _vizOpts: { beats?: number; showGrid: boolean; scale: string }; _resizeObserver: ResizeObserver | null })._vizDecl = viz;
		(container as unknown as { _vizOpts: { beats?: number; showGrid: boolean; scale: string } })._vizOpts = {
			beats: opts.beats != null ? (opts.beats as number) : undefined,
			showGrid: (opts.showGrid as boolean) ?? true,
			scale: (opts.scale as string) ?? 'chromatic'
		};
		(container as unknown as { _resizeObserver: ResizeObserver | null })._resizeObserver = resizeObserver;

		return container;
	},

	update(element: HTMLElement, viz: VizDecl, beatPos: number, isPlaying: boolean): void {
		const canvas = element.querySelector('canvas');
		if (!canvas) return;

		const vizOpts = (element as unknown as { _vizOpts: { beats?: number; showGrid: boolean; scale: string } })._vizOpts
			?? { showGrid: true, scale: 'chromatic' };

		const { events, cycleLength } = getPatternEvents(viz);
		render(canvas, events, cycleLength, beatPos, isPlaying, vizOpts);
	},

	destroy(element: HTMLElement): void {
		(element as unknown as { _resizeObserver: ResizeObserver | null })?._resizeObserver?.disconnect();
	}
};

// Register the piano roll renderer
registerRenderer(VizType.PianoRoll, pianoRollRenderer);

export { pianoRollRenderer };
