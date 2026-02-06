/**
 * Spectrum Visualization Renderer
 *
 * Displays a frequency-domain (FFT) view of a signal.
 * Uses a simple DFT for 64 frequency bins (more bins would need optimized FFT).
 */

import type { VisualizationRenderer } from './registry';
import { registerRenderer } from './registry';
import { VizType, type VizDecl } from '$lib/stores/audio.svelte';
import { audioEngine } from '$lib/stores/audio.svelte';

interface SpectrumState {
	canvas: HTMLCanvasElement;
	ctx: CanvasRenderingContext2D;
	lastUpdateTime: number;
	// Smoothed magnitude values for smoother visualization
	smoothedMagnitudes: Float32Array;
}

const stateMap = new WeakMap<HTMLElement, SpectrumState>();

// Number of frequency bins to display
const NUM_BINS = 64;

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
			smoothedMagnitudes: new Float32Array(NUM_BINS)
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
			for (let i = 0; i < NUM_BINS; i++) {
				state.smoothedMagnitudes[i] *= 0.9;
			}
			drawSpectrum(state.canvas, state.ctx, state.canvas.width, state.canvas.height, state.smoothedMagnitudes);
			return;
		}

		audioEngine.getProbeData(viz.stateId).then(samples => {
			if (samples) {
				// Compute spectrum
				const magnitudes = computeSpectrum(samples, NUM_BINS);

				// Smooth the magnitudes (exponential moving average)
				for (let i = 0; i < NUM_BINS; i++) {
					state.smoothedMagnitudes[i] = state.smoothedMagnitudes[i] * 0.7 + magnitudes[i] * 0.3;
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
 * Compute spectrum magnitudes using a simple DFT
 * For real-time use with small bin counts, DFT is fast enough
 */
function computeSpectrum(samples: Float32Array, numBins: number): Float32Array {
	const magnitudes = new Float32Array(numBins);
	const N = samples.length;

	// Apply Hanning window
	const windowed = new Float32Array(N);
	for (let i = 0; i < N; i++) {
		const window = 0.5 * (1 - Math.cos((2 * Math.PI * i) / (N - 1)));
		windowed[i] = samples[i] * window;
	}

	// Compute DFT for each bin
	// We only need the first half of frequencies (up to Nyquist)
	for (let k = 0; k < numBins; k++) {
		// Map bin to frequency index (logarithmic spacing for better low-freq resolution)
		const freq = Math.pow(k / numBins, 1.5) * (N / 2);
		const freqIdx = Math.floor(freq);

		let real = 0;
		let imag = 0;

		// DFT at this frequency
		const omega = (2 * Math.PI * freqIdx) / N;
		for (let n = 0; n < N; n++) {
			real += windowed[n] * Math.cos(omega * n);
			imag -= windowed[n] * Math.sin(omega * n);
		}

		// Magnitude (normalized)
		magnitudes[k] = Math.sqrt(real * real + imag * imag) / N;
	}

	return magnitudes;
}

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

	const barWidth = width / magnitudes.length;
	const maxHeight = height * 0.95;

	// Create theme-aware gradient for bars
	const gradient = ctx.createLinearGradient(0, height, 0, 0);
	gradient.addColorStop(0, vizColor);        // Viz color at bottom
	gradient.addColorStop(0.7, warningColor);  // Warning in upper range
	gradient.addColorStop(1, errorColor);      // Error at top

	for (let i = 0; i < magnitudes.length; i++) {
		// Convert to dB scale (with floor at -60dB)
		const magnitude = magnitudes[i];
		const db = magnitude > 0 ? 20 * Math.log10(magnitude) : -60;
		const normalizedDb = Math.max(0, (db + 60) / 60);  // 0 to 1
		const barHeight = normalizedDb * maxHeight;

		const x = i * barWidth;

		// Draw bar
		ctx.fillStyle = gradient;
		ctx.fillRect(x + 1, height - barHeight, barWidth - 2, barHeight);
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
