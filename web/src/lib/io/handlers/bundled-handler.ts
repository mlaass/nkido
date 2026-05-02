/**
 * bundled:// scheme handler. Looks up assets in a build-time table.
 *
 * The table is empty in v1 — the slot exists so akkado source like
 * `samples("bundled://default-808")` can compile, and a future build
 * step (or host registration) can populate it without changing the
 * resolver dispatch.
 */
import { FileLoadError } from '../errors';
import type { LoadOptions, LoadResult } from '../file-loader';
import type { UriHandler } from '../uri-resolver';

const BUNDLED: Map<string, ArrayBuffer> = new Map();

function strip(uri: string): string {
	if (uri.startsWith('bundled://')) return uri.slice('bundled://'.length);
	if (uri.startsWith('bundled:')) return uri.slice('bundled:'.length);
	return uri;
}

export function registerBundledAsset(name: string, bytes: ArrayBuffer): void {
	BUNDLED.set(name, bytes);
}

export const bundledHandler: UriHandler = {
	scheme: 'bundled',
	async load(uri: string, _options: LoadOptions): Promise<LoadResult> {
		const name = strip(uri);
		const data = BUNDLED.get(name);
		if (!data) {
			throw new FileLoadError('not_found', `bundled asset not found: '${name}'`);
		}
		return { data, name };
	}
};
