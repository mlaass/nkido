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
}

const stateMap = new WeakMap<HTMLElement, OscilloscopeState>();

/**
 * Oscilloscope Renderer
 */
const oscilloscopeRenderer: VisualizationRenderer = {
	create(viz: VizDecl): HTMLElement {
		const container = document.createElement('div');
		container.className = 'viz-oscilloscope';
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
		canvas.width = 400;  // 2x for retina
		canvas.height = 64;
		canvas.style.cssText = 'display: block; width: 200px; height: 32px;';
		container.appendChild(canvas);

		const ctx = canvas.getContext('2d');
		if (ctx) {
			// Initial empty state
			drawOscilloscope(ctx, canvas.width, canvas.height, null);
		}

		// Store state
		stateMap.set(container, {
			canvas,
			ctx: ctx!,
			animationId: null,
			lastUpdateTime: 0
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
			drawOscilloscope(state.ctx, state.canvas.width, state.canvas.height, null);
			return;
		}

		// Fetch probe data asynchronously
		audioEngine.getProbeData(viz.stateId).then(samples => {
			if (samples) {
				drawOscilloscope(state.ctx, state.canvas.width, state.canvas.height, samples);
			}
		});
	},

	destroy(element: HTMLElement): void {
		const state = stateMap.get(element);
		if (state?.animationId) {
			cancelAnimationFrame(state.animationId);
		}
		stateMap.delete(element);
	}
};

/**
 * Draw oscilloscope waveform
 */
function drawOscilloscope(
	ctx: CanvasRenderingContext2D,
	width: number,
	height: number,
	samples: Float32Array | null
): void {
	// Clear background
	ctx.fillStyle = '#1a1a1a';
	ctx.fillRect(0, 0, width, height);

	// Draw center line
	ctx.strokeStyle = '#333';
	ctx.lineWidth = 1;
	ctx.beginPath();
	ctx.moveTo(0, height / 2);
	ctx.lineTo(width, height / 2);
	ctx.stroke();

	if (!samples || samples.length === 0) {
		// No data - show placeholder
		ctx.fillStyle = '#444';
		ctx.font = '11px monospace';
		ctx.textAlign = 'center';
		ctx.fillText('No signal', width / 2, height / 2 + 4);
		return;
	}

	// Draw waveform with anti-aliasing
	ctx.strokeStyle = '#4ade80';  // Green
	ctx.lineWidth = 1.5;
	ctx.lineCap = 'round';
	ctx.lineJoin = 'round';
	ctx.beginPath();

	const centerY = height / 2;
	const amplitude = (height / 2) * 0.9;  // Leave some margin

	// Map samples to canvas width
	const step = samples.length / width;
	for (let x = 0; x < width; x++) {
		const sampleIndex = Math.floor(x * step);
		const sample = samples[sampleIndex];
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
	ctx.strokeStyle = '#333';
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

	ctx.setLineDash([]);
}

// Register the oscilloscope renderer
registerRenderer(VizType.Oscilloscope, oscilloscopeRenderer);

export { oscilloscopeRenderer };
