/**
 * IndexedDB-based LRU file cache for audio samples
 */

const DB_NAME = 'nkido-file-cache';
const STORE_NAME = 'files';
const DB_VERSION = 1;
const MAX_CACHE_SIZE = 500 * 1024 * 1024; // 500MB

interface CacheEntry {
	key: string;
	data: ArrayBuffer;
	size: number;
	lastAccess: number;
}

function openDB(): Promise<IDBDatabase> {
	return new Promise((resolve, reject) => {
		const request = indexedDB.open(DB_NAME, DB_VERSION);

		request.onupgradeneeded = () => {
			const db = request.result;
			if (!db.objectStoreNames.contains(STORE_NAME)) {
				const store = db.createObjectStore(STORE_NAME, { keyPath: 'key' });
				store.createIndex('lastAccess', 'lastAccess');
			}
		};

		request.onsuccess = () => resolve(request.result);
		request.onerror = () => reject(request.error);
	});
}

export class FileCache {
	private dbPromise: Promise<IDBDatabase> | null = null;

	private getDB(): Promise<IDBDatabase> {
		if (!this.dbPromise) {
			this.dbPromise = openDB().catch((err) => {
				this.dbPromise = null;
				throw err;
			});
		}
		return this.dbPromise;
	}

	async get(key: string): Promise<ArrayBuffer | null> {
		try {
			const db = await this.getDB();
			return new Promise((resolve, reject) => {
				const tx = db.transaction(STORE_NAME, 'readwrite');
				const store = tx.objectStore(STORE_NAME);
				const request = store.get(key);

				request.onsuccess = () => {
					const entry = request.result as CacheEntry | undefined;
					if (!entry) {
						resolve(null);
						return;
					}

					// Update last access time
					entry.lastAccess = Date.now();
					store.put(entry);

					resolve(entry.data);
				};

				request.onerror = () => reject(request.error);
			});
		} catch {
			return null;
		}
	}

	async set(key: string, data: ArrayBuffer): Promise<void> {
		try {
			const db = await this.getDB();

			// Evict if needed before inserting
			await this.evictIfNeeded(db, data.byteLength);

			return new Promise((resolve, reject) => {
				const tx = db.transaction(STORE_NAME, 'readwrite');
				const store = tx.objectStore(STORE_NAME);

				const entry: CacheEntry = {
					key,
					data,
					size: data.byteLength,
					lastAccess: Date.now()
				};

				const request = store.put(entry);
				request.onsuccess = () => resolve();
				request.onerror = () => reject(request.error);
			});
		} catch {
			// Silently fail - cache is optional
		}
	}

	async delete(key: string): Promise<void> {
		try {
			const db = await this.getDB();
			return new Promise((resolve, reject) => {
				const tx = db.transaction(STORE_NAME, 'readwrite');
				const store = tx.objectStore(STORE_NAME);
				const request = store.delete(key);

				request.onsuccess = () => resolve();
				request.onerror = () => reject(request.error);
			});
		} catch {
			// Silently fail
		}
	}

	async clear(): Promise<void> {
		try {
			const db = await this.getDB();
			return new Promise((resolve, reject) => {
				const tx = db.transaction(STORE_NAME, 'readwrite');
				const store = tx.objectStore(STORE_NAME);
				const request = store.clear();

				request.onsuccess = () => resolve();
				request.onerror = () => reject(request.error);
			});
		} catch {
			// Silently fail
		}
	}

	private async evictIfNeeded(db: IDBDatabase, incomingSize: number): Promise<void> {
		return new Promise((resolve, reject) => {
			const tx = db.transaction(STORE_NAME, 'readwrite');
			const store = tx.objectStore(STORE_NAME);
			const index = store.index('lastAccess');

			// Calculate total size
			const allRequest = store.getAll();

			allRequest.onsuccess = () => {
				const entries = allRequest.result as CacheEntry[];
				let totalSize = entries.reduce((sum, e) => sum + e.size, 0);

				if (totalSize + incomingSize <= MAX_CACHE_SIZE) {
					resolve();
					return;
				}

				// Sort by lastAccess ascending (oldest first) for LRU eviction
				entries.sort((a, b) => a.lastAccess - b.lastAccess);

				// Evict oldest entries until we have room
				const evictTx = db.transaction(STORE_NAME, 'readwrite');
				const evictStore = evictTx.objectStore(STORE_NAME);

				for (const entry of entries) {
					if (totalSize + incomingSize <= MAX_CACHE_SIZE) break;
					evictStore.delete(entry.key);
					totalSize -= entry.size;
				}

				evictTx.oncomplete = () => resolve();
				evictTx.onerror = () => reject(evictTx.error);
			};

			allRequest.onerror = () => reject(allRequest.error);
		});
	}
}

/// Generate a cache key from a URL
export function cacheKeyFromUrl(url: string): string {
	return `url:${url}`;
}

/// Generate a cache key from a File
export function cacheKeyFromFile(file: File): string {
	return `file:${file.name}:${file.size}:${file.lastModified}`;
}

/// Singleton cache instance
export const fileCache = new FileCache();
