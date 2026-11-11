/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cstdint>

namespace folly::symbolizer {

inline constexpr uint32_t kDwarfReservedInitialLengthStart = 0xfffffff0;

} // namespace folly::symbolizer

// The DWARF constants the symbolizer uses, so it doesn't depend on a
// dwarf.h from libdwarf or elfutils. Values and section numbers are from the
// DWARF Debugging Information Format, Version 5 (https://dwarfstd.org); the
// GNU split DWARF extensions are from https://gcc.gnu.org/wiki/DebugFission.
//
// They are macros with the standard names, spelled token for token as in
// libdwarf's dwarf.h, so a translation unit can include both headers in either
// order: the second definition of each macro is an identical redefinition.

// Tags (DWARF 5 section 7.5.3)
#define DW_TAG_entry_point 0x03
#define DW_TAG_formal_parameter 0x05
#define DW_TAG_lexical_block 0x0b
#define DW_TAG_compile_unit 0x11
#define DW_TAG_common_block 0x1a
#define DW_TAG_inlined_subroutine 0x1d
#define DW_TAG_catch_block 0x25
#define DW_TAG_subprogram 0x2e
#define DW_TAG_try_block 0x32
#define DW_TAG_partial_unit 0x3c
#define DW_TAG_skeleton_unit 0x4a

// Children determination (section 7.5.3)
#define DW_CHILDREN_no 0x00
#define DW_CHILDREN_yes 0x01

// Attribute names (section 7.5.4)
#define DW_AT_location 0x02
#define DW_AT_name 0x03
#define DW_AT_stmt_list 0x10
#define DW_AT_low_pc 0x11
#define DW_AT_high_pc 0x12
#define DW_AT_comp_dir 0x1b
#define DW_AT_abstract_origin 0x31
#define DW_AT_decl_line 0x3b
#define DW_AT_external 0x3f
#define DW_AT_specification 0x47
#define DW_AT_ranges 0x55
#define DW_AT_call_file 0x58
#define DW_AT_call_line 0x59
#define DW_AT_linkage_name 0x6e
#define DW_AT_str_offsets_base 0x72
#define DW_AT_addr_base 0x73
#define DW_AT_rnglists_base 0x74
#define DW_AT_dwo_name 0x76
#define DW_AT_loclists_base 0x8c
// GNU extensions for split DWARF (pre-DWARF 5 DebugFission)
#define DW_AT_GNU_dwo_name 0x2130
#define DW_AT_GNU_dwo_id 0x2131
#define DW_AT_GNU_ranges_base 0x2132
#define DW_AT_GNU_addr_base 0x2133

// Attribute forms (section 7.5.6)
#define DW_FORM_addr 0x01
#define DW_FORM_block2 0x03
#define DW_FORM_block4 0x04
#define DW_FORM_data2 0x05
#define DW_FORM_data4 0x06
#define DW_FORM_data8 0x07
#define DW_FORM_string 0x08
#define DW_FORM_block 0x09
#define DW_FORM_block1 0x0a
#define DW_FORM_data1 0x0b
#define DW_FORM_flag 0x0c
#define DW_FORM_sdata 0x0d
#define DW_FORM_strp 0x0e
#define DW_FORM_udata 0x0f
#define DW_FORM_ref_addr 0x10
#define DW_FORM_ref1 0x11
#define DW_FORM_ref2 0x12
#define DW_FORM_ref4 0x13
#define DW_FORM_ref8 0x14
#define DW_FORM_ref_udata 0x15
#define DW_FORM_indirect 0x16
#define DW_FORM_sec_offset 0x17
#define DW_FORM_exprloc 0x18
#define DW_FORM_flag_present 0x19
#define DW_FORM_strx 0x1a
#define DW_FORM_addrx 0x1b
#define DW_FORM_ref_sup4 0x1c
#define DW_FORM_strp_sup 0x1d
#define DW_FORM_data16 0x1e
#define DW_FORM_line_strp 0x1f
#define DW_FORM_ref_sig8 0x20
#define DW_FORM_implicit_const 0x21
#define DW_FORM_loclistx 0x22
#define DW_FORM_rnglistx 0x23
#define DW_FORM_ref_sup8 0x24
#define DW_FORM_strx1 0x25
#define DW_FORM_strx2 0x26
#define DW_FORM_strx3 0x27
#define DW_FORM_strx4 0x28
#define DW_FORM_addrx1 0x29
#define DW_FORM_addrx2 0x2a
#define DW_FORM_addrx3 0x2b
#define DW_FORM_addrx4 0x2c
// GNU extensions for split DWARF (pre-DWARF 5 DebugFission)
#define DW_FORM_GNU_addr_index 0x1f01
#define DW_FORM_GNU_str_index 0x1f02

// Unit header unit types (section 7.5.1)
#define DW_UT_compile 0x01
#define DW_UT_type 0x02
#define DW_UT_skeleton 0x04
#define DW_UT_split_compile 0x05

// Line number standard opcodes (section 7.22)
#define DW_LNS_copy 0x01
#define DW_LNS_advance_pc 0x02
#define DW_LNS_advance_line 0x03
#define DW_LNS_set_file 0x04
#define DW_LNS_set_column 0x05
#define DW_LNS_negate_stmt 0x06
#define DW_LNS_set_basic_block 0x07
#define DW_LNS_const_add_pc 0x08
#define DW_LNS_fixed_advance_pc 0x09
#define DW_LNS_set_prologue_end 0x0a
#define DW_LNS_set_epilogue_begin 0x0b
#define DW_LNS_set_isa 0x0c

// Line number extended opcodes (section 7.22)
#define DW_LNE_end_sequence 0x01
#define DW_LNE_set_address 0x02
#define DW_LNE_define_file 0x03
#define DW_LNE_set_discriminator 0x04

// Line number header entry formats (section 7.22)
#define DW_LNCT_path 0x1
#define DW_LNCT_directory_index 0x2
#define DW_LNCT_timestamp 0x3
#define DW_LNCT_size 0x4
#define DW_LNCT_MD5 0x5

// Range list entries (section 7.25)
#define DW_RLE_end_of_list 0x00
#define DW_RLE_base_addressx 0x01
#define DW_RLE_startx_endx 0x02
#define DW_RLE_startx_length 0x03
#define DW_RLE_offset_pair 0x04
#define DW_RLE_base_address 0x05
#define DW_RLE_start_end 0x06
#define DW_RLE_start_length 0x07

// Package file section identifiers (section 7.3.5)
#define DW_SECT_INFO 1
#define DW_SECT_ABBREV 3
#define DW_SECT_STR_OFFSETS 6
#define DW_SECT_RNGLISTS 8
