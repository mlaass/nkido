/**
 * WASM module helper for tests
 *
 * Loads and wraps the Nkido WASM module for testing.
 */

import { readFileSync } from 'fs';
import { resolve } from 'path';

// Module instance cached between tests
let moduleInstance: NkidoModule | null = null;

export interface NkidoModule {
	// Memory access
	ccall: (name: string, returnType: string | null, argTypes: string[], args: unknown[]) => unknown;
	cwrap: (name: string, returnType: string | null, argTypes: string[]) => (...args: unknown[]) => unknown;
	getValue: (ptr: number, type: string) => number;
	setValue: (ptr: number, value: number, type: string) => void;
	UTF8ToString: (ptr: number) => string;
	stringToUTF8: (str: string, ptr: number, maxBytes: number) => void;
	lengthBytesUTF8: (str: string) => number;
	_nkido_malloc: (size: number) => number;
	_nkido_free: (ptr: number) => void;
	HEAPF32: Float32Array;
	HEAPU8: Uint8Array;
	wasmMemory?: WebAssembly.Memory;
}

export interface WrappedNkido {
	module: NkidoModule;

	// Cedar VM
	cedar_init: () => void;
	cedar_destroy: () => void;
	cedar_set_sample_rate: (rate: number) => void;
	cedar_set_bpm: (bpm: number) => void;
	cedar_load_program: (bytecode: Uint8Array) => number;
	cedar_process_block: () => void;
	cedar_get_output_left: () => number;
	cedar_get_output_right: () => number;
	cedar_reset: () => void;
	cedar_has_program: () => number;

	// Akkado compiler
	akkado_compile: (source: string) => number;
	akkado_get_bytecode: () => number;
	akkado_get_bytecode_size: () => number;
	akkado_get_diagnostic_count: () => number;
	akkado_get_diagnostic_message: (index: number) => string;
	akkado_get_diagnostic_line: (index: number) => number;
	akkado_get_disassembly: () => string;
}

/**
 * Load the Nkido WASM module
 */
export async function loadNkido(): Promise<WrappedNkido> {
	if (moduleInstance) {
		return wrapModule(moduleInstance);
	}

	// Read the WASM files
	const wasmJsPath = resolve(__dirname, '../static/wasm/nkido.js');
	const wasmBinaryPath = resolve(__dirname, '../static/wasm/nkido.wasm');

	// Load the JS glue code
	const jsCode = readFileSync(wasmJsPath, 'utf-8');
	const wasmBinary = readFileSync(wasmBinaryPath);

	// Create a function from the JS code
	const createModule = new Function(
		'module',
		`${jsCode}; return createNkidoModule;`
	)({});

	// Initialize the module with the WASM binary
	const instance: NkidoModule = await createModule({
		wasmBinary: wasmBinary.buffer
	});
	moduleInstance = instance;

	return wrapModule(instance);
}

function wrapModule(module: NkidoModule): WrappedNkido {
	const ccall = module.ccall.bind(module);
	const cwrap = module.cwrap.bind(module);

	// Create wrapped functions
	const cedar_init = cwrap('cedar_init', null, []) as () => void;
	const cedar_destroy = cwrap('cedar_destroy', null, []) as () => void;
	const cedar_set_sample_rate = cwrap('cedar_set_sample_rate', null, ['number']) as (
		rate: number
	) => void;
	const cedar_set_bpm = cwrap('cedar_set_bpm', null, ['number']) as (bpm: number) => void;
	const cedar_process_block = cwrap('cedar_process_block', null, []) as () => void;
	const cedar_get_output_left = cwrap('cedar_get_output_left', 'number', []) as () => number;
	const cedar_get_output_right = cwrap('cedar_get_output_right', 'number', []) as () => number;
	const cedar_reset = cwrap('cedar_reset', null, []) as () => void;
	const cedar_has_program = cwrap('cedar_has_program', 'number', []) as () => number;

	const akkado_get_bytecode = cwrap('akkado_get_bytecode', 'number', []) as () => number;
	const akkado_get_bytecode_size = cwrap('akkado_get_bytecode_size', 'number', []) as () => number;
	const akkado_get_diagnostic_count = cwrap(
		'akkado_get_diagnostic_count',
		'number',
		[]
	) as () => number;

	return {
		module,

		cedar_init,
		cedar_destroy,
		cedar_set_sample_rate,
		cedar_set_bpm,
		cedar_process_block,
		cedar_get_output_left,
		cedar_get_output_right,
		cedar_reset,
		cedar_has_program,

		cedar_load_program: (bytecode: Uint8Array) => {
			const ptr = module._nkido_malloc(bytecode.length);
			// Write bytecode to WASM memory using fresh heap view
			const heap = new Uint8Array(module.wasmMemory?.buffer ?? module.HEAPU8?.buffer);
			heap.set(bytecode, ptr);
			const result = ccall('cedar_load_program', 'number', ['number', 'number'], [ptr, bytecode.length]) as number;
			module._nkido_free(ptr);
			return result;
		},

		akkado_compile: (source: string) => {
			// Get actual UTF-8 byte length (not JS string length)
			const utf8ByteLen = module.lengthBytesUTF8(source);
			const allocLen = utf8ByteLen + 1; // +1 for null terminator
			const ptr = module._nkido_malloc(allocLen);
			module.stringToUTF8(source, ptr, allocLen);
			const result = ccall('akkado_compile', 'number', ['number', 'number'], [ptr, utf8ByteLen]) as number;
			module._nkido_free(ptr);
			return result;
		},

		akkado_get_bytecode,
		akkado_get_bytecode_size,
		akkado_get_diagnostic_count,

		akkado_get_diagnostic_message: (index: number) => {
			const ptr = ccall('akkado_get_diagnostic_message', 'number', ['number'], [index]) as number;
			return module.UTF8ToString(ptr);
		},

		akkado_get_diagnostic_line: (index: number) => {
			return ccall('akkado_get_diagnostic_line', 'number', ['number'], [index]) as number;
		},

		akkado_get_disassembly: () => {
			const ptr = ccall('akkado_get_disassembly', 'number', [], []) as number;
			return module.UTF8ToString(ptr);
		}
	};
}

/**
 * Get bytecode from module after compilation
 */
export function getBytecode(nkido: WrappedNkido): Uint8Array {
	const ptr = nkido.akkado_get_bytecode();
	const size = nkido.akkado_get_bytecode_size();
	// Create a copy to avoid issues with memory growth
	const result = new Uint8Array(size);
	const heap = new Uint8Array(nkido.module.wasmMemory?.buffer ?? nkido.module.HEAPU8.buffer);
	for (let i = 0; i < size; i++) {
		result[i] = heap[ptr + i];
	}
	return result;
}

/**
 * Get output buffers after processing
 */
export function getOutputBuffers(nkido: WrappedNkido): { left: Float32Array; right: Float32Array } {
	const leftPtr = nkido.cedar_get_output_left();
	const rightPtr = nkido.cedar_get_output_right();
	const blockSize = 128;

	// Create copies to avoid issues with memory growth
	const left = new Float32Array(blockSize);
	const right = new Float32Array(blockSize);
	const heap = new Float32Array(nkido.module.wasmMemory?.buffer ?? nkido.module.HEAPF32.buffer);
	const leftIdx = leftPtr / 4;
	const rightIdx = rightPtr / 4;

	for (let i = 0; i < blockSize; i++) {
		left[i] = heap[leftIdx + i];
		right[i] = heap[rightIdx + i];
	}

	return { left, right };
}

/**
 * Parse disassembly JSON
 */
export interface DisassemblyInstruction {
	index: number;
	opcode: string;
	opcodeNum: number;
	out: number;
	inputs: number[];
	stateId: number;
	rate: number;
	stateful: boolean;
}

export interface Disassembly {
	instructions: DisassemblyInstruction[];
	summary: {
		totalInstructions: number;
		statefulCount: number;
		uniqueStateIds: number;
		stateIds: number[];
	};
}

export function getDisassembly(nkido: WrappedNkido): Disassembly {
	const json = nkido.akkado_get_disassembly();
	return JSON.parse(json);
}
