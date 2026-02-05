/**
 * Visualization Registry
 *
 * Central registry for visualization renderers.
 * Each visualization type (piano roll, oscilloscope, etc.) registers
 * a renderer that knows how to create and update its canvas.
 */

import type { VizDecl, VizType } from '$lib/stores/audio.svelte';

/**
 * Interface for visualization renderers
 */
export interface VisualizationRenderer {
	/**
	 * Create the DOM element for this visualization
	 * @param viz The visualization declaration from the compiler
	 * @returns The root DOM element (typically a canvas wrapper)
	 */
	create(viz: VizDecl): HTMLElement;

	/**
	 * Update/redraw the visualization
	 * Called on each animation frame when playing
	 * @param element The root element created by create()
	 * @param viz The visualization declaration
	 * @param beatPos Current beat position (for playhead)
	 * @param isPlaying Whether audio is playing
	 */
	update(element: HTMLElement, viz: VizDecl, beatPos: number, isPlaying: boolean): void;

	/**
	 * Clean up resources (cancel animation frames, etc.)
	 * @param element The root element
	 */
	destroy(element: HTMLElement): void;
}

/**
 * Registry of visualization renderers by type
 */
const renderers = new Map<VizType, VisualizationRenderer>();

/**
 * Register a renderer for a visualization type
 */
export function registerRenderer(type: VizType, renderer: VisualizationRenderer): void {
	renderers.set(type, renderer);
}

/**
 * Get the renderer for a visualization type
 */
export function getRenderer(type: VizType): VisualizationRenderer | undefined {
	return renderers.get(type);
}

/**
 * Check if a renderer is registered for a type
 */
export function hasRenderer(type: VizType): boolean {
	return renderers.has(type);
}
