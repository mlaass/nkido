/**
 * Array Integration Tests
 *
 * Tests that array features compile correctly and produce expected bytecode.
 */

import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import {
	loadEnkido,
	getBytecode,
	getDisassembly,
	getOutputBuffers,
	type WrappedEnkido
} from './wasm-helper';

describe('Array Compilation', () => {
	let enkido: WrappedEnkido;

	beforeAll(async () => {
		enkido = await loadEnkido();
		enkido.cedar_init();
		enkido.cedar_set_sample_rate(48000);
		enkido.cedar_set_bpm(120);
	});

	afterAll(() => {
		enkido.cedar_reset();
	});

	it('compiles array literal [1, 2, 3]', () => {
		const result = enkido.akkado_compile('[1, 2, 3]');
		expect(result).toBe(1);

		const disasm = getDisassembly(enkido);
		expect(disasm.instructions.length).toBeGreaterThan(0);

		// Should have PUSH_CONST instructions for each element
		const pushConsts = disasm.instructions.filter((i) => i.opcode === 'PUSH_CONST');
		expect(pushConsts.length).toBeGreaterThanOrEqual(3);
	});

	it('compiles array indexing with constant index arr[0]', () => {
		const source = `
			arr = [100, 200, 300]
			arr[0] |> out(%, %)
		`;
		const result = enkido.akkado_compile(source);
		expect(result).toBe(1);

		const disasm = getDisassembly(enkido);

		// Should have OUTPUT instruction
		const outputs = disasm.instructions.filter((i) => i.opcode === 'OUTPUT');
		expect(outputs.length).toBe(1);
	});

	it('compiles array indexing with dynamic index arr[lfo()]', () => {
		const source = `
			arr = [220, 330, 440, 550]
			arr[lfo(1) * 3.99] |> osc("sin", %) |> out(%, %)
		`;
		const result = enkido.akkado_compile(source);
		expect(result).toBe(1);

		const disasm = getDisassembly(enkido);

		// Should have ARRAY_INDEX for dynamic indexing
		const arrayIndex = disasm.instructions.filter((i) => i.opcode === 'ARRAY_INDEX');
		expect(arrayIndex.length).toBe(1);

		// Should also have ARRAY_PACK to pack the multi-buffer array
		const arrayPack = disasm.instructions.filter((i) => i.opcode === 'ARRAY_PACK');
		expect(arrayPack.length).toBeGreaterThanOrEqual(1);
	});

	it.todo('compiles array with UGen auto-expansion', () => {
		// TODO: Implement UGen auto-expansion for arrays
		// Array through oscillator should expand to multiple oscillators
		const source = `
			[220, 330, 440] |> osc("sin", %) |> sum() |> out(%, %)
		`;
		const result = enkido.akkado_compile(source);
		expect(result).toBe(1);

		const disasm = getDisassembly(enkido);

		// Should have 3 OSC_SIN instructions (one for each frequency)
		const oscs = disasm.instructions.filter((i) => i.opcode === 'OSC_SIN');
		expect(oscs.length).toBe(3);
	});

	it.todo('compiles map() over array', () => {
		// TODO: Implement map() for arrays
		const source = `
			[1, 2, 3] |> map(x => x * 2) |> sum() |> out(%, %)
		`;
		const result = enkido.akkado_compile(source);
		expect(result).toBe(1);

		const disasm = getDisassembly(enkido);

		// Should have 3 MUL instructions (one for each element)
		const muls = disasm.instructions.filter((i) => i.opcode === 'MUL');
		expect(muls.length).toBe(3);
	});

	it.todo('compiles sum() of array', () => {
		// TODO: Implement sum() for arrays
		const source = `
			[1, 2, 3, 4] |> sum() |> out(%, %)
		`;
		const result = enkido.akkado_compile(source);
		expect(result).toBe(1);

		const disasm = getDisassembly(enkido);

		// Sum of 4 elements requires 3 ADD instructions
		const adds = disasm.instructions.filter((i) => i.opcode === 'ADD');
		expect(adds.length).toBe(3);
	});

	it('compiles len() of array', () => {
		const source = `
			arr = [10, 20, 30, 40, 50]
			len(arr) |> out(%, %)
		`;
		const result = enkido.akkado_compile(source);
		expect(result).toBe(1);

		const disasm = getDisassembly(enkido);

		// len() should emit a PUSH_CONST with value 5
		const pushConsts = disasm.instructions.filter((i) => i.opcode === 'PUSH_CONST');
		// One of them should be the length constant
		expect(pushConsts.length).toBeGreaterThan(0);
	});
});

describe('Array Audio Processing', () => {
	let enkido: WrappedEnkido;

	beforeAll(async () => {
		enkido = await loadEnkido();
		enkido.cedar_init();
		enkido.cedar_set_sample_rate(48000);
		enkido.cedar_set_bpm(120);
	});

	afterAll(() => {
		enkido.cedar_reset();
	});

	it.todo('produces audio from chord (array of frequencies)', () => {
		// TODO: Requires UGen auto-expansion and sum()
		const source = `
			[220, 330, 440] |> osc("sin", %) |> sum() * 0.3 |> out(%, %)
		`;
		const compileResult = enkido.akkado_compile(source);
		expect(compileResult).toBe(1);

		const bytecode = getBytecode(enkido);
		expect(bytecode.length).toBeGreaterThan(0);

		const loadResult = enkido.cedar_load_program(bytecode);
		expect(loadResult).toBe(0);

		// Process a few blocks
		for (let i = 0; i < 10; i++) {
			enkido.cedar_process_block();
		}

		const output = getOutputBuffers(enkido);

		// Should have non-zero audio output
		const maxSample = Math.max(...Array.from(output.left).map(Math.abs));
		expect(maxSample).toBeGreaterThan(0);
		expect(maxSample).toBeLessThan(1); // Should be scaled by 0.3
	});

	it('produces audio from wavetable-style indexing', () => {
		// Simple wavetable: phasor scans through array values
		const source = `
			wave = [0, 0.5, 1, 0.5, 0, -0.5, -1, -0.5]
			phasor(110) * 7.99 |> wave[%] * 0.5 |> out(%, %)
		`;
		const compileResult = enkido.akkado_compile(source);
		expect(compileResult).toBe(1);

		const bytecode = getBytecode(enkido);
		const loadResult = enkido.cedar_load_program(bytecode);
		expect(loadResult).toBe(0);

		// Process several blocks to get stable output
		for (let i = 0; i < 20; i++) {
			enkido.cedar_process_block();
		}

		const output = getOutputBuffers(enkido);

		// Should have audio output in range [-0.5, 0.5]
		const maxSample = Math.max(...Array.from(output.left).map(Math.abs));
		expect(maxSample).toBeGreaterThan(0);
		expect(maxSample).toBeLessThanOrEqual(0.5 + 0.01); // Small tolerance for float precision
	});
});

describe('Array Error Handling', () => {
	let enkido: WrappedEnkido;

	beforeAll(async () => {
		enkido = await loadEnkido();
		enkido.cedar_init();
	});

	afterAll(() => {
		enkido.cedar_reset();
	});

	it('reports error for undefined array variable', () => {
		const result = enkido.akkado_compile('undefined_arr[0]');
		expect(result).toBe(0);

		const diagCount = enkido.akkado_get_diagnostic_count();
		expect(diagCount).toBeGreaterThan(0);
	});

	it('compiles empty array without error', () => {
		const result = enkido.akkado_compile('[] |> out(%, %)');
		expect(result).toBe(1);
	});
});
