/**
 * Scheme-keyed URI resolver for asset loading on the web.
 *
 * Mirrors the C++ `cedar::UriResolver`. Handlers register for a scheme
 * ("https", "github", "bundled", "blob", "idb"); `load(uri)` parses the
 * scheme and dispatches.
 *
 * The native side (cedar) handles file://, http://, https://, github:,
 * bundled:// itself. On the web, http/https/github fetch via the browser;
 * file:// is unavailable; bundled:// looks up a build-time table.
 */
import { FileLoadError } from './errors';
import type { LoadOptions, LoadResult } from './file-loader';

export interface UriHandler {
	/** Lowercase scheme identifier without trailing punctuation. */
	scheme: string;
	load(uri: string, options: LoadOptions): Promise<LoadResult>;
}

const BLOB_PREFIX = 'blob:nkido:';

function extractScheme(uri: string): string {
	if (!uri) return '';
	// Bare paths and Windows drive letters are not URIs.
	if (uri.startsWith('/') || uri.startsWith('.')) return '';
	if (/^[a-zA-Z]:[\\/]/.test(uri)) return '';
	const colon = uri.indexOf(':');
	if (colon <= 0) return '';
	return uri.slice(0, colon).toLowerCase();
}

class UriResolver {
	private handlers = new Map<string, UriHandler>();
	private blobs = new Map<string, File | ArrayBuffer>();
	private inFlight = new Map<string, Promise<LoadResult>>();

	register(handler: UriHandler): void {
		this.handlers.set(handler.scheme.toLowerCase(), handler);
	}

	handlerFor(scheme: string): UriHandler | undefined {
		return this.handlers.get(scheme.toLowerCase());
	}

	async load(uri: string, options: LoadOptions = {}): Promise<LoadResult> {
		if (!uri) {
			throw new FileLoadError('invalid_format', 'empty URI');
		}

		// Concurrent loads of the same URI share the same network request.
		// Skipped when caller passes onProgress (progress reports would
		// interleave between callers in a confusing way).
		const dedupKey = options.signal || options.onProgress ? null : uri;
		if (dedupKey) {
			const existing = this.inFlight.get(dedupKey);
			if (existing) return existing;
		}

		const scheme = extractScheme(uri) || 'file';
		const handler = this.handlerFor(scheme);
		if (!handler) {
			throw new FileLoadError(
				'invalid_format',
				`no handler registered for scheme '${scheme}'`
			);
		}

		const promise = handler.load(uri, options).finally(() => {
			if (dedupKey) this.inFlight.delete(dedupKey);
		});
		if (dedupKey) this.inFlight.set(dedupKey, promise);
		return promise;
	}

	/**
	 * Register a transient File or ArrayBuffer under a synthetic
	 * `blob:nkido:<uuid>` URI. Returns the URI. Caller MUST call
	 * `unregisterBlob` (e.g. in a try/finally) to free the reference.
	 */
	registerBlob(source: File | ArrayBuffer): string {
		const uuid =
			typeof crypto !== 'undefined' && 'randomUUID' in crypto
				? crypto.randomUUID()
				: Math.random().toString(36).slice(2) + Date.now().toString(36);
		const uri = BLOB_PREFIX + uuid;
		this.blobs.set(uri, source);
		return uri;
	}

	unregisterBlob(uri: string): void {
		this.blobs.delete(uri);
	}

	/** Internal lookup used by the blob handler. */
	resolveBlob(uri: string): File | ArrayBuffer | undefined {
		return this.blobs.get(uri);
	}
}

export const uriResolver = new UriResolver();

export { extractScheme };
