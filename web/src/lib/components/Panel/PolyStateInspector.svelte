<script lang="ts">
	import type { StateInspection } from '$lib/stores/audio.svelte';

	interface Props {
		stateData: StateInspection;
	}

	let { stateData }: Props = $props();

	interface Voice {
		freq: number;
		vel: number;
		gate: number;
		active: boolean;
		releasing: boolean;
		age: number;
	}

	const MODE_NAMES = ['poly', 'mono', 'legato'] as const;

	let mode = $derived((stateData.mode as number) ?? 0);
	let modeName = $derived(MODE_NAMES[mode] ?? `mode${mode}`);
	let maxVoices = $derived((stateData.max_voices as number) ?? 0);
	let voices = $derived((stateData.voices as Voice[]) ?? []);
	let activeCount = $derived(voices.filter((v) => v && v.active).length);
	let firingCount = $derived(voices.filter((v) => v && v.active && !v.releasing && v.gate > 0.5).length);

	function freqToNote(hz: number): string {
		if (!hz || hz <= 0) return '—';
		// 69 = A4 = 440 Hz
		const midi = Math.round(69 + 12 * Math.log2(hz / 440));
		if (midi < 0 || midi > 127) return '—';
		const names = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
		return `${names[midi % 12]}${Math.floor(midi / 12) - 1}`;
	}
</script>

<div class="poly-summary">
	<div class="summary-row">
		<span class="summary-label">mode</span>
		<span class="summary-value mode-{modeName}">{modeName}</span>
	</div>
	<div class="summary-row">
		<span class="summary-label">voices</span>
		<span class="summary-value">{firingCount} firing · {activeCount} active · {maxVoices} max</span>
	</div>
</div>

<div class="voice-grid">
	<div class="voice-header">
		<span class="col-idx">#</span>
		<span class="col-state">·</span>
		<span class="col-note">note</span>
		<span class="col-freq">Hz</span>
		<span class="col-vel">vel</span>
		<span class="col-age">age</span>
	</div>
	{#each voices as voice, i (i)}
		{#if voice && voice.active}
			<div
				class="voice-row"
				class:firing={!voice.releasing && voice.gate > 0.5}
				class:releasing={voice.releasing}
			>
				<span class="col-idx">{i}</span>
				<span class="col-state" title={voice.releasing ? 'releasing' : voice.gate > 0.5 ? 'firing' : 'idle'}>
					{voice.releasing ? '○' : voice.gate > 0.5 ? '●' : '·'}
				</span>
				<span class="col-note">{freqToNote(voice.freq)}</span>
				<span class="col-freq">{voice.freq.toFixed(1)}</span>
				<span class="col-vel">
					<span class="vel-bar" style="width: {Math.min(100, voice.vel * 100)}%"></span>
					<span class="vel-text">{voice.vel.toFixed(2)}</span>
				</span>
				<span class="col-age">{voice.age}</span>
			</div>
		{/if}
	{/each}
	{#if firingCount === 0 && activeCount === 0}
		<div class="empty-state">No active voices</div>
	{/if}
</div>

<style>
	.poly-summary {
		display: flex;
		flex-direction: column;
		gap: 4px;
		padding: 4px 6px;
		background: var(--bg-secondary);
		border-radius: 4px;
		margin-bottom: var(--spacing-sm);
	}

	.summary-row {
		display: flex;
		justify-content: space-between;
		font-size: 11px;
	}

	.summary-label {
		color: var(--text-secondary);
		font-family: var(--font-mono);
	}

	.summary-value {
		color: var(--syntax-number);
		font-family: var(--font-mono);
	}

	.mode-poly {
		color: var(--syntax-function);
	}
	.mode-mono {
		color: var(--syntax-keyword);
	}
	.mode-legato {
		color: var(--syntax-string);
	}

	.voice-grid {
		display: flex;
		flex-direction: column;
		gap: 1px;
		font-family: var(--font-mono);
		font-size: 10px;
	}

	.voice-header,
	.voice-row {
		display: grid;
		grid-template-columns: 24px 16px 40px 1fr 1fr 36px;
		gap: 4px;
		align-items: center;
		padding: 2px 4px;
		border-radius: 3px;
	}

	.voice-header {
		color: var(--text-muted);
		font-weight: 600;
		border-bottom: 1px solid var(--border-muted);
		padding-bottom: 3px;
		margin-bottom: 2px;
	}

	.voice-row {
		color: var(--text-secondary);
		transition: background-color var(--transition-fast);
	}

	.voice-row.firing {
		background: color-mix(in srgb, var(--syntax-function) 15%, transparent);
		color: var(--text-primary);
	}

	.voice-row.releasing {
		opacity: 0.6;
		color: var(--text-muted);
	}

	.voice-row:hover {
		background: var(--bg-hover);
	}

	.col-idx {
		color: var(--text-muted);
		text-align: right;
	}

	.col-state {
		text-align: center;
	}

	.voice-row.firing .col-state {
		color: var(--syntax-function);
	}

	.voice-row.releasing .col-state {
		color: var(--text-muted);
	}

	.col-note {
		color: var(--syntax-keyword);
	}

	.col-freq {
		color: var(--syntax-number);
		text-align: right;
	}

	.col-vel {
		position: relative;
		display: inline-block;
		text-align: right;
	}

	.vel-bar {
		position: absolute;
		left: 0;
		top: 1px;
		bottom: 1px;
		background: color-mix(in srgb, var(--syntax-number) 25%, transparent);
		border-radius: 2px;
		z-index: 0;
	}

	.vel-text {
		position: relative;
		z-index: 1;
		padding-right: 2px;
	}

	.col-age {
		color: var(--text-muted);
		text-align: right;
	}

	.empty-state {
		padding: var(--spacing-sm);
		color: var(--text-muted);
		font-size: 11px;
		font-style: italic;
		text-align: center;
	}
</style>
