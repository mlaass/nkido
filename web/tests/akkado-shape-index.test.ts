/**
 * Tests for the pure-JS helpers in `akkado-shape-index.ts`.
 *
 * The WASM-side serializer is exercised by `akkado/tests/test_shape_index.cpp`
 * (Catch2 tag `[shape-index]`); no need to duplicate that surface here.
 */

import { describe, it, expect } from 'vitest';
import { fnv1a32, utf8ByteOffset } from '$lib/editor/akkado-shape-index';

describe('fnv1a32', () => {
	it('matches the C++ side for an empty string (FNV offset basis)', () => {
		// 2166136261 is the 32-bit FNV-1a offset basis. C++ side seeds with
		// the same value and never iterates for empty input.
		expect(fnv1a32('')).toBe(2166136261);
	});

	it('produces a stable 32-bit hash for ASCII input', () => {
		const a = fnv1a32('hello');
		const b = fnv1a32('hello');
		expect(a).toBe(b);
		expect(a).toBeGreaterThanOrEqual(0);
		expect(a).toBeLessThan(2 ** 32);
	});

	it('distinguishes inputs that differ by a single byte', () => {
		expect(fnv1a32('cfg')).not.toBe(fnv1a32('cfh'));
		expect(fnv1a32('a')).not.toBe(fnv1a32('A'));
	});

	it('hashes UTF-8 multibyte input by bytes, not codepoints', () => {
		// A multibyte character must produce a different hash from any
		// single ASCII char — confirms we're encoding to UTF-8 first.
		const ascii = fnv1a32('a');
		const multibyte = fnv1a32('é');
		expect(multibyte).not.toBe(ascii);
	});
});

describe('utf8ByteOffset', () => {
	it('returns 0 for charPos at start', () => {
		expect(utf8ByteOffset('hello', 0)).toBe(0);
	});

	it('matches character index for ASCII text', () => {
		const s = 'cfg = {x: 1}';
		expect(utf8ByteOffset(s, 6)).toBe(6); // before the `{`
		expect(utf8ByteOffset(s, s.length)).toBe(s.length);
	});

	it('counts 2 bytes for U+00E9 (é)', () => {
		// `é` is encoded as 2 UTF-8 bytes (0xC3 0xA9). After it the byte
		// offset is 2 even though the character index is 1.
		const s = 'éx';
		expect(utf8ByteOffset(s, 0)).toBe(0);
		expect(utf8ByteOffset(s, 1)).toBe(2);
		expect(utf8ByteOffset(s, 2)).toBe(3);
	});

	it('clamps gracefully for out-of-range positions', () => {
		const s = 'abc';
		expect(utf8ByteOffset(s, -5)).toBe(0);
		expect(utf8ByteOffset(s, 999)).toBe(3);
	});
});
