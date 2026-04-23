export interface PatchMeta {
	slug: string;
	title: string;
	description?: string;
}

const SLUG_RE = /^[a-z0-9][a-z0-9-]*$/;

export function isValidSlug(slug: string): boolean {
	return SLUG_RE.test(slug);
}

type FetchFn = typeof globalThis.fetch;

export async function loadPatchIndex(fetchFn: FetchFn = globalThis.fetch): Promise<PatchMeta[]> {
	const res = await fetchFn('/patches/index.json');
	if (!res.ok) throw new Error(`Failed to load patches index: HTTP ${res.status}`);
	return res.json();
}

export async function loadPatch(slug: string, fetchFn: FetchFn = globalThis.fetch): Promise<string> {
	if (!isValidSlug(slug)) throw new Error(`Invalid patch slug: ${slug}`);
	const res = await fetchFn(`/patches/${slug}.akk`);
	if (!res.ok) throw new Error(`Patch not found: ${slug}`);
	return res.text();
}
