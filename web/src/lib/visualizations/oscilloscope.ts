/**
 * Oscilloscope Visualization Renderer
 *
 * Displays a time-domain waveform view of a signal.
 * Shows a short rolling window (~21ms at 48kHz = 1024 samples).
 *
 * The y-axis auto-scales to fit the actual signal range with a floor of ±1.
 * Audio in the [-1, +1] range fills the canvas with the unity boundaries
 * pinned at ~80% of the half-height. LFOs and control signals that extend
 * beyond ±1 expand the scale so the whole curve is visible. Asymmetric
 * signals (e.g. a 0..5 unipolar LFO) are rendered with the corresponding
 * asymmetric viewport — the ±1 boundary lines stay visible so you can still
 * see where the audio-clip threshold lies relative to the signal.
 *
 * Out-of-range portions (|signal| > 1) overdraw in red.
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

	// Smoothed display bounds. Initialized to ±1; expand instantly to fit
	// observed signal, contract slowly so brief peaks don't keep the scale
	// permanently zoomed out.
	displayMin: number;
	displayMax: number;
}

const stateMap = new WeakMap<HTMLElement, OscilloscopeState>();

const DEFAULT_WIDTH = 200;
const DEFAULT_HEIGHT = 50;
const LABEL_HEIGHT = 18;

// Decay factor per frame at ~30fps. Higher = quicker recovery to a smaller
// signal range. 0.03 gives ~63% recovery over ~30 frames (~1s).
const SCALE_DECAY = 0.03;

const oscilloscopeRenderer: VisualizationRenderer = {
	create(viz: VizDecl): HTMLElement {
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

		const state: OscilloscopeState = {
			canvas,
			ctx,
			animationId: null,
			lastUpdateTime: 0,
			triggerLevel,
			triggerEdge,
			resizeObserver: null,
			displayMin: -1,
			displayMax: 1
		};

		drawOscilloscope(state, canvas.width, canvas.height, null);

		if (isRelativeWidth || isRelativeHeight) {
			state.resizeObserver = new ResizeObserver(entries => {
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
			drawOscilloscope(state, state.canvas.width, state.canvas.height, null);
			return;
		}

		audioEngine.getProbeData(viz.stateId).then(samples => {
			if (samples) {
				updateDisplayRange(state, samples);
				drawOscilloscope(state, state.canvas.width, state.canvas.height, samples);
			}
		});
	},

	destroy(element: HTMLElement): void {
		const state = stateMap.get(element);
		if (state?.animationId) cancelAnimationFrame(state.animationId);
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
 * Observe the sample buffer's min/max and update the display range.
 * Snap up immediately on expansion (so fast peaks don't get clipped on first
 * frame), decay slowly toward the smaller target on contraction. The display
 * range is floored at ±1 so the unity reference lines are always on-canvas.
 */
function updateDisplayRange(state: OscilloscopeState, samples: Float32Array): void {
	if (samples.length === 0) return;
	let obsMin = Infinity;
	let obsMax = -Infinity;
	for (let i = 0; i < samples.length; i++) {
		const s = samples[i];
		if (s < obsMin) obsMin = s;
		if (s > obsMax) obsMax = s;
	}
	const targetMax = Math.max(obsMax, 1);
	const targetMin = Math.min(obsMin, -1);
	state.displayMax =
		targetMax > state.displayMax ? targetMax : state.displayMax + (targetMax - state.displayMax) * SCALE_DECAY;
	state.displayMin =
		targetMin < state.displayMin ? targetMin : state.displayMin + (targetMin - state.displayMin) * SCALE_DECAY;
}

function drawOscilloscope(
	state: OscilloscopeState,
	width: number,
	height: number,
	samples: Float32Array | null
): void {
	const { ctx, canvas, triggerLevel, triggerEdge, displayMin, displayMax } = state;

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

	// ±1 unity boundary lines (dashed). With the ±1 floor on the display
	// range these always lie within the canvas.
	ctx.setLineDash([4, 3]);
	ctx.beginPath();
	ctx.moveTo(0, yPlusOne);
	ctx.lineTo(width, yPlusOne);
	ctx.moveTo(0, yMinusOne);
	ctx.lineTo(width, yMinusOne);
	ctx.stroke();
	ctx.setLineDash([]);

	if (!samples || samples.length === 0) {
		ctx.fillStyle = mutedColor;
		ctx.font = '11px monospace';
		ctx.textAlign = 'center';
		ctx.fillText('No signal', width / 2, yZero + 4);
		return;
	}

	const triggerOffset = findTriggerPoint(samples, triggerLevel, triggerEdge);
	const visibleSamples = samples.subarray(triggerOffset);

	// Pre-compute y per column for two-pass rendering.
	const ys = new Float32Array(width);
	const step = visibleSamples.length / width;
	for (let x = 0; x < width; x++) {
		const sample = visibleSamples[Math.floor(x * step)] ?? 0;
		ys[x] = valueToY(sample);
	}

	ctx.lineCap = 'round';
	ctx.lineJoin = 'round';

	// Pass 1: full waveform in vizColor.
	ctx.strokeStyle = vizColor;
	ctx.lineWidth = 1.5;
	ctx.beginPath();
	ctx.moveTo(0, ys[0]);
	for (let x = 1; x < width; x++) ctx.lineTo(x, ys[x]);
	ctx.stroke();

	// Pass 2: overdraw any segment whose endpoints lie outside the ±1 band
	// in red. The "out-of-range" test is in y-space against the ±1 lines —
	// y < yPlusOne means above +1 (since y increases downward), y > yMinusOne
	// means below -1.
	ctx.strokeStyle = errorColor;
	ctx.lineWidth = 1.75;
	let inClip = false;
	for (let x = 1; x < width; x++) {
		const y0 = ys[x - 1];
		const y1 = ys[x];
		const clipped = y0 < yPlusOne || y0 > yMinusOne || y1 < yPlusOne || y1 > yMinusOne;
		if (clipped) {
			if (!inClip) {
				ctx.beginPath();
				ctx.moveTo(x - 1, y0);
				inClip = true;
			}
			ctx.lineTo(x, y1);
		} else if (inClip) {
			ctx.stroke();
			inClip = false;
		}
	}
	if (inClip) ctx.stroke();

	// Trigger level line when non-zero. Drawn at its actual value within the
	// auto-scaled viewport.
	if (triggerLevel !== 0) {
		const triggerY = valueToY(triggerLevel);
		if (triggerY >= 0 && triggerY <= height) {
			ctx.strokeStyle = mutedColor;
			ctx.lineWidth = 1;
			ctx.setLineDash([2, 4]);
			ctx.beginPath();
			ctx.moveTo(0, triggerY);
			ctx.lineTo(width, triggerY);
			ctx.stroke();
			ctx.setLineDash([]);
		}
	}
}

registerRenderer(VizType.Oscilloscope, oscilloscopeRenderer);

export { oscilloscopeRenderer };
