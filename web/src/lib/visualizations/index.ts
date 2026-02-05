/**
 * Visualizations Index
 *
 * Import this file to register all visualization renderers.
 */

// Registry and types
export { registerRenderer, getRenderer, hasRenderer, type VisualizationRenderer } from './registry';

// Register all built-in renderers by importing them
import './pianoroll';
import './oscilloscope';
import './waveform';
import './spectrum';
