/**
 * blob: scheme handler. Resolves `blob:nkido:<uuid>` URIs registered via
 * `uriResolver.registerBlob(file | arraybuffer)` to in-memory bytes.
 *
 * Caller MUST call `uriResolver.unregisterBlob(uri)` after use; we keep
 * a strong reference until then. Typical usage is a try/finally around
 * the load.
 */
import { FileLoadError } from '../errors';
import type { LoadOptions, LoadResult } from '../file-loader';
import { uriResolver, type UriHandler } from '../uri-resolver';

export const blobHandler: UriHandler = {
	scheme: 'blob',
	async load(uri: string, _options: LoadOptions): Promise<LoadResult> {
		// blob:nkido:<uuid> only — leave native browser blob: URLs (object
		// URLs) unhandled so we don't accidentally claim them.
		if (!uri.startsWith('blob:nkido:')) {
			throw new FileLoadError('not_found', `unrecognized blob URI: ${uri}`);
		}
		const source = uriResolver.resolveBlob(uri);
		if (!source) {
			throw new FileLoadError('not_found', `blob not registered: ${uri}`);
		}
		if (source instanceof ArrayBuffer) {
			return { data: source, name: uri };
		}
		const data = await source.arrayBuffer();
		return { data, name: source.name };
	}
};
