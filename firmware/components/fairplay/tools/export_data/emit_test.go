package airplay

// Emits the constants of the reference implementation as C initializers so the
// C port can be built from exactly the same bytes.

import (
	"fmt"
	"os"
	"strings"
	"testing"
)

type cbuf struct{ b strings.Builder }

func (c *cbuf) p(f string, a ...any) { fmt.Fprintf(&c.b, f+"\n", a...) }

func (c *cbuf) bytes(name, ctype string, data []byte) {
	c.p("static const uint8_t %s[%d] = {", name, len(data))
	for i := 0; i < len(data); i += 12 {
		end := i + 12
		if end > len(data) {
			end = len(data)
		}
		var parts []string
		for _, v := range data[i:end] {
			parts = append(parts, fmt.Sprintf("0x%02x", v))
		}
		c.p("\t%s,", strings.Join(parts, ", "))
	}
	c.p("};")
	c.p("")
}

func (c *cbuf) u32(name string, data []uint32) {
	c.p("static const uint32_t %s[%d] = {", name, len(data))
	for i := 0; i < len(data); i += 8 {
		end := min(i+8, len(data))
		var parts []string
		for _, v := range data[i:end] {
			parts = append(parts, fmt.Sprintf("0x%08x", v))
		}
		c.p("\t%s,", strings.Join(parts, ", "))
	}
	c.p("};")
	c.p("")
}

func (c *cbuf) tables(name string, t *fpsapNetworkTables) {
	c.p("static const struct lookup %s_round[9][16] = {", name)
	for _, round := range t.roundSubstitution {
		var parts []string
		for _, ref := range round {
			parts = append(parts, fmt.Sprintf("{%d,0x%02x,0x%02x}", ref.table, ref.inputXOR, ref.outputXOR))
		}
		c.p("\t{%s},", strings.Join(parts, ","))
	}
	c.p("};")
	c.p("static const struct lookup %s_mix[4][4] = {", name)
	for _, col := range t.mixColumns {
		var parts []string
		for _, ref := range col {
			parts = append(parts, fmt.Sprintf("{%d,0x%02x,0x%02x}", ref.table, ref.inputXOR, ref.outputXOR))
		}
		c.p("\t{%s},", strings.Join(parts, ","))
	}
	c.p("};")
	c.p("static const struct lookup %s_final[16] = {", name)
	var parts []string
	for _, ref := range t.finalSubstitution {
		parts = append(parts, fmt.Sprintf("{%d,0x%02x,0x%02x}", ref.table, ref.inputXOR, ref.outputXOR))
	}
	c.p("\t%s,", strings.Join(parts, ","))
	c.p("};")
	c.p("")
}

func TestEmitCConstants(t *testing.T) {
	out := os.Getenv("EMIT_OUT")
	if out == "" {
		t.Skip("EMIT_OUT not set")
	}
	var c cbuf
	c.p("/* Generated from the Go reference implementation. Do not edit. */")
	c.p("#ifndef FAIRPLAY_DATA_H")
	c.p("#define FAIRPLAY_DATA_H")
	c.p("")
	c.p("#include <stdint.h>")
	c.p("")
	c.p("struct lookup { uint8_t table; uint8_t in_xor; uint8_t out_xor; };")
	c.p("")
	c.bytes("fairplay_md5_shift", "uint8_t", func() []byte {
		b := make([]byte, len(fairplayMD5Shift))
		for i, v := range fairplayMD5Shift {
			b[i] = uint8(v)
		}
		return b
	}())
	c.u32("fairplay_md5_constant", fairplayMD5Constant[:])
	c.bytes("sap_initial_hash", "uint8_t", sapInitialState.hash[:])
	c.bytes("sap_initial_matrix", "uint8_t", sapInitialState.matrix[:])
	c.bytes("sap_seed", "uint8_t", sapSeed[:])
	c.bytes("fairplay_initial_session_key", "uint8_t", fairplayInitialSessionKey[:])
	c.bytes("fairplay_kdf_prefix", "uint8_t", fairplayKDFPrefix[:])
	c.bytes("fairplay_kdf_suffix", "uint8_t", fairplayKDFSuffix[:])
	c.p("static const uint8_t fairplay_message_iv[4][16] = {")
	for _, iv := range fairplayMessageIV {
		var parts []string
		for _, v := range iv {
			parts = append(parts, fmt.Sprintf("0x%02x", v))
		}
		c.p("\t{%s},", strings.Join(parts, ", "))
	}
	c.p("};")
	c.bytes("fairplay_round_key0", "uint8_t", fairplayMessageRoundKey0[:])
	c.bytes("fairplay_round_key10", "uint8_t", fairplayMessageRoundKey10[:])
	c.p("static const uint8_t fairplay_middle_keys[4][9][16] = {")
	for _, mode := range fairplayMessageMiddleKeys {
		c.p("\t{")
		for _, k := range mode {
			var parts []string
			for _, v := range k {
				parts = append(parts, fmt.Sprintf("0x%02x", v))
			}
			c.p("\t\t{%s},", strings.Join(parts, ", "))
		}
		c.p("\t},")
	}
	c.p("};")
	c.bytes("fairplay_inverse_sbox", "uint8_t", inverseAESSBox[:])
	c.bytes("fpsap_m1_payload", "uint8_t", fpsapM1Payload[:])
	c.bytes("fpsap_m3_label", "uint8_t", fpsapM3Label[:])
	c.bytes("fpsap_descriptor_prefix", "uint8_t", fpsapDescriptorPrefix[:])
	c.bytes("fpsap_descriptor_suffix", "uint8_t", fpsapDescriptorSuffix[:])
	c.bytes("fpsap_fixed_block", "uint8_t", fpsapFixedBlock[:])
	c.bytes("fpsap_first_position_map", "uint8_t", fpsapFirstPositionMap[:])
	c.bytes("fpsap_second_position_map", "uint8_t", fpsapSecondPositionMap[:])
	c.bytes("fpsap_first_input_mask", "uint8_t", fpsapFirstInputMask[:])
	c.bytes("fpsap_second_output_mask", "uint8_t", fpsapSecondOutputMask[:])
	c.p("static const uint8_t fpsap_substitution_bases[6][256] = {")
	for _, base := range fpsapSubstitutionBases {
		var parts []string
		for _, v := range base {
			parts = append(parts, fmt.Sprintf("0x%02x", v))
		}
		c.p("\t{")
		for i := 0; i < 256; i += 12 {
			c.p("\t\t%s,", strings.Join(parts[i:min(i+12, 256)], ", "))
		}
		c.p("\t},")
	}
	c.p("};")
	c.p("static const uint8_t fpsap_mix_bases[7][256] = {")
	for _, base := range fpsapMixBases {
		var parts []string
		for _, v := range base {
			parts = append(parts, fmt.Sprintf("0x%02x", v))
		}
		c.p("\t{")
		for i := 0; i < 256; i += 12 {
			c.p("\t\t%s,", strings.Join(parts[i:min(i+12, 256)], ", "))
		}
		c.p("\t},")
	}
	c.p("};")
	c.tables("fpsap_first", &fpsapFirstTables)
	c.tables("fpsap_second", &fpsapSecondTables)
	c.p("#endif")
	if err := os.WriteFile(out, []byte(c.b.String()), 0o644); err != nil {
		t.Fatal(err)
	}
	fmt.Printf("wrote %s (%d bytes)\n", out, c.b.Len())
}
