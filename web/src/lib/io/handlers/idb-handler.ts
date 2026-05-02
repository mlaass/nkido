/**
 * idb: scheme handler. Direct read from the IndexedDB cache by key
 * (form: `idb:<key>`). Used by future flows that want to round-trip
 * bytes through the cache without re-fetching.
 */
import { FileLoadError } from '../errors';
import { fileCache } from '../file-cache';
import type { LoadOptions, LoadResult } from '../file-loader';
import type { UriHandler } from '../uri-resolver';

export const idbHandler: UriHandler = {
	scheme: 'idb',
	async load(uri: string, _options: LoadOptions): Promise<LoadResult> {
		const key = uri.startsWith('idb:') ? uri.slice('idb:'.length) : uri;
		const data = await fileCache.get(key);
		if (!data) {
			throw new FileLoadError('not_found', `IDB key not in cache: '${key}'`);
		}
		return { data, name: key };
	}
};
