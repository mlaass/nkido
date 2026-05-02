/**
 * URI-keyed asset loader. All file loading goes through the singleton
 * `uriResolver`; this module is a thin wrapper that ensures the default
 * handlers are registered and exports the public types.
 *
 * Schemes available: http, https, github, bundled, blob, idb.
 *
 * For File / ArrayBuffer inputs, register a transient blob URI:
 *   const uri = uriResolver.registerBlob(file);
 *   try {
 *     const result = await loadFile(uri);
 *     // ... use result.data ...
 *   } finally {
 *     uriResolver.unregisterBlob(uri);
 *   }
 */
import { FileLoadError } from './errors';
import { uriResolver } from './uri-resolver';
import './handlers';  // side effect: registers default handlers

export interface LoadOptions {
	onProgress?: (loaded: number, total: number) => void;
	signal?: AbortSignal;
	cache?: boolean;
}

export interface LoadResult {
	data: ArrayBuffer;
	name: string;
}

export async function loadFile(uri: string, options?: LoadOptions): Promise<LoadResult> {
	const opts = options ?? {};
	if (opts.signal?.aborted) {
		throw new FileLoadError('aborted', 'Load was aborted');
	}
	return uriResolver.load(uri, opts);
}
