/**
 * Gradient Presets for Waterfall Visualization
 *
 * Each preset is a 256-entry RGBA lookup table (1024 bytes)
 * for direct ImageData pixel writes.
 */

export type GradientLUT = Uint8ClampedArray; // length: 1024 (256 * 4 RGBA)

interface ColorStop {
	pos: number; // 0..1
	r: number;
	g: number;
	b: number;
}

function generateGradient(stops: ColorStop[]): GradientLUT {
	const lut = new Uint8ClampedArray(256 * 4);

	for (let i = 0; i < 256; i++) {
		const t = i / 255;

		// Find surrounding stops
		let lo = stops[0];
		let hi = stops[stops.length - 1];
		for (let s = 0; s < stops.length - 1; s++) {
			if (t >= stops[s].pos && t <= stops[s + 1].pos) {
				lo = stops[s];
				hi = stops[s + 1];
				break;
			}
		}

		// Interpolate
		const range = hi.pos - lo.pos;
		const f = range > 0 ? (t - lo.pos) / range : 0;
		const offset = i * 4;
		lut[offset] = Math.round(lo.r + (hi.r - lo.r) * f);
		lut[offset + 1] = Math.round(lo.g + (hi.g - lo.g) * f);
		lut[offset + 2] = Math.round(lo.b + (hi.b - lo.b) * f);
		lut[offset + 3] = 255; // Alpha
	}

	return lut;
}

// Color stops derived from standard matplotlib/d3 colormap definitions
export const GRADIENT_PRESETS: Record<string, GradientLUT> = {
	magma: generateGradient([
		{ pos: 0.0, r: 0, g: 0, b: 4 },
		{ pos: 0.2, r: 43, g: 10, b: 73 },
		{ pos: 0.4, r: 120, g: 28, b: 109 },
		{ pos: 0.6, r: 196, g: 63, b: 79 },
		{ pos: 0.8, r: 249, g: 142, b: 47 },
		{ pos: 1.0, r: 252, g: 253, b: 191 }
	]),
	viridis: generateGradient([
		{ pos: 0.0, r: 68, g: 1, b: 84 },
		{ pos: 0.25, r: 59, g: 82, b: 139 },
		{ pos: 0.5, r: 33, g: 145, b: 140 },
		{ pos: 0.75, r: 94, g: 201, b: 98 },
		{ pos: 1.0, r: 253, g: 231, b: 37 }
	]),
	inferno: generateGradient([
		{ pos: 0.0, r: 0, g: 0, b: 4 },
		{ pos: 0.2, r: 40, g: 11, b: 84 },
		{ pos: 0.4, r: 120, g: 28, b: 109 },
		{ pos: 0.6, r: 197, g: 57, b: 51 },
		{ pos: 0.8, r: 246, g: 158, b: 12 },
		{ pos: 1.0, r: 252, g: 255, b: 164 }
	]),
	thermal: generateGradient([
		{ pos: 0.0, r: 0, g: 0, b: 0 },
		{ pos: 0.25, r: 0, g: 0, b: 180 },
		{ pos: 0.5, r: 180, g: 0, b: 0 },
		{ pos: 0.75, r: 255, g: 180, b: 0 },
		{ pos: 1.0, r: 255, g: 255, b: 255 }
	]),
	grayscale: generateGradient([
		{ pos: 0.0, r: 0, g: 0, b: 0 },
		{ pos: 1.0, r: 255, g: 255, b: 255 }
	])
};

export const DEFAULT_GRADIENT = 'magma';
