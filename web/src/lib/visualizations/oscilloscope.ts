/**
 * Oscilloscope Visualization Renderer
 *
 * Displays a time-domain waveform view of a signal.
 * Shows a short rolling window (~21ms at 48kHz = 1024 samples).
 */

import type { VisualizationRenderer } from './registry';
import { registerRenderer } from './registry';
import { VizType, type VizDecl } from '$lib/stores/audio.svelte';
import { audioEngine } from '$lib/stores/audio.svelte';

interface OscilloscopeState {
	canvas: HTMLCanvasElement;
	ctx: CanvasRenderingContext2D;
	animationId: number | null;
	lastUpdateTime: number;
	triggerLevel: number;
	triggerEdge: 'rising' | 'falling';
	resizeObserver: ResizeObserver | null;
}

const stateMap = new WeakMap<HTMLElement, OscilloscopeState>();

// Default dimensions for visualization widgets
const DEFAULT_WIDTH = 200;
const DEFAULT_HEIGHT = 50;
const LABEL_HEIGHT = 18;

/**
 * Oscilloscope Renderer
 */
const oscilloscopeRenderer: VisualizationRenderer = {
	create(viz: VizDecl): HTMLElement {
		// Extract dimensions and parameters from options
		const opts = viz.options || {};
		const isRelativeWidth = typeof opts.width === 'string';
		const isRelativeHeight = typeof opts.height === 'string';
		const width = isRelativeWidth ? DEFAULT_WIDTH : ((opts.width as number) ?? DEFAULT_WIDTH);
		const height = isRelativeHeight ? DEFAULT_HEIGHT : ((opts.height as number) ?? DEFAULT_HEIGHT);
		const canvasHeight = height - LABEL_HEIGHT;
		const triggerLevel = (opts.triggerLevel as number) ?? 0;
		const triggerEdge = ((opts.triggerEdge as string) ?? 'rising') as 'rising' | 'falling';

		const container = document.createElement('div');
		container.className = 'viz-oscilloscope';
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

		const ctx = canvas.getContext('2d');
		if (ctx) {
			// Initial empty state
			drawOscilloscope(canvas, ctx, canvas.width, canvas.height, null, triggerLevel, triggerEdge);
		}

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

		// Store state
		stateMap.set(container, {
			canvas,
			ctx: ctx!,
			animationId: null,
			lastUpdateTime: 0,
			triggerLevel,
			triggerEdge,
			resizeObserver
		});

		return container;
	},

	update(element: HTMLElement, viz: VizDecl, _beatPos: number, isPlaying: boolean): void {
		const state = stateMap.get(element);
		if (!state) return;

		// Only update when playing and at reasonable rate (~30fps)
		const now = performance.now();
		if (now - state.lastUpdateTime < 33) return;  // ~30fps
		state.lastUpdateTime = now;

		if (!isPlaying || !viz.stateId) {
			drawOscilloscope(state.canvas, state.ctx, state.canvas.width, state.canvas.height, null, state.triggerLevel, state.triggerEdge);
			return;
		}

		// Fetch probe data asynchronously
		audioEngine.getProbeData(viz.stateId).then(samples => {
			if (samples) {
				drawOscilloscope(state.canvas, state.ctx, state.canvas.width, state.canvas.height, samples, state.triggerLevel, state.triggerEdge);
			}
		});
	},

	destroy(element: HTMLElement): void {
		const state = stateMap.get(element);
		if (state?.animationId) {
			cancelAnimationFrame(state.animationId);
		}
		state?.resizeObserver?.disconnect();
		stateMap.delete(element);
	}
};

/**
 * Find the first sample index where the signal crosses the trigger level.
 * Returns 0 if no crossing is found.
 */
function findTriggerPoint(
	samples: Float32Array,
	level: number,
	edge: 'rising' | 'falling'
): number {
	for (let i = 1; i < samples.length - 1; i++) {
		if (edge === 'rising' && samples[i - 1] < level && samples[i] >= level) return i;
		if (edge === 'falling' && samples[i - 1] > level && samples[i] <= level) return i;
	}
	return 0;
}

/**
 * Draw oscilloscope waveform
 */
function drawOscilloscope(
	canvas: HTMLCanvasElement,
	ctx: CanvasRenderingContext2D,
	width: number,
	height: number,
	samples: Float32Array | null,
	triggerLevel: number,
	triggerEdge: 'rising' | 'falling'
): void {
	// Read theme colors from CSS
	const style = getComputedStyle(canvas);
	const bgColor = style.getPropertyValue('--bg-secondary').trim() || '#1a1a1a';
	const gridColor = style.getPropertyValue('--border-muted').trim() || '#333';
	const vizColor = style.getPropertyValue('--accent-viz').trim() || '#4ade80';
	const mutedColor = style.getPropertyValue('--text-muted').trim() || '#444';

	// Clear background
	ctx.fillStyle = bgColor;
	ctx.fillRect(0, 0, width, height);

	const centerY = height / 2;
	const amplitude = (height / 2) * 0.9;  // Leave some margin

	// Draw center line
	ctx.strokeStyle = gridColor;
	ctx.lineWidth = 1;
	ctx.beginPath();
	ctx.moveTo(0, height / 2);
	ctx.lineTo(width, height / 2);
	ctx.stroke();

	if (!samples || samples.length === 0) {
		// No data - show placeholder
		ctx.fillStyle = mutedColor;
		ctx.font = '11px monospace';
		ctx.textAlign = 'center';
		ctx.fillText('No signal', width / 2, height / 2 + 4);
		return;
	}

	// Apply trigger: find crossing point and start rendering from there
	const triggerOffset = findTriggerPoint(samples, triggerLevel, triggerEdge);
	const visibleSamples = samples.subarray(triggerOffset);

	// Draw waveform with anti-aliasing
	ctx.strokeStyle = vizColor;
	ctx.lineWidth = 1.5;
	ctx.lineCap = 'round';
	ctx.lineJoin = 'round';
	ctx.beginPath();

	// Map samples to canvas width
	const step = visibleSamples.length / width;
	for (let x = 0; x < width; x++) {
		const sampleIndex = Math.floor(x * step);
		const sample = visibleSamples[sampleIndex];
		// Clamp sample to -1..1 range
		const clampedSample = Math.max(-1, Math.min(1, sample));
		const y = centerY - clampedSample * amplitude;

		if (x === 0) {
			ctx.moveTo(x, y);
		} else {
			ctx.lineTo(x, y);
		}
	}

	ctx.stroke();

	// Draw grid markers for amplitude reference
	ctx.strokeStyle = gridColor;
	ctx.lineWidth = 0.5;
	ctx.setLineDash([2, 4]);

	// +0.5 line
	ctx.beginPath();
	ctx.moveTo(0, centerY - amplitude * 0.5);
	ctx.lineTo(width, centerY - amplitude * 0.5);
	ctx.stroke();

	// -0.5 line
	ctx.beginPath();
	ctx.moveTo(0, centerY + amplitude * 0.5);
	ctx.lineTo(width, centerY + amplitude * 0.5);
	ctx.stroke();

	// Draw trigger level line when non-zero
	if (triggerLevel !== 0) {
		const triggerY = centerY - Math.max(-1, Math.min(1, triggerLevel)) * amplitude;
		ctx.strokeStyle = mutedColor;
		ctx.beginPath();
		ctx.moveTo(0, triggerY);
		ctx.lineTo(width, triggerY);
		ctx.stroke();
	}

	ctx.setLineDash([]);
}

// Register the oscilloscope renderer
registerRenderer(VizType.Oscilloscope, oscilloscopeRenderer);

export { oscilloscopeRenderer };
