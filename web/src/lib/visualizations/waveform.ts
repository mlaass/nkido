/**
 * Waveform Visualization Renderer
 *
 * Displays a longer time-domain view of a signal with min/max envelope rendering.
 * Uses the same probe buffer as oscilloscope but with different rendering style.
 */

import type { VisualizationRenderer } from './registry';
import { registerRenderer } from './registry';
import { VizType, type VizDecl } from '$lib/stores/audio.svelte';
import { audioEngine } from '$lib/stores/audio.svelte';

interface WaveformState {
	canvas: HTMLCanvasElement;
	ctx: CanvasRenderingContext2D;
	lastUpdateTime: number;
}

const stateMap = new WeakMap<HTMLElement, WaveformState>();

// Default dimensions for visualization widgets
const DEFAULT_WIDTH = 200;
const DEFAULT_HEIGHT = 50;
const LABEL_HEIGHT = 18;

/**
 * Waveform Renderer
 */
const waveformRenderer: VisualizationRenderer = {
	create(viz: VizDecl): HTMLElement {
		// Extract dimensions from options
		const opts = viz.options || {};
		const width = (opts.width as number) ?? DEFAULT_WIDTH;
		const height = (opts.height as number) ?? DEFAULT_HEIGHT;
		const canvasHeight = height - LABEL_HEIGHT;

		const container = document.createElement('div');
		container.className = 'viz-waveform';
		container.style.cssText = `
			display: inline-block;
			border-radius: 4px;
			overflow: hidden;
			background: var(--bg-secondary, #1a1a1a);
			border: 1px solid var(--border-primary, #333);
			width: ${width}px;
			height: ${height}px;
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

		// Add canvas (2x for retina)
		const canvas = document.createElement('canvas');
		canvas.width = width * 2;
		canvas.height = canvasHeight * 2;
		canvas.style.cssText = `display: block; width: ${width}px; height: ${canvasHeight}px;`;
		container.appendChild(canvas);

		const ctx = canvas.getContext('2d');
		if (ctx) {
			drawWaveform(canvas, ctx, canvas.width, canvas.height, null);
		}

		stateMap.set(container, {
			canvas,
			ctx: ctx!,
			lastUpdateTime: 0
		});

		return container;
	},

	update(element: HTMLElement, viz: VizDecl, _beatPos: number, isPlaying: boolean): void {
		const state = stateMap.get(element);
		if (!state) return;

		// Update at ~30fps
		const now = performance.now();
		if (now - state.lastUpdateTime < 33) return;
		state.lastUpdateTime = now;

		if (!isPlaying || !viz.stateId) {
			drawWaveform(state.canvas, state.ctx, state.canvas.width, state.canvas.height, null);
			return;
		}

		audioEngine.getProbeData(viz.stateId).then(samples => {
			if (samples) {
				drawWaveform(state.canvas, state.ctx, state.canvas.width, state.canvas.height, samples);
			}
		});
	},

	destroy(element: HTMLElement): void {
		stateMap.delete(element);
	}
};

/**
 * Convert a hex color (#rrggbb) to rgba string with given alpha
 */
function hexToRgba(hex: string, alpha: number): string {
	const r = parseInt(hex.slice(1, 3), 16);
	const g = parseInt(hex.slice(3, 5), 16);
	const b = parseInt(hex.slice(5, 7), 16);
	return `rgba(${r}, ${g}, ${b}, ${alpha})`;
}

/**
 * Draw waveform with min/max envelope style
 */
function drawWaveform(
	canvas: HTMLCanvasElement,
	ctx: CanvasRenderingContext2D,
	width: number,
	height: number,
	samples: Float32Array | null
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

	// Draw center line
	ctx.strokeStyle = gridColor;
	ctx.lineWidth = 1;
	ctx.beginPath();
	ctx.moveTo(0, height / 2);
	ctx.lineTo(width, height / 2);
	ctx.stroke();

	if (!samples || samples.length === 0) {
		ctx.fillStyle = mutedColor;
		ctx.font = '11px monospace';
		ctx.textAlign = 'center';
		ctx.fillText('No signal', width / 2, height / 2 + 4);
		return;
	}

	const centerY = height / 2;
	const amplitude = (height / 2) * 0.9;

	// Calculate min/max for each column (envelope rendering)
	const samplesPerPixel = samples.length / width;

	// Fill envelope (filled area between min/max)
	ctx.fillStyle = hexToRgba(vizColor, 0.3);
	ctx.beginPath();

	// Draw top half (max values)
	for (let x = 0; x < width; x++) {
		const startIdx = Math.floor(x * samplesPerPixel);
		const endIdx = Math.min(Math.floor((x + 1) * samplesPerPixel), samples.length);

		let max = -Infinity;
		for (let i = startIdx; i < endIdx; i++) {
			if (samples[i] > max) max = samples[i];
		}
		max = Math.max(-1, Math.min(1, max));
		const y = centerY - max * amplitude;

		if (x === 0) {
			ctx.moveTo(x, y);
		} else {
			ctx.lineTo(x, y);
		}
	}

	// Draw bottom half (min values) in reverse
	for (let x = width - 1; x >= 0; x--) {
		const startIdx = Math.floor(x * samplesPerPixel);
		const endIdx = Math.min(Math.floor((x + 1) * samplesPerPixel), samples.length);

		let min = Infinity;
		for (let i = startIdx; i < endIdx; i++) {
			if (samples[i] < min) min = samples[i];
		}
		min = Math.max(-1, Math.min(1, min));
		const y = centerY - min * amplitude;
		ctx.lineTo(x, y);
	}

	ctx.closePath();
	ctx.fill();

	// Draw outline
	ctx.strokeStyle = vizColor;
	ctx.lineWidth = 1;

	// Top outline
	ctx.beginPath();
	for (let x = 0; x < width; x++) {
		const startIdx = Math.floor(x * samplesPerPixel);
		const endIdx = Math.min(Math.floor((x + 1) * samplesPerPixel), samples.length);

		let max = -Infinity;
		for (let i = startIdx; i < endIdx; i++) {
			if (samples[i] > max) max = samples[i];
		}
		max = Math.max(-1, Math.min(1, max));
		const y = centerY - max * amplitude;

		if (x === 0) {
			ctx.moveTo(x, y);
		} else {
			ctx.lineTo(x, y);
		}
	}
	ctx.stroke();

	// Bottom outline
	ctx.beginPath();
	for (let x = 0; x < width; x++) {
		const startIdx = Math.floor(x * samplesPerPixel);
		const endIdx = Math.min(Math.floor((x + 1) * samplesPerPixel), samples.length);

		let min = Infinity;
		for (let i = startIdx; i < endIdx; i++) {
			if (samples[i] < min) min = samples[i];
		}
		min = Math.max(-1, Math.min(1, min));
		const y = centerY - min * amplitude;

		if (x === 0) {
			ctx.moveTo(x, y);
		} else {
			ctx.lineTo(x, y);
		}
	}
	ctx.stroke();
}

// Register the waveform renderer
registerRenderer(VizType.Waveform, waveformRenderer);

export { waveformRenderer };
