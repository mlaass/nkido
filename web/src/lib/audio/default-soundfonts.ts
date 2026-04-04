export interface DefaultSoundFont {
	name: string; // Reference name for soundfont() calls
	urls: string[]; // Static asset URLs (tried in order, first success wins)
}

export const DEFAULT_SOUNDFONTS: DefaultSoundFont[] = [
	{ name: 'gm', urls: ['/soundfonts/TimGM6mb.sf3', '/soundfonts/TimGM6mb.sf2'] }
];

export function resolveDefaultSoundFontUrls(name: string): string[] {
	const sf = DEFAULT_SOUNDFONTS.find((s) => s.name === name);
	return sf?.urls ?? [];
}
