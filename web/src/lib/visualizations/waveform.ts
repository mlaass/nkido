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
	scale: number;
	filled: boolean;
	resizeObserver: ResizeObserver | null;
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
		// Extract dimensions and parameters from options
		const opts = viz.options || {};
		const isRelativeWidth = typeof opts.width === 'string';
		const isRelativeHeight = typeof opts.height === 'string';
		const width = isRelativeWidth ? DEFAULT_WIDTH : ((opts.width as number) ?? DEFAULT_WIDTH);
		const height = isRelativeHeight ? DEFAULT_HEIGHT : ((opts.height as number) ?? DEFAULT_HEIGHT);
		const canvasHeight = height - LABEL_HEIGHT;
		const scale = (opts.scale as number) ?? 1.0;
		const filled = (opts.filled as boolean) ?? true;

		const container = document.createElement('div');
		container.className = 'viz-waveform';
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
			drawWaveform(canvas, ctx, canvas.width, canvas.height, null, scale, filled);
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

		stateMap.set(container, {
			canvas,
			ctx: ctx!,
			lastUpdateTime: 0,
			scale,
			filled,
			resizeObserver
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
			drawWaveform(state.canvas, state.ctx, state.canvas.width, state.canvas.height, null, state.scale, state.filled);
			return;
		}

		audioEngine.getProbeData(viz.stateId).then(samples => {
			if (samples) {
				drawWaveform(state.canvas, state.ctx, state.canvas.width, state.canvas.height, samples, state.scale, state.filled);
			}
		});
	},

	destroy(element: HTMLElement): void {
		const state = stateMap.get(element);
		state?.resizeObserver?.disconnect();
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
 * Draw waveform with min/max envelope style or line style
 */
function drawWaveform(
	canvas: HTMLCanvasElement,
	ctx: CanvasRenderingContext2D,
	width: number,
	height: number,
	samples: Float32Array | null,
	scale: number,
	filled: boolean
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
	const amplitude = (height / 2) * 0.9 * scale;

	// Calculate min/max for each column
	const samplesPerPixel = samples.length / width;

	if (!filled) {
		// Line mode: draw center-line waveform (average of min/max per column)
		ctx.strokeStyle = vizColor;
		ctx.lineWidth = 1.5;
		ctx.lineCap = 'round';
		ctx.lineJoin = 'round';
		ctx.beginPath();

		for (let x = 0; x < width; x++) {
			const startIdx = Math.floor(x * samplesPerPixel);
			const endIdx = Math.min(Math.floor((x + 1) * samplesPerPixel), samples.length);

			let min = Infinity;
			let max = -Infinity;
			for (let i = startIdx; i < endIdx; i++) {
				if (samples[i] < min) min = samples[i];
				if (samples[i] > max) max = samples[i];
			}
			const avg = (min + max) / 2;
			const y = Math.max(0, Math.min(height, centerY - avg * amplitude));

			if (x === 0) {
				ctx.moveTo(x, y);
			} else {
				ctx.lineTo(x, y);
			}
		}

		ctx.stroke();
		return;
	}

	// Filled envelope mode (default)

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
		const y = Math.max(0, Math.min(height, centerY - max * amplitude));

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
		const y = Math.max(0, Math.min(height, centerY - min * amplitude));
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
		const y = Math.max(0, Math.min(height, centerY - max * amplitude));

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
		const y = Math.max(0, Math.min(height, centerY - min * amplitude));

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
