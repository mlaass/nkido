/**
 * Convert a user-supplied path or URI into a URI the resolver accepts.
 *
 * The URI resolver requires an explicit scheme (`http://`, `https://`,
 * `github:`, `bundled://`, `blob:`, `idb:`); bare paths default to
 * `file://`, which has no handler on the web. Any path that arrives from
 * Akkado source (`wt_load`, `samples`, `soundfont`, etc.) must be
 * converted before reaching `loadFile` / `loadAsset`.
 *
 * Schemed inputs pass through unchanged. Bare paths are resolved against
 * the current document origin so the http handler can fetch them.
 */
export function pathToFetchUri(path: string): string {
	// RFC 3986 scheme: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) followed
	// by ":". Matches http://, github:, bundled://, blob:nkido:..., idb:...
	if (/^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(path)) return path;
	if (typeof window === 'undefined') return path;
	const cleaned = path.startsWith('/') ? path : '/' + path;
	return window.location.origin + cleaned;
}
