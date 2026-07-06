#!/usr/bin/env python3
# Copyright (c) 2026 The yosys-slang contributors
# SPDX-License-Identifier: ISC

import argparse
import re
from pathlib import Path


UNSUPPORTED_PATTERNS = (
    "strong(",
    " throughout ",
    "[->",
    "[*",
)

DISABLED_REASON = {
    "prim_alert_rxtx_assert_fpv.sv": {
        "PingEn_M": "uses throughout and goto repetition",
        "AlertReqAck_A": "uses unbounded strong eventuality",
        "AlertPingIgnored_A": "uses throughout and goto repetition",
        "AlertCheck0_A": "uses consecutive repetition",
        "AlertCheck1_A": "uses unbounded strong eventuality",
        "FsmLivenessSender_A": "uses unbounded strong eventuality",
        "FsmLivenessReceiver_A": "uses unbounded strong eventuality",
    },
    "prim_esc_rxtx_assert_fpv.sv": {
        "EscDeassert_A": "uses unbounded strong eventuality and consecutive repetition",
        "FsmLivenessSender_A": "uses unbounded strong eventuality",
        "FsmLivenessReceiver_A": "uses unbounded strong eventuality",
    },
    "sha3pad_assert_if.sv": {
        "ProcessToRun_A": "uses unbounded strong eventuality",
        "RunThenComplete_M": "uses unbounded strong eventuality",
    },
}

DISABLE_ALL_PROPERTIES = {
    "prim_alert_sender.sv",
    "prim_alert_receiver.sv",
    "prim_esc_sender.sv",
    "prim_esc_receiver.sv",
    "prim_count.sv",
    "prim_diff_decode.sv",
}


def copy_macro_statement(lines, index):
    out = []
    depth = 0
    started = False
    while index < len(lines):
        line = lines[index]
        out.append(line)
        depth += line.count("(") - line.count(")")
        if "(" in line:
            started = True
        index += 1
        if started and depth <= 0:
            break
    return out, index


def copy_sequence(lines, index):
    out = []
    while index < len(lines):
        out.append(lines[index])
        if lines[index].strip() == "endsequence":
            return out, index + 1
        index += 1
    return out, index


def macro_name(line):
    match = re.match(r"\s*`(?:ASSERT|ASSUME|ASSERT_FPV|ASSUME_FPV|COVER|COVER_FPV)\(([^,\s)]+)", line)
    if not match:
        return None
    return match.group(1)


def disable_block(out, source_name, name, reason):
    out.append(f"  // Disabled for yosys-slang SVA stress: {name}: {reason}.\n")
    out.append(f"  // Original source: tests/third_party/opentitan/.../{source_name}\n")


def filter_source(src, dst):
    source_name = src.name
    disabled = DISABLED_REASON.get(source_name, {})
    lines = src.read_text().splitlines(keepends=True)
    out = []
    index = 0
    while index < len(lines):
        stripped = lines[index].strip()

        if stripped.startswith("sequence "):
            name = stripped.split()[1].rstrip(";")
            seq, index = copy_sequence(lines, index)
            text = "".join(seq)
            reason = disabled.get(name)
            if source_name in DISABLE_ALL_PROPERTIES:
                disable_block(out, source_name, name, "inline RTL sequence disabled to focus this stress target on the FPV VIP")
            elif reason or any(pattern in text for pattern in UNSUPPORTED_PATTERNS):
                disable_block(out, source_name, name, reason or "uses unsupported sequence constructs")
            else:
                out.extend(seq)
            continue

        name = macro_name(lines[index])
        if name:
            stmt, index = copy_macro_statement(lines, index)
            text = "".join(stmt)
            reason = disabled.get(name)
            if source_name in DISABLE_ALL_PROPERTIES:
                disable_block(out, source_name, name, "inline RTL assertion disabled to focus this stress target on the FPV VIP")
            elif reason or any(pattern in text for pattern in UNSUPPORTED_PATTERNS):
                disable_block(out, source_name, name, reason or "uses unsupported SVA constructs")
            else:
                out.extend(stmt)
            continue

        out.append(lines[index])
        index += 1

    text = "".join(out)
    text = text.replace("prim_alert_rxtx_tb.", "prim_alert_rxtx_sva_harness.u_tb.")
    text = text.replace("prim_esc_rxtx_tb.", "prim_esc_rxtx_sva_harness.u_tb.")
    if source_name == "prim_alert_rxtx_assert_fpv.sv":
        text = re.sub(
            r"assign init_pending = mubi4_test_true_strict\(init_trig_i\) \|\|"
            r"\s*prim_alert_rxtx_sva_harness\.u_tb\.i_prim_alert_receiver\.state_q inside \{"
            r"\s*prim_alert_rxtx_sva_harness\.u_tb\.i_prim_alert_receiver\.InitReq,"
            r"\s*prim_alert_rxtx_sva_harness\.u_tb\.i_prim_alert_receiver\.InitAckWait\};",
            "assign init_pending = mubi4_test_true_strict(init_trig_i);",
            text,
            flags=re.MULTILINE,
        )
        text = text.replace("mubi4_test_true_strict(init_trig_i)", "(init_trig_i == 4'h6)")
        text = text.replace(
            "prim_alert_rxtx_sva_harness.u_tb.i_prim_alert_sender.Idle",
            "3'd0",
        )
        text = text.replace(
            "prim_alert_rxtx_sva_harness.u_tb.i_prim_alert_receiver.Idle",
            "3'd0",
        )
        text = text.replace(
            "prim_alert_rxtx_sva_harness.u_tb.i_prim_alert_sender.PingHsPhase1",
            "3'd3",
        )
        text = text.replace(
            "prim_alert_rxtx_sva_harness.u_tb.i_prim_alert_sender.PingHsPhase2",
            "3'd4",
        )
    if source_name == "clkmgr_cg_en_sva_if.sv":
        text = text.replace("scanmode == prim_mubi_pkg::MuBi4True", "scanmode == 4'h6")
        text = text.replace("$onehot0({ping_err_pi, ping_err_ni})", "!(ping_err_pi && ping_err_ni)")
        text = text.replace("$onehot0({ack_err_pi, ack_err_ni})", "!(ack_err_pi && ack_err_ni)")
        text = text.replace("$onehot0({alert_err_pi, alert_err_ni})", "!(alert_err_pi && alert_err_ni)")
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(text)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--src", type=Path, required=True)
    parser.add_argument("--dst", type=Path, required=True)
    args = parser.parse_args()
    filter_source(args.src, args.dst)


if __name__ == "__main__":
    main()
