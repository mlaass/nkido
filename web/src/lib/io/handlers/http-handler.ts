/**
 * http:// and https:// scheme handler. Wraps `fetch()` with optional
 * IndexedDB caching via the existing FileCache.
 */
import { FileLoadError } from '../errors';
import { fileCache, cacheKeyFromUrl } from '../file-cache';
import type { LoadOptions, LoadResult } from '../file-loader';
import type { UriHandler } from '../uri-resolver';

function nameFromUrl(url: string): string {
	try {
		const pathname = new URL(url, 'https://localhost').pathname;
		const segments = pathname.split('/');
		return segments[segments.length - 1] || 'unknown';
	} catch {
		return 'unknown';
	}
}

async function loadFromUrl(url: string, options: LoadOptions): Promise<LoadResult> {
	const { signal, cache, onProgress } = options;

	if (cache) {
		const cached = await fileCache.get(cacheKeyFromUrl(url));
		if (cached) {
			return { data: cached, name: nameFromUrl(url) };
		}
	}

	const response = await fetch(url, { signal }).catch((err) => {
		if (err.name === 'AbortError') {
			throw new FileLoadError('aborted', 'Fetch aborted');
		}
		throw new FileLoadError('network', `Network error: ${err.message}`);
	});

	if (!response.ok) {
		if (response.status === 404) {
			throw new FileLoadError('not_found', `Not found: ${url}`);
		}
		throw new FileLoadError('network', `HTTP ${response.status}: ${response.statusText}`);
	}

	let data: ArrayBuffer;

	const contentLength = response.headers.get('Content-Length');
	if (onProgress && contentLength && response.body) {
		const total = parseInt(contentLength, 10);
		let loaded = 0;
		const reader = response.body.getReader();
		const chunks: Uint8Array[] = [];

		// eslint-disable-next-line no-constant-condition
		while (true) {
			const { done, value } = await reader.read();
			if (done) break;
			chunks.push(value);
			loaded += value.length;
			onProgress(loaded, total);
		}

		const combined = new Uint8Array(loaded);
		let offset = 0;
		for (const chunk of chunks) {
			combined.set(chunk, offset);
			offset += chunk.length;
		}
		data = combined.buffer;
	} else {
		data = await response.arrayBuffer();
	}

	if (cache) {
		fileCache.set(cacheKeyFromUrl(url), data).catch(() => {
			// Cache write failure is non-fatal
		});
	}

	return { data, name: nameFromUrl(url) };
}

export const httpHandler: UriHandler = {
	scheme: 'http',
	load: loadFromUrl
};

export const httpsHandler: UriHandler = {
	scheme: 'https',
	load: loadFromUrl
};
