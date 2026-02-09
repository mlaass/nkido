/**
 * Web Audio API-based audio decoding
 *
 * Decodes OGG, FLAC, MP3 (and WAV) using the browser's built-in decoder.
 * Returns interleaved float samples suitable for loading into Cedar.
 */

export interface DecodedAudio {
	samples: Float32Array; // Interleaved (L, R, L, R for stereo)
	channels: number;
	sampleRate: number;
	frames: number;
}

/**
 * Decode audio file data using Web Audio API
 *
 * This leverages the browser's native decoders for all formats it supports
 * (typically WAV, OGG, MP3, FLAC, AAC, etc.)
 */
export async function decodeAudioFile(
	data: ArrayBuffer,
	audioContext: AudioContext | OfflineAudioContext
): Promise<DecodedAudio> {
	// decodeAudioData needs its own copy (it detaches the buffer)
	const copy = data.slice(0);

	const audioBuffer = await audioContext.decodeAudioData(copy);

	const channels = audioBuffer.numberOfChannels;
	const frames = audioBuffer.length;
	const sampleRate = audioBuffer.sampleRate;

	// Interleave channels
	const samples = new Float32Array(frames * channels);

	if (channels === 1) {
		// Mono - direct copy
		samples.set(audioBuffer.getChannelData(0));
	} else {
		// Interleave multi-channel
		const channelData: Float32Array[] = [];
		for (let c = 0; c < channels; c++) {
			channelData.push(audioBuffer.getChannelData(c));
		}

		for (let i = 0; i < frames; i++) {
			for (let c = 0; c < channels; c++) {
				samples[i * channels + c] = channelData[c][i];
			}
		}
	}

	return { samples, channels, sampleRate, frames };
}
