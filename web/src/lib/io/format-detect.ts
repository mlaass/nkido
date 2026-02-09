/**
 * Audio format detection from magic bytes
 */

export type AudioFormat = 'wav' | 'ogg' | 'flac' | 'mp3' | 'unknown';

export function detectAudioFormat(data: ArrayBuffer): AudioFormat {
	const view = new Uint8Array(data, 0, Math.min(data.byteLength, 12));

	if (view.length < 4) return 'unknown';

	// WAV: "RIFF" ... "WAVE"
	if (
		view.length >= 12 &&
		view[0] === 0x52 &&
		view[1] === 0x49 &&
		view[2] === 0x46 &&
		view[3] === 0x46 && // RIFF
		view[8] === 0x57 &&
		view[9] === 0x41 &&
		view[10] === 0x56 &&
		view[11] === 0x45 // WAVE
	) {
		return 'wav';
	}

	// OGG: "OggS"
	if (view[0] === 0x4f && view[1] === 0x67 && view[2] === 0x67 && view[3] === 0x53) {
		return 'ogg';
	}

	// FLAC: "fLaC"
	if (view[0] === 0x66 && view[1] === 0x4c && view[2] === 0x61 && view[3] === 0x43) {
		return 'flac';
	}

	// MP3: ID3 tag or frame sync
	if (view[0] === 0x49 && view[1] === 0x44 && view[2] === 0x33) {
		return 'mp3'; // ID3v2
	}
	if (view[0] === 0xff && (view[1] & 0xe0) === 0xe0) {
		return 'mp3'; // MPEG frame sync
	}

	return 'unknown';
}
