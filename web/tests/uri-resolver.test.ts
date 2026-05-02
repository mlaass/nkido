import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

import { uriResolver, extractScheme } from '../src/lib/io/uri-resolver';
import '../src/lib/io/handlers';
import { githubToHttps } from '../src/lib/io/handlers/github-handler';
import { registerBundledAsset } from '../src/lib/io/handlers/bundled-handler';
import { loadFile } from '../src/lib/io/file-loader';
import { FileLoadError } from '../src/lib/io/errors';

describe('extractScheme', () => {
	it('parses URI schemes', () => {
		expect(extractScheme('https://example.com/x')).toBe('https');
		expect(extractScheme('github:user/repo')).toBe('github');
		expect(extractScheme('bundled://default-808')).toBe('bundled');
		expect(extractScheme('blob:nkido:abc')).toBe('blob');
		expect(extractScheme('idb:foo')).toBe('idb');
	});

	it('treats bare paths as schemeless', () => {
		expect(extractScheme('/abs/file.wav')).toBe('');
		expect(extractScheme('./rel/file.wav')).toBe('');
		expect(extractScheme('../up.wav')).toBe('');
	});

	it('does not parse Windows drive letters as schemes', () => {
		expect(extractScheme('C:\\Windows\\file.wav')).toBe('');
		expect(extractScheme('C:/Windows/file.wav')).toBe('');
		expect(extractScheme('d:/dir')).toBe('');
	});

	it('returns empty for empty input', () => {
		expect(extractScheme('')).toBe('');
	});
});

describe('githubToHttps', () => {
	it('user/repo defaults to main + strudel.json', () => {
		expect(githubToHttps('github:tidalcycles/Dirt-Samples')).toBe(
			'https://raw.githubusercontent.com/tidalcycles/Dirt-Samples/main/strudel.json'
		);
	});

	it('user/repo/branch', () => {
		expect(githubToHttps('github:foo/bar/develop')).toBe(
			'https://raw.githubusercontent.com/foo/bar/develop/strudel.json'
		);
	});

	it('user/repo/branch/sub/dir', () => {
		expect(githubToHttps('github:foo/bar/main/sub/dir')).toBe(
			'https://raw.githubusercontent.com/foo/bar/main/sub/dir/strudel.json'
		);
	});

	it('audio extensions fetched as-is', () => {
		expect(githubToHttps('github:foo/bar/main/path/file.wav')).toBe(
			'https://raw.githubusercontent.com/foo/bar/main/path/file.wav'
		);
		expect(githubToHttps('github:foo/bar/main/x.sf2')).toBe(
			'https://raw.githubusercontent.com/foo/bar/main/x.sf2'
		);
	});

	it('rejects malformed input', () => {
		expect(githubToHttps('github:')).toBe('');
		expect(githubToHttps('github:onlyone')).toBe('');
		expect(githubToHttps('notgithub:foo/bar')).toBe('');
	});
});

describe('uriResolver', () => {
	let originalFetch: typeof fetch;

	beforeEach(() => {
		originalFetch = globalThis.fetch;
	});

	afterEach(() => {
		globalThis.fetch = originalFetch;
	});

	it('rejects empty URI', async () => {
		await expect(loadFile('')).rejects.toThrow(FileLoadError);
	});

	it('rejects unknown scheme', async () => {
		await expect(loadFile('definitely-not-a-real-scheme-xyz://foo')).rejects.toThrow(
			/no handler registered/
		);
	});

	it('routes bare path to file:// (which is unregistered on web)', async () => {
		// On the web, file:// has no handler — bare paths fail with the
		// same "no handler" error rather than being mistaken for some
		// other scheme.
		await expect(loadFile('/abs/path/file.wav')).rejects.toThrow(/no handler registered/);
	});

	it('blob lifecycle: register, load, unregister', async () => {
		const buf = new ArrayBuffer(8);
		new Uint8Array(buf).set([1, 2, 3, 4, 5, 6, 7, 8]);
		const uri = uriResolver.registerBlob(buf);
		expect(uri.startsWith('blob:nkido:')).toBe(true);

		const result = await loadFile(uri);
		expect(result.data.byteLength).toBe(8);
		expect(new Uint8Array(result.data)[0]).toBe(1);

		uriResolver.unregisterBlob(uri);
		await expect(loadFile(uri)).rejects.toThrow(/blob not registered/);
	});

	it('blob handler unwraps File objects', async () => {
		const file = new File([new Uint8Array([0xa, 0xb, 0xc])], 'kit.wav');
		const uri = uriResolver.registerBlob(file);
		try {
			const result = await loadFile(uri);
			expect(result.name).toBe('kit.wav');
			expect(result.data.byteLength).toBe(3);
		} finally {
			uriResolver.unregisterBlob(uri);
		}
	});

	it('bundled handler returns registered asset', async () => {
		const bytes = new Uint8Array([42, 43, 44]).buffer;
		registerBundledAsset('test-kit', bytes);
		const result = await loadFile('bundled://test-kit');
		expect(result.data.byteLength).toBe(3);
	});

	it('bundled handler returns NotFound for missing name', async () => {
		await expect(loadFile('bundled://does-not-exist')).rejects.toThrow(/bundled asset not found/);
	});

	it('https handler dispatches via fetch', async () => {
		const fakeBody = new Uint8Array([1, 2, 3, 4]);
		globalThis.fetch = vi.fn().mockResolvedValue(
			new Response(fakeBody, {
				status: 200,
				headers: { 'Content-Type': 'application/octet-stream' }
			})
		);

		const result = await loadFile('https://example.com/sample.wav');
		expect(result.data.byteLength).toBe(4);
		expect(result.name).toBe('sample.wav');
		expect(globalThis.fetch).toHaveBeenCalledTimes(1);
	});

	it('github handler recurses into https with transformed URL', async () => {
		const fakeBody = new Uint8Array([5, 6, 7]);
		const fetchSpy = vi.fn().mockResolvedValue(
			new Response(fakeBody, { status: 200 })
		);
		globalThis.fetch = fetchSpy;

		const result = await loadFile('github:foo/bar/main/path/file.wav');
		expect(result.data.byteLength).toBe(3);
		expect(fetchSpy).toHaveBeenCalledTimes(1);
		const fetchedUrl = fetchSpy.mock.calls[0][0];
		expect(fetchedUrl).toBe(
			'https://raw.githubusercontent.com/foo/bar/main/path/file.wav'
		);
	});

	it('https handler 404 throws not_found', async () => {
		globalThis.fetch = vi.fn().mockResolvedValue(
			new Response(null, { status: 404, statusText: 'Not Found' })
		);
		await expect(loadFile('https://example.com/missing.wav')).rejects.toMatchObject({
			code: 'not_found'
		});
	});
});
