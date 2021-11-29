#include "readDwarf.h"
#include <assert.h>
#include <array>
#include <windows.h>

#include "PEImage.h"
#include "dwarf.h"
#include "mspdb.h"
extern "C" {
	#include "mscvpdb.h"
}

PEImage* DIECursor::img;
abbrevMap_t DIECursor::abbrevMap;
DebugLevel DIECursor::debug;

void DIECursor::setContext(PEImage* img_, DebugLevel debug_)
{
	img = img_;
	debug = debug_;
	abbrevMap.clear();
}

RNGCursor::RNGCursor(const DIECursor& parent_, unsigned long off)
	: parent(parent_)
{
	isRngLists = false;
	if (parent.cu->version >= 5) {
		isRngLists = true;
	}

	if (isRngLists)
	{
		ptr = (byte*)parent.img->debug_rnglists.base;
		end = ptr + parent.img->debug_rnglists.length;
		base = parent.cuOffsets->base_address;
	} else
	{
		ptr = (byte*)parent.img->debug_ranges.base;
		end = ptr + parent.img->debug_ranges.length;
		base = 0;
	}

	ptr += off;
	default_address_size = parent.img->isX64() ? 8 : 4;
}

bool RNGCursor::readNext(RNGEntry &entry)
{
	if (parent.cu->version < 5)
	{
		while (ptr < end)
		{
			entry.pclo = parent.RDAddr(ptr);
			entry.pchi = parent.RDAddr(ptr);

			if (!entry.pclo && !entry.pchi)
			{
				return false;
			}

			if (entry.pclo >= entry.pchi)
			{
				continue;
			}

			entry.addBase(base);
			return true;
		}

		return false;
	}
	else
	{
		while (ptr < end)
		{
			byte rle = *ptr++;
			switch (rle)
			{
			case DW_RLE_end_of_list:
				return false;
			case DW_RLE_start_length:
				entry.pclo = parent.RDAddr(ptr);
				entry.pchi = entry.pclo + LEB128(ptr);
				return true;
			case DW_RLE_start_end:
				entry.pclo = parent.RDAddr(ptr);
				entry.pchi = parent.RDAddr(ptr);
				return true;

			case DW_RLE_base_address:
				base = parent.RDAddr(ptr);
				break;
			case DW_RLE_base_addressx: {
				if (auto* pBase = parent.resolveAddressIndex(LEB128(ptr))) {
					base = parent.RDAddr(pBase);
				}
				break;
			}
			case DW_RLE_startx_endx: {
				auto* pStart = parent.resolveAddressIndex(LEB128(ptr));
				auto* pEnd = parent.resolveAddressIndex(LEB128(ptr));
				if (pStart && pEnd) {
					entry.pclo = parent.RDAddr(pStart);
					entry.pchi = parent.RDAddr(pEnd);
					return true;
				}
				break;
			}
			case DW_RLE_startx_length: {
				auto* pStart = parent.resolveAddressIndex(LEB128(ptr));
				auto len = LEB128(ptr);
				if (pStart) {
					entry.pclo = parent.RDAddr(pStart);
					entry.pchi = entry.pclo + len;
					return true;
				}
				break;
			}
			case DW_RLE_offset_pair:
				entry.pclo = LEB128(ptr);
				entry.pchi = LEB128(ptr);
				entry.addBase(base);
				return true;
			default:
				fprintf(stderr, "ERROR: %s:%d: Unknown rnglist entry value: %d, offs=0x%x DIEOffset=0x%x\n", __FUNCTION__, __LINE__,
						rle, parent.img->debug_rnglists.sectOff(ptr - 1), parent.entryOff);
				assert(false && "unknown rnglists value");
				return false;
			}
		}

		return false;
	}
}

static Location mkInReg(unsigned reg)
{
	Location l;
	l.type = Location::InReg;
	l.reg = reg;
	l.off = 0;
	return l;
}

static Location mkAbs(int off)
{
	Location l;
	l.type = Location::Abs;
	l.reg = 0;
	l.off = off;
	return l;
}

static Location mkRegRel(int reg, int off)
{
	Location l;
	l.type = Location::RegRel;
	l.reg = reg;
	l.off = off;
	return l;
}

LOCCursor::LOCCursor(const DIECursor& parent_, unsigned long off)
	: parent(parent_)
{
	isLocLists = false;
	if (parent.cu->version >= 5) {
		isLocLists = true;
	}

	if (isLocLists)
	{
		ptr = (byte*)parent.img->debug_loclists.base;
		end = ptr + parent.img->debug_loclists.length;
		base = parent.cuOffsets->loclist_base_offset;
		off += base;
	} else
	{
		ptr = (byte*)parent.img->debug_loc.base;
		end = ptr + parent.img->debug_loc.length;
		base = 0;
	}

	ptr += off;
	default_address_size = parent.img->isX64() ? 8 : 4;
}

bool LOCCursor::readNext(LOCEntry& entry)
{
	if (ptr >= end)
		return false;


	if (isLocLists)
	{
		byte type = 0;
		while (ptr < end) {
			entry.ptr = ptr;
			type = *ptr++;
			switch (type) {
			case DW_LLE_end_of_list:
				return false;
			case DW_LLE_base_addressx:
				if (auto* pBase = parent.resolveAddressIndex(LEB128(ptr))) {
					base = parent.RDAddr(pBase);
				}
				continue;
			case DW_LLE_startx_endx: {
				auto* pStart = parent.resolveAddressIndex(LEB128(ptr));
				auto* pEnd = parent.resolveAddressIndex(LEB128(ptr));
				if (pStart && pEnd) {
					entry.beg_offset = parent.RDAddr(pStart);
					entry.end_offset = parent.RDAddr(pEnd);
					goto decode_counted_loc;
				} else {
					goto skip_counted_loc;
				}
			}
			case DW_LLE_startx_length: {
				auto* pStart = parent.resolveAddressIndex(LEB128(ptr));
				auto len = LEB128(ptr);
				if (pStart) {
					entry.beg_offset = parent.RDAddr(pStart);
					entry.end_offset = entry.beg_offset + len;
					goto decode_counted_loc;
				} else {
					goto skip_counted_loc;
				}
			}
			case DW_LLE_offset_pair:
				entry.beg_offset = base + LEB128(ptr);
				entry.end_offset = base + LEB128(ptr);
				goto decode_counted_loc;

			case DW_LLE_default_location:
				entry.beg_offset = 0;
				entry.end_offset = 0;
				entry.isDefault = true;
				goto decode_counted_loc;

			case DW_LLE_base_address:
				base = parent.RDAddr(ptr);
				continue;

			case DW_LLE_start_end:
				entry.beg_offset = parent.RDAddr(ptr);
				entry.end_offset = parent.RDAddr(ptr);
				goto decode_counted_loc;

			case DW_LLE_start_length:
				entry.beg_offset = parent.RDAddr(ptr);
				entry.end_offset = entry.beg_offset + LEB128(ptr);
				goto decode_counted_loc;

			case DW_LLE_view_pair:
				LEB128(ptr);
				LEB128(ptr);
				continue;

			default:
				assert(false && "unknown loclists value");
				return false;
			}
skip_counted_loc:
			auto len = LEB128(ptr);
			ptr += len;
		}

		return false;

decode_counted_loc:
		auto len = LEB128(ptr);

		if (parent.debug & DbgDwarfLocLists)
			fprintf(stderr, "%s:%d: reading loclist entry at offs=%x, type=%d, len=%d, parentOffs=%x\n", __FUNCTION__, __LINE__,
					parent.img->debug_loclists.sectOff(entry.ptr), type, len, parent.entryOff);



		DWARF_Attribute attr;
		attr.type = Block;
		attr.block.len = RD2(ptr);
		attr.block.ptr = ptr;
		entry.loc = decodeLocation(*parent.img, attr);
		ptr += len;
		return true;
	} else
	{
		entry.beg_offset = (unsigned long)RDsize(ptr, default_address_size);
		entry.end_offset = (unsigned long)RDsize(ptr, default_address_size);
		if (entry.eol())
			return false;

		DWARF_Attribute attr;
		attr.type = Block;
		attr.block.len = RD2(ptr);
		attr.block.ptr = ptr;
		entry.loc = decodeLocation(*parent.img, attr);
		ptr += attr.expr.len;
		return true;
	}
}

Location decodeLocation(const PEImage& img, const DWARF_Attribute& attr, const Location* frameBase, int at)
{
	static Location invalid = { Location::Invalid };

	if (attr.type == Const)
		return mkAbs(attr.cons);

	if (attr.type != ExprLoc && attr.type != Block) // same memory layout
		return invalid;

	byte*p = attr.expr.ptr;
	byte*end = attr.expr.ptr + attr.expr.len;

	Location stack[256];
	int stackDepth = 0;
    if (at == DW_AT_data_member_location)
        stack[stackDepth++] = mkAbs(0);

	for (;;)
	{
		if (p >= end)
			break;

		int op = *p++;
		if (op == 0)
			break;

		switch (op)
		{
			case DW_OP_reg0:  case DW_OP_reg1:  case DW_OP_reg2:  case DW_OP_reg3:
			case DW_OP_reg4:  case DW_OP_reg5:  case DW_OP_reg6:  case DW_OP_reg7:
			case DW_OP_reg8:  case DW_OP_reg9:  case DW_OP_reg10: case DW_OP_reg11:
			case DW_OP_reg12: case DW_OP_reg13: case DW_OP_reg14: case DW_OP_reg15:
			case DW_OP_reg16: case DW_OP_reg17: case DW_OP_reg18: case DW_OP_reg19:
			case DW_OP_reg20: case DW_OP_reg21: case DW_OP_reg22: case DW_OP_reg23:
			case DW_OP_reg24: case DW_OP_reg25: case DW_OP_reg26: case DW_OP_reg27:
			case DW_OP_reg28: case DW_OP_reg29: case DW_OP_reg30: case DW_OP_reg31:
				stack[stackDepth++] = mkInReg(op - DW_OP_reg0);
				break;
			case DW_OP_regx:
				stack[stackDepth++] = mkInReg(LEB128(p));
				break;

			case DW_OP_const1u: stack[stackDepth++] = mkAbs(*p++); break;
			case DW_OP_const2u: stack[stackDepth++] = mkAbs(RD2(p)); break;
			case DW_OP_const4u: stack[stackDepth++] = mkAbs(RD4(p)); break;
			case DW_OP_const1s: stack[stackDepth++] = mkAbs((char)*p++); break;
			case DW_OP_const2s: stack[stackDepth++] = mkAbs((short)RD2(p)); break;
			case DW_OP_const4s: stack[stackDepth++] = mkAbs((int)RD4(p)); break;
			case DW_OP_constu:  stack[stackDepth++] = mkAbs(LEB128(p)); break;
			case DW_OP_consts:  stack[stackDepth++] = mkAbs(SLEB128(p)); break;

			case DW_OP_plus_uconst:
				if (stack[stackDepth - 1].is_inreg())
					return invalid;
				stack[stackDepth - 1].off += LEB128(p);
				break;

			case DW_OP_lit0:  case DW_OP_lit1:  case DW_OP_lit2:  case DW_OP_lit3:
			case DW_OP_lit4:  case DW_OP_lit5:  case DW_OP_lit6:  case DW_OP_lit7:
			case DW_OP_lit8:  case DW_OP_lit9:  case DW_OP_lit10: case DW_OP_lit11:
			case DW_OP_lit12: case DW_OP_lit13: case DW_OP_lit14: case DW_OP_lit15:
			case DW_OP_lit16: case DW_OP_lit17: case DW_OP_lit18: case DW_OP_lit19:
			case DW_OP_lit20: case DW_OP_lit21: case DW_OP_lit22: case DW_OP_lit23:
				stack[stackDepth++] = mkAbs(op - DW_OP_lit0);
				break;

			case DW_OP_breg0:  case DW_OP_breg1:  case DW_OP_breg2:  case DW_OP_breg3:
			case DW_OP_breg4:  case DW_OP_breg5:  case DW_OP_breg6:  case DW_OP_breg7:
			case DW_OP_breg8:  case DW_OP_breg9:  case DW_OP_breg10: case DW_OP_breg11:
			case DW_OP_breg12: case DW_OP_breg13: case DW_OP_breg14: case DW_OP_breg15:
			case DW_OP_breg16: case DW_OP_breg17: case DW_OP_breg18: case DW_OP_breg19:
			case DW_OP_breg20: case DW_OP_breg21: case DW_OP_breg22: case DW_OP_breg23:
			case DW_OP_breg24: case DW_OP_breg25: case DW_OP_breg26: case DW_OP_breg27:
			case DW_OP_breg28: case DW_OP_breg29: case DW_OP_breg30: case DW_OP_breg31:
				stack[stackDepth++] = mkRegRel(op - DW_OP_breg0, SLEB128(p));
				break;
			case DW_OP_bregx:
			{
				unsigned reg = LEB128(p);
				stack[stackDepth++] = mkRegRel(reg, SLEB128(p));
			}   break;


			case DW_OP_abs: case DW_OP_neg: case DW_OP_not:
			{
				Location& op1 = stack[stackDepth - 1];
				if (!op1.is_abs())
					return invalid;
				switch (op)
				{
					case DW_OP_abs:   op1 = mkAbs(abs(op1.off)); break;
					case DW_OP_neg:   op1 = mkAbs(-op1.off); break;
					case DW_OP_not:   op1 = mkAbs(~op1.off); break;
				}
			}   break;

			case DW_OP_plus:  // op2 + op1
			{
				Location& op1 = stack[stackDepth - 1];
				Location& op2 = stack[stackDepth - 2];
				// Can add only two offsets or a regrel and an offset.
				if (op2.is_regrel() && op1.is_abs())
					op2 = mkRegRel(op2.reg, op2.off + op1.off);
				else if (op2.is_abs() && op1.is_regrel())
					op2 = mkRegRel(op1.reg, op2.off + op1.off);
				else if (op2.is_abs() && op1.is_abs())
					op2 = mkAbs(op2.off + op1.off);
				else
					return invalid;
				--stackDepth;
			}   break;

			case DW_OP_minus: // op2 - op1
			{
				Location& op1 = stack[stackDepth - 1];
				Location& op2 = stack[stackDepth - 2];
				if (op2.is_regrel() && op1.is_regrel() && op2.reg == op1.reg)
					op2 = mkAbs(0); // X - X == 0
				else if (op2.is_regrel() && op1.is_abs())
					op2 = mkRegRel(op2.reg, op2.off - op1.off);
				else if (op2.is_abs() && op1.is_abs())
					op2 = mkAbs(op2.off - op1.off);
				else
					return invalid;
				--stackDepth;
			}   break;

			case DW_OP_mul:
			{
				Location& op1 = stack[stackDepth - 1];
				Location& op2 = stack[stackDepth - 2];
				if ((op1.is_abs() && op1.off == 0) || (op2.is_abs() && op2.off == 0))
					op2 = mkAbs(0); // X * 0 == 0
				else if (op1.is_abs() && op2.is_abs())
					op2 = mkAbs(op1.off * op2.off);
				else
					return invalid;
				--stackDepth;
			}   break;

			case DW_OP_and:
			{
				Location& op1 = stack[stackDepth - 1];
				Location& op2 = stack[stackDepth - 2];
				if ((op1.is_abs() && op1.off == 0) || (op2.is_abs() && op2.off == 0))
					op2 = mkAbs(0); // X & 0 == 0
				else if (op1.is_abs() && op2.is_abs())
					op2 = mkAbs(op1.off & op2.off);
				else
					return invalid;
				--stackDepth;
			}   break;

			case DW_OP_div: case DW_OP_mod: case DW_OP_shl:
			case DW_OP_shr: case DW_OP_shra: case DW_OP_or:
			case DW_OP_xor:
			case DW_OP_eq:  case DW_OP_ge:  case DW_OP_gt:
			case DW_OP_le:  case DW_OP_lt:  case DW_OP_ne:
			{
				Location& op1 = stack[stackDepth - 1];
				Location& op2 = stack[stackDepth - 2];
				if (!op1.is_abs() || !op2.is_abs()) // can't combine unless both are constants
					return invalid;
				switch (op)
				{
					case DW_OP_div:   op2.off = op2.off / op1.off; break;
					case DW_OP_mod:   op2.off = op2.off % op1.off; break;
					case DW_OP_shl:   op2.off = op2.off << op1.off; break;
					case DW_OP_shr:   op2.off = op2.off >> op1.off; break;
					case DW_OP_shra:  op2.off = op2.off >> op1.off; break;
					case DW_OP_or:    op2.off = op2.off | op1.off; break;
					case DW_OP_xor:   op2.off = op2.off ^ op1.off; break;
					case DW_OP_eq:    op2.off = op2.off == op1.off; break;
					case DW_OP_ge:    op2.off = op2.off >= op1.off; break;
					case DW_OP_gt:    op2.off = op2.off > op1.off; break;
					case DW_OP_le:    op2.off = op2.off <= op1.off; break;
					case DW_OP_lt:    op2.off = op2.off < op1.off; break;
					case DW_OP_ne:    op2.off = op2.off != op1.off; break;
				}
				--stackDepth;
			}   break;

			case DW_OP_fbreg:
			{
				if (!frameBase)
					return invalid;

				Location loc;
				if (frameBase->is_inreg()) // ok in frame base specification, per DWARF4 spec #3.3.5
					loc = mkRegRel(frameBase->reg, SLEB128(p));
				else if (frameBase->is_regrel())
					loc = mkRegRel(frameBase->reg, frameBase->off + SLEB128(p));
				else
					return invalid;
				stack[stackDepth++] = loc;
			}   break;

			case DW_OP_dup:   stack[stackDepth] = stack[stackDepth - 1]; stackDepth++; break;
			case DW_OP_drop:  stackDepth--; break;
			case DW_OP_over:  stack[stackDepth] = stack[stackDepth - 2]; stackDepth++; break;
			case DW_OP_pick:  stack[stackDepth++] = stack[*p]; break;
			case DW_OP_swap:  { Location tmp = stack[stackDepth - 1]; stack[stackDepth - 1] = stack[stackDepth - 2]; stack[stackDepth - 2] = tmp; } break;
			case DW_OP_rot:   { Location tmp = stack[stackDepth - 1]; stack[stackDepth - 1] = stack[stackDepth - 2]; stack[stackDepth - 2] = stack[stackDepth - 3]; stack[stackDepth - 3] = tmp; } break;

			case DW_OP_addr:
				stack[stackDepth++] = mkAbs(RD4(p)); // TODO: 64-bit
				break;

			case DW_OP_skip:
			{
				unsigned off = RD2(p);
				p = p + off;
			}   break;

			case DW_OP_bra:
			{
				Location& op1 = stack[stackDepth - 1];
				if (!op1.is_abs())
					return invalid;
				if (op1.off != 0)
				{
					unsigned off = RD2(p);
					p = p + off;
				}
				--stackDepth;
			}   break;

			case DW_OP_nop:
				break;

			case DW_OP_call_frame_cfa: // assume ebp+8/rbp+16
				stack[stackDepth++] = Location{ Location::RegRel, DW_REG_CFA, 0 };
				break;

			case DW_OP_deref:
			case DW_OP_deref_size:
			case DW_OP_push_object_address:
			case DW_OP_call2:
			case DW_OP_call4:
			case DW_OP_form_tls_address:
			case DW_OP_call_ref:
			case DW_OP_bit_piece:
			case DW_OP_implicit_value:
			case DW_OP_stack_value:
			default:
				return invalid;
		}
	}

	assert(stackDepth > 0);
	return stack[0];
}

void mergeAbstractOrigin(DWARF_InfoData& id, const DIECursor& parent)
{
	DIECursor specCursor(parent, id.abstract_origin);
	DWARF_InfoData idspec;
	specCursor.readNext(idspec);
	// assert seems invalid, combination DW_TAG_member and DW_TAG_variable found in the wild
	// assert(id.tag == idspec.tag);
	if (idspec.abstract_origin)
		mergeAbstractOrigin(idspec, parent);
	if (idspec.specification)
		mergeSpecification(idspec, parent);
	id.merge(idspec);
}

void mergeSpecification(DWARF_InfoData& id, const DIECursor& parent)
{
	DIECursor specCursor(parent, id.specification);
	DWARF_InfoData idspec;
	specCursor.readNext(idspec);
	//assert seems invalid, combination DW_TAG_member and DW_TAG_variable found in the wild
	//assert(id.tag == idspec.tag);
	if (idspec.abstract_origin)
		mergeAbstractOrigin(idspec, parent);
	if (idspec.specification)
		mergeSpecification(idspec, parent);
	id.merge(idspec);
}

byte* DIECursor::resolveAddressIndex(unsigned long idx) const
{
	auto addrSize = cu->address_size == 4 ? 4 : 8;
	auto offset = cuOffsets->addr_base_offset + addrSize * idx;
	if (offset + addrSize >= img->debug_addr.length) {
		assert(false && "invalid addr index");
		return nullptr;
	}

	return (byte*)img->debug_addr.base + offset;
}

DIECursor::DIECursor(DWARF_CompilationUnit* cu_, CompilationUnitOffsets *cuOffsets_, byte* ptr_)
{
	cu = cu_;
	cuOffsets = cuOffsets_;
	ptr = ptr_;
	level = 0;
	hasChild = false;
	sibling = 0;
}

DIECursor::DIECursor(const DIECursor& parent, byte* ptr_)
	: DIECursor(parent)
{
	ptr = ptr_;
}

void DIECursor::gotoSibling()
{
	if (sibling)
	{
		// use sibling pointer, if available
		ptr = sibling;
		hasChild = false;
	}
	else if (hasChild)
	{
		int currLevel = level;
		level = currLevel + 1;
		hasChild = false;

		DWARF_InfoData dummy;
		// read untill we pop back to the level we were at
		while (level > currLevel)
			readNext(dummy, true);
	}
}

bool DIECursor::readSibling(DWARF_InfoData& id)
{
    gotoSibling();
	return readNext(id, true);
}

DIECursor DIECursor::getSubtreeCursor()
{
	if (hasChild)
	{
		DIECursor subtree = *this;
		subtree.level = 0;
		subtree.hasChild = false;
		return subtree;
	}
	else // Return invalid cursor
	{
		DIECursor subtree = *this;
		subtree.level = -1;
		return subtree;
	}
}

bool DIECursor::readNext(DWARF_InfoData& id, bool stopAtNull)
{
	id.clear();

	if (hasChild)
		++level;

	for (;;)
	{
		if (level == -1)
			return false; // we were already at the end of the subtree

		if (ptr >= ((byte*)cu + sizeof(cu->unit_length) + cu->unit_length))
			return false; // root of the tree does not have a null terminator, but we know the length

		id.entryPtr = ptr;
		entryOff = img->debug_info.sectOff(ptr);
		id.code = LEB128(ptr);
		if (id.code == 0)
		{
			--level; // pop up one level
			if (stopAtNull)
			{
				hasChild = false;
				return false;
			}
			continue; // read the next DIE
		}

		break;
	}

	byte* abbrev = getDWARFAbbrev(cu->debug_abbrev_offset, id.code);
	if (!abbrev) {
		fprintf(stderr, "ERROR: %s:%d: unknown abbrev: num=%d off=%x\n", __FUNCTION__, __LINE__, id.code, entryOff);
		assert(abbrev);
		return false;
	}


	id.abbrev = abbrev;
	id.tag = LEB128(abbrev);
	id.hasChild = *abbrev++;
	
	if (debug & DbgDwarfAttrRead)
		fprintf(stderr, "%s:%d: offs=%d level=%d tag=%d abbrev=%d\n", __FUNCTION__, __LINE__, entryOff, level, id.tag, id.code);

	int attr, form;
	for (;;)
	{
		attr = LEB128(abbrev);
		form = LEB128(abbrev);

		if (attr == 0 && form == 0)
			break;

		if (debug & DbgDwarfAttrRead)
			fprintf(stderr, "%s:%d: offs=%x, attr=%d, form=%d\n", __FUNCTION__, __LINE__, attr, form, img->debug_info.sectOff(ptr));

		while (form == DW_FORM_indirect) {
			form = LEB128(ptr);
			if (debug & DbgDwarfAttrRead)
				fprintf(stderr, "%s:%d: attr=%d, form=%d\n", __FUNCTION__, __LINE__, attr, form);
		}

		DWARF_Attribute a;
		switch (form)
		{
			case DW_FORM_addr:           a.type = Addr; a.addr = RDAddr(ptr); break;
			case DW_FORM_addrx: {
				auto p = resolveAddressIndex(LEB128(ptr));
				a.type = Addr;
				a.addr = RDAddr(p);
				break;
			}
			case DW_FORM_addrx1:
			case DW_FORM_addrx2:
			case DW_FORM_addrx3:
			case DW_FORM_addrx4: {
				auto p = resolveAddressIndex(LEB128(ptr));
				a.type = Addr;
				a.addr = RDsize(p, 1 + (form - DW_FORM_addrx1));
				break;
			}
			case DW_FORM_block:          a.type = Block; a.block.len = LEB128(ptr); a.block.ptr = ptr; ptr += a.block.len; break;
			case DW_FORM_block1:         a.type = Block; a.block.len = *ptr++;      a.block.ptr = ptr; ptr += a.block.len; break;
			case DW_FORM_block2:         a.type = Block; a.block.len = RD2(ptr);   a.block.ptr = ptr; ptr += a.block.len; break;
			case DW_FORM_block4:         a.type = Block; a.block.len = RD4(ptr);   a.block.ptr = ptr; ptr += a.block.len; break;
			case DW_FORM_data1:          a.type = Const; a.cons = *ptr++; break;
			case DW_FORM_data2:          a.type = Const; a.cons = RD2(ptr); break;
			case DW_FORM_data4:          a.type = Const; a.cons = RD4(ptr); break;
			case DW_FORM_data8:          a.type = Const; a.cons = RD8(ptr); break;
			case DW_FORM_data16:         a.type = Const16; memcpy(a.cons16, ptr, 16); ptr += 16; break;
			case DW_FORM_sdata:          a.type = Const; a.cons = SLEB128(ptr); break;
			case DW_FORM_udata:          a.type = Const; a.cons = LEB128(ptr); break;
			case DW_FORM_string:         a.type = String; a.string = (const char*)ptr; ptr += strlen(a.string) + 1; break;
            case DW_FORM_strp:           a.type = String; a.string = (const char*)(img->debug_str.base + RDsize(ptr, cu->isDWARF64() ? 8 : 4)); break;
			case DW_FORM_flag:           a.type = Flag; a.flag = (*ptr++ != 0); break;
			case DW_FORM_flag_present:   a.type = Flag; a.flag = true; break;
			case DW_FORM_ref1:           a.type = Ref; a.ref = (byte*)cu + *ptr++; break;
			case DW_FORM_ref2:           a.type = Ref; a.ref = (byte*)cu + RD2(ptr); break;
			case DW_FORM_ref4:           a.type = Ref; a.ref = (byte*)cu + RD4(ptr); break;
			case DW_FORM_ref8:           a.type = Ref; a.ref = (byte*)cu + RD8(ptr); break;
			case DW_FORM_ref_udata:      a.type = Ref; a.ref = (byte*)cu + LEB128(ptr); break;
			case DW_FORM_ref_addr:       a.type = Ref; a.ref = (byte*)img->debug_info.base + (cu->isDWARF64() ? RD8(ptr) : RD4(ptr)); break;
			case DW_FORM_ref_sig8:       a.type = Invalid; ptr += 8;  break;
			case DW_FORM_exprloc:        a.type = ExprLoc; a.expr.len = LEB128(ptr); a.expr.ptr = ptr; ptr += a.expr.len; break;
			case DW_FORM_sec_offset:     a.type = SecOffset;  a.sec_offset = cu->isDWARF64() ? RD8(ptr) : RD4(ptr); break;
			case DW_FORM_line_strp:      a.type = String; a.string = (const char*)img->debug_line_str.base + (cu->isDWARF64() ? RD8(ptr) : RD4(ptr)); break;
			case DW_FORM_implicit_const: a.type = Const; a.cons = LEB128(abbrev); break;
			default:
				fprintf(stderr, "ERROR: %s:%d: Unsupported DWARF attribute form offs=%x %d for tag %d (abbrev %d)", __FUNCTION__, __LINE__,
						entryOff, form, id.tag, id.code);
				assert(false && "Unsupported DWARF attribute form");
				return false;
		}

		switch (attr)
		{
			case DW_AT_byte_size:
				assert(a.type == Const || a.type == Ref || a.type == ExprLoc);
				if (a.type == Const) // TODO: other types not supported yet
					id.byte_size = a.cons;
				break;
			case DW_AT_sibling:   assert(a.type == Ref); id.sibling = a.ref; break;
			case DW_AT_encoding:  assert(a.type == Const); id.encoding = a.cons; break;
			case DW_AT_name:      assert(a.type == String); id.name = a.string; break;
			case DW_AT_MIPS_linkage_name: assert(a.type == String); id.linkage_name = a.string; break;
			case DW_AT_comp_dir:  assert(a.type == String); id.dir = a.string; break;
			case DW_AT_low_pc:    assert(a.type == Addr); id.pclo = a.addr; break;
			case DW_AT_high_pc:
				if (a.type == Addr)
					id.pchi = a.addr;
				else if (a.type == Const)
					id.pchi = id.pclo + a.cons;
				else
					assert(false);
			    break;
		    case DW_AT_entry_pc:
			    if (a.type == Addr)
				    id.pcentry = a.addr;
			    else if (a.type == Const)
				    id.pcentry = id.pclo + a.cons;
			    else
				    assert(false);
			    break;
			case DW_AT_ranges:
				if (a.type == SecOffset)
					id.ranges = a.sec_offset;
				else if (a.type == Const)
					id.ranges = a.cons;
				else
					assert(false);
			    break;
			case DW_AT_type:      assert(a.type == Ref); id.type = a.ref; break;
			case DW_AT_inline:    assert(a.type == Const); id.inlined = a.cons; break;
			case DW_AT_external:  assert(a.type == Flag); id.external = a.flag; break;
			case DW_AT_upper_bound:
				assert(a.type == Const || a.type == Ref || a.type == ExprLoc || a.type == Block);
				if (a.type == Const) // TODO: other types not supported yet
					id.upper_bound = a.cons;
				break;
			case DW_AT_lower_bound:
				assert(a.type == Const || a.type == Ref || a.type == ExprLoc);
				if (a.type == Const)
				{
					// TODO: other types not supported yet
					id.lower_bound = a.cons;
					id.has_lower_bound = true;
				}
				break;
			case DW_AT_containing_type: assert(a.type == Ref); id.containing_type = a.ref; break;
			case DW_AT_specification: assert(a.type == Ref); id.specification = a.ref; break;
			case DW_AT_abstract_origin: assert(a.type == Ref); id.abstract_origin = a.ref; break;
			case DW_AT_data_member_location: id.member_location = a; break;
			case DW_AT_location: id.location = a; break;
			case DW_AT_frame_base: id.frame_base = a; break;
			case DW_AT_language: assert(a.type == Const); id.language = a.cons; break;
			case DW_AT_const_value:
				switch (a.type)
				{
				case Const:
					id.const_value = a.cons;
					id.has_const_value = true;
					break;

				// TODO: handle these
				case String:
				case Block:
					break;

				default:
					assert(false);
					break;
				}
				break;
			case DW_AT_str_offsets_base: assert(a.type == SecOffset); this->cuOffsets->str_base_offset = a.sec_offset; break;
			case DW_AT_addr_base: assert(a.type == SecOffset); this->cuOffsets->addr_base_offset = a.sec_offset; break;
			case DW_AT_loclists_base: assert(a.type == SecOffset); this->cuOffsets->loclist_base_offset = a.sec_offset; break;
		    case DW_AT_artificial:
				assert(a.type == Flag);
				id.has_artificial = true;
				id.is_artificial = true;
				break;
		}
	}

	hasChild = id.hasChild != 0;
	sibling = id.sibling;

	return true;
}

byte* DIECursor::getDWARFAbbrev(unsigned off, unsigned findcode)
{
	if (!img->debug_abbrev.isPresent())
		return 0;

	std::pair<unsigned, unsigned> key = std::make_pair(off, findcode);
	abbrevMap_t::iterator it = abbrevMap.find(key);
	if (it != abbrevMap.end())
	{
		return it->second;
	}

	byte* p = (byte*)img->debug_abbrev.base + off;
	byte* end = (byte*)img->debug_abbrev.base + img->debug_abbrev.length;
	while (p < end)
	{
		int code = LEB128(p);
		if (code == findcode)
		{
			abbrevMap.insert(std::make_pair(key, p));
			return p;
		}
		if (code == 0)
			return 0;

		int tag = LEB128(p);
		int hasChild = *p++;

		// skip attributes
		int attr, form;
		do
		{
			attr = LEB128(p);
			form = LEB128(p);
			if (form == DW_FORM_implicit_const)
				LEB128(p);
		} while (attr || form);
	}
	return 0;
}
