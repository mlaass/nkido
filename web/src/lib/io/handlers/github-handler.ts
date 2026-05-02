/**
 * github: scheme handler. Transforms `github:user/repo[/branch][/path]`
 * into a https://raw.githubusercontent.com/... URL and recurses through
 * the resolver. Mirrors `cedar::GithubHandler` byte-for-byte.
 */
import { FileLoadError } from '../errors';
import type { LoadOptions, LoadResult } from '../file-loader';
import { uriResolver, type UriHandler } from '../uri-resolver';

const AUDIO_EXTENSIONS = ['.wav', '.ogg', '.flac', '.mp3', '.aiff', '.sf2', '.sf3', '.json'];

function looksLikeFile(path: string): boolean {
	const lower = path.toLowerCase();
	return AUDIO_EXTENSIONS.some((ext) => lower.endsWith(ext));
}

export function githubToHttps(uri: string): string {
	const prefix = 'github:';
	if (!uri.startsWith(prefix)) return '';

	let rest = uri.slice(prefix.length);
	while (rest.startsWith('/')) rest = rest.slice(1);

	const segs = rest.split('/').filter((s) => s.length > 0);
	if (segs.length < 2) return '';

	const [user, repo, ...tail] = segs;
	const branch = tail[0] ?? 'main';
	const subPath = tail.length > 1 ? '/' + tail.slice(1).join('/') : '';

	const base = `https://raw.githubusercontent.com/${user}/${repo}/${branch}`;
	if (!subPath) return base + '/strudel.json';
	if (looksLikeFile(subPath)) return base + subPath;
	return base + subPath + '/strudel.json';
}

export const githubHandler: UriHandler = {
	scheme: 'github',
	async load(uri: string, options: LoadOptions): Promise<LoadResult> {
		const url = githubToHttps(uri);
		if (!url) {
			throw new FileLoadError('invalid_format', `invalid github: URI: ${uri}`);
		}
		return uriResolver.load(url, options);
	}
};
