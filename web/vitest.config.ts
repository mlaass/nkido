import { defineConfig } from 'vitest/config';
import { resolve } from 'path';

export default defineConfig({
	test: {
		include: ['tests/**/*.test.ts'],
		globals: true,
		testTimeout: 30000 // WASM loading can take a while
	},
	resolve: {
		alias: {
			$lib: resolve('./src/lib')
		}
	}
});
