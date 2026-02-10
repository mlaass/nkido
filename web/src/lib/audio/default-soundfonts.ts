export interface DefaultSoundFont {
	name: string; // Reference name for soundfont() calls
	url: string; // Static asset URL
}

export const DEFAULT_SOUNDFONTS: DefaultSoundFont[] = [
	{ name: 'gm', url: '/soundfonts/MuseScore_General.sf3' }
];

export function resolveDefaultSoundFontUrl(name: string): string | null {
	const sf = DEFAULT_SOUNDFONTS.find((s) => s.name === name);
	return sf?.url ?? null;
}
