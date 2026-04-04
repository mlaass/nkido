export interface DefaultSoundFont {
	name: string; // Reference name for soundfont() calls
	urls: string[]; // Static asset URLs (tried in order, first success wins)
	preload?: boolean; // Auto-load on engine init (default: false)
}

export const DEFAULT_SOUNDFONTS: DefaultSoundFont[] = [
	{ name: 'gm', urls: ['/soundfonts/TimGM6mb.sf3', '/soundfonts/TimGM6mb.sf2'], preload: true },
	{ name: 'gm_medium', urls: ['/soundfonts/FluidR3Mono_GM.sf3'] },
	{ name: 'gm_large', urls: ['/soundfonts/MuseScore_General.sf3'] }
];

export function resolveDefaultSoundFontUrls(name: string): string[] {
	const sf = DEFAULT_SOUNDFONTS.find((s) => s.name === name);
	return sf?.urls ?? [];
}
