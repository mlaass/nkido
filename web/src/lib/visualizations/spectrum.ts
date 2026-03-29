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
	logScale: boolean;
	minDb: number;
	maxDb: number;
	fftSize: number;
	resizeObserver: ResizeObserver | null;
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
		// Extract dimensions and parameters from options
		const opts = viz.options || {};
		const isRelativeWidth = typeof opts.width === 'string';
		const isRelativeHeight = typeof opts.height === 'string';
		const width = isRelativeWidth ? DEFAULT_WIDTH : ((opts.width as number) ?? DEFAULT_WIDTH);
		const height = isRelativeHeight ? DEFAULT_HEIGHT : ((opts.height as number) ?? DEFAULT_HEIGHT);
		const canvasHeight = height - LABEL_HEIGHT;
		const logScale = (opts.logScale as boolean) ?? false;
		const minDb = (opts.minDb as number) ?? -90;
		const maxDb = (opts.maxDb as number) ?? 0;
		const fftSize = (opts.fft as number) ?? 1024;

		const container = document.createElement('div');
		container.className = 'viz-spectrum';
		container.style.cssText = `
			display: inline-block;
			border-radius: 4px;
			overflow: hidden;
			background: var(--bg-secondary, #1a1a1a);
			border: 1px solid var(--border-primary, #333);
			width: ${isRelativeWidth ? '100%' : width + 'px'};
			height: ${isRelativeHeight ? '100%' : height + 'px'};
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
			drawSpectrum(canvas, ctx, canvas.width, canvas.height, null, logScale, fftSize);
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
			smoothedMagnitudes: new Float32Array(0),
			logScale,
			minDb,
			maxDb,
			fftSize,
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
			// Decay smoothed values when stopped
			for (let i = 0; i < state.smoothedMagnitudes.length; i++) {
				state.smoothedMagnitudes[i] *= 0.9;
			}
			drawSpectrum(state.canvas, state.ctx, state.canvas.width, state.canvas.height, state.smoothedMagnitudes, state.logScale, state.fftSize);
			return;
		}

		const { minDb, maxDb } = state;
		const dbRange = maxDb - minDb;

		audioEngine.getFFTProbeData(viz.stateId).then(data => {
			if (data) {
				const { magnitudes, binCount } = data;

				// Resize smoothed array if bin count changed
				if (state.smoothedMagnitudes.length !== binCount) {
					state.smoothedMagnitudes = new Float32Array(binCount);
				}

				// Smooth the magnitudes (exponential moving average)
				// Magnitudes arrive as dB values; normalize to 0..1 using configurable dB range
				for (let i = 0; i < binCount; i++) {
					const normalized = Math.max(0, Math.min(1, (magnitudes[i] - minDb) / dbRange));
					state.smoothedMagnitudes[i] = state.smoothedMagnitudes[i] * 0.7 + normalized * 0.3;
				}

				drawSpectrum(state.canvas, state.ctx, state.canvas.width, state.canvas.height, state.smoothedMagnitudes, state.logScale, state.fftSize);
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
 * Draw spectrum bars
 */
function drawSpectrum(
	canvas: HTMLCanvasElement,
	ctx: CanvasRenderingContext2D,
	width: number,
	height: number,
	magnitudes: Float32Array | null,
	logScale: boolean,
	fftSize: number
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
	const barWidth = width / maxBars;
	const maxHeight = height * 0.95;

	// Create theme-aware gradient for bars
	const gradient = ctx.createLinearGradient(0, height, 0, 0);
	gradient.addColorStop(0, vizColor);        // Viz color at bottom
	gradient.addColorStop(0.7, warningColor);  // Warning in upper range
	gradient.addColorStop(1, errorColor);      // Error at top

	const sampleRate = 48000;
	const nyquist = sampleRate / 2;
	const hzPerBin = sampleRate / fftSize;

	for (let i = 0; i < maxBars; i++) {
		let startBin: number;
		let endBin: number;

		if (logScale) {
			// Logarithmic frequency mapping: each bar covers a geometric frequency range
			const minFreq = 20;
			const logMin = Math.log(minFreq);
			const logMax = Math.log(nyquist);
			const freqLo = Math.exp(logMin + (logMax - logMin) * (i / maxBars));
			const freqHi = Math.exp(logMin + (logMax - logMin) * ((i + 1) / maxBars));
			startBin = Math.max(1, Math.floor(freqLo / hzPerBin));
			endBin = Math.min(Math.ceil(freqHi / hzPerBin), magnitudes.length);
			// Ensure at least 1 bin per bar
			if (endBin <= startBin) endBin = startBin + 1;
		} else {
			// Linear bin spacing (original behavior)
			const binsPerBar = magnitudes.length / maxBars;
			startBin = Math.floor(i * binsPerBar);
			endBin = Math.min(Math.floor((i + 1) * binsPerBar), magnitudes.length);
		}

		// Average bins for this bar
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

	if (logScale) {
		// Log scale: markers at decade boundaries
		const markers = [100, 1000, 10000];
		const labels = ['100', '1k', '10k'];
		const minFreq = 20;
		const logMin = Math.log(minFreq);
		const logMax = Math.log(nyquist);
		for (let m = 0; m < markers.length; m++) {
			if (markers[m] < minFreq || markers[m] > nyquist) continue;
			const x = ((Math.log(markers[m]) - logMin) / (logMax - logMin)) * width;
			ctx.fillText(labels[m], x, height - 2);
		}
	} else {
		// Linear scale: endpoints
		ctx.fillText('0', 10, height - 2);
		const nyquistLabel = nyquist >= 1000 ? `${Math.round(nyquist / 1000)}k` : `${nyquist}`;
		ctx.fillText(nyquistLabel, width - 15, height - 2);
	}
}

// Register the spectrum renderer
registerRenderer(VizType.Spectrum, spectrumRenderer);

export { spectrumRenderer };
