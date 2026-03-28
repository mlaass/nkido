/**
 * Spectrum Visualization Renderer
 *
 * Displays a frequency-domain (FFT) view of a signal.
 * Uses WASM FFT via the FFT_PROBE opcode for high-resolution analysis.
 */

import type { VisualizationRenderer } from './registry';
import { registerRenderer } from './registry';
import { VizType, type VizDecl } from '$lib/stores/audio.svelte';
import { audioEngine } from '$lib/stores/audio.svelte';

interface SpectrumState {
	canvas: HTMLCanvasElement;
	ctx: CanvasRenderingContext2D;
	lastUpdateTime: number;
	smoothedMagnitudes: Float32Array;
}

const stateMap = new WeakMap<HTMLElement, SpectrumState>();

// Default dimensions for visualization widgets
const DEFAULT_WIDTH = 200;
const DEFAULT_HEIGHT = 50;
const LABEL_HEIGHT = 18;

/**
 * Spectrum Renderer
 */
const spectrumRenderer: VisualizationRenderer = {
	create(viz: VizDecl): HTMLElement {
		// Extract dimensions from options
		const opts = viz.options || {};
		const width = (opts.width as number) ?? DEFAULT_WIDTH;
		const height = (opts.height as number) ?? DEFAULT_HEIGHT;
		const canvasHeight = height - LABEL_HEIGHT;

		const container = document.createElement('div');
		container.className = 'viz-spectrum';
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
			drawSpectrum(canvas, ctx, canvas.width, canvas.height, null);
		}

		stateMap.set(container, {
			canvas,
			ctx: ctx!,
			lastUpdateTime: 0,
			smoothedMagnitudes: new Float32Array(0)
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
			// Decay smoothed values when stopped
			for (let i = 0; i < state.smoothedMagnitudes.length; i++) {
				state.smoothedMagnitudes[i] *= 0.9;
			}
			drawSpectrum(state.canvas, state.ctx, state.canvas.width, state.canvas.height, state.smoothedMagnitudes);
			return;
		}

		audioEngine.getFFTProbeData(viz.stateId).then(data => {
			if (data) {
				const { magnitudes, binCount } = data;

				// Resize smoothed array if bin count changed
				if (state.smoothedMagnitudes.length !== binCount) {
					state.smoothedMagnitudes = new Float32Array(binCount);
				}

				// Smooth the magnitudes (exponential moving average)
				// Magnitudes arrive as dB values; normalize to 0..1
				for (let i = 0; i < binCount; i++) {
					const normalized = Math.max(0, (magnitudes[i] + 90) / 90);
					state.smoothedMagnitudes[i] = state.smoothedMagnitudes[i] * 0.7 + normalized * 0.3;
				}

				drawSpectrum(state.canvas, state.ctx, state.canvas.width, state.canvas.height, state.smoothedMagnitudes);
			}
		});
	},

	destroy(element: HTMLElement): void {
		stateMap.delete(element);
	}
};

/**
 * Draw spectrum bars
 */
function drawSpectrum(
	canvas: HTMLCanvasElement,
	ctx: CanvasRenderingContext2D,
	width: number,
	height: number,
	magnitudes: Float32Array | null
): void {
	// Read theme colors from CSS
	const style = getComputedStyle(canvas);
	const bgColor = style.getPropertyValue('--bg-secondary').trim() || '#1a1a1a';
	const vizColor = style.getPropertyValue('--accent-viz').trim() || '#4ade80';
	const warningColor = style.getPropertyValue('--accent-warning').trim() || '#d29922';
	const errorColor = style.getPropertyValue('--accent-error').trim() || '#f85149';
	const mutedColor = style.getPropertyValue('--text-muted').trim() || '#444';

	// Clear background
	ctx.fillStyle = bgColor;
	ctx.fillRect(0, 0, width, height);

	if (!magnitudes || magnitudes.length === 0) {
		ctx.fillStyle = mutedColor;
		ctx.font = '11px monospace';
		ctx.textAlign = 'center';
		ctx.fillText('No signal', width / 2, height / 2 + 4);
		return;
	}

	// Downsample bins to fit display width
	// With 513+ bins from FFT, we group bins into visual bars
	const maxBars = Math.min(magnitudes.length, Math.floor(width / 3)); // ~3px per bar minimum
	const binsPerBar = magnitudes.length / maxBars;
	const barWidth = width / maxBars;
	const maxHeight = height * 0.95;

	// Create theme-aware gradient for bars
	const gradient = ctx.createLinearGradient(0, height, 0, 0);
	gradient.addColorStop(0, vizColor);        // Viz color at bottom
	gradient.addColorStop(0.7, warningColor);  // Warning in upper range
	gradient.addColorStop(1, errorColor);      // Error at top

	for (let i = 0; i < maxBars; i++) {
		// Average bins for this bar
		const startBin = Math.floor(i * binsPerBar);
		const endBin = Math.min(Math.floor((i + 1) * binsPerBar), magnitudes.length);
		let sum = 0;
		for (let b = startBin; b < endBin; b++) {
			sum += magnitudes[b];
		}
		const avg = sum / (endBin - startBin);
		const barHeight = avg * maxHeight;

		const x = i * barWidth;

		// Draw bar
		ctx.fillStyle = gradient;
		ctx.fillRect(x + 0.5, height - barHeight, barWidth - 1, barHeight);
	}

	// Draw frequency markers
	ctx.fillStyle = mutedColor;
	ctx.font = '9px monospace';
	ctx.textAlign = 'center';

	// Low frequency marker
	ctx.fillText('0', 10, height - 2);

	// Nyquist marker (assuming 48kHz = 24kHz Nyquist)
	ctx.fillText('24k', width - 15, height - 2);
}

// Register the spectrum renderer
registerRenderer(VizType.Spectrum, spectrumRenderer);

export { spectrumRenderer };
