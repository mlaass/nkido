<script lang="ts">
	import { onMount, onDestroy } from 'svelte';
	import { page } from '$app/stores';
	import { goto } from '$app/navigation';
	import Transport from '$components/Transport/Transport.svelte';
	import Editor from '$components/Editor/Editor.svelte';
	import { editorStore } from '$stores/editor.svelte';
	import { loadPatch, loadPatchIndex, isValidSlug, type PatchMeta } from '$lib/patches/loader';

	const DEFAULT_SLUG = 'hello-sine';

	let patches = $state<PatchMeta[]>([]);
	let currentSlug = $state<string>(DEFAULT_SLUG);
	let loadError = $state<string | null>(null);
	let patchLoaded = $state(false);

	const currentMeta = $derived(patches.find((p) => p.slug === currentSlug));

	function slugFromURL(): string {
		const raw = $page.url.searchParams.get('patch');
		if (!raw) return DEFAULT_SLUG;
		return isValidSlug(raw) ? raw : DEFAULT_SLUG;
	}

	async function applyPatch(slug: string) {
		loadError = null;
		// Unmount the editor while the new patch loads so CodeMirror reinitializes
		// with the fresh doc on remount.
		patchLoaded = false;
		try {
			const code = await loadPatch(slug);
			editorStore.setCode(code);
			currentSlug = slug;
			patchLoaded = true;
		} catch (err) {
			loadError = err instanceof Error ? err.message : String(err);
			// Keep patchLoaded false so the editor stays hidden on error.
			throw err;
		}
	}

	// Origin allow-list for cross-frame patch switching. The site embeds this
	// route via iframe and posts `nkido:switch-patch` to swap demos without a
	// full WASM reload.
	const ORIGIN_ALLOWLIST: Array<string | RegExp> = [
		'https://nkido.cc',
		'https://www.nkido.cc',
		/^https:\/\/[a-z0-9-]+--nkido-cc\.netlify\.app$/,
		/^https:\/\/deploy-preview-\d+--nkido-cc\.netlify\.app$/,
		/^http:\/\/localhost(:\d+)?$/,
		/^http:\/\/127\.0\.0\.1(:\d+)?$/
	];

	function isOriginAllowed(origin: string): boolean {
		return ORIGIN_ALLOWLIST.some((rule) =>
			typeof rule === 'string' ? rule === origin : rule.test(origin)
		);
	}

	async function handleParentMessage(event: MessageEvent) {
		if (!isOriginAllowed(event.origin)) return;
		const data = event.data;
		if (!data || typeof data !== 'object') return;
		if (data.type !== 'nkido:switch-patch') return;

		const slug = data.patch;
		if (typeof slug !== 'string' || !isValidSlug(slug)) {
			(event.source as Window | null)?.postMessage(
				{ type: 'nkido:patch-error', patch: String(slug), reason: 'Invalid patch slug' },
				{ targetOrigin: event.origin }
			);
			return;
		}

		try {
			await applyPatch(slug);
			// Recompile so the new patch starts playing immediately, without
			// requiring the user to press Ctrl+Enter inside the iframe.
			await editorStore.evaluate();
			(event.source as Window | null)?.postMessage(
				{ type: 'nkido:patch-loaded', patch: slug },
				{ targetOrigin: event.origin }
			);
		} catch (err) {
			(event.source as Window | null)?.postMessage(
				{
					type: 'nkido:patch-error',
					patch: slug,
					reason: err instanceof Error ? err.message : String(err)
				},
				{ targetOrigin: event.origin }
			);
		}
	}

	function handlePatchChange(e: Event) {
		const target = e.target as HTMLSelectElement;
		const slug = target.value;
		const url = new URL($page.url);
		url.searchParams.set('patch', slug);
		goto(url.pathname + url.search, { replaceState: false, noScroll: true, keepFocus: true });
		applyPatch(slug);
	}

	onMount(async () => {
		// Do not let the embed overwrite the main app's localStorage-saved code.
		editorStore.setPersistenceEnabled(false);

		// Listen for patch-switch messages from the embedding parent frame.
		window.addEventListener('message', handleParentMessage);

		// Load the index in parallel with the requested patch.
		const requestedSlug = slugFromURL();
		const [indexResult] = await Promise.allSettled([
			loadPatchIndex(),
			applyPatch(requestedSlug).catch(() => undefined)
		]);

		if (indexResult.status === 'fulfilled') {
			patches = indexResult.value;
		} else {
			// Index failure is non-fatal — the editor still works with the loaded patch.
			console.warn('Could not load patches index:', indexResult.reason);
		}

		// Tell the parent we're ready for postMessage commands. Sent only when
		// embedded — top-level loads have no parent listening.
		if (window.parent && window.parent !== window) {
			window.parent.postMessage({ type: 'nkido:embed-ready' }, '*');
		}
	});

	onDestroy(() => {
		if (typeof window !== 'undefined') {
			window.removeEventListener('message', handleParentMessage);
		}
		editorStore.setPersistenceEnabled(true);
		editorStore.reloadFromPersistence();
	});
</script>

<svelte:head>
	<title>{currentMeta?.title ?? currentSlug} — nkido</title>
</svelte:head>

<div class="embed-app">
	<header class="embed-header">
		<Transport />
		<div class="patch-selector">
			{#if patches.length > 0}
				<label for="patch-select" class="patch-label">Patch</label>
				<select id="patch-select" value={currentSlug} onchange={handlePatchChange}>
					{#each patches as meta (meta.slug)}
						<option value={meta.slug}>{meta.title}</option>
					{/each}
				</select>
			{:else if currentMeta}
				<span class="patch-name">{currentMeta.title}</span>
			{:else}
				<span class="patch-name">{currentSlug}</span>
			{/if}
		</div>
	</header>

	{#if loadError}
		<div class="load-error" role="alert">
			Could not load patch "{currentSlug}": {loadError}
		</div>
	{/if}

	<main class="embed-main">
		{#if patchLoaded}
			{#key currentSlug}
				<Editor />
			{/key}
		{:else if !loadError}
			<div class="embed-placeholder">Loading patch…</div>
		{/if}
	</main>
</div>

<style>
	.embed-app {
		display: flex;
		flex-direction: column;
		height: 100vh;
		width: 100vw;
		background-color: var(--bg-primary);
		color: var(--text-primary);
		overflow: hidden;
	}

	.embed-header {
		display: flex;
		align-items: center;
		justify-content: space-between;
		gap: var(--spacing-md);
		padding: var(--spacing-sm) var(--spacing-md);
		border-bottom: 1px solid var(--border-muted);
		background-color: var(--bg-secondary);
		flex-shrink: 0;
	}

	.patch-selector {
		display: flex;
		align-items: center;
		gap: var(--spacing-sm);
		min-width: 0;
	}

	.patch-label {
		font-size: 12px;
		font-weight: 500;
		color: var(--text-secondary);
		text-transform: uppercase;
		letter-spacing: 0.5px;
	}

	.patch-selector select {
		background-color: var(--bg-tertiary);
		color: var(--text-primary);
		border: 1px solid var(--border-default);
		border-radius: 4px;
		padding: 4px 8px;
		font-size: 13px;
		font-family: inherit;
		max-width: 200px;
	}

	.patch-name {
		font-size: 13px;
		color: var(--text-secondary);
		white-space: nowrap;
		overflow: hidden;
		text-overflow: ellipsis;
	}

	.load-error {
		padding: var(--spacing-sm) var(--spacing-md);
		background-color: rgba(255, 80, 80, 0.12);
		color: var(--accent-danger, #ff5050);
		border-bottom: 1px solid var(--border-muted);
		font-size: 13px;
	}

	.embed-main {
		flex: 1;
		min-height: 0;
		position: relative;
		display: flex;
	}

	.embed-placeholder {
		flex: 1;
		display: flex;
		align-items: center;
		justify-content: center;
		color: var(--text-muted);
		font-size: 14px;
	}
</style>
