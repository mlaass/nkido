/**
 * Live audio input source acquisition for the in() opcode.
 *
 * Three source types are supported (per docs/prd-audio-input.md):
 *   - 'mic': microphone via getUserMedia
 *   - 'tab': tab/system audio via getDisplayMedia
 *   - 'file:NAME': uploaded file decoded into an AudioBufferSourceNode loop
 *
 * The active source's output node is connected to the AudioWorklet's input
 * port. The AudioWorklet then forwards each block into the WASM heap (see
 * cedar-processor.js process()).
 */

export type InputSourceKind = 'none' | 'mic' | 'tab' | 'file';

export interface InputConstraints {
	echoCancellation: boolean;
	noiseSuppression: boolean;
	autoGainControl: boolean;
}

export const DEFAULT_INPUT_CONSTRAINTS: InputConstraints = {
	echoCancellation: false,
	noiseSuppression: false,
	autoGainControl: false
};

export interface InputSourceConfig {
	kind: InputSourceKind;
	deviceId?: string;        // Specific mic device (default = browser default)
	fileName?: string;        // For 'file' kind — name as registered with bankRegistry
	constraints?: InputConstraints;
}

export type InputStatus = 'idle' | 'connecting' | 'active' | 'denied' | 'unavailable' | 'error';

export interface ActiveInputSource {
	config: InputSourceConfig;
	node: AudioNode;          // Source node connected to the worklet
	stream?: MediaStream;     // Set for mic/tab sources
	stop: () => void;         // Releases all resources
}

/**
 * Enumerate available capture devices (microphones/line-in).
 * Permission may be required to see device labels — call after
 * a successful getUserMedia, otherwise labels can be empty strings.
 */
export async function enumerateInputDevices(): Promise<MediaDeviceInfo[]> {
	if (!navigator.mediaDevices?.enumerateDevices) return [];
	const devices = await navigator.mediaDevices.enumerateDevices();
	return devices.filter((d) => d.kind === 'audioinput');
}

/**
 * Acquire a microphone source via getUserMedia. Throws on permission
 * denial / no device — the caller maps the error to a UI status.
 */
export async function acquireMicSource(
	ctx: AudioContext,
	deviceId: string | undefined,
	constraints: InputConstraints
): Promise<ActiveInputSource> {
	const audio: MediaTrackConstraints = {
		echoCancellation: constraints.echoCancellation,
		noiseSuppression: constraints.noiseSuppression,
		autoGainControl: constraints.autoGainControl
	};
	if (deviceId) audio.deviceId = { exact: deviceId };

	const stream = await navigator.mediaDevices.getUserMedia({ audio, video: false });
	const node = ctx.createMediaStreamSource(stream);
	return {
		config: { kind: 'mic', deviceId, constraints },
		node,
		stream,
		stop: () => {
			node.disconnect();
			for (const track of stream.getTracks()) track.stop();
		}
	};
}

/**
 * Acquire tab/system audio via getDisplayMedia. The browser displays a
 * tab/window picker; the user must enable audio sharing on the chosen tab.
 */
export async function acquireTabSource(ctx: AudioContext): Promise<ActiveInputSource> {
	// getDisplayMedia returns a video stream with audio tracks; we ignore video.
	const stream = await navigator.mediaDevices.getDisplayMedia({
		audio: true,
		video: true
	});
	// Drop the video tracks so we don't waste resources
	for (const track of stream.getVideoTracks()) track.stop();
	if (stream.getAudioTracks().length === 0) {
		for (const track of stream.getTracks()) track.stop();
		throw new Error("Tab audio sharing was not enabled — re-pick the tab and check 'Share tab audio'.");
	}

	const node = ctx.createMediaStreamSource(stream);
	return {
		config: { kind: 'tab' },
		node,
		stream,
		stop: () => {
			node.disconnect();
			for (const track of stream.getTracks()) track.stop();
		}
	};
}

/**
 * Loop an in-memory ArrayBuffer (decoded WAV/etc) through an
 * AudioBufferSourceNode. WebAudio handles resampling automatically.
 */
export async function acquireFileSource(
	ctx: AudioContext,
	fileName: string,
	audioData: ArrayBuffer
): Promise<ActiveInputSource> {
	const buf = await ctx.decodeAudioData(audioData.slice(0));
	const src = ctx.createBufferSource();
	src.buffer = buf;
	src.loop = true;
	src.start();
	return {
		config: { kind: 'file', fileName },
		node: src,
		stop: () => {
			try { src.stop(); } catch {
				// already stopped
			}
			src.disconnect();
		}
	};
}
