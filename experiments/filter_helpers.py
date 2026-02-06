"""
Shared Filter Analysis Helpers
==============================
Common functions used across filter opcode tests.
"""

import numpy as np
import cedar_core as cedar
from cedar_testing import CedarTestHost


def get_impulse(duration_sec, sample_rate):
    """Generate a unit impulse signal."""
    n = int(duration_sec * sample_rate)
    x = np.zeros(n, dtype=np.float32)
    x[0] = 1.0
    return x


def get_bode_data(impulse_response, sr):
    """Convert IR to Magnitude (dB) vs Frequency."""
    H = np.fft.rfft(impulse_response)
    freqs = np.fft.rfftfreq(len(impulse_response), 1/sr)
    mag = 20 * np.log10(np.abs(H) + 1e-10)
    return freqs, mag


def analyze_filter(filter_op, cutoff, res, filter_type_name):
    """
    Runs an impulse through the specified filter opcode and plots frequency response.
    """
    sr = 48000
    host = CedarTestHost(sr)

    buf_in = 0
    buf_freq = host.set_param("cutoff", cutoff)
    buf_res = host.set_param("res", res)
    buf_out = 1

    state_id = cedar.hash(f"{filter_type_name}_state") & 0xFFFF
    host.load_instruction(
        cedar.Instruction.make_ternary(filter_op, buf_out, buf_in, buf_freq, buf_res, state_id)
    )
    host.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
    )

    impulse = get_impulse(0.1, sr)
    response = host.process(impulse)

    freqs, mag_db = get_bode_data(response, sr)
    return freqs, mag_db
