import { describe, it, expect, vi, afterEach } from 'vitest';
import { pathToFetchUri } from '../src/lib/io/path-to-uri';

describe('pathToFetchUri', () => {
	afterEach(() => {
		vi.unstubAllGlobals();
	});

	describe('schemed inputs pass through unchanged', () => {
		it('http://', () => {
			expect(pathToFetchUri('http://example.com/x.wav')).toBe('http://example.com/x.wav');
		});
		it('https://', () => {
			expect(pathToFetchUri('https://cdn.example.com/x.wav')).toBe(
				'https://cdn.example.com/x.wav'
			);
		});
		it('github:', () => {
			expect(pathToFetchUri('github:user/repo')).toBe('github:user/repo');
		});
		it('bundled://', () => {
			expect(pathToFetchUri('bundled://default-808')).toBe('bundled://default-808');
		});
		it('blob:nkido:', () => {
			expect(pathToFetchUri('blob:nkido:abc-123')).toBe('blob:nkido:abc-123');
		});
		it('idb:', () => {
			expect(pathToFetchUri('idb:my-blob')).toBe('idb:my-blob');
		});
	});

	describe('bare paths resolve against window.location.origin', () => {
		it('absolute path with leading slash', () => {
			vi.stubGlobal('window', { location: { origin: 'http://localhost:5173' } });
			expect(pathToFetchUri('/wavetables/sine_to_saw.wav')).toBe(
				'http://localhost:5173/wavetables/sine_to_saw.wav'
			);
		});
		it('relative path without leading slash', () => {
			vi.stubGlobal('window', { location: { origin: 'http://localhost:5173' } });
			expect(pathToFetchUri('wavetables/sine_to_saw.wav')).toBe(
				'http://localhost:5173/wavetables/sine_to_saw.wav'
			);
		});
		it('honors current origin (e.g. deployed host)', () => {
			vi.stubGlobal('window', { location: { origin: 'https://nkido.cc' } });
			expect(pathToFetchUri('samples/kick.wav')).toBe('https://nkido.cc/samples/kick.wav');
		});
	});

	describe('SSR / no window', () => {
		it('returns the path unchanged when window is undefined', () => {
			vi.stubGlobal('window', undefined);
			expect(pathToFetchUri('/wavetables/x.wav')).toBe('/wavetables/x.wav');
		});
	});

	describe('does not misclassify Windows-like inputs', () => {
		it('treats unknown short prefixes as paths only when they lack a scheme', () => {
			vi.stubGlobal('window', { location: { origin: 'http://localhost' } });
			// "samples/..." has no colon, so it is a bare path.
			expect(pathToFetchUri('samples/kick.wav')).toBe('http://localhost/samples/kick.wav');
		});
	});
});
