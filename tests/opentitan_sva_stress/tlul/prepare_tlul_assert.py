#!/usr/bin/env python3
# Copyright (c) 2026 The yosys-slang contributors
# SPDX-License-Identifier: ISC

import argparse
from pathlib import Path


DISABLED_ASSERTIONS = {
    "aDataKnown_A": "uses $isunknown, which current yosys-slang does not synthesize",
    "aDataKnown_M": "uses $isunknown, which current yosys-slang does not synthesize",
    "dDataKnown_A": "uses $isunknown, which current yosys-slang does not synthesize",
    "dDataKnown_M": "uses $isunknown, which current yosys-slang does not synthesize",
}

SEQUENCE_EXPR = {
    "h2d_pre_S": "h2d.a_valid",
    "legalAOpcode_S": "((h2d.a_opcode === 3'h0) || (h2d.a_opcode === 3'h4) || (h2d.a_opcode === 3'h1))",
    "legalAParam_S": "(h2d.a_param === '0)",
    "sizeGTEMask_S": "((h2d.a_opcode == 3'h0) || ((1 << h2d.a_size) >= $countones(h2d.a_mask)))",
    "sizeMatchesMask_S": "(((h2d.a_opcode == 3'h1) || (h2d.a_opcode == 3'h4)) || ((1 << h2d.a_size) === $countones(h2d.a_mask)))",
    "pendingReqPerSrc_S": "(pend_req[h2d.a_source].pend == 0)",
    "addrSizeAligned_S": "((h2d.a_address & ((1 << h2d.a_size)-1)) == '0)",
    "contigMask_pre_S": "(h2d.a_opcode != 3'h1)",
    "contigMask_S": "($countones(h2d.a_mask ^ {h2d.a_mask[$bits(h2d.a_mask)-2:0], 1'b0}) <= 2)",
    "aDataKnown_pre_S": "(h2d.a_opcode != 3'h4)",
    "aDataKnown_S": "(((!a_mask[0]) || (a_mask[0] && !$isunknown(a_data[8*0 +: 8]))) && ((!a_mask[1]) || (a_mask[1] && !$isunknown(a_data[8*1 +: 8]))) && ((!a_mask[2]) || (a_mask[2] && !$isunknown(a_data[8*2 +: 8]))) && ((!a_mask[3]) || (a_mask[3] && !$isunknown(a_data[8*3 +: 8]))) && ((!a_mask[4]) || (a_mask[4] && !$isunknown(a_data[8*4 +: 8]))) && ((!a_mask[5]) || (a_mask[5] && !$isunknown(a_data[8*5 +: 8]))) && ((!a_mask[6]) || (a_mask[6] && !$isunknown(a_data[8*6 +: 8]))) && ((!a_mask[7]) || (a_mask[7] && !$isunknown(a_data[8*7 +: 8]))))",
    "d2h_pre_S": "d2h.d_valid",
    "respOpcode_S": "(d2h.d_opcode === (((curr_fwd ? curr_req.opcode : pend_req[d2h.d_source].opcode) == 3'h4) ? 3'h1 : 3'h0))",
    "legalDParam_S": "(d2h.d_param === '0)",
    "respSzEqReqSz_S": "(d2h.d_size === (curr_fwd ? curr_req.size : pend_req[d2h.d_source].size))",
    "respMustHaveReq_S": "(curr_fwd | pend_req[d2h.d_source].pend)",
    "dDataKnown_pre_S": "(d2h.d_opcode == 3'h1)",
    "dDataKnown_S": "(((!d_mask[0]) || (d_mask[0] && !$isunknown(d_data[8*0 +: 8]))) && ((!d_mask[1]) || (d_mask[1] && !$isunknown(d_data[8*1 +: 8]))) && ((!d_mask[2]) || (d_mask[2] && !$isunknown(d_data[8*2 +: 8]))) && ((!d_mask[3]) || (d_mask[3] && !$isunknown(d_data[8*3 +: 8]))) && ((!d_mask[4]) || (d_mask[4] && !$isunknown(d_data[8*4 +: 8]))) && ((!d_mask[5]) || (d_mask[5] && !$isunknown(d_data[8*5 +: 8]))) && ((!d_mask[6]) || (d_mask[6] && !$isunknown(d_data[8*6 +: 8]))) && ((!d_mask[7]) || (d_mask[7] && !$isunknown(d_data[8*7 +: 8]))))",
    "d_error_pre_S": "(h2d.a_valid && d2h.a_ready)",
    "legalAOpcodeErr_S": "(!(h2d.a_opcode inside {3'h0, 3'h4, 3'h1}))",
    "sizeGTEMaskErr_S": "((1 << h2d.a_size) < $countones(h2d.a_mask))",
    "sizeMatchesMaskErr_S": "((h2d.a_opcode == 3'h0) && ((1 << h2d.a_size) != $countones(h2d.a_mask)))",
    "addrSizeAlignedErr_S": "((h2d.a_address & ((1 << h2d.a_size)-1)) != '0)",
}


def copy_statement(lines, index):
    out = []
    depth = 0
    seen_statement = False
    while index < len(lines):
        line = lines[index]
        out.append(line)
        stripped = line.strip()
        if stripped.startswith(("assert property", "assume property", "cover property")):
            seen_statement = True
        depth += line.count("(") - line.count(")")
        index += 1
        if seen_statement and depth <= 0:
            if index < len(lines) and lines[index].lstrip().startswith("else "):
                out.append(lines[index])
                index += 1
            break
    return out, index


def skip_sequence(lines, index):
    while index < len(lines):
        if lines[index].strip() == "endsequence":
            return index + 1
        index += 1
    return index


def inline_sequences(statement, explicit_clocking):
    text = "".join(statement)
    for name, expr in sorted(SEQUENCE_EXPR.items(), key=lambda item: len(item[0]), reverse=True):
        text = text.replace(name, f"({expr})")
    property_prefix = "property (disable iff (disable_sva || !rst_ni) "
    if explicit_clocking:
        property_prefix = "property (@(posedge clk_i) disable iff (disable_sva || !rst_ni) "
    text = text.replace("property (", property_prefix)
    return text.splitlines(keepends=True)


def make_stub(label, kind):
    flavor = "assumption" if kind == "M" else "assertion"
    reason = DISABLED_ASSERTIONS[label]
    return [
        f"      // Disabled for yosys-slang SVA: {label} {flavor} {reason}.\n",
        f"      // Original OpenTitan property is left in tests/third_party/opentitan/hw/ip/tlul/rtl/tlul_assert.sv.\n",
    ]


def filter_tlul_assert(src, dst, explicit_clocking=False):
    lines = src.read_text().splitlines(keepends=True)
    out = []
    index = 0
    while index < len(lines):
        stripped = lines[index].strip()
        if stripped == "// SVA coverage //":
            out.append(lines[index])
            out.append("  // Disabled for yosys-slang SVA: TLUL cover properties are not assertion checks,\n")
            out.append("  // and some coverage sequences use match-item locals and goto repetition.\n")
            index += 1
            while index < len(lines) and lines[index].strip() != "`ifdef UVM":
                index += 1
            continue

        if stripped == "`ifdef UVM":
            block_start = index
            index += 1
            ifdef_depth = 1
            while index < len(lines) and ifdef_depth:
                nested = lines[index].strip()
                if nested.startswith(("`ifdef", "`ifndef", "`if")):
                    ifdef_depth += 1
                elif nested == "`endif":
                    ifdef_depth -= 1
                index += 1
            block = "".join(lines[block_start:index])
            if "uvm_config_db" in block:
                out.append("  // Tied off for yosys-slang formal: UVM runtime assertion control is not used.\n")
                out.append("  always_comb disable_sva = 1'b0;\n")
            else:
                out.append("  // Disabled for yosys-slang formal: UVM imports are not used.\n")
            continue

        if stripped.startswith(("`ASSERT_KNOWN_IF(", "`ASSERT_KNOWN(")):
            out.append("  // Disabled for yosys-slang SVA: OpenTitan knownness macro uses $isunknown.\n")
            depth = lines[index].count("(") - lines[index].count(")")
            index += 1
            while index < len(lines) and depth > 0:
                depth += lines[index].count("(") - lines[index].count(")")
                index += 1
            continue

        if stripped.startswith("sequence "):
            seq_name = stripped.split()[1].rstrip(";")
            if seq_name in SEQUENCE_EXPR:
                out.append(f"  // Inlined for yosys-slang SVA: sequence {seq_name}.\n")
            else:
                out.append(f"  // Disabled for yosys-slang SVA: coverage sequence {seq_name}.\n")
            index = skip_sequence(lines, index + 1)
            continue

        if stripped.startswith("`TLUL_COVER(") or stripped.startswith("`TLUL_A_CHAN_CONTENT_CHANGED_WO_ACCEPTED(") or stripped.startswith("`TLUL_D_CHAN_CONTENT_CHANGED_WO_ACCEPTED("):
            out.append("    // Disabled for yosys-slang SVA: OpenTitan TLUL coverage sequences use match-item\n")
            out.append("    // local variables and goto repetition, which are not needed for assertion BMC here.\n")
            index += 1
            continue

        if stripped.endswith(("_A:", "_M:")):
            label = stripped[:-1]
            if label in DISABLED_ASSERTIONS:
                kind = label.rsplit("_", 1)[1]
                _, index = copy_statement(lines, index + 1)
                out.extend(make_stub(label, kind))
                continue
            if index + 1 < len(lines) and " property " in lines[index + 1]:
                out.append(lines[index])
                statement, index = copy_statement(lines, index + 1)
                out.extend(inline_sequences(statement, explicit_clocking))
                continue

        out.append(lines[index])
        index += 1

    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text("".join(out))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--src", required=True, type=Path)
    parser.add_argument("--dst", required=True, type=Path)
    parser.add_argument(
        "--explicit-clocking",
        action="store_true",
        help="Rewrite default-clocked OpenTitan properties with explicit @(posedge clk_i).",
    )
    args = parser.parse_args()
    filter_tlul_assert(args.src, args.dst, explicit_clocking=args.explicit_clocking)


if __name__ == "__main__":
    main()
