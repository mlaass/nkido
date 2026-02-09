/**
 * Unified file loader for audio files
 *
 * Supports loading from URLs, File objects, and raw ArrayBuffers
 * with optional IndexedDB caching for URL sources.
 */

import { FileLoadError } from './errors';
import { fileCache, cacheKeyFromUrl, cacheKeyFromFile } from './file-cache';

export type FileSource =
	| { type: 'url'; url: string }
	| { type: 'file'; file: File }
	| { type: 'arraybuffer'; data: ArrayBuffer; name: string };

export interface LoadOptions {
	onProgress?: (loaded: number, total: number) => void;
	signal?: AbortSignal;
	cache?: boolean;
}

export interface LoadResult {
	data: ArrayBuffer;
	name: string;
}

export async function loadFile(source: FileSource, options?: LoadOptions): Promise<LoadResult> {
	const { signal, cache = false, onProgress } = options ?? {};

	if (signal?.aborted) {
		throw new FileLoadError('aborted', 'Load was aborted');
	}

	switch (source.type) {
		case 'url':
			return loadFromUrl(source.url, { signal, cache, onProgress });

		case 'file':
			return loadFromFile(source.file, { signal, cache });

		case 'arraybuffer':
			return { data: source.data, name: source.name };
	}
}

async function loadFromUrl(
	url: string,
	options: { signal?: AbortSignal; cache?: boolean; onProgress?: LoadOptions['onProgress'] }
): Promise<LoadResult> {
	const { signal, cache, onProgress } = options;

	// Check cache first
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

	// Use streaming reader for progress if Content-Length is available
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

		// Concatenate chunks
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

	// Store in cache
	if (cache) {
		fileCache.set(cacheKeyFromUrl(url), data).catch(() => {
			// Cache write failure is non-fatal
		});
	}

	return { data, name: nameFromUrl(url) };
}

async function loadFromFile(
	file: File,
	options: { signal?: AbortSignal; cache?: boolean }
): Promise<LoadResult> {
	const { signal, cache } = options;

	if (signal?.aborted) {
		throw new FileLoadError('aborted', 'Load was aborted');
	}

	// Check cache first
	if (cache) {
		const cached = await fileCache.get(cacheKeyFromFile(file));
		if (cached) {
			return { data: cached, name: file.name };
		}
	}

	const data = await file.arrayBuffer();

	// Store in cache
	if (cache) {
		fileCache.set(cacheKeyFromFile(file), data).catch(() => {
			// Cache write failure is non-fatal
		});
	}

	return { data, name: file.name };
}

function nameFromUrl(url: string): string {
	try {
		const pathname = new URL(url, 'https://localhost').pathname;
		const segments = pathname.split('/');
		return segments[segments.length - 1] || 'unknown';
	} catch {
		return 'unknown';
	}
}
