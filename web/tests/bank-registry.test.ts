import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

// audio.svelte uses Svelte 5 runes which the vitest config does not preprocess;
// stub the module so importing bank-registry does not pull it in.
vi.mock('$lib/stores/audio.svelte', () => ({
	audioEngine: {
		loadAsset: vi.fn().mockResolvedValue(true)
	}
}));

import { bankRegistry } from '../src/lib/audio/bank-registry';
import '../src/lib/io/handlers';

describe('bankRegistry.loadBank double-fetch regression', () => {
	let originalFetch: typeof fetch;

	beforeEach(() => {
		bankRegistry.clearAll();
		originalFetch = globalThis.fetch;
	});

	afterEach(() => {
		globalThis.fetch = originalFetch;
	});

	it('loadBank("github:user/repo") performs exactly one network fetch for the manifest', async () => {
		const manifest = {
			_name: 'test-bank',
			_base: 'https://example.com/samples/',
			bd: ['bd_0.wav', 'bd_1.wav'],
			cp: 'cp.wav'
		};
		const fetchSpy = vi.fn().mockResolvedValue(
			new Response(JSON.stringify(manifest), {
				status: 200,
				headers: { 'Content-Type': 'application/json' }
			})
		);
		globalThis.fetch = fetchSpy;

		const result = await bankRegistry.loadBank('github:tidalcycles/Dirt-Samples');

		expect(fetchSpy).toHaveBeenCalledTimes(1);
		const fetchedUrl = fetchSpy.mock.calls[0][0];
		expect(fetchedUrl).toBe(
			'https://raw.githubusercontent.com/tidalcycles/Dirt-Samples/main/strudel.json'
		);
		expect(result.name).toBe('test-bank');
		expect(result.samples.size).toBe(2);
	});

	it('loadBank("https://...") performs exactly one network fetch', async () => {
		const manifest = { _name: 'http-bank', kick: 'kick.wav' };
		const fetchSpy = vi.fn().mockResolvedValue(
			new Response(JSON.stringify(manifest), { status: 200 })
		);
		globalThis.fetch = fetchSpy;

		await bankRegistry.loadBank('https://example.com/path/strudel.json');

		expect(fetchSpy).toHaveBeenCalledTimes(1);
	});

	it('derives baseUrl from github URI when manifest omits _base', async () => {
		const manifest = { bd: 'bd.wav' };
		globalThis.fetch = vi.fn().mockResolvedValue(
			new Response(JSON.stringify(manifest), { status: 200 })
		);

		const result = await bankRegistry.loadBank('github:foo/bar/main/sub');
		expect(result.baseUrl).toBe(
			'https://raw.githubusercontent.com/foo/bar/main/sub/'
		);
	});
});
