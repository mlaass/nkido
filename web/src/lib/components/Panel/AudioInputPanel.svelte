<script lang="ts">
	import { audioEngine } from '$lib/stores/audio.svelte';
	import type { InputSourceKind } from '$lib/audio/input-source';

	let devices = $state<MediaDeviceInfo[]>([]);
	let fileNames = $state<string[]>([]);

	// Derived view of audio engine input state.
	const kind = $derived(audioEngine.inputKind);
	const status = $derived(audioEngine.inputStatus);
	const error = $derived(audioEngine.inputError);
	const constraints = $derived(audioEngine.inputConstraints);
	const deviceId = $derived(audioEngine.inputDeviceId);
	const fileName = $derived(audioEngine.inputFileName);

	async function refreshDevices() {
		devices = await audioEngine.listInputDevices();
	}

	function refreshFiles() {
		fileNames = audioEngine.getInputFileNames();
	}

	$effect(() => {
		refreshDevices();
		refreshFiles();
	});

	function setKind(k: InputSourceKind) {
		if (k === 'mic') {
			audioEngine.setInputSource({
				kind: 'mic',
				deviceId: deviceId ?? undefined,
				constraints
			}).then(refreshDevices);
		} else if (k === 'tab') {
			audioEngine.setInputSource({ kind: 'tab' });
		} else if (k === 'file') {
			// Only switch if a file is already uploaded
			if (fileName) {
				audioEngine.setInputSource({ kind: 'file', fileName });
			} else if (fileNames.length > 0) {
				audioEngine.setInputSource({ kind: 'file', fileName: fileNames[0] });
			} else {
				// No file yet — surface a helpful note in inputError
				audioEngine.setInputSource({ kind: 'none' });
			}
		} else {
			audioEngine.setInputSource({ kind: 'none' });
		}
	}

	async function pickFiles(ev: Event) {
		const input = ev.target as HTMLInputElement;
		const files = input.files;
		if (!files) return;
		for (const file of files) {
			const buf = await file.arrayBuffer();
			audioEngine.registerInputFile(file.name, buf);
		}
		refreshFiles();
		input.value = '';
		// If the user hasn't picked a source yet, autoselect the first uploaded file
		if (fileNames.length > 0 && (kind === 'none' || kind === 'file')) {
			audioEngine.setInputSource({ kind: 'file', fileName: fileNames[0] });
		}
	}

	function chooseFile(name: string) {
		audioEngine.setInputSource({ kind: 'file', fileName: name });
	}

	function chooseDevice(id: string) {
		audioEngine.setInputSource({
			kind: 'mic',
			deviceId: id || undefined,
			constraints
		});
	}

	function toggleConstraint(key: 'echoCancellation' | 'noiseSuppression' | 'autoGainControl') {
		audioEngine.setInputConstraints({ [key]: !constraints[key] });
	}

	const statusLabel = $derived.by(() => {
		switch (status) {
			case 'idle': return 'Idle';
			case 'connecting': return 'Connecting…';
			case 'active': return 'Active';
			case 'denied': return 'Permission denied';
			case 'unavailable': return 'Unavailable';
			case 'error': return 'Error';
		}
	});

	const statusClass = $derived(`status status-${status}`);
</script>

<div class="audio-input-panel">
	<div class="header">
		<span class="title">Audio Input</span>
		<span class={statusClass}>{statusLabel}</span>
	</div>

	{#if error}
		<div class="error-line">{error}</div>
	{/if}

	<div class="source-row">
		<span class="source-label">Source</span>
		<div class="segmented-control" role="radiogroup" aria-label="Audio input source">
			<button class:active={kind === 'none'} onclick={() => setKind('none')}>None</button>
			<button class:active={kind === 'mic'} onclick={() => setKind('mic')}>Mic</button>
			<button class:active={kind === 'tab'} onclick={() => setKind('tab')}>Tab</button>
			<button class:active={kind === 'file'} onclick={() => setKind('file')}>File</button>
		</div>
	</div>

	{#if kind === 'mic'}
		<div class="sub-block">
			<label class="setting-label" for="input-device">Device</label>
			<select
				id="input-device"
				value={deviceId ?? ''}
				onchange={(e) => chooseDevice((e.target as HTMLSelectElement).value)}
			>
				<option value="">System default</option>
				{#each devices as dev (dev.deviceId)}
					<option value={dev.deviceId}>
						{dev.label || `Device ${dev.deviceId.slice(0, 6)}`}
					</option>
				{/each}
			</select>

			<div class="constraints">
				<label class="toggle-setting">
					<input
						type="checkbox"
						checked={constraints.echoCancellation}
						onchange={() => toggleConstraint('echoCancellation')}
					/>
					<span>Echo cancellation</span>
				</label>
				<label class="toggle-setting">
					<input
						type="checkbox"
						checked={constraints.noiseSuppression}
						onchange={() => toggleConstraint('noiseSuppression')}
					/>
					<span>Noise suppression</span>
				</label>
				<label class="toggle-setting">
					<input
						type="checkbox"
						checked={constraints.autoGainControl}
						onchange={() => toggleConstraint('autoGainControl')}
					/>
					<span>Auto gain</span>
				</label>
			</div>
		</div>
	{:else if kind === 'file'}
		<div class="sub-block">
			<input
				type="file"
				accept="audio/*"
				multiple
				onchange={pickFiles}
			/>
			{#if fileNames.length > 0}
				<div class="file-list">
					{#each fileNames as name (name)}
						<button
							class="file-item"
							class:active={fileName === name}
							onclick={() => chooseFile(name)}
						>{name}</button>
					{/each}
				</div>
			{:else}
				<p class="hint">Upload an audio file to use as the in() source.</p>
			{/if}
		</div>
	{:else if kind === 'tab'}
		<p class="hint">
			Pick a tab in the browser dialog and enable “Share tab audio”.
			Tab audio is captured per-tab; clicking Tab again re-opens the picker.
		</p>
	{/if}
</div>

<style>
	.audio-input-panel {
		display: flex;
		flex-direction: column;
		gap: 0.5rem;
		padding: 0.5rem 0;
	}

	.header {
		display: flex;
		align-items: center;
		justify-content: space-between;
	}

	.title {
		font-weight: 600;
		font-size: var(--ui-font-size-sm, 0.85rem);
		color: var(--text-primary);
	}

	.status {
		font-size: 0.75rem;
		padding: 0.125rem 0.5rem;
		border-radius: 999px;
		background: var(--bg-secondary);
		color: var(--text-secondary);
		border: 1px solid var(--border);
	}

	.status-active {
		color: var(--accent, #4caf50);
		border-color: var(--accent, #4caf50);
	}

	.status-connecting {
		color: var(--text-secondary);
	}

	.status-denied,
	.status-unavailable,
	.status-error {
		color: var(--error, #e0625a);
		border-color: var(--error, #e0625a);
	}

	.error-line {
		font-size: 0.75rem;
		color: var(--error, #e0625a);
	}

	.source-row {
		display: flex;
		flex-direction: column;
		gap: 0.25rem;
	}

	.source-label,
	.setting-label {
		font-size: 0.75rem;
		color: var(--text-secondary);
	}

	.segmented-control {
		display: flex;
		border: 1px solid var(--border);
		border-radius: 4px;
		overflow: hidden;
	}

	.segmented-control button {
		flex: 1;
		background: var(--bg-secondary);
		color: var(--text-secondary);
		border: none;
		padding: 0.25rem 0.5rem;
		font-size: 0.8rem;
		cursor: pointer;
	}

	.segmented-control button.active {
		background: var(--bg-tertiary, var(--accent, #4caf50));
		color: var(--text-primary);
	}

	.sub-block {
		display: flex;
		flex-direction: column;
		gap: 0.5rem;
	}

	select,
	input[type='file'] {
		background: var(--bg-secondary);
		border: 1px solid var(--border);
		border-radius: 4px;
		color: var(--text-primary);
		padding: 0.25rem 0.5rem;
		font-size: 0.8rem;
	}

	.constraints {
		display: flex;
		flex-direction: column;
		gap: 0.25rem;
	}

	.toggle-setting {
		display: flex;
		align-items: center;
		gap: 0.5rem;
		font-size: 0.8rem;
		color: var(--text-secondary);
	}

	.file-list {
		display: flex;
		flex-direction: column;
		gap: 0.125rem;
	}

	.file-item {
		text-align: left;
		background: var(--bg-secondary);
		border: 1px solid var(--border);
		border-radius: 4px;
		color: var(--text-primary);
		padding: 0.25rem 0.5rem;
		font-size: 0.8rem;
		cursor: pointer;
	}

	.file-item.active {
		border-color: var(--accent, #4caf50);
	}

	.hint {
		font-size: 0.75rem;
		color: var(--text-secondary);
		margin: 0;
	}
</style>
