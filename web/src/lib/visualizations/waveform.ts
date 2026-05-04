/**
 * Waveform Visualization Renderer
 *
 * Long-horizon scrolling envelope view (waterfall-style for time-domain data).
 * Each pixel column represents a slice of recent time; the column's vertical
 * extent shows the min/max of all samples that fell into that slice. New data
 * arrives on the right, old data scrolls off the left.
 *
 * Distinct from oscilloscope: oscilloscope shows the most recent ~10ms of raw
 * samples; waveform shows seconds (default 5s) of envelope history. Both share
 * the same probe ring buffer as their data source — waveform accumulates poll
 * results into a sliding history client-side.
 *
 * The y-axis auto-scales to fit the actual signal range observed across the
 * visible history. The display floor is ±1 (so audio-rate signals always show
 * the unity reference at the same place); LFOs / control signals that range
 * outside ±1 expand the viewport to fit. Asymmetric ranges (e.g. a 0..5
 * unipolar LFO) get an asymmetric viewport. The ±1 boundary stays visible as
 * dashed reference lines, and any portion exceeding ±1 is drawn red.
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
	durationMs: number;
	resizeObserver: ResizeObserver | null;

	// Sliding history of per-column envelopes. min/max are interleaved
	// (history[2*i] = min, history[2*i+1] = max). Length tracks canvas width.
	history: Float32Array;
	numColumns: number;
	writePos: number; // index of next column to write (oldest column when full)
	filledColumns: number; // how many columns have been written (caps at numColumns)
	lastColumnTime: number;

	// Accumulator for the column currently being filled.
	accMin: number;
	accMax: number;
	accAny: boolean;

	// Smoothed display bounds (auto-scaling y-axis).
	displayMin: number;
	displayMax: number;
}

const stateMap = new WeakMap<HTMLElement, WaveformState>();

const DEFAULT_WIDTH = 200;
const DEFAULT_HEIGHT = 50;
const LABEL_HEIGHT = 18;
const DEFAULT_DURATION_SEC = 5;
const SCALE_DECAY = 0.03;

function makeHistory(numColumns: number): Float32Array {
	return new Float32Array(numColumns * 2);
}

function resetAccumulator(state: WaveformState): void {
	state.accMin = Infinity;
	state.accMax = -Infinity;
	state.accAny = false;
}

const waveformRenderer: VisualizationRenderer = {
	create(viz: VizDecl): HTMLElement {
		const opts = viz.options || {};
		const isRelativeWidth = typeof opts.width === 'string';
		const isRelativeHeight = typeof opts.height === 'string';
		const width = isRelativeWidth ? DEFAULT_WIDTH : ((opts.width as number) ?? DEFAULT_WIDTH);
		const height = isRelativeHeight ? DEFAULT_HEIGHT : ((opts.height as number) ?? DEFAULT_HEIGHT);
		const canvasHeight = height - LABEL_HEIGHT;
		const scale = (opts.scale as number) ?? 1.0;
		const filled = (opts.filled as boolean) ?? true;
		const duration = (opts.duration as number) ?? DEFAULT_DURATION_SEC;

		const container = document.createElement('div');
		container.className = 'viz-waveform';
		container.style.width = isRelativeWidth ? '100%' : `${width}px`;
		container.style.height = isRelativeHeight ? '100%' : `${height}px`;

		const label = document.createElement('div');
		label.className = 'viz-label';
		label.textContent = viz.name;
		container.appendChild(label);

		const canvas = document.createElement('canvas');
		canvas.width = width * 2;
		canvas.height = canvasHeight * 2;
		canvas.style.width = `${width}px`;
		canvas.style.height = `${canvasHeight}px`;
		container.appendChild(canvas);

		const ctx = canvas.getContext('2d')!;

		const numColumns = canvas.width;
		const state: WaveformState = {
			canvas,
			ctx,
			lastUpdateTime: 0,
			scale,
			filled,
			durationMs: duration * 1000,
			resizeObserver: null,
			history: makeHistory(numColumns),
			numColumns,
			writePos: 0,
			filledColumns: 0,
			lastColumnTime: performance.now(),
			accMin: Infinity,
			accMax: -Infinity,
			accAny: false,
			displayMin: -1,
			displayMax: 1
		};

		drawWaveform(state, canvas.width, canvas.height, false);

		if (isRelativeWidth || isRelativeHeight) {
			state.resizeObserver = new ResizeObserver(entries => {
				for (const entry of entries) {
					const rect = entry.contentRect;
					const newWidth = isRelativeWidth ? rect.width : width;
					const newHeight = isRelativeHeight ? rect.height : height;
					const newCanvasHeight = newHeight - LABEL_HEIGHT;
					if (newCanvasHeight <= 0) return;
					const newCanvasW = Math.round(newWidth * 2);
					if (canvas.width !== newCanvasW) {
						canvas.width = newCanvasW;
						state.numColumns = newCanvasW;
						state.history = makeHistory(newCanvasW);
						state.writePos = 0;
						state.filledColumns = 0;
						resetAccumulator(state);
						state.lastColumnTime = performance.now();
					}
					canvas.height = Math.round(newCanvasHeight * 2);
					canvas.style.width = `${newWidth}px`;
					canvas.style.height = `${newCanvasHeight}px`;
				}
			});
			state.resizeObserver.observe(container);
		}

		stateMap.set(container, state);
		return container;
	},

	update(element: HTMLElement, viz: VizDecl, _beatPos: number, isPlaying: boolean): void {
		const state = stateMap.get(element);
		if (!state) return;

		const now = performance.now();
		if (now - state.lastUpdateTime < 33) return;
		state.lastUpdateTime = now;

		if (!isPlaying || !viz.stateId) {
			drawWaveform(state, state.canvas.width, state.canvas.height, false);
			return;
		}

		audioEngine.getProbeData(viz.stateId).then(samples => {
			if (samples && samples.length > 0) {
				for (let i = 0; i < samples.length; i++) {
					const s = samples[i];
					if (s < state.accMin) state.accMin = s;
					if (s > state.accMax) state.accMax = s;
				}
				state.accAny = true;
			}

			const msPerColumn = state.durationMs / state.numColumns;
			let owed = Math.floor((now - state.lastColumnTime) / msPerColumn);
			if (owed > state.numColumns) owed = state.numColumns;
			if (owed > 0) {
				const min = state.accAny ? state.accMin : 0;
				const max = state.accAny ? state.accMax : 0;
				for (let i = 0; i < owed; i++) {
					const idx = state.writePos * 2;
					state.history[idx] = min;
					state.history[idx + 1] = max;
					state.writePos = (state.writePos + 1) % state.numColumns;
					if (state.filledColumns < state.numColumns) state.filledColumns++;
				}
				state.lastColumnTime += owed * msPerColumn;
				resetAccumulator(state);
			}

			updateDisplayRange(state);
			drawWaveform(state, state.canvas.width, state.canvas.height, true);
		});
	},

	destroy(element: HTMLElement): void {
		const state = stateMap.get(element);
		state?.resizeObserver?.disconnect();
		stateMap.delete(element);
	}
};

function hexToRgba(hex: string, alpha: number): string {
	const r = parseInt(hex.slice(1, 3), 16);
	const g = parseInt(hex.slice(3, 5), 16);
	const b = parseInt(hex.slice(5, 7), 16);
	return `rgba(${r}, ${g}, ${b}, ${alpha})`;
}

/**
 * Walk the visible history, find global min/max, smooth into displayMin/Max.
 * Same attack-snap / decay-slow behavior as the oscilloscope: expand
 * instantly on encountering bigger excursions, shrink slowly when they pass
 * out of the visible window. Floor at ±1.
 */
function updateDisplayRange(state: WaveformState): void {
	const filled = state.filledColumns;
	if (filled === 0) return;

	const cols = state.numColumns;
	const oldestIdx = filled < cols ? 0 : state.writePos;

	let obsMin = Infinity;
	let obsMax = -Infinity;
	for (let i = 0; i < filled; i++) {
		const histIdx = (oldestIdx + i) % cols;
		const min = state.history[histIdx * 2];
		const max = state.history[histIdx * 2 + 1];
		if (min < obsMin) obsMin = min;
		if (max > obsMax) obsMax = max;
	}
	if (!isFinite(obsMin) || !isFinite(obsMax)) return;

	const targetMax = Math.max(obsMax, 1);
	const targetMin = Math.min(obsMin, -1);
	state.displayMax =
		targetMax > state.displayMax ? targetMax : state.displayMax + (targetMax - state.displayMax) * SCALE_DECAY;
	state.displayMin =
		targetMin < state.displayMin ? targetMin : state.displayMin + (targetMin - state.displayMin) * SCALE_DECAY;
}

function drawWaveform(
	state: WaveformState,
	width: number,
	height: number,
	hasData: boolean
): void {
	const { ctx, canvas, displayMin, displayMax } = state;
	const style = getComputedStyle(canvas);
	const bgColor = style.getPropertyValue('--bg-secondary').trim() || '#1a1a1a';
	const gridColor = style.getPropertyValue('--border-muted').trim() || '#333';
	const vizColor = style.getPropertyValue('--accent-viz').trim() || '#4ade80';
	const mutedColor = style.getPropertyValue('--text-muted').trim() || '#444';
	const errorColor = style.getPropertyValue('--accent-error').trim() || '#f85149';

	ctx.fillStyle = bgColor;
	ctx.fillRect(0, 0, width, height);

	// Auto-scaled value→y mapping. The viewport is [displayMin, displayMax]
	// with a small inner margin so peaks don't touch the canvas edge.
	const margin = Math.max(2, height * 0.05);
	const usableH = height - 2 * margin;
	const range = Math.max(1e-9, displayMax - displayMin);
	const valueToY = (v: number) => margin + ((displayMax - v) / range) * usableH;

	const yZero = valueToY(0);
	const yPlusOne = valueToY(1);
	const yMinusOne = valueToY(-1);

	// Zero line.
	ctx.strokeStyle = gridColor;
	ctx.lineWidth = 1;
	ctx.beginPath();
	ctx.moveTo(0, yZero);
	ctx.lineTo(width, yZero);
	ctx.stroke();

	// ±1 unity boundary lines.
	ctx.setLineDash([4, 3]);
	ctx.beginPath();
	ctx.moveTo(0, yPlusOne);
	ctx.lineTo(width, yPlusOne);
	ctx.moveTo(0, yMinusOne);
	ctx.lineTo(width, yMinusOne);
	ctx.stroke();
	ctx.setLineDash([]);

	if (!hasData && state.filledColumns === 0) {
		ctx.fillStyle = mutedColor;
		ctx.font = '11px monospace';
		ctx.textAlign = 'center';
		ctx.fillText('No signal', width / 2, yZero + 4);
		return;
	}

	const cols = state.numColumns;
	const filled = state.filledColumns;
	const oldestIdx = filled < cols ? 0 : state.writePos;
	const fillStyle = hexToRgba(vizColor, 0.35);

	// Helper: clamp a y to canvas bounds.
	const clampY = (y: number) => Math.max(0, Math.min(height, y));

	for (let x = 0; x < width; x++) {
		if (filled === 0) continue;
		const t = x / Math.max(1, width - 1);
		const readOffset = Math.floor(t * (filled - 1));
		const histIdx = (oldestIdx + readOffset) % cols;
		const min = state.history[histIdx * 2];
		const max = state.history[histIdx * 2 + 1];

		// Empty/silent column — skip drawing the bar but the chrome (zero/±1
		// lines) is still in place from above.
		if (min === 0 && max === 0) continue;

		const yMax = valueToY(max); // top of bar
		const yMin = valueToY(min); // bottom of bar

		// Decompose the [yMax, yMin] vertical range into in-band and out-of-band
		// portions. In-band = [yPlusOne, yMinusOne]; above-band = [top..yPlusOne]
		// (sample > +1); below-band = [yMinusOne..bot] (sample < -1).
		const yTop = clampY(yMax);
		const yBot = clampY(yMin);

		// In-band portion.
		const inTop = Math.max(yTop, yPlusOne);
		const inBot = Math.min(yBot, yMinusOne);
		if (inBot > inTop) {
			ctx.fillStyle = state.filled ? fillStyle : vizColor;
			ctx.fillRect(x, inTop, 1, Math.max(1, inBot - inTop));
		}
		// In filled mode, also tip the min/max edges with full vizColor for a
		// crisper "outline" look.
		if (state.filled) {
			if (yMax >= yPlusOne && yMax <= yMinusOne) {
				ctx.fillStyle = vizColor;
				ctx.fillRect(x, yMax, 1, 1);
			}
			if (yMin >= yPlusOne && yMin <= yMinusOne) {
				ctx.fillStyle = vizColor;
				ctx.fillRect(x, Math.max(0, yMin - 1), 1, 1);
			}
		}

		// Above +1 (sample > +1 → y < yPlusOne).
		if (yMax < yPlusOne) {
			const top = clampY(yMax);
			const bot = Math.min(yPlusOne, clampY(yMin));
			if (bot > top) {
				ctx.fillStyle = errorColor;
				ctx.fillRect(x, top, 1, bot - top);
			}
		}
		// Below -1 (sample < -1 → y > yMinusOne).
		if (yMin > yMinusOne) {
			const top = Math.max(yMinusOne, clampY(yMax));
			const bot = clampY(yMin);
			if (bot > top) {
				ctx.fillStyle = errorColor;
				ctx.fillRect(x, top, 1, bot - top);
			}
		}
	}
}

registerRenderer(VizType.Waveform, waveformRenderer);

export { waveformRenderer };
