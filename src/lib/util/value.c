/*
 *   This library is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Lesser General Public
 *   License as published by the Free Software Foundation; either
 *   version 2.1 of the License, or (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *   Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 * @file value.c
 * @brief Manipulate boxed values representing all internal data types.
 *
 * There are three notional data formats used in the server:
 *
 * - #fr_value_box_t are the INTERNAL format.  This is usually close to the in-memory representation
 *   of the data, though uint32s and IPs are always converted to/from octets with BIG ENDIAN
 *   uint8 ordering for consistency.
 *   - #fr_value_box_cast is used to convert (cast) #fr_value_box_t between INTERNAL formats.
 *   - #fr_value_box_strdup* is used to ingest nul terminated strings into the INTERNAL format.
 *   - #fr_value_box_memdup* is used to ingest binary data into the INTERNAL format.
 *
 * - NETWORK format is the format we send/receive on the wire.  It is not a perfect representation
 *   of data packing for all protocols, so you will likely need to overload conversion for some types.
 *   - fr_value_box_to_network is used to covert INTERNAL format data to generic NETWORK format data.
 *     For uint32s, IP addresses etc... This means BIG ENDIAN uint8 ordering.
 *   - fr_value_box_from_network is used to convert packet buffer fragments in NETWORK format to
 *     INTERNAL format.
 *
 * - PRESENTATION format is what we print to the screen, and what we get from the user, databases
 *   and configuration files.
 *   - #fr_value_box_asprint is used to convert INTERNAL format PRESENTATION format.
 *   - #fr_value_box_from_str is used to convert from INTERNAL to PRESENTATION format.
 *
 * @copyright 2014-2017 The FreeRADIUS server project
 * @copyright 2017 Arran Cudbard-Bell <a.cudbardb@freeradius.org>
 */
RCSID("$Id$")

#include <freeradius-devel/libradius.h>
#include <freeradius-devel/rad_assert.h>
#include <ctype.h>

/** Sanity checks
 *
 * There should never be an instance where these fail.
 */
static_assert(SIZEOF_MEMBER(fr_value_box_t, datum.ip.addr.v4.s_addr) == 4,
	      "in_addr.s_addr has unexpected length");
static_assert(SIZEOF_MEMBER(fr_value_box_t, datum.ip.addr.v6.s6_addr) == 16,
	      "in6_addr.s6_addr has unexpected length");
static_assert(SIZEOF_MEMBER(fr_value_box_t, datum.ifid) == 8,
	      "datum.ifid has unexpected length");
static_assert(SIZEOF_MEMBER(fr_value_box_t, datum.ether) == 6,
	      "datum.ether has unexpected length");

static_assert(SIZEOF_MEMBER(fr_value_box_t, datum.boolean) == 1,
	      "datum.boolean has unexpected length");
static_assert(SIZEOF_MEMBER(fr_value_box_t, datum.uint8) == 1,
	      "datum.uint8 has unexpected length");
static_assert(SIZEOF_MEMBER(fr_value_box_t, datum.uint16) == 2,
	      "datum.uint16 has unexpected length");
static_assert(SIZEOF_MEMBER(fr_value_box_t, datum.uint32) == 4,
	      "datum.uint32 has unexpected length");
static_assert(SIZEOF_MEMBER(fr_value_box_t, datum.uint64) == 8,
	      "datum.uint64 has unexpected length");

static_assert(SIZEOF_MEMBER(fr_value_box_t, datum.int8) == 1,
	      "datum.int16 has unexpected length");
static_assert(SIZEOF_MEMBER(fr_value_box_t, datum.int16) == 2,
	      "datum.int16 has unexpected length");
static_assert(SIZEOF_MEMBER(fr_value_box_t, datum.int32) == 4,
	      "datum.int32 has unexpected length");
static_assert(SIZEOF_MEMBER(fr_value_box_t, datum.int64) == 8,
	      "datum.int64 has unexpected length");

static_assert(SIZEOF_MEMBER(fr_value_box_t, datum.float32) == 4,
	      "datum.float32 has unexpected length");
static_assert(SIZEOF_MEMBER(fr_value_box_t, datum.float64) == 8,
	      "datum.float64 has unexpected length");

/** How many bytes on-the-wire would a #fr_value_box_t value consume
 *
 * This is for the generic NETWORK format.  For field sizes in the in-memory
 * structure use #fr_value_box_field_sizes.
 *
 * @note Don't use this array directly when determining the length
 *	 that would be consumed by the on-the-wire representation.
 *	 Use #fr_value_box_network_length instead, as that deals with variable
 *	 length attributes too.
 */
static size_t const fr_value_box_network_sizes[FR_TYPE_MAX + 1][2] = {
	[FR_TYPE_INVALID]			= {~0, 0},

	[FR_TYPE_STRING]			= {0, ~0},
	[FR_TYPE_OCTETS]			= {0, ~0},

	[FR_TYPE_IPV4_ADDR]			= {4, 4},
	[FR_TYPE_IPV4_PREFIX]			= {6, 6},
	[FR_TYPE_IPV6_ADDR]			= {16, 16},
	[FR_TYPE_IPV6_PREFIX]			= {18, 18},
	[FR_TYPE_IFID]				= {8, 8},
	[FR_TYPE_ETHERNET]			= {6, 6},

	[FR_TYPE_BOOL]				= {1, 1},
	[FR_TYPE_UINT8]				= {1, 1},
	[FR_TYPE_UINT16]			= {2, 2},
	[FR_TYPE_UINT32]			= {4, 4},
	[FR_TYPE_UINT64]			= {8, 8},

	[FR_TYPE_INT8]				= {1, 1},
	[FR_TYPE_INT16]				= {2, 2},
	[FR_TYPE_INT32]				= {4, 4},
	[FR_TYPE_INT64]				= {8, 8},

	[FR_TYPE_FLOAT32]			= {4, 4},
	[FR_TYPE_FLOAT64]			= {8, 8},

	[FR_TYPE_DATE]				= {4, 4},
	[FR_TYPE_DATE_MILLISECONDS]		= {8, 8},
	[FR_TYPE_DATE_MICROSECONDS]		= {8, 8},
	[FR_TYPE_DATE_NANOSECONDS]		= {8, 8},

	[FR_TYPE_ABINARY]			= {32, ~0},
	[FR_TYPE_MAX]				= {~0, 0}		//!< Ensure array covers all types.
};

/** How many uint8s wide each of the value data fields are
 *
 * This is useful when copying a value from a fr_value_box_t to a memory
 * location passed as a void *.
 */
size_t const fr_value_box_field_sizes[] = {
	[FR_TYPE_STRING]			= SIZEOF_MEMBER(fr_value_box_t, datum.strvalue),
	[FR_TYPE_OCTETS]			= SIZEOF_MEMBER(fr_value_box_t, datum.octets),

	[FR_TYPE_IPV4_ADDR]			= SIZEOF_MEMBER(fr_value_box_t, datum.ip),
	[FR_TYPE_IPV4_PREFIX]			= SIZEOF_MEMBER(fr_value_box_t, datum.ip),
	[FR_TYPE_IPV6_ADDR]			= SIZEOF_MEMBER(fr_value_box_t, datum.ip),
	[FR_TYPE_IPV6_PREFIX]			= SIZEOF_MEMBER(fr_value_box_t, datum.ip),
	[FR_TYPE_IFID]				= SIZEOF_MEMBER(fr_value_box_t, datum.ifid),
	[FR_TYPE_ETHERNET]			= SIZEOF_MEMBER(fr_value_box_t, datum.ether),

	[FR_TYPE_BOOL]				= SIZEOF_MEMBER(fr_value_box_t, datum.boolean),
	[FR_TYPE_UINT8]				= SIZEOF_MEMBER(fr_value_box_t, datum.uint8),
	[FR_TYPE_UINT16]			= SIZEOF_MEMBER(fr_value_box_t, datum.uint16),
	[FR_TYPE_UINT32]			= SIZEOF_MEMBER(fr_value_box_t, datum.uint32),
	[FR_TYPE_UINT64]			= SIZEOF_MEMBER(fr_value_box_t, datum.uint64),

	[FR_TYPE_INT8]				= SIZEOF_MEMBER(fr_value_box_t, datum.int8),
	[FR_TYPE_INT16]				= SIZEOF_MEMBER(fr_value_box_t, datum.int16),
	[FR_TYPE_INT32]				= SIZEOF_MEMBER(fr_value_box_t, datum.int32),
	[FR_TYPE_INT64]				= SIZEOF_MEMBER(fr_value_box_t, datum.int64),

	[FR_TYPE_FLOAT32]			= SIZEOF_MEMBER(fr_value_box_t, datum.float32),
	[FR_TYPE_FLOAT64]			= SIZEOF_MEMBER(fr_value_box_t, datum.float64),

	[FR_TYPE_DATE]				= SIZEOF_MEMBER(fr_value_box_t, datum.date),
	[FR_TYPE_DATE_MILLISECONDS]		= SIZEOF_MEMBER(fr_value_box_t, datum.date_milliseconds),
	[FR_TYPE_DATE_MICROSECONDS]		= SIZEOF_MEMBER(fr_value_box_t, datum.date_microseconds),
	[FR_TYPE_DATE_NANOSECONDS]		= SIZEOF_MEMBER(fr_value_box_t, datum.date_nanoseconds),

	[FR_TYPE_TIMEVAL]			= SIZEOF_MEMBER(fr_value_box_t, datum.timeval),
	[FR_TYPE_SIZE]				= SIZEOF_MEMBER(fr_value_box_t, datum.size),

	[FR_TYPE_ABINARY]			= SIZEOF_MEMBER(fr_value_box_t, datum.filter),
	[FR_TYPE_MAX]				= 0	//!< Ensure array covers all types.
};

/** Where the value starts in the #fr_value_box_t
 *
 */
size_t const fr_value_box_offsets[] = {
	[FR_TYPE_STRING]			= offsetof(fr_value_box_t, datum.strvalue),
	[FR_TYPE_OCTETS]			= offsetof(fr_value_box_t, datum.octets),

	[FR_TYPE_IPV4_ADDR]			= offsetof(fr_value_box_t, datum.ip),
	[FR_TYPE_IPV4_PREFIX]			= offsetof(fr_value_box_t, datum.ip),
	[FR_TYPE_IPV6_ADDR]			= offsetof(fr_value_box_t, datum.ip),
	[FR_TYPE_IPV6_PREFIX]			= offsetof(fr_value_box_t, datum.ip),
	[FR_TYPE_IFID]				= offsetof(fr_value_box_t, datum.ifid),
	[FR_TYPE_ETHERNET]			= offsetof(fr_value_box_t, datum.ether),

	[FR_TYPE_BOOL]				= offsetof(fr_value_box_t, datum.boolean),
	[FR_TYPE_UINT8]				= offsetof(fr_value_box_t, datum.uint8),
	[FR_TYPE_UINT16]			= offsetof(fr_value_box_t, datum.uint16),
	[FR_TYPE_UINT32]			= offsetof(fr_value_box_t, datum.uint32),
	[FR_TYPE_UINT64]			= offsetof(fr_value_box_t, datum.uint64),

	[FR_TYPE_INT8]				= offsetof(fr_value_box_t, datum.int8),
	[FR_TYPE_INT16]				= offsetof(fr_value_box_t, datum.int16),
	[FR_TYPE_INT32]				= offsetof(fr_value_box_t, datum.int32),
	[FR_TYPE_INT64]				= offsetof(fr_value_box_t, datum.int64),

	[FR_TYPE_FLOAT32]			= offsetof(fr_value_box_t, datum.float32),
	[FR_TYPE_FLOAT64]			= offsetof(fr_value_box_t, datum.float64),

	[FR_TYPE_DATE]				= offsetof(fr_value_box_t, datum.date),
	[FR_TYPE_DATE_MILLISECONDS]		= offsetof(fr_value_box_t, datum.date_milliseconds),
	[FR_TYPE_DATE_MICROSECONDS]		= offsetof(fr_value_box_t, datum.date_microseconds),
	[FR_TYPE_DATE_NANOSECONDS]		= offsetof(fr_value_box_t, datum.date_nanoseconds),

	[FR_TYPE_TIMEVAL]			= offsetof(fr_value_box_t, datum.timeval),
	[FR_TYPE_SIZE]				= offsetof(fr_value_box_t, datum.size),

	[FR_TYPE_ABINARY]			= offsetof(fr_value_box_t, datum.filter),
	[FR_TYPE_MAX]				= 0	//!< Ensure array covers all types.
};

/** Allocate a value box of a specific type
 *
 * Allocates memory for the box, and sets the length of the value
 * for fixed length types.
 *
 * @param[in] ctx	to allocate the value_box in.
 * @param[in] type	of value.
 * @return
 *	- A new fr_value_box_t.
 *	- NULL on error.
 */
fr_value_box_t *fr_value_box_alloc(TALLOC_CTX *ctx, fr_type_t type)
{
	fr_value_box_t *value;

	value = talloc_zero(ctx, fr_value_box_t);
	if (!value) return NULL;
	value->type = type;

	return value;
}

/** Clear/free any existing value
 *
 * @note Do not use on uninitialised memory.
 *
 * @param[in] data to clear.
 */
inline void fr_value_box_clear(fr_value_box_t *data)
{
	switch (data->type) {
	case FR_TYPE_OCTETS:
	case FR_TYPE_STRING:
		TALLOC_FREE(data->datum.ptr);
		data->datum.length = 0;
		break;

	case FR_TYPE_STRUCTURAL:
		if (!fr_cond_assert(0)) return;

	case FR_TYPE_INVALID:
		return;

	default:
		memset(&data->datum, 0, dict_attr_sizes[data->type][1]);
		break;
	}

	data->tainted = false;
	data->type = FR_TYPE_INVALID;
}

/** Copy flags and type data from one value box to another
 *
 * @param[in] dst to copy flags to
 * @param[in] src of data.
 */
static inline void fr_value_box_copy_meta(fr_value_box_t *dst, fr_value_box_t const *src)
{
	switch (src->type) {
	case FR_TYPE_VARIABLE_SIZE:
		dst->datum.length = src->datum.length;
		break;

	default:
		break;
	}

	dst->enumv = src->enumv;
	dst->type = src->type;
	dst->tainted = src->tainted;
}

/** Compare two values
 *
 * @param[in] a Value to compare.
 * @param[in] b Value to compare.
 * @return
 *	- -1 if a is less than b.
 *	- 0 if both are equal.
 *	- 1 if a is more than b.
 *	- < -1 on failure.
 */
int fr_value_box_cmp(fr_value_box_t const *a, fr_value_box_t const *b)
{
	int compare = 0;

	if (!fr_cond_assert(a->type != FR_TYPE_INVALID)) return -1;
	if (!fr_cond_assert(b->type != FR_TYPE_INVALID)) return -1;

	if (a->type != b->type) {
		fr_strerror_printf("%s: Can't compare values of different types", __FUNCTION__);
		return -2;
	}

	/*
	 *	After doing the previous check for special comparisons,
	 *	do the per-type comparison here.
	 */
	switch (a->type) {
	case FR_TYPE_VARIABLE_SIZE:
	{
		size_t length;

		if (a->datum.length < b->datum.length) {
			length = a->datum.length;
		} else {
			length = b->datum.length;
		}

		if (length) {
			compare = memcmp(a->datum.octets, b->datum.octets, length);
			if (compare != 0) break;
		}

		/*
		 *	Contents are the same.  The return code
		 *	is therefore the difference in lengths.
		 *
		 *	i.e. "0x00" is smaller than "0x0000"
		 */
		compare = a->datum.length - b->datum.length;
	}
		break;

		/*
		 *	Short-hand for simplicity.
		 */
#define CHECK(_type) if (a->datum._type < b->datum._type)   { compare = -1; \
		} else if (a->datum._type > b->datum._type) { compare = +1; }

	case FR_TYPE_BOOL:
		CHECK(boolean);
		break;

	case FR_TYPE_DATE:
		CHECK(date);
		break;

	case FR_TYPE_UINT8:
		CHECK(uint8);
		break;

	case FR_TYPE_UINT16:
		CHECK(uint16);
		break;

	case FR_TYPE_UINT32:
		CHECK(int32);
		break;

	case FR_TYPE_UINT64:
		CHECK(uint64);
		break;

	case FR_TYPE_INT8:
		CHECK(int8);
		break;

	case FR_TYPE_INT16:
		CHECK(int16);
		break;

	case FR_TYPE_INT32:
		CHECK(int32);
		break;

	case FR_TYPE_INT64:
		CHECK(int64);
		break;

	case FR_TYPE_SIZE:
		CHECK(size);
		break;

	case FR_TYPE_TIMEVAL:
		compare = fr_timeval_cmp(&a->datum.timeval, &b->datum.timeval);
		break;

	case FR_TYPE_FLOAT32:
		CHECK(float32);
		break;

	case FR_TYPE_FLOAT64:
		CHECK(float64);
		break;

	case FR_TYPE_DATE_MILLISECONDS:
		CHECK(date_milliseconds);
		break;

	case FR_TYPE_DATE_MICROSECONDS:
		CHECK(date_microseconds);
		break;

	case FR_TYPE_DATE_NANOSECONDS:
		CHECK(date_nanoseconds);
		break;

	case FR_TYPE_ETHERNET:
		compare = memcmp(a->datum.ether, b->datum.ether, sizeof(a->datum.ether));
		break;

	case FR_TYPE_IPV4_ADDR:
	case FR_TYPE_IPV4_PREFIX:
	case FR_TYPE_IPV6_ADDR:
	case FR_TYPE_IPV6_PREFIX:
		compare = memcmp(&a->datum.ip, &b->datum.ip, sizeof(a->datum.ip));
		break;

	case FR_TYPE_IFID:
		compare = memcmp(a->datum.ifid, b->datum.ifid, sizeof(a->datum.ifid));
		break;

	/*
	 *	These should be handled at some point
	 */
	case FR_TYPE_NON_VALUES:
		(void)fr_cond_assert(0);	/* unknown type */
		return -2;

	/*
	 *	Do NOT add a default here, as new types are added
	 *	static analysis will warn us they're not handled
	 */
	}

	if (compare > 0) return 1;
	if (compare < 0) return -1;
	return 0;
}

/*
 *	We leverage the fact that IPv4 and IPv6 prefixes both
 *	have the same format:
 *
 *	reserved, prefix-len, data...
 */
static int fr_value_box_cidr_cmp_op(FR_TOKEN op, int uint8s,
				 uint8_t a_net, uint8_t const *a,
				 uint8_t b_net, uint8_t const *b)
{
	int i, common;
	uint32_t mask;

	/*
	 *	Handle the case of netmasks being identical.
	 */
	if (a_net == b_net) {
		int compare;

		compare = memcmp(a, b, uint8s);

		/*
		 *	If they're identical return true for
		 *	identical.
		 */
		if ((compare == 0) &&
		    ((op == T_OP_CMP_EQ) ||
		     (op == T_OP_LE) ||
		     (op == T_OP_GE))) {
			return true;
		}

		/*
		 *	Everything else returns false.
		 *
		 *	10/8 == 24/8  --> false
		 *	10/8 <= 24/8  --> false
		 *	10/8 >= 24/8  --> false
		 */
		return false;
	}

	/*
	 *	Netmasks are different.  That limits the
	 *	possible results, based on the operator.
	 */
	switch (op) {
	case T_OP_CMP_EQ:
		return false;

	case T_OP_NE:
		return true;

	case T_OP_LE:
	case T_OP_LT:	/* 192/8 < 192.168/16 --> false */
		if (a_net < b_net) {
			return false;
		}
		break;

	case T_OP_GE:
	case T_OP_GT:	/* 192/16 > 192.168/8 --> false */
		if (a_net > b_net) {
			return false;
		}
		break;

	default:
		return false;
	}

	if (a_net < b_net) {
		common = a_net;
	} else {
		common = b_net;
	}

	/*
	 *	Do the check uint8 by uint8.  If the uint8s are
	 *	identical, it MAY be a match.  If they're different,
	 *	it is NOT a match.
	 */
	i = 0;
	while (i < uint8s) {
		/*
		 *	All leading uint8s are identical.
		 */
		if (common == 0) return true;

		/*
		 *	Doing bitmasks takes more work.
		 */
		if (common < 8) break;

		if (a[i] != b[i]) return false;

		common -= 8;
		i++;
		continue;
	}

	mask = 1;
	mask <<= (8 - common);
	mask--;
	mask = ~mask;

	if ((a[i] & mask) == ((b[i] & mask))) {
		return true;
	}

	return false;
}

/** Compare two attributes using an operator
 *
 * @param[in] op to use in comparison.
 * @param[in] a Value to compare.
 * @param[in] b Value to compare.
 * @return
 *	- 1 if true
 *	- 0 if false
 *	- -1 on failure.
 */
int fr_value_box_cmp_op(FR_TOKEN op, fr_value_box_t const *a, fr_value_box_t const *b)
{
	int compare = 0;

	if (!a || !b) return -1;

	if (!fr_cond_assert(a->type != FR_TYPE_INVALID)) return -1;
	if (!fr_cond_assert(b->type != FR_TYPE_INVALID)) return -1;

	switch (a->type) {
	case FR_TYPE_IPV4_ADDR:
		switch (b->type) {
		case FR_TYPE_IPV4_ADDR:		/* IPv4 and IPv4 */
			goto cmp;

		case FR_TYPE_IPV4_PREFIX:	/* IPv4 and IPv4 Prefix */
			return fr_value_box_cidr_cmp_op(op, 4, 32, (uint8_t const *) &a->datum.ip.addr.v4.s_addr,
						     b->datum.ip.prefix, (uint8_t const *) &b->datum.ip.addr.v4.s_addr);

		default:
			fr_strerror_printf("Cannot compare IPv4 with IPv6 address");
			return -1;
		}

	case FR_TYPE_IPV4_PREFIX:		/* IPv4 and IPv4 Prefix */
		switch (b->type) {
		case FR_TYPE_IPV4_ADDR:
			return fr_value_box_cidr_cmp_op(op, 4, a->datum.ip.prefix,
						     (uint8_t const *) &a->datum.ip.addr.v4.s_addr,
						     32, (uint8_t const *) &b->datum.ip.addr.v4);

		case FR_TYPE_IPV4_PREFIX:	/* IPv4 Prefix and IPv4 Prefix */
			return fr_value_box_cidr_cmp_op(op, 4, a->datum.ip.prefix,
						     (uint8_t const *) &a->datum.ip.addr.v4.s_addr,
						     b->datum.ip.prefix, (uint8_t const *) &b->datum.ip.addr.v4.s_addr);

		default:
			fr_strerror_printf("Cannot compare IPv4 with IPv6 address");
			return -1;
		}

	case FR_TYPE_IPV6_ADDR:
		switch (b->type) {
		case FR_TYPE_IPV6_ADDR:		/* IPv6 and IPv6 */
			goto cmp;

		case FR_TYPE_IPV6_PREFIX:	/* IPv6 and IPv6 Preifx */
			return fr_value_box_cidr_cmp_op(op, 16, 128, (uint8_t const *) &a->datum.ip.addr.v6,
						     b->datum.ip.prefix, (uint8_t const *) &b->datum.ip.addr.v6);

		default:
			fr_strerror_printf("Cannot compare IPv6 with IPv4 address");
			return -1;
		}

	case FR_TYPE_IPV6_PREFIX:
		switch (b->type) {
		case FR_TYPE_IPV6_ADDR:		/* IPv6 Prefix and IPv6 */
			return fr_value_box_cidr_cmp_op(op, 16, a->datum.ip.prefix,
						     (uint8_t const *) &a->datum.ip.addr.v6,
						     128, (uint8_t const *) &b->datum.ip.addr.v6);

		case FR_TYPE_IPV6_PREFIX:	/* IPv6 Prefix and IPv6 */
			return fr_value_box_cidr_cmp_op(op, 16, a->datum.ip.prefix,
						     (uint8_t const *) &a->datum.ip.addr.v6,
						     b->datum.ip.prefix, (uint8_t const *) &b->datum.ip.addr.v6);

		default:
			fr_strerror_printf("Cannot compare IPv6 with IPv4 address");
			return -1;
		}

	default:
	cmp:
		compare = fr_value_box_cmp(a, b);
		if (compare < -1) {	/* comparison error */
			return -1;
		}
	}

	/*
	 *	Now do the operator comparison.
	 */
	switch (op) {
	case T_OP_CMP_EQ:
		return (compare == 0);

	case T_OP_NE:
		return (compare != 0);

	case T_OP_LT:
		return (compare < 0);

	case T_OP_GT:
		return (compare > 0);

	case T_OP_LE:
		return (compare <= 0);

	case T_OP_GE:
		return (compare >= 0);

	default:
		return 0;
	}
}

static char const hextab[] = "0123456789abcdef";

/** Convert a string value with escape sequences into its binary form
 *
 * The quote character determines the escape sequences recognised.
 *
 * Literal mode ("'" quote char) will unescape:
 @verbatim
   - \\        - Literal backslash.
   - \<quote>  - The quotation char.
 @endverbatim
 *
 * Expanded mode (any other quote char) will also unescape:
 @verbatim
   - \r        - Carriage return.
   - \n        - Newline.
   - \t        - Tab.
   - \<oct>    - An octal escape sequence.
   - \x<hex>   - A hex escape sequence.
 @endverbatim
 *
 * Verbatim mode ("\0") passing \0 as the quote char copies in to out verbatim.
 *
 * @note The resulting string will not be \0 terminated, and may contain embedded \0s.
 * @note Invalid escape sequences will be copied verbatim.
 * @note in and out may point to the same buffer.
 *
 * @param[out] out	Where to write the unescaped string.
 *			Unescaping never introduces additional chars.
 * @param[in] in	The string to unescape.
 * @param[in] inlen	Length of input string.
 * @param[in] quote	Character around the string, determines unescaping mode.
 *
 * @return >= 0 the number of uint8s written to out.
 */
size_t value_str_unescape(uint8_t *out, char const *in, size_t inlen, char quote)
{
	char const	*p = in;
	uint8_t		*out_p = out;
	int		x;

	/*
	 *	No de-quoting.  Just copy the string.
	 */
	if (!quote) {
		memcpy(out, in, inlen);
		return inlen;
	}

	/*
	 *	Do escaping for single quoted strings.  Only
	 *	single quotes get escaped.  Everything else is
	 *	left as-is.
	 */
	if (quote == '\'') {
		while (p < (in + inlen)) {
			/*
			 *	The quotation character is escaped.
			 */
			if ((p[0] == '\\') &&
			    (p[1] == quote)) {
				*(out_p++) = quote;
				p += 2;
				continue;
			}

			/*
			 *	Two backslashes get mangled to one.
			 */
			if ((p[0] == '\\') &&
			    (p[1] == '\\')) {
				*(out_p++) = '\\';
				p += 2;
				continue;
			}

			/*
			 *	Not escaped, just copy it over.
			 */
			*(out_p++) = *(p++);
		}
		return out_p - out;
	}

	/*
	 *	It's "string" or `string`, do all standard
	 *	escaping.
	 */
	while (p < (in + inlen)) {
		uint8_t c = *p++;
		uint8_t *h0, *h1;

		/*
		 *	We copy all invalid escape sequences verbatim,
		 *	even if they occur at the end of sthe string.
		 */
		if ((c == '\\') && (p >= (in + inlen))) {
		invalid_escape:
			*out_p++ = c;
			while (p < (in + inlen)) *out_p++ = *p++;
			return out_p - out;
		}

		/*
		 *	Fix up \[rnt\\] -> ... the binary form of it.
		 */
		if (c == '\\') {
			switch (*p) {
			case 'r':
				c = '\r';
				p++;
				break;

			case 'n':
				c = '\n';
				p++;
				break;

			case 't':
				c = '\t';
				p++;
				break;

			case '\\':
				c = '\\';
				p++;
				break;

			default:
				/*
				 *	\" --> ", but only inside of double quoted strings, etc.
				 */
				if (*p == quote) {
					c = quote;
					p++;
					break;
				}

				/*
				 *	We need at least three chars, for either octal or hex
				 */
				if ((p + 2) >= (in + inlen)) goto invalid_escape;

				/*
				 *	\x00 --> binary zero character
				 */
				if ((p[0] == 'x') &&
				    (h0 = memchr((uint8_t const *)hextab, tolower((int) p[1]), sizeof(hextab))) &&
				    (h1 = memchr((uint8_t const *)hextab, tolower((int) p[2]), sizeof(hextab)))) {
				 	c = ((h0 - (uint8_t const *)hextab) << 4) + (h1 - (uint8_t const *)hextab);
				 	p += 3;
				}

				/*
				 *	\000 --> binary zero character
				 */
				if ((p[0] >= '0') &&
				    (p[0] <= '9') &&
				    (p[1] >= '0') &&
				    (p[1] <= '9') &&
				    (p[2] >= '0') &&
				    (p[2] <= '9') &&
				    (sscanf(p, "%3o", &x) == 1)) {
					c = x;
					p += 3;
				}

				/*
				 *	Else It's not a recognised escape sequence DON'T
				 *	consume the backslash. This is identical
				 *	behaviour to bash and most other things that
				 *	use backslash escaping.
				 */
			}
		}
		*out_p++ = c;
	}

	return out_p - out;
}

/** Performs byte order reversal for types that need it
 *
 * @param[in] dst	Where to write the result.  May be the same as src.
 * @param[in] src	#fr_value_box_t containing an uint32 value.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
int fr_value_box_hton(fr_value_box_t *dst, fr_value_box_t const *src)
{
	if (!fr_cond_assert(src->type != FR_TYPE_INVALID)) return -1;

	switch (src->type) {
	case FR_TYPE_UINT16:
		dst->datum.uint16 = htons(src->datum.uint16);
		break;

	case FR_TYPE_UINT32:
		dst->datum.uint32 = htonl(src->datum.uint32);
		break;

	case FR_TYPE_UINT64:
		dst->datum.uint64 = htonll(src->datum.uint64);
		break;

	case FR_TYPE_INT16:
		dst->datum.uint16 = htons(src->datum.uint16);	/* Not a typo, uses the union to avoid memcpy */
		break;

	case FR_TYPE_INT32:
		dst->datum.uint32 = htonl(src->datum.uint32);	/* Not a typo, uses the union to avoid memcpy */
		break;

	case FR_TYPE_INT64:
		dst->datum.uint64 = htonll(src->datum.uint64);	/* Not a typo, uses the union to avoid memcpy */
		break;

	case FR_TYPE_FLOAT32:
		dst->datum.uint32 = htonl(dst->datum.uint32);	/* Not a typo, uses the union to avoid memcpy */
		break;

	case FR_TYPE_FLOAT64:
		dst->datum.uint64 = htonll(dst->datum.uint64);	/* Not a typo, uses the union to avoid memcpy */
		break;

	case FR_TYPE_DATE:
		dst->datum.date = htonl(src->datum.date);
		break;

	case FR_TYPE_DATE_MILLISECONDS:
		dst->datum.date_milliseconds = htonll(src->datum.date_milliseconds);
		break;

	case FR_TYPE_DATE_MICROSECONDS:
		dst->datum.date_microseconds = htonll(src->datum.date_microseconds);
		break;

	case FR_TYPE_DATE_NANOSECONDS:
		dst->datum.date_nanoseconds = htonll(src->datum.date_nanoseconds);
		break;

	case FR_TYPE_BOOL:
	case FR_TYPE_UINT8:
	case FR_TYPE_INT8:
	case FR_TYPE_IPV4_ADDR:
	case FR_TYPE_IPV4_PREFIX:
	case FR_TYPE_IPV6_ADDR:
	case FR_TYPE_IPV6_PREFIX:
	case FR_TYPE_IFID:
	case FR_TYPE_ETHERNET:
	case FR_TYPE_SIZE:
	case FR_TYPE_TIMEVAL:
	case FR_TYPE_ABINARY:
		fr_value_box_copy(NULL, dst, src);
		return 0;

	case FR_TYPE_OCTETS:
	case FR_TYPE_STRING:
	case FR_TYPE_NON_VALUES:
		if (!fr_cond_assert(0)) return -1; /* shouldn't happen */
	}

	if (dst != src) fr_value_box_copy_meta(dst, src);

	return 0;
}
/** Get the size of the value held by the fr_value_box_t
 *
 * This is the length of the NETWORK presentation
 */
size_t fr_value_box_network_length(fr_value_box_t *value)
{
	switch (value->type) {
	case FR_TYPE_VARIABLE_SIZE:
		return value->datum.length;

	default:
		return fr_value_box_network_sizes[value->type][0];
	}
}

/** Encode a single value box, serializing its contents in generic network format
 *
 * The serialized form of #fr_value_box_t may not match the requirements of your protocol
 * completely.  In cases where they do not, you should overload specific types in the
 * function calling #fr_value_box_to_network.
 *
 * The general serialization rules are:
 *
 * - Octets are encoded in binary form (not hex).
 * - Strings are encoded without the trailing \0 byte.
 * - Integers are encoded big-endian.
 * - Bools are encoded using one byte, with value 0x00 (false) or 0x01 (true).
 * - Signed integers are encoded two's complement, with the MSB as the sign bit.
 *   Byte order is big-endian.
 * - Network addresses are encoded big-endian.
 * - IPv4 prefixes are encoded with 1 byte for the prefix, then 4 bytes of address.
 * - IPv6 prefixes are encoded with 1 byte for the scope_id, 1 byte for the prefix,
 *   and 16 bytes of address.
 * - Floats are encoded in IEEE-754 format with a big-endian byte order.  We rely
 *   on the fact that the C standards require floats to be represented in IEEE-754
 *   format in memory.
 * - Dates are encoded as 32bit unsigned UNIX timestamps.
 *
 * #FR_TYPE_TIMEVAL and #FR_TYPE_SIZE are not encodable, as they're system specific.
 * #FR_TYPE_ABINARY is RADIUS specific and should be encoded by the RADIUS encoder.
 *
 * This function will not encode complex types (TLVs, VSAs etc...).  These are usually
 * specific to the protocol anyway.
 *
 * @param[out] need	if not NULL, how many bytes are required to serialize
 *			the remainder of the boxed data.
 *			Note: Only variable length types will be partially
 *			encoded. Fixed length types will not be partially encoded.
 * @param[out] dst	Where to write serialized data.
 * @param[in] dst_len	The length of the output buffer, or maximum value fragment
 *			size.
 * @param[in] value	to encode.
 * @return
 *	- 0 no bytes were written, see need value to determine if it was because
 *	  the fr_value_box_t was #FR_TYPE_OCTETS/#FR_TYPE_STRING and was
 *	  NULL (which is unfortunately valid)
 *	- >0 the number of bytes written to out.
 *	- <0 on error.
 */
ssize_t fr_value_box_to_network(size_t *need, uint8_t *dst, size_t dst_len, fr_value_box_t const *value)
{
	size_t min, max;

	/*
	 *	Variable length types
	 */
	switch (value->type) {
	case FR_TYPE_OCTETS:
	case FR_TYPE_STRING:
	{
		size_t len;

		/*
		 *	Figure dst if we'd overflow
		 */
		if (value->datum.length > dst_len) {
			if (need) *need = value->datum.length;
			len = dst_len;
		} else {
			if (need) *need = 0;
			len = value->datum.length;
		}

		if (value->datum.length > 0) memcpy(dst, value->datum.ptr, len);

		return len;
	}

	default:
		break;
	}

	min = fr_value_box_network_sizes[value->type][0];
	max = fr_value_box_network_sizes[value->type][1];

	/*
	 *	It's an unsupported type
	 */
	if ((min == 0) && (max == 0)) {
	unsupported:
		fr_strerror_printf("Cannot encode type \"%s\"",
				   fr_int2str(dict_attr_types, value->type, "<INVALID>"));
		return -1;
	}

	/*
	 *	Fixed type would overflow dstput buffer
	 */
	if (max > dst_len) {
		if (need) *need = max;
		return 0;
	}

	switch (value->type) {
	/*
	 *	Needs special mangling
	 */
	case FR_TYPE_IPV4_PREFIX:
	 	dst[0] = value->datum.ip.prefix;
		memcpy(&dst[1], (uint8_t const *)&value->datum.ip.addr.v4.s_addr, sizeof(value->datum.ip.addr.v4.s_addr));
		break;

	case FR_TYPE_IPV6_PREFIX:
		dst[0] = value->datum.ip.scope_id;
		dst[1] = value->datum.ip.prefix;
		memcpy(&dst[2], value->datum.ip.addr.v6.s6_addr, sizeof(value->datum.ip.addr.v6.s6_addr));
		break;

	case FR_TYPE_BOOL:
		dst[0] = value->datum.boolean ? 0x01 : 0x00;
		break;

	/*
	 *	Already in network byte-order
	 */
	case FR_TYPE_IPV4_ADDR:
	case FR_TYPE_IPV6_ADDR:
	case FR_TYPE_IFID:
	case FR_TYPE_ETHERNET:
	case FR_TYPE_UINT8:
	case FR_TYPE_INT8:
		if (!fr_cond_assert(fr_value_box_field_sizes[value->type] == min)) return -1;
		memcpy(dst, ((uint8_t const *)&value->datum) + fr_value_box_offsets[value->type], min);
		break;

	/*
	 *	Needs a bytesex operation
	 */
	case FR_TYPE_UINT16:
	case FR_TYPE_UINT32:
	case FR_TYPE_UINT64:
	case FR_TYPE_INT16:
	case FR_TYPE_INT32:
	case FR_TYPE_INT64:
	case FR_TYPE_DATE:
	case FR_TYPE_FLOAT32:
	case FR_TYPE_FLOAT64:
	case FR_TYPE_DATE_MILLISECONDS:
	case FR_TYPE_DATE_MICROSECONDS:
	case FR_TYPE_DATE_NANOSECONDS:
	{
		fr_value_box_t tmp;

		if (!fr_cond_assert(fr_value_box_field_sizes[value->type] == min)) return -1;

		fr_value_box_hton(&tmp, value);
		memcpy(dst, ((uint8_t const *)&tmp.datum) + fr_value_box_offsets[value->type], min);
	}
		break;

	case FR_TYPE_OCTETS:
	case FR_TYPE_STRING:
	case FR_TYPE_SIZE:
	case FR_TYPE_TIMEVAL:
	case FR_TYPE_ABINARY:
	case FR_TYPE_NON_VALUES:
		goto unsupported;
	}

	if (need) *need = 0;

	return min;
}

/** Decode a #fr_value_box_t from serialized binary data
 *
 * The general deserialization rules are:
 *
 * - Octets are decoded in binary form (not hex).
 * - Strings are decoded without the trailing \0 byte. Strings must consist only of valid UTF8 chars.
 * - Integers are decoded big-endian.
 * - Bools are decoded using one byte, with value 0x00 (false) or 0x01 (true).
 * - Signed integers are decoded two's complement, with the MSB as the sign bit.
 *   Byte order is big-endian.
 * - Network addresses are decoded big-endian.
 * - IPv4 prefixes are decoded with 1 byte for the prefix, then 4 bytes of address.
 * - IPv6 prefixes are decoded with 1 byte for the scope_id, 1 byte for the prefix,
 *   and 16 bytes of address.
 * - Floats are decoded in IEEE-754 format with a big-endian byte order.  We rely
 *   on the fact that the C standards require floats to be represented in IEEE-754
 *   format in memory.
 * - Dates are decoded as 32bit unsigned UNIX timestamps.
 *
 * @param[in] ctx	Where to allocate any talloc buffers required.
 * @param[out] dst	value_box to write the result to.
 * @param[in] src	Binary data to decode.
 * @param[in] len	Length of data to decode.  For fixed length types we only
 *			decode complete values.
 * @param[in] type	to decode data to.
 * @param[in] tainted	Whether the value came from a trusted source.
 * @return
 *	- >= 0 The number of bytes consumed.
 *	- <0 on error.
 */
ssize_t fr_value_box_from_network(TALLOC_CTX *ctx, fr_value_box_t *dst, uint8_t const *src, size_t len,
				  fr_type_t type, bool tainted)
{
	if (len < fr_value_box_network_sizes[type][0]) {
		fr_strerror_printf("Got truncated value parsing type \"%s\". "
				   "Expected length >= %zu bytes, got %zu bytes",
				   fr_int2str(dict_attr_types, type, "<INVALID>"),
				   fr_value_box_network_sizes[type][0], len);
		return -1;
	}
	if (len > fr_value_box_network_sizes[type][1]) {
		fr_strerror_printf("Found trailing garbage parsing type \"%s\". "
				   "Expected length <= %zu bytes, got %zu bytes",
				   fr_int2str(dict_attr_types, type, "<INVALID>"),
				   fr_value_box_network_sizes[type][1], len);
		return -1;
	}

	switch (type) {
	case FR_TYPE_STRING:
		if (fr_value_box_bstrndup(ctx, dst, (char const *)src, len, tainted) < 0) return -1;
		return len;

	case FR_TYPE_OCTETS:
		if (fr_value_box_memdup(ctx, dst, src, len, tainted) < 0) return -1;
		return len;

	/*
	 *	Already in network byte order
	 */
	case FR_TYPE_IPV4_ADDR:
		memcpy(&dst->datum.ip.addr.v4, src, len);
		dst->datum.ip.af = AF_INET;
		dst->datum.ip.scope_id = 0;
		dst->datum.ip.prefix = 32;
		break;

	case FR_TYPE_IPV4_PREFIX:
		memcpy(&dst->datum.ip.addr.v4, src + 1, len - 1);
		dst->datum.ip.af = AF_INET;
		dst->datum.ip.scope_id = 0;
		dst->datum.ip.prefix = src[0];
		break;

	case FR_TYPE_IPV6_ADDR:
		memcpy(&dst->datum.ip.addr.v6, src, len - 1);
		dst->datum.ip.af = AF_INET6;
		dst->datum.ip.scope_id = 0;
		dst->datum.ip.prefix = 128;
		break;

	case FR_TYPE_IPV6_PREFIX:
		memcpy(&dst->datum.ip.addr.v6, src + 2, len - 2);
		dst->datum.ip.af = AF_INET6;
		dst->datum.ip.scope_id = src[0];
		dst->datum.ip.prefix = src[1];
		break;

	case FR_TYPE_BOOL:
		dst->datum.boolean = src[0] > 0 ? true : false;
		break;

	case FR_TYPE_IFID:
	case FR_TYPE_ETHERNET:
	case FR_TYPE_UINT8:
	case FR_TYPE_INT8:
		memcpy(((uint8_t *)&dst->datum) + fr_value_box_offsets[type], src, len);
		break;

	/*
	 *	Needs a bytesex operation
	 */
	case FR_TYPE_UINT16:
	case FR_TYPE_UINT32:
	case FR_TYPE_UINT64:
	case FR_TYPE_INT16:
	case FR_TYPE_INT32:
	case FR_TYPE_INT64:
	case FR_TYPE_DATE:
	case FR_TYPE_FLOAT32:
	case FR_TYPE_FLOAT64:
	case FR_TYPE_DATE_MILLISECONDS:
	case FR_TYPE_DATE_MICROSECONDS:
	case FR_TYPE_DATE_NANOSECONDS:
		memcpy(((uint8_t *)&dst->datum) + fr_value_box_offsets[type], src, len);
		fr_value_box_hton(dst, dst);	/* Operate in-place */
		break;

	case FR_TYPE_SIZE:
	case FR_TYPE_TIMEVAL:
	case FR_TYPE_ABINARY:
	case FR_TYPE_NON_VALUES:
		fr_strerror_printf("Cannot decode type \"%s\" - Is not a value",
				   fr_int2str(dict_attr_types, type, "<INVALID>"));
		break;
	}

	dst->type = type;
	dst->tainted = tainted;
	dst->enumv = NULL;
	dst->next = NULL;

	return len;
}

/** v4 to v6 mapping prefix
 *
 * Part of the IPv6 range is allocated to represent IPv4 addresses.
 */
static uint8_t const v4_v6_map[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0xff, 0xff };

/** Convert any supported type to a string
 *
 * All non-structural types are allowed.
 *
 * @param ctx		unused.
 * @param dst		Where to write result of casting.
 * @param dst_type	to cast to.
 * @param dst_enumv	unused.
 * @param src		Input data.
 */
static inline int fr_value_box_cast_to_strvalue(TALLOC_CTX *ctx, fr_value_box_t *dst,
						fr_type_t dst_type, UNUSED fr_dict_attr_t const *dst_enumv,
						fr_value_box_t const *src)
{
	if (!fr_cond_assert(dst_type == FR_TYPE_STRING)) return -1;

	switch (src->type) {
	/*
	 *	The presentation format of octets is hex
	 *	What we actually want here is the raw string
	 */
	case FR_TYPE_OCTETS:
		dst->datum.strvalue = talloc_bstrndup(ctx, (char const *)src->datum.octets, src->datum.length);
		dst->datum.length = src->datum.length;	/* It's the same, even though the buffer is slightly bigger */
		break;

	/*
	 *	Get the presentation format
	 */
	default:
		dst->datum.strvalue = fr_value_box_asprint(ctx, src, '\0');
		dst->datum.length = talloc_array_length(dst->datum.strvalue) - 1;
		break;
	}

	if (!dst->datum.strvalue) return -1;
	dst->type = FR_TYPE_STRING;

	return 0;
}

/** Convert any supported type to octets
 *
 * All non-structural types are allowed.
 *
 * @param ctx		unused.
 * @param dst		Where to write result of casting.
 * @param dst_type	to cast to.
 * @param dst_enumv	unused.
 * @param src		Input data.
 */
static inline int fr_value_box_cast_to_octets(TALLOC_CTX *ctx, fr_value_box_t *dst,
					      fr_type_t dst_type, UNUSED fr_dict_attr_t const *dst_enumv,
					      fr_value_box_t const *src)
{
	uint8_t *bin;

	if (!fr_cond_assert(dst_type == FR_TYPE_OCTETS)) return -1;

	switch (src->type) {
	/*
	 *	<string> (excluding terminating \0)
	 */
	case FR_TYPE_STRING:
		bin = talloc_memdup(ctx, (uint8_t const *)src->datum.strvalue, src->datum.length);
		break;

	/*
	 *	<4 uint8s address>
	 */
	case FR_TYPE_IPV4_ADDR:
	{
		bin = talloc_memdup(ctx, (uint8_t const *)&src->datum.ip.addr.v4.s_addr,
				    sizeof(src->datum.ip.addr.v4.s_addr));
	}
		break;

	/*
	 *	<1 uint8 prefix> + <4 uint8s address>
	 */
	case FR_TYPE_IPV4_PREFIX:
		bin = talloc_array(ctx, uint8_t, sizeof(src->datum.ip.addr.v4.s_addr) + 1);
		bin[0] = src->datum.ip.prefix;
		memcpy(&bin[1], (uint8_t const *)&src->datum.ip.addr.v4.s_addr, sizeof(src->datum.ip.addr.v4.s_addr));
		break;

	/*
	 *	<16 uint8s address>
	 */
	case FR_TYPE_IPV6_ADDR:
		bin = talloc_memdup(ctx, (uint8_t const *)src->datum.ip.addr.v6.s6_addr,
				    sizeof(src->datum.ip.addr.v6.s6_addr));
		break;

	/*
	 *	<1 uint8 prefix> + <1 uint8 scope> + <16 uint8s address>
	 */
	case FR_TYPE_IPV6_PREFIX:
		bin = talloc_array(ctx, uint8_t, sizeof(src->datum.ip.addr.v6.s6_addr) + 2);
		bin[0] = src->datum.ip.scope_id;
		bin[1] = src->datum.ip.prefix;
		memcpy(&bin[2], src->datum.ip.addr.v6.s6_addr, sizeof(src->datum.ip.addr.v6.s6_addr));
		break;
	/*
	 *	Get the raw binary in memory representation
	 */
	default:
		fr_value_box_hton(dst, src);	/* Flip any uint32 representations */
		bin = talloc_memdup(ctx, ((uint8_t *)&dst->datum) + fr_value_box_offsets[src->type],
				    fr_value_box_field_sizes[src->type]);
		break;
	}

	if (!bin) return -1;

	talloc_set_type(bin, uint8_t);
	fr_value_box_memsteal(ctx, dst, bin, src->tainted);
	dst->type = FR_TYPE_OCTETS;

	return 0;
}

/** Convert any supported type to an IPv4 address
 *
 * Allowed input types are:
 * - FR_TYPE_IPV6_ADDR (with v4 prefix).
 * - FR_TYPE_IPV4_PREFIX (with 32bit mask).
 * - FR_TYPE_IPV6_PREFIX (with v4 prefix and 128bit mask).
 * - FR_TYPE_OCTETS (of length 4).
 * - FR_TYPE_UINT32
 *
 * @param ctx		unused.
 * @param dst		Where to write result of casting.
 * @param dst_type	to cast to.
 * @param dst_enumv	unused.
 * @param src		Input data.
 */
static inline int fr_value_box_cast_to_ipv4addr(TALLOC_CTX *ctx, fr_value_box_t *dst,
						fr_type_t dst_type, fr_dict_attr_t const *dst_enumv,
						fr_value_box_t const *src)
{
	if (!fr_cond_assert(dst_type == FR_TYPE_IPV4_ADDR)) return -1;

	switch (src->type) {
	case FR_TYPE_IPV6_ADDR:
		if (memcmp(src->datum.ip.addr.v6.s6_addr, v4_v6_map, sizeof(v4_v6_map)) != 0) {
		bad_v6_prefix_map:
			fr_strerror_printf("Invalid cast from %s to %s.  No IPv4-IPv6 mapping prefix",
					   fr_int2str(dict_attr_types, src->type, "<INVALID>"),
					   fr_int2str(dict_attr_types, dst_type, "<INVALID>"));
			return -1;
		}

		memcpy(&dst->datum.ip.addr.v4, &src->datum.ip.addr.v6.s6_addr[sizeof(v4_v6_map)],
		       sizeof(dst->datum.ip.addr.v4));

		break;

	case FR_TYPE_IPV4_PREFIX:
		if (src->datum.ip.prefix != 32) {
			fr_strerror_printf("Invalid cast from %s to %s.  Only /32 (not %i/) prefixes may be "
					   "cast to IP address types",
					   fr_int2str(dict_attr_types, src->type, "<INVALID>"),
					   fr_int2str(dict_attr_types, dst_type, "<INVALID>"),
					   src->datum.ip.prefix);
			return -1;
		}
		memcpy(&dst->datum.ip.addr.v4, &src->datum.ip.addr.v4, sizeof(dst->datum.ip.addr.v4));
		break;

	case FR_TYPE_IPV6_PREFIX:
		if (src->datum.ip.prefix != 128) {
			fr_strerror_printf("Invalid cast from %s to %s.  Only /128 (not /%i) prefixes may be "
					   "cast to IP address types",
					   fr_int2str(dict_attr_types, src->type, "<INVALID>"),
					   fr_int2str(dict_attr_types, dst_type, "<INVALID>"),
					   src->datum.ip.prefix);
			return -1;
		}
		if (memcmp(&src->datum.ip.addr.v6.s6_addr, v4_v6_map, sizeof(v4_v6_map)) != 0) goto bad_v6_prefix_map;
		memcpy(&dst->datum.ip.addr.v4, &src->datum.ip.addr.v6.s6_addr[sizeof(v4_v6_map)],
		       sizeof(dst->datum.ip.addr.v4));
		break;

	case FR_TYPE_STRING:
		if (fr_value_box_from_str(ctx, dst, &dst_type, dst_enumv,
				          src->datum.strvalue, src->datum.length, '\0', false) < 0) return -1;
		break;

	case FR_TYPE_OCTETS:
		if (src->datum.length != sizeof(dst->datum.ip.addr.v4.s_addr)) {
			fr_strerror_printf("Invalid cast from %s to %s.  Only %zu uint8 octet strings "
					   "may be cast to IP address types",
					   fr_int2str(dict_attr_types, src->type, "<INVALID>"),
					   fr_int2str(dict_attr_types, dst_type, "<INVALID>"),
					   sizeof(dst->datum.ip.addr.v4.s_addr));
			return -1;
		}
		memcpy(&dst->datum.ip.addr.v4, src->datum.octets, sizeof(dst->datum.ip.addr.v4.s_addr));
		break;

	case FR_TYPE_UINT32:
	{
		uint32_t net;

		net = ntohl(src->datum.uint32);
		memcpy(&dst->datum.ip.addr.v4, (uint8_t *)&net, sizeof(dst->datum.ip.addr.v4.s_addr));
	}
		break;

	default:
		fr_strerror_printf("Invalid cast from %s to %s.  Unsupported",
				   fr_int2str(dict_attr_types, src->type, "<INVALID>"),
				   fr_int2str(dict_attr_types, dst_type, "<INVALID>"));
		return -1;
	}

	dst->datum.ip.af = AF_INET;
	dst->datum.ip.prefix = 32;
	dst->datum.ip.scope_id = 0;
	dst->type = FR_TYPE_IPV4_ADDR;

	return 0;
}

/** Convert any supported type to an IPv6 address
 *
 * Allowed input types are:
 * - FR_TYPE_IPV4_ADDR
 * - FR_TYPE_IPV4_PREFIX (with 32bit mask).
 * - FR_TYPE_IPV6_PREFIX (with 128bit mask).
 * - FR_TYPE_OCTETS (of length 16).
 *
 * @param ctx		unused.
 * @param dst		Where to write result of casting.
 * @param dst_type	to cast to.
 * @param dst_enumv	unused.
 * @param src		Input data.
 */
static inline int fr_value_box_cast_to_ipv4prefix(TALLOC_CTX *ctx, fr_value_box_t *dst,
						  fr_type_t dst_type, fr_dict_attr_t const *dst_enumv,
						  fr_value_box_t const *src)
{
	if (!fr_cond_assert(dst_type == FR_TYPE_IPV4_PREFIX)) return -1;

	switch (src->type) {
	case FR_TYPE_IPV4_ADDR:
		memcpy(&dst->datum.ip, &src->datum.ip, sizeof(dst->datum.ip));
		break;

	/*
	 *	Copy the last four uint8s, to make an IPv4prefix
	 */
	case FR_TYPE_IPV6_ADDR:
		if (memcmp(src->datum.ip.addr.v6.s6_addr, v4_v6_map, sizeof(v4_v6_map)) != 0) {
		bad_v6_prefix_map:
			fr_strerror_printf("Invalid cast from %s to %s.  No IPv4-IPv6 mapping prefix",
					   fr_int2str(dict_attr_types, src->type, "<INVALID>"),
					   fr_int2str(dict_attr_types, dst_type, "<INVALID>"));
			return -1;
		}
		memcpy(&dst->datum.ip.addr.v4.s_addr, &src->datum.ip.addr.v6.s6_addr[sizeof(v4_v6_map)],
		       sizeof(dst->datum.ip.addr.v4.s_addr));
		dst->datum.ip.prefix = 32;
		break;

	case FR_TYPE_IPV6_PREFIX:
		if (memcmp(src->datum.ip.addr.v6.s6_addr, v4_v6_map, sizeof(v4_v6_map)) != 0) goto bad_v6_prefix_map;

		if (src->datum.ip.prefix < (sizeof(v4_v6_map) << 3)) {
			fr_strerror_printf("Invalid cast from %s to %s. Expected prefix >= %u bits got %u bits",
					   fr_int2str(dict_attr_types, src->type, "<INVALID>"),
					   fr_int2str(dict_attr_types, dst_type, "<INVALID>"),
					   (unsigned int)(sizeof(v4_v6_map) << 3), src->datum.ip.prefix);
			return -1;
		}
		memcpy(&dst->datum.ip.addr.v4.s_addr, &src->datum.ip.addr.v6.s6_addr[sizeof(v4_v6_map)],
		       sizeof(dst->datum.ip.addr.v4.s_addr));

		/*
		 *	Subtract the bits used by the v4_v6_map to get the v4 prefix bits
		 */
		dst->datum.ip.prefix = src->datum.ip.prefix - (sizeof(v4_v6_map) << 3);
		break;

	case FR_TYPE_STRING:
		if (fr_value_box_from_str(ctx, dst, &dst_type, dst_enumv,
				          src->datum.strvalue, src->datum.length, '\0', false) < 0) return -1;
		break;


	case FR_TYPE_OCTETS:
		if (src->datum.length != sizeof(dst->datum.ip.addr.v4.s_addr) + 1) {
			fr_strerror_printf("Invalid cast from %s to %s.  Only %zu uint8 octet strings "
					   "may be cast to IP address types",
					   fr_int2str(dict_attr_types, src->type, "<INVALID>"),
					   fr_int2str(dict_attr_types, dst_type, "<INVALID>"),
					   sizeof(dst->datum.ip.addr.v4.s_addr) + 1);
			return -1;
		}
		dst->datum.ip.prefix = src->datum.octets[0];
		memcpy(&dst->datum.ip.addr.v4, &src->datum.octets[1], sizeof(dst->datum.ip.addr.v4.s_addr));
		break;

	case FR_TYPE_UINT32:
	{
		uint32_t net;

		net = ntohl(src->datum.uint32);
		memcpy(&dst->datum.ip.addr.v4, (uint8_t *)&net, sizeof(dst->datum.ip.addr.v4.s_addr));
		dst->datum.ip.prefix = 32;
	}

	default:
		break;
	}

	dst->datum.ip.af = AF_INET;
	dst->datum.ip.scope_id = 0;
	dst->type = FR_TYPE_IPV4_PREFIX;

	return 0;
}

/** Convert any supported type to an IPv6 address
 *
 * Allowed input types are:
 * - FR_TYPE_IPV4_ADDR
 * - FR_TYPE_IPV4_PREFIX (with 32bit mask).
 * - FR_TYPE_IPV6_PREFIX (with 128bit mask).
 * - FR_TYPE_OCTETS (of length 16).
 *
 * @param ctx		unused.
 * @param dst		Where to write result of casting.
 * @param dst_type	to cast to.
 * @param dst_enumv	unused.
 * @param src		Input data.
 */
static inline int fr_value_box_cast_to_ipv6addr(TALLOC_CTX *ctx, fr_value_box_t *dst,
						fr_type_t dst_type, fr_dict_attr_t const *dst_enumv,
						fr_value_box_t const *src)
{
	if (!fr_cond_assert(dst_type == FR_TYPE_IPV6_ADDR)) return -1;

	static_assert((sizeof(v4_v6_map) + sizeof(src->datum.ip.addr.v4)) <=
		      sizeof(src->datum.ip.addr.v6), "IPv6 storage too small");

	switch (src->type) {
	case FR_TYPE_IPV4_ADDR:
	{
		uint8_t *p = dst->datum.ip.addr.v6.s6_addr;

		/* Add the v4/v6 mapping prefix */
		memcpy(p, v4_v6_map, sizeof(v4_v6_map));
		p += sizeof(v4_v6_map);
		memcpy(p, (uint8_t const *)&src->datum.ip.addr.v4.s_addr, sizeof(src->datum.ip.addr.v4.s_addr));
		dst->datum.ip.scope_id = 0;
	}
		break;

	case FR_TYPE_IPV4_PREFIX:
	{
		uint8_t *p = dst->datum.ip.addr.v6.s6_addr;

		if (src->datum.ip.prefix != 32) {
			fr_strerror_printf("Invalid cast from %s to %s.  Only /32 (not /%i) prefixes may be "
			   		   "cast to IP address types",
			   		   fr_int2str(dict_attr_types, src->type, "<INVALID>"),
					   fr_int2str(dict_attr_types, dst_type, "<INVALID>"),
					   src->datum.ip.prefix);
			return -1;
		}

		/* Add the v4/v6 mapping prefix */
		memcpy(p, v4_v6_map, sizeof(v4_v6_map));
		p += sizeof(v4_v6_map);
		memcpy(p, (uint8_t const *)&src->datum.ip.addr.v4.s_addr, sizeof(src->datum.ip.addr.v4.s_addr));
		dst->datum.ip.scope_id = 0;
	}
		break;

	case FR_TYPE_IPV6_PREFIX:
		if (src->datum.ip.prefix != 128) {
			fr_strerror_printf("Invalid cast from %s to %s.  Only /128 (not /%i) prefixes may be "
			   		   "cast to IP address types",
			   		   fr_int2str(dict_attr_types, src->type, "<INVALID>"),
					   fr_int2str(dict_attr_types, dst_type, "<INVALID>"),
					   src->datum.ip.prefix);
			return -1;
		}
		memcpy(dst->datum.ip.addr.v6.s6_addr, src->datum.ip.addr.v6.s6_addr,
		       sizeof(dst->datum.ip.addr.v6.s6_addr));
		dst->datum.ip.scope_id = src->datum.ip.scope_id;
		break;

	case FR_TYPE_STRING:
		if (fr_value_box_from_str(ctx, dst, &dst_type, dst_enumv,
				          src->datum.strvalue, src->datum.length, '\0', false) < 0) return -1;
		break;

	case FR_TYPE_OCTETS:
		if (src->datum.length != sizeof(dst->datum.ip.addr.v6.s6_addr)) {
			fr_strerror_printf("Invalid cast from %s to %s.  Only %zu uint8 octet strings "
					   "may be cast to IP address types",
					   fr_int2str(dict_attr_types, src->type, "<INVALID>"),
					   fr_int2str(dict_attr_types, dst_type, "<INVALID>"),
					   sizeof(dst->datum.ip.addr.v6.s6_addr));
			return -1;
		}
		memcpy(&dst->datum.ip.addr.v6.s6_addr, src->datum.octets, sizeof(dst->datum.ip.addr.v6.s6_addr));
		break;

	default:
		fr_strerror_printf("Invalid cast from %s to %s.  Unsupported",
				   fr_int2str(dict_attr_types, src->type, "<INVALID>"),
				   fr_int2str(dict_attr_types, dst_type, "<INVALID>"));
		break;
	}

	dst->datum.ip.af = AF_INET6;
	dst->datum.ip.prefix = 128;
	dst->type = FR_TYPE_IPV6_ADDR;

	return 0;
}

/** Convert any supported type to an IPv6 address
 *
 * Allowed input types are:
 * - FR_TYPE_IPV4_ADDR
 * - FR_TYPE_IPV4_PREFIX (with 32bit mask).
 * - FR_TYPE_IPV6_PREFIX (with 128bit mask).
 * - FR_TYPE_OCTETS (of length 16).
 *
 * @param ctx		unused.
 * @param dst		Where to write result of casting.
 * @param dst_type	to cast to.
 * @param dst_enumv	unused.
 * @param src		Input data.
 */
static inline int fr_value_box_cast_to_ipv6prefix(TALLOC_CTX *ctx, fr_value_box_t *dst,
						  fr_type_t dst_type, fr_dict_attr_t const *dst_enumv,
						  fr_value_box_t const *src)
{
	switch (src->type) {
	case FR_TYPE_IPV4_ADDR:
	{
		uint8_t *p = dst->datum.ip.addr.v6.s6_addr;

		/* Add the v4/v6 mapping prefix */
		memcpy(p, v4_v6_map, sizeof(v4_v6_map));
		p += sizeof(v4_v6_map);
		memcpy(p, (uint8_t const *)&src->datum.ip.addr.v4.s_addr, sizeof(src->datum.ip.addr.v4.s_addr));
		dst->datum.ip.prefix = 128;
		dst->datum.ip.scope_id = 0;
	}
		break;

	case FR_TYPE_IPV4_PREFIX:
	{
		uint8_t *p = dst->datum.ip.addr.v6.s6_addr;

		/* Add the v4/v6 mapping prefix */
		memcpy(p, v4_v6_map, sizeof(v4_v6_map));
		p += sizeof(v4_v6_map);
		memcpy(p, (uint8_t const *)&src->datum.ip.addr.v4.s_addr, sizeof(src->datum.ip.addr.v4.s_addr));
		dst->datum.ip.prefix = (sizeof(v4_v6_map) << 3) + src->datum.ip.prefix;
		dst->datum.ip.scope_id = 0;
	}
		break;

	case FR_TYPE_IPV6_ADDR:
		memcpy(dst->datum.ip.addr.v6.s6_addr, src->datum.ip.addr.v6.s6_addr,
		       sizeof(dst->datum.ip.addr.v6.s6_addr));
		dst->datum.ip.prefix = 128;
		dst->datum.ip.scope_id = src->datum.ip.scope_id;
		break;

	case FR_TYPE_STRING:
		if (fr_value_box_from_str(ctx, dst, &dst_type, dst_enumv,
				          src->datum.strvalue, src->datum.length, '\0', false) < 0) return -1;
		break;

	case FR_TYPE_OCTETS:
		if (src->datum.length != (sizeof(dst->datum.ip.addr.v6.s6_addr) + 2)) {
			fr_strerror_printf("Invalid cast from %s to %s.  Only %zu uint8 octet strings "
					   "may be cast to IP address types",
					   fr_int2str(dict_attr_types, src->type, "<INVALID>"),
					   fr_int2str(dict_attr_types, dst_type, "<INVALID>"),
					   sizeof(dst->datum.ip.addr.v6.s6_addr) + 2);
			return -1;
		}
		dst->datum.ip.scope_id = src->datum.octets[0];
		dst->datum.ip.prefix = src->datum.octets[1];
		memcpy(&dst->datum.ip.addr.v6.s6_addr, src->datum.octets, sizeof(dst->datum.ip.addr.v6.s6_addr));
		break;

	default:
		break;
	}

	dst->datum.ip.af = AF_INET6;
	dst->type = FR_TYPE_IPV6_PREFIX;

	return 0;
}

/** Convert one type of fr_value_box_t to another
 *
 * This should be the canonical function used to convert between INTERNAL data formats.
 *
 * If you want to convert from PRESENTATION format, use #fr_value_box_from_str.
 *
 * @param ctx		to allocate buffers in (usually the same as dst)
 * @param dst		Where to write result of casting.
 * @param dst_type	to cast to.
 * @param dst_enumv	Aliases for values contained within this fr_value_box_t.
 *			If #fr_value_box_t is passed to #fr_value_box_asprint
 *			aliases will be printed instead of actual value.
 * @param src		Input data.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
int fr_value_box_cast(TALLOC_CTX *ctx, fr_value_box_t *dst,
		      fr_type_t dst_type, fr_dict_attr_t const *dst_enumv,
		      fr_value_box_t const *src)
{
	if (!fr_cond_assert(dst_type != FR_TYPE_INVALID)) return -1;
	if (!fr_cond_assert(src->type != FR_TYPE_INVALID)) return -1;

	if (fr_dict_non_data_types[dst_type]) {
		fr_strerror_printf("Invalid cast from %s to %s.  Can only cast simple data types.",
				   fr_int2str(dict_attr_types, src->type, "<INVALID>"),
				   fr_int2str(dict_attr_types, dst_type, "<INVALID>"));
		return -1;
	}

	/*
	 *	If it's the same type, copy.
	 */
	if (dst_type == src->type) return fr_value_box_copy(ctx, dst, src);

	/*
	 *	Initialise dst
	 */
	memset(dst, 0, sizeof(*dst));

	/*
	 *	Dispatch to specialised cast functions
	 */
	switch (dst_type) {
	case FR_TYPE_STRING:
		return fr_value_box_cast_to_strvalue(ctx, dst, dst_type, dst_enumv, src);

	case FR_TYPE_OCTETS:
		return fr_value_box_cast_to_octets(ctx, dst, dst_type, dst_enumv, src);

	case FR_TYPE_IPV4_ADDR:
		return fr_value_box_cast_to_ipv4addr(ctx, dst, dst_type, dst_enumv, src);

	case FR_TYPE_IPV4_PREFIX:
		return fr_value_box_cast_to_ipv4prefix(ctx, dst, dst_type, dst_enumv, src);

	case FR_TYPE_IPV6_ADDR:
		return fr_value_box_cast_to_ipv6addr(ctx, dst, dst_type, dst_enumv, src);

	case FR_TYPE_IPV6_PREFIX:
		return fr_value_box_cast_to_ipv6prefix(ctx, dst, dst_type, dst_enumv, src);

	/*
	 *	Need func
	 */
	case FR_TYPE_IFID:
	case FR_TYPE_COMBO_IP_ADDR:
	case FR_TYPE_COMBO_IP_PREFIX:
	case FR_TYPE_ETHERNET:
	case FR_TYPE_BOOL:
	case FR_TYPE_UINT8:
	case FR_TYPE_UINT16:
	case FR_TYPE_UINT32:
	case FR_TYPE_UINT64:
	case FR_TYPE_INT8:
	case FR_TYPE_INT16:
	case FR_TYPE_INT32:
	case FR_TYPE_INT64:
	case FR_TYPE_FLOAT32:
	case FR_TYPE_FLOAT64:
	case FR_TYPE_DATE:
	case FR_TYPE_DATE_MILLISECONDS:
	case FR_TYPE_DATE_MICROSECONDS:
	case FR_TYPE_DATE_NANOSECONDS:

	case FR_TYPE_SIZE:
	case FR_TYPE_TIMEVAL:
	case FR_TYPE_ABINARY:
		break;

	/*
	 *	Invalid types for casting (should have been caught earlier)
	 */
	case FR_TYPE_STRUCTURAL:
	case FR_TYPE_INVALID:
	case FR_TYPE_MAX:
		if (!fr_cond_assert(0)) return -1;
	}

	/*
	 *	Deserialise a fr_value_box_t
	 */
	if (src->type == FR_TYPE_STRING) return fr_value_box_from_str(ctx, dst, &dst_type, dst_enumv,
								      src->datum.strvalue,
								      src->datum.length, '\0', false);

	if ((src->type == FR_TYPE_IFID) &&
	    (dst_type == FR_TYPE_UINT64)) {
		memcpy(&dst->datum.uint64, src->datum.ifid, sizeof(src->datum.ifid));
		dst->datum.uint64 = htonll(dst->datum.uint64);

	fixed_length:
		dst->type = dst_type;
		dst->enumv = dst_enumv;

		return 0;
	}

	if ((src->type == FR_TYPE_UINT64) &&
	    (dst_type == FR_TYPE_ETHERNET)) {
		uint8_t array[8];
		uint64_t i;

		i = htonll(src->datum.uint64);
		memcpy(array, &i, 8);

		/*
		 *	For OUIs in the DB.
		 */
		if ((array[0] != 0) || (array[1] != 0)) return -1;

		memcpy(dst->datum.ether, &array[2], 6);
		goto fixed_length;
	}

	if (dst_type == FR_TYPE_UINT16) {
		switch (src->type) {
		case FR_TYPE_UINT8:
			dst->datum.uint16 = src->datum.uint8;
			break;

		case FR_TYPE_OCTETS:
			goto do_octets;

		default:
			goto invalid_cast;
		}
		goto fixed_length;
	}

	/*
	 *	We can cast LONG uint32s to SHORTER ones, so long
	 *	as the long one is on the LHS.
	 */
	if (dst_type == FR_TYPE_UINT32) {
		switch (src->type) {
		case FR_TYPE_UINT8:
			dst->datum.uint32 = src->datum.uint8;
			break;

		case FR_TYPE_UINT16:
			dst->datum.uint32 = src->datum.uint16;
			break;

		case FR_TYPE_INT32:
			if (src->datum.int32 < 0 ) {
				fr_strerror_printf("Invalid cast: From signed to uint32.  "
						   "signed value %d is negative ", src->datum.int32);
				return -1;
			}
			dst->datum.uint32 = (uint32_t)src->datum.int32;
			break;

		case FR_TYPE_OCTETS:
			goto do_octets;

		default:
			goto invalid_cast;
		}
		goto fixed_length;
	}

	/*
	 *	For uint32s, we allow the casting of a SMALL type to
	 *	a larger type, but not vice-versa.
	 */
	if (dst_type == FR_TYPE_UINT64) {
		switch (src->type) {
		case FR_TYPE_UINT8:
			dst->datum.uint64 = src->datum.uint8;
			break;

		case FR_TYPE_UINT16:
			dst->datum.uint64 = src->datum.uint16;
			break;

		case FR_TYPE_UINT32:
			dst->datum.uint64 = src->datum.uint32;
			break;

		case FR_TYPE_DATE:
			dst->datum.uint64 = src->datum.date;
			break;

		case FR_TYPE_OCTETS:
			goto do_octets;

		default:
		invalid_cast:
			fr_strerror_printf("Invalid cast from %s to %s",
					   fr_int2str(dict_attr_types, src->type, "<INVALID>"),
					   fr_int2str(dict_attr_types, dst_type, "<INVALID>"));
			return -1;

		}
		goto fixed_length;
	}

	/*
	 *	We can cast uint32s less that < INT_MAX to signed
	 */
	if (dst_type == FR_TYPE_INT32) {
		switch (src->type) {
		case FR_TYPE_UINT8:
			dst->datum.int32 = src->datum.uint8;
			break;

		case FR_TYPE_UINT16:
			dst->datum.int32 = src->datum.uint16;
			break;

		case FR_TYPE_UINT32:
			if (src->datum.uint32 > INT_MAX) {
				fr_strerror_printf("Invalid cast: From uint32 to signed.  uint32 value %u is larger "
						   "than max signed int and would overflow", src->datum.uint32);
				return -1;
			}
			dst->datum.int32 = (int)src->datum.uint32;
			break;

		case FR_TYPE_UINT64:
			if (src->datum.uint32 > INT_MAX) {
				fr_strerror_printf("Invalid cast: From uint64 to signed.  uint64 value %" PRIu64
						   " is larger than max signed int and would overflow", src->datum.uint64);
				return -1;
			}
			dst->datum.int32 = (int)src->datum.uint64;
			break;

		case FR_TYPE_OCTETS:
			goto do_octets;

		default:
			goto invalid_cast;
		}
		goto fixed_length;
	}

	if (dst_type == FR_TYPE_TIMEVAL) {
		switch (src->type) {
		case FR_TYPE_UINT8:
			dst->datum.timeval.tv_sec = src->datum.uint8;
			dst->datum.timeval.tv_usec = 0;
			break;

		case FR_TYPE_UINT16:
			dst->datum.timeval.tv_sec = src->datum.uint16;
			dst->datum.timeval.tv_usec = 0;
			break;

		case FR_TYPE_UINT32:
			dst->datum.timeval.tv_sec = src->datum.uint32;
			dst->datum.timeval.tv_usec = 0;
			break;

		case FR_TYPE_UINT64:
			/*
			 *	tv_sec is a time_t, which is variable in size
			 *	depending on the system.
			 *
			 *	It should be >= 64bits on modern systems,
			 *	but you never know...
			 */
			if (sizeof(uint64_t) > SIZEOF_MEMBER(struct timeval, tv_sec)) goto invalid_cast;
			dst->datum.timeval.tv_sec = src->datum.uint64;
			dst->datum.timeval.tv_usec = 0;
			break;

		default:
			goto invalid_cast;
		}
	}

	if (src->type == FR_TYPE_OCTETS) {
		fr_value_box_t tmp;

	do_octets:
		if (src->datum.length < fr_value_box_network_sizes[dst_type][0]) {
			fr_strerror_printf("Invalid cast from %s to %s.  Source is length %zd is smaller than "
					   "destination type size %zd",
					   fr_int2str(dict_attr_types, src->type, "<INVALID>"),
					   fr_int2str(dict_attr_types, dst_type, "<INVALID>"),
					   src->datum.length,
					   fr_value_box_network_sizes[dst_type][0]);
			return -1;
		}

		if (src->datum.length > fr_value_box_network_sizes[dst_type][1]) {
			fr_strerror_printf("Invalid cast from %s to %s.  Source length %zd is greater than "
					   "destination type size %zd",
					   fr_int2str(dict_attr_types, src->type, "<INVALID>"),
					   fr_int2str(dict_attr_types, dst_type, "<INVALID>"),
					   src->datum.length,
					   fr_value_box_network_sizes[dst_type][1]);
			return -1;
		}

		memset(&tmp, 0, sizeof(tmp));

		/*
		 *	Copy the raw octets into the datum of a value_box
		 *	inverting bytesex for uint32s (if LE).
		 */
		memcpy(&tmp.datum, src->datum.octets, fr_value_box_field_sizes[dst_type]);
		tmp.type = dst_type;
		dst->enumv = dst_enumv;

		fr_value_box_hton(dst, &tmp);

		/*
		 *	Fixup IP addresses.
		 */
		switch (dst->type) {
		case FR_TYPE_IPV4_ADDR:
			dst->datum.ip.af = AF_INET;
			dst->datum.ip.prefix = 128;
			dst->datum.ip.scope_id = 0;
			break;

		case FR_TYPE_IPV6_ADDR:
			dst->datum.ip.af = AF_INET6;
			dst->datum.ip.prefix = 128;
			dst->datum.ip.scope_id = 0;
			break;

		default:
			break;
		}

		return 0;
	}

	/*
	 *	Convert host order to network uint8 order.
	 */
	if ((dst_type == FR_TYPE_IPV4_ADDR) &&
	    ((src->type == FR_TYPE_UINT32) ||
	     (src->type == FR_TYPE_DATE) ||
	     (src->type == FR_TYPE_INT32))) {
	     	dst->datum.ip.af = AF_INET;
	     	dst->datum.ip.prefix = 32;
	     	dst->datum.ip.scope_id = 0;
		dst->datum.ip.addr.v4.s_addr = htonl(src->datum.uint32);

	} else if ((src->type == FR_TYPE_IPV4_ADDR) &&
		   ((dst_type == FR_TYPE_UINT32) ||
		    (dst_type == FR_TYPE_DATE) ||
		    (dst_type == FR_TYPE_INT32))) {
		dst->datum.uint32 = htonl(src->datum.ip.addr.v4.s_addr);

	} else {		/* they're of the same uint8 order */
		memcpy(&dst->datum, &src->datum, fr_value_box_field_sizes[src->type]);
	}

	dst->type = dst_type;
	dst->enumv = dst_enumv;

	return 0;
}

/** Assign a #fr_value_box_t value from an #fr_ipaddr_t
 *
 * Automatically determines the type of the value box from the ipaddr address family
 * and the length of the prefix field.
 *
 * @param[in] dst	to assign ipaddr to.
 * @param[in] ipaddr	to copy address from.
 * @param[in] tainted	Whether the value came from a trusted source.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
int fr_value_box_from_ipaddr(fr_value_box_t *dst, fr_ipaddr_t const *ipaddr, bool tainted)
{
	fr_type_t type;

	switch (ipaddr->af) {
	case AF_INET:
		if (ipaddr->prefix > 32) {
			fr_strerror_printf("Invalid IPv6 prefix length %i", ipaddr->prefix);
			return -1;
		}

		if (ipaddr->prefix == 32) {
			type = FR_TYPE_IPV4_ADDR;
		} else {
			type = FR_TYPE_IPV4_PREFIX;
		}
		break;

	case AF_INET6:
		if (ipaddr->prefix > 128) {
			fr_strerror_printf("Invalid IPv6 prefix length %i", ipaddr->prefix);
			return -1;
		}

		if (ipaddr->prefix == 128) {
			type = FR_TYPE_IPV6_ADDR;
		} else {
			type = FR_TYPE_IPV6_PREFIX;
		}
		break;

	default:
		fr_strerror_printf("Invalid address family %i", ipaddr->af);
		return -1;
	}

	dst->type = type;
	dst->tainted = tainted;
	dst->datum.ip = *ipaddr;
	dst->enumv = NULL;
	dst->next = NULL;

	return 0;
}

/** Copy value data verbatim duplicating any buffers
 *
 * @note Will free any exiting buffers associated with the dst #fr_value_box_t.
 *
 * @param ctx To allocate buffers in.
 * @param dst Where to copy value_box to.
 * @param src Where to copy value_box from.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
int fr_value_box_copy(TALLOC_CTX *ctx, fr_value_box_t *dst, const fr_value_box_t *src)
{
	if (!fr_cond_assert(src->type != FR_TYPE_INVALID)) return -1;

	switch (src->type) {
	default:
		memcpy(((uint8_t *)dst) + fr_value_box_offsets[src->type],
		       ((uint8_t const *)src) + fr_value_box_offsets[src->type],
		       fr_value_box_field_sizes[src->type]);
		break;

	case FR_TYPE_STRING:
	{
		char *str = NULL;

		/*
		 *	Zero length strings still have a one uint8 buffer
		 */
		str = talloc_bstrndup(ctx, src->datum.strvalue, src->datum.length);
		if (!str) {
			fr_strerror_printf("Failed allocating string buffer");
			return -1;
		}
		dst->datum.strvalue = str;
	}
		break;

	case FR_TYPE_OCTETS:
	{
		uint8_t *bin = NULL;

		if (src->datum.length) {
			bin = talloc_memdup(ctx, src->datum.octets, src->datum.length);
			if (!bin) {
				fr_strerror_printf("Failed allocating octets buffer");
				return -1;
			}
			talloc_set_type(bin, uint8_t);
		}
		dst->datum.octets = bin;
	}
		break;
	}

	fr_value_box_copy_meta(dst, src);

	return 0;
}

/** Perform a shallow copy of a value_box
 *
 * Like #fr_value_box_copy, but does not duplicate the buffers of the src value_box.
 *
 * For #FR_TYPE_STRING and #FR_TYPE_OCTETS adds a reference from ctx so that the
 * buffer cannot be freed until the ctx is freed.
 *
 * @note Will free any exiting buffers associated with the dst #fr_value_box_t.
 *
 * @param[in] ctx	to add reference from.  If NULL no reference will be added.
 * @param[in] dst	to copy value to.
 * @param[in] src	to copy value from.
 */
void fr_value_box_copy_shallow(TALLOC_CTX *ctx, fr_value_box_t *dst, fr_value_box_t const *src)
{
	switch (src->type) {
	default:
		fr_value_box_copy(ctx, dst, src);
		break;

	case FR_TYPE_STRING:
	case FR_TYPE_OCTETS:
		dst->datum.ptr = ctx ? talloc_reference(ctx, src->datum.ptr) : src->datum.ptr;
		fr_value_box_copy_meta(dst, src);
		break;
	}
}

/** Copy value data verbatim moving any buffers to the specified context
 *
 * @note Will free any exiting buffers associated with the dst #fr_value_box_t.
 *
 * @param[in] ctx 	to allocate any new buffers in.
 * @param[in] dst	to copy value to.
 * @param[in] src	to copy value from.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
int fr_value_box_steal(TALLOC_CTX *ctx, fr_value_box_t *dst, fr_value_box_t const *src)
{
	if (!fr_cond_assert(src->type != FR_TYPE_INVALID)) return -1;

	switch (src->type) {
	default:
		return fr_value_box_copy(ctx, dst, src);

	case FR_TYPE_STRING:
	{
		char const *str;

		str = talloc_steal(ctx, src->datum.strvalue);
		if (!str) {
			fr_strerror_printf("Failed stealing string buffer");
			return -1;
		}
		dst->datum.strvalue = str;
		fr_value_box_copy_meta(dst, src);
	}
		return 0;

	case FR_TYPE_OCTETS:
	{
		uint8_t const *bin;

 		bin = talloc_steal(ctx, src->datum.octets);
		if (!bin) {
			fr_strerror_printf("Failed stealing octets buffer");
			return -1;
		}
		dst->datum.octets = bin;
		fr_value_box_copy_meta(dst, src);
	}
		return 0;
	}
}

/** Copy a nul terminated string to a #fr_value_box_t
 *
 * @param[in] ctx 	to allocate any new buffers in.
 * @param[in] dst 	to assign new buffer to.
 * @param[in] src 	a nul terminated buffer.
 * @param[in] tainted	Whether the value came from a trusted source.
 */
int fr_value_box_strdup(TALLOC_CTX *ctx, fr_value_box_t *dst, char const *src, bool tainted)
{
	char const	*str;

	str = talloc_typed_strdup(ctx, src);
	if (!str) {
		fr_strerror_printf("Failed allocating string buffer");
		return -1;
	}

	dst->type = FR_TYPE_STRING;
	dst->tainted = tainted;
	dst->datum.strvalue = str;
	dst->datum.length = talloc_array_length(str) - 1;
	dst->enumv = NULL;
	dst->next = NULL;

	return 0;
}

/** Copy a string to to a #fr_value_box_t
 *
 * @param[in] ctx 	to allocate any new buffers in.
 * @param[in] dst 	to assign new buffer to.
 * @param[in] src 	a string.
 * @param[in] len	of src.
 * @param[in] tainted	Whether the value came from a trusted source.
 */
int fr_value_box_bstrndup(TALLOC_CTX *ctx, fr_value_box_t *dst, char const *src, size_t len, bool tainted)
{
	char const	*str;

	str = talloc_bstrndup(ctx, src, len);
	if (!str) {
		fr_strerror_printf("Failed allocating string buffer");
		return -1;
	}

	dst->type = FR_TYPE_STRING;
	dst->tainted = tainted;
	dst->datum.strvalue = str;
	dst->datum.length = len;
	dst->enumv = NULL;
	dst->next = NULL;

	return 0;
}

/** Copy a nul terminated talloced buffer to a #fr_value_box_t
 *
 * Copy a talloced nul terminated buffer, setting fields in the dst value box appropriately.
 *
 * The buffer must be \0 terminated, or an error will be returned.
 *
 * @param[in] ctx 	to allocate any new buffers in.
 * @param[in] dst 	to assign new buffer to.
 * @param[in] src 	a talloced nul terminated buffer.
 * @param[in] tainted	Whether the value came from a trusted source.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
int fr_value_box_strdup_buffer(TALLOC_CTX *ctx, fr_value_box_t *dst, char const *src, bool tainted)
{
	char	*str;
	size_t	len;

	len = talloc_array_length(src);
	if ((len == 1) || (src[len - 1] != '\0')) {
		fr_strerror_printf("Input buffer not \\0 terminated");
		return -1;
	}

	str = talloc_bstrndup(ctx, src, len - 1);
	if (!str) {
		fr_strerror_printf("Failed allocating string buffer");
		return -1;
	}

	dst->type = FR_TYPE_STRING;
	dst->tainted = tainted;
	dst->datum.strvalue = str;
	dst->datum.length = talloc_array_length(str) - 1;
	dst->enumv = NULL;
	dst->next = NULL;

	return 0;
}

/** Steal a nul terminated talloced buffer into a specified ctx, and assign to a #fr_value_box_t
 *
 * Steal a talloced nul terminated buffer, setting fields in the dst value box appropriately.
 *
 * The buffer must be \0 terminated, or an error will be returned.
 *
 * @param[in] ctx 	to allocate any new buffers in.
 * @param[in] dst 	to assign new buffer to.
 * @param[in] src 	a talloced nul terminated buffer.
 * @param[in] tainted	Whether the value came from a trusted source.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
int fr_value_box_strsteal(TALLOC_CTX *ctx, fr_value_box_t *dst, char *src, bool tainted)
{
	size_t	len;
	char	*str;

	len = talloc_array_length(src);
	if ((len == 1) || (src[len - 1] != '\0')) {
		fr_strerror_printf("Input buffer not \\0 terminated");
		return -1;
	}

	str = talloc_steal(ctx, src);
	if (!str) {
		fr_strerror_printf("Failed stealing string buffer");
		return -1;
	}

	dst->type = FR_TYPE_STRING;
	dst->tainted = tainted;
	dst->datum.strvalue = str;
	dst->datum.length = len - 1;
	dst->enumv = NULL;
	dst->next = NULL;

	return 0;
}

/** Assign a buffer containing a nul terminated string to a box, but don't copy it
 *
 * @param[in] dst	to assign string to.
 * @param[in] src	to copy string to.
 * @param[in] tainted	Whether the value came from a trusted source.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
int fr_value_box_strdup_shallow(fr_value_box_t *dst, char const *src, bool tainted)
{
	dst->type = FR_TYPE_STRING;
	dst->tainted = tainted;
	dst->datum.strvalue = src;
	dst->datum.length = strlen(src);
	dst->enumv = NULL;
	dst->next = NULL;

	return 0;
}

/** Assign a talloced buffer containing a nul terminated string to a box, but don't copy it
 *
 * Adds a reference to the src buffer so that it cannot be freed until the ctx is freed.
 *
 * @param[in] ctx	to add reference from.  If NULL no reference will be added.
 * @param[in] dst	to assign string to.
 * @param[in] src	to copy string to.
 * @param[in] tainted	Whether the value came from a trusted source.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
int fr_value_box_strdup_buffer_shallow(TALLOC_CTX *ctx, fr_value_box_t *dst, char const *src, bool tainted)
{
	size_t	len;

	(void) talloc_get_type_abort(src, char);

	len = talloc_array_length(src);
	if ((len == 1) || (src[len - 1] != '\0')) {
		fr_strerror_printf("Input buffer not \\0 terminated");
		return -1;
	}

	dst->type = FR_TYPE_STRING;
	dst->tainted = tainted;
	dst->datum.strvalue = ctx ? talloc_reference(ctx, src) : src;
	dst->datum.length = len - 1;
	dst->enumv = NULL;
	dst->next = NULL;

	return 0;
}

/** Copy a buffer to a fr_value_box_t
 *
 * Copy a buffer containing binary data, setting fields in the dst value box appropriately.
 *
 * Caller should set dst->taint = true, where the value was acquired from an untrusted source.
 *
 * @param[in] ctx	to allocate any new buffers in.
 * @param[in] dst	to assign new buffer to.
 * @param[in] src	a buffer.
 * @param[in] len	of data in the buffer.
 * @param[in] tainted	Whether the value came from a trusted source.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
int fr_value_box_memdup(TALLOC_CTX *ctx, fr_value_box_t *dst, uint8_t const *src, size_t len, bool tainted)
{
	uint8_t *bin;

	bin = talloc_memdup(ctx, src, len);
	if (!bin) {
		fr_strerror_printf("Failed allocating octets buffer");
		return -1;
	}

	dst->type = FR_TYPE_OCTETS;
	dst->tainted = tainted;
	dst->datum.octets = bin;
	dst->datum.length = len;
	dst->enumv = NULL;
	dst->next = NULL;

	return 0;
}

/** Copy a talloced buffer to a fr_value_box_t
 *
 * Copy a buffer containing binary data, setting fields in the dst value box appropriately.
 *
 * @param[in] ctx	to allocate any new buffers in.
 * @param[in] dst	to assign new buffer to.
 * @param[in] src	a buffer.
 * @param[in] tainted	Whether the value came from a trusted source.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
int fr_value_box_memdup_buffer(TALLOC_CTX *ctx, fr_value_box_t *dst, uint8_t *src, bool tainted)
{
	(void) talloc_get_type_abort(src, uint8_t);

	return fr_value_box_memdup(ctx, dst, src, talloc_array_length(src), tainted);
}

/** Steal a talloced buffer into a specified ctx, and assign to a #fr_value_box_t
 *
 * Steal a talloced buffer, setting fields in the dst value box appropriately.
 *
 * @param[in] ctx 	to allocate any new buffers in.
 * @param[in] dst 	to assign new buffer to.
 * @param[in] src 	a talloced nul terminated buffer.
 * @param[in] tainted	Whether the value came from a trusted source.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
int fr_value_box_memsteal(TALLOC_CTX *ctx, fr_value_box_t *dst, uint8_t const *src, bool tainted)
{
	uint8_t const	*bin;

	(void) talloc_get_type_abort(src, uint8_t);

	bin = talloc_steal(ctx, src);
	if (!bin) {
		fr_strerror_printf("Failed stealing buffer");
		return -1;
	}

	dst->type = FR_TYPE_OCTETS;
	dst->tainted = tainted;
	dst->datum.octets = bin;
	dst->datum.length = talloc_array_length(src);
	dst->enumv = NULL;
	dst->next = NULL;

	return 0;
}

/** Assign a buffer to a box, but don't copy it
 *
 * Adds a reference to the src buffer so that it cannot be freed until the ctx is freed.
 *
 * Caller should set dst->taint = true, where the value was acquired from an untrusted source.
 *
 * @note Will free any exiting buffers associated with the value box.
 *
 * @param[in] dst 	to assign buffer to.
 * @param[in] src	a talloced buffer.
 * @param[in] len	of buffer.
 * @param[in] tainted	Whether the value came from a trusted source.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
int fr_value_box_memdup_shallow(fr_value_box_t *dst, uint8_t *src, size_t len, bool tainted)
{
	dst->type = FR_TYPE_OCTETS;
	dst->tainted = tainted;
	dst->datum.octets = src;
	dst->datum.length = len;
	dst->enumv = NULL;
	dst->next = NULL;

	return 0;
}

/** Assign a talloced buffer to a box, but don't copy it
 *
 * Adds a reference to the src buffer so that it cannot be freed until the ctx is freed.
 *
 * @param[in] ctx 	to allocate any new buffers in.
 * @param[in] dst 	to assign buffer to.
 * @param[in] src	a talloced buffer.
 * @param[in] tainted	Whether the value came from a trusted source.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
int fr_value_box_memdup_buffer_shallow(TALLOC_CTX *ctx, fr_value_box_t *dst, uint8_t *src, bool tainted)
{
	(void) talloc_get_type_abort(src, uint8_t);

	dst->type = FR_TYPE_OCTETS;
	dst->tainted = tainted;
	dst->datum.octets = ctx ? talloc_reference(ctx, src) : src;
	dst->datum.length = talloc_array_length(src);
	dst->enumv = NULL;
	dst->next = NULL;

	return 0;
}

/** Convert integer encoded as string to a fr_value_box_t type
 *
 * @param[out] dst		where to write parsed value.
 * @param[in] dst_type		type of integer to convert string to.
 * @param[in] in		String to convert to integer.
 * @return
 *	- 0 on success.
 *	- -1 on parse error.
 */
static int fr_value_box_integer_str(fr_value_box_t *dst, fr_type_t dst_type, char const *in)
{
	uint64_t	uinteger = 0;
	int64_t		sinteger = 0;
	char 		*p = NULL;

	switch (dst_type) {
	case FR_TYPE_UINT8:
	case FR_TYPE_UINT16:
	case FR_TYPE_UINT32:
	case FR_TYPE_UINT64:
	case FR_TYPE_DATE_MILLISECONDS:
	case FR_TYPE_DATE_MICROSECONDS:
	case FR_TYPE_DATE_NANOSECONDS:
		uinteger = fr_strtoll(in, &p);
		if (*p != '\0') {
			fr_strerror_printf("Invalid integer value \"%s\"", in);

			return -1;
		}
		break;

	case FR_TYPE_INT8:
	case FR_TYPE_INT16:
	case FR_TYPE_INT32:
	case FR_TYPE_INT64:
		sinteger = fr_strtoll(in, &p);
		if (*p != '\0') {
			fr_strerror_printf("Invalid integer value \"%s\"", in);

			return -1;
		}
		break;

	default:
		if (!fr_cond_assert(0)) return -1;
	}

#define IN_RANGE_UNSIGNED(_type) \
	do { \
		if (uinteger > _type ## _MAX) { \
			fr_strerror_printf("Value %" PRIu64 " is invalid for type %s (must be in range " \
				    	   "0-%" PRIu64 ")", \
					   uinteger, fr_int2str(dict_attr_types, dst_type, "<INVALID>"), \
					   (uint64_t) _type ## _MAX); \
			return -1; \
		} \
	} while (0)

#define IN_RANGE_SIGNED(_type) \
	do { \
		if ((sinteger > _type ## _MAX) || (sinteger < _type ## _MIN)) { \
			fr_strerror_printf("Value %" PRIu64 " is invalid for type %s (must be in range " \
					   "%" PRIu64 "-%" PRIu64 ")", \
					   sinteger, fr_int2str(dict_attr_types, dst_type, "<INVALID>"), \
					   (int64_t) _type ## _MIN, (int64_t) _type ## _MAX); \
			return -1; \
		} \
	} while (0)

	switch (dst_type) {
	case FR_TYPE_UINT8:
		IN_RANGE_UNSIGNED(UINT8);
		dst->datum.uint8 = (uint8_t)uinteger;
		break;

	case FR_TYPE_UINT16:
		IN_RANGE_UNSIGNED(UINT16);
		dst->datum.uint16 = (uint16_t)uinteger;
		break;

	case FR_TYPE_UINT32:
		IN_RANGE_UNSIGNED(UINT32);
		dst->datum.uint32 = (uint32_t)uinteger;
		break;

	case FR_TYPE_UINT64:
		IN_RANGE_UNSIGNED(UINT64);
		dst->datum.uint64 = (uint64_t)uinteger;
		break;

	case FR_TYPE_INT8:
		IN_RANGE_SIGNED(INT8);
		dst->datum.int8 = (int8_t)sinteger;
		break;

	case FR_TYPE_INT16:
		IN_RANGE_SIGNED(INT16);
		dst->datum.int16 = (int16_t)sinteger;
		break;

	case FR_TYPE_INT32:
		IN_RANGE_SIGNED(INT32);
		dst->datum.int32 = (int32_t)sinteger;
		break;

	case FR_TYPE_INT64:
		IN_RANGE_SIGNED(INT64);
		dst->datum.int64 = (int64_t)sinteger;
		break;

	case FR_TYPE_DATE_MILLISECONDS:
		IN_RANGE_UNSIGNED(UINT64);
		dst->datum.date_milliseconds = (uint64_t)uinteger;
		break;

	case FR_TYPE_DATE_MICROSECONDS:
		IN_RANGE_UNSIGNED(UINT64);
		dst->datum.date_microseconds = (uint64_t)uinteger;
		break;

	case FR_TYPE_DATE_NANOSECONDS:
		IN_RANGE_UNSIGNED(UINT64);
		dst->datum.date_nanoseconds = (uint64_t)uinteger;
		break;

	default:
		if (!fr_cond_assert(0)) return -1;
	}

	return 0;
}

/** Convert string value to a fr_value_box_t type
 *
 * @todo Should take taint param.
 *
 * @param[in] ctx		to alloc strings in.
 * @param[out] dst		where to write parsed value.
 * @param[in,out] dst_type	of value data to create/dst_type of value created.
 * @param[in] dst_enumv		fr_dict_attr_t with string aliases for uint32 values.
 * @param[in] in		String to convert. Binary safe for variable length values
 *				if len is provided.
 * @param[in] inlen		may be < 0 in which case strlen(len) is used to determine
 *				length, else inlen should be the length of the string or
 *				sub string to parse.
 * @param[in] quote		character used set unescape mode.  @see value_str_unescape.
 * @param[in] tainted		Whether the value came from a trusted source.
 * @return
 *	- 0 on success.
 *	- -1 on parse error.
 */
int fr_value_box_from_str(TALLOC_CTX *ctx, fr_value_box_t *dst,
			  fr_type_t *dst_type, fr_dict_attr_t const *dst_enumv,
			  char const *in, ssize_t inlen, char quote, bool tainted)
{
	size_t		len;
	ssize_t		ret;
	char		buffer[256];

	if (!fr_cond_assert(*dst_type != FR_TYPE_INVALID)) return -1;

	if (!in) return -1;

	len = (inlen < 0) ? strlen(in) : (size_t)inlen;

	/*
	 *	Set size for all fixed length attributes.
	 */
	ret = dict_attr_sizes[*dst_type][1];	/* Max length */

	/*
	 *	Lookup any aliases before continuing
	 */
	if (dst_enumv) {
		char		*tmp = NULL;
		char		*alias;
		size_t		alias_len;
		fr_dict_enum_t	*enumv;

		if (len > (sizeof(buffer) - 1)) {
			tmp = talloc_array(NULL, char, len + 1);
			if (!tmp) return -1;

			alias_len = value_str_unescape((uint8_t *)tmp, in, len, quote);
			alias = tmp;
		} else {
			alias_len = value_str_unescape((uint8_t *)buffer, in, len, quote);
			alias = buffer;
		}
		alias[alias_len] = '\0';

		/*
		 *	Check the alias name is valid first before bothering
		 *	it look up up.
		 *
		 *	Catches any embedded \0 bytes that might cause
		 *	incorrect results.
		 */
		if (fr_dict_valid_name(alias, alias_len) < 0) {
			if (tmp) talloc_free(tmp);
			goto parse;
		}

		enumv = fr_dict_enum_by_alias(NULL, dst_enumv, alias);
		if (tmp) talloc_free(tmp);
		if (!enumv) goto parse;

		fr_value_box_copy(ctx, dst, enumv->value);
		dst->enumv = dst_enumv;

		return 0;
	}

parse:
	/*
	 *	It's a variable ret src->dst_type so we just alloc a new buffer
	 *	of size len and copy.
	 */
	switch (*dst_type) {
	case FR_TYPE_STRING:
	{
		char *buff, *p;

		buff = talloc_bstrndup(ctx, in, len);

		/*
		 *	No de-quoting.  Just copy the string.
		 */
		if (!quote) {
			ret = len;
			dst->datum.strvalue = buff;
			goto finish;
		}

		len = value_str_unescape((uint8_t *)buff, in, len, quote);

		/*
		 *	Shrink the buffer to the correct size
		 *	and \0 terminate it.  There is a significant
		 *	amount of legacy code that assumes the string
		 *	buffer in value pairs is a C string.
		 *
		 *	It's better for the server to print partial
		 *	strings, instead of SEGV.
		 */
		dst->datum.strvalue = p = talloc_realloc(ctx, buff, char, len + 1);
		p[len] = '\0';
		ret = len;
	}
		goto finish;

	case FR_TYPE_VSA:
		fr_strerror_printf("Must use 'Attr-26 = ...' instead of 'Vendor-Specific = ...'");
		return -1;

	/* raw octets: 0x01020304... */
	case FR_TYPE_OCTETS:
	{
		uint8_t	*p;

		/*
		 *	No 0x prefix, just copy verbatim.
		 */
		if ((len < 2) || (strncasecmp(in, "0x", 2) != 0)) {
			dst->datum.octets = talloc_memdup(ctx, (uint8_t const *)in, len);
			talloc_set_type(dst->datum.octets, uint8_t);
			ret = len;
			goto finish;
		}

		len -= 2;

		/*
		 *	Invalid.
		 */
		if ((len & 0x01) != 0) {
			fr_strerror_printf("Length of Hex String is not even, got %zu uint8s", len);
			return -1;
		}

		ret = len >> 1;
		p = talloc_array(ctx, uint8_t, ret);
		if (fr_hex2bin(p, ret, in + 2, len) != (size_t)ret) {
			talloc_free(p);
			fr_strerror_printf("Invalid hex data");
			return -1;
		}

		dst->datum.octets = p;
	}
		goto finish;

	case FR_TYPE_ABINARY:
#ifdef WITH_ASCEND_BINARY
		if ((len > 1) && (strncasecmp(in, "0x", 2) == 0)) {
			ssize_t bin;

			if (len > ((sizeof(dst->datum.filter) + 1) * 2)) {
				fr_strerror_printf("Hex data is too large for ascend filter");
				return -1;
			}

			bin = fr_hex2bin((uint8_t *) &dst->datum.filter, ret, in + 2, len - 2);
			if (bin < ret) {
				memset(((uint8_t *) &dst->datum.filter) + bin, 0, ret - bin);
			}
		} else {
			if (ascend_parse_filter(dst, in, len) < 0 ) {
				/* Allow ascend_parse_filter's strerror to bubble up */
				return -1;
			}
		}

		ret = sizeof(dst->datum.filter);
		goto finish;
#else
		/*
		 *	If Ascend binary is NOT defined,
		 *	then fall through to raw octets, so that
		 *	the user can at least make them by hand...
		 */
	 	goto do_octets;
#endif

	case FR_TYPE_IPV4_ADDR:
	{
		fr_ipaddr_t addr;

		if (fr_inet_pton4(&addr, in, inlen, fr_hostname_lookups, false, true) < 0) return -1;

		/*
		 *	We allow v4 addresses to have a /32 suffix as some databases (PostgreSQL)
		 *	print them this way.
		 */
		if (addr.prefix != 32) {
			fr_strerror_printf("Invalid IPv4 mask length \"/%i\".  Only \"/32\" permitted "
					   "for non-prefix types", addr.prefix);
			return -1;
		}

		memcpy(&dst->datum.ip, &addr, sizeof(dst->datum.ip));
	}
		goto finish;

	case FR_TYPE_IPV4_PREFIX:
		if (fr_inet_pton4(&dst->datum.ip, in, inlen, fr_hostname_lookups, false, true) < 0) return -1;
		goto finish;

	case FR_TYPE_IPV6_ADDR:
	{
		fr_ipaddr_t addr;

		if (fr_inet_pton6(&addr, in, inlen, fr_hostname_lookups, false, true) < 0) return -1;

		/*
		 *	We allow v6 addresses to have a /128 suffix as some databases (PostgreSQL)
		 *	print them this way.
		 */
		if (addr.prefix != 128) {
			fr_strerror_printf("Invalid IPv6 mask length \"/%i\".  Only \"/128\" permitted "
					   "for non-prefix types", addr.prefix);
			return -1;
		}

		memcpy(&dst->datum.ip, &addr, sizeof(dst->datum.ip));
	}
		goto finish;

	case FR_TYPE_IPV6_PREFIX:
		if (fr_inet_pton6(&dst->datum.ip, in, inlen, fr_hostname_lookups, false, true) < 0) return -1;
		goto finish;

	/*
	 *	Dealt with below
	 */
	default:
		break;

	case FR_TYPE_STRUCTURAL_EXCEPT_VSA:
	case FR_TYPE_VENDOR:
	case FR_TYPE_BAD:
		fr_strerror_printf("Invalid dst_type %d", *dst_type);
		return -1;
	}

	/*
	 *	It's a fixed size src->dst_type, copy to a temporary buffer and
	 *	\0 terminate if insize >= 0.
	 */
	if (inlen > 0) {
		if (len >= sizeof(buffer)) {
			fr_strerror_printf("Temporary buffer too small");
			return -1;
		}

		memcpy(buffer, in, inlen);
		buffer[inlen] = '\0';
		in = buffer;
	}

	switch (*dst_type) {
	case FR_TYPE_IPV4_ADDR:
	case FR_TYPE_IPV4_PREFIX:
	case FR_TYPE_IPV6_ADDR:
	case FR_TYPE_IPV6_PREFIX:
		break;

	case FR_TYPE_UINT8:
	case FR_TYPE_UINT16:
	case FR_TYPE_UINT32:
	case FR_TYPE_UINT64:
	case FR_TYPE_INT8:
	case FR_TYPE_INT16:
	case FR_TYPE_INT32:
	case FR_TYPE_INT64:
	case FR_TYPE_DATE_MILLISECONDS:
	case FR_TYPE_DATE_MICROSECONDS:
	case FR_TYPE_DATE_NANOSECONDS:
		if (fr_value_box_integer_str(dst, *dst_type, in) < 0) return -1;
		break;

	case FR_TYPE_SIZE:
	{
		size_t i;

		if (sscanf(in, "%zu", &i) != 1) {
			fr_strerror_printf("Failed parsing \"%s\" as a file or memory size", in);
			return -1;
		}
		dst->datum.size = i;
	}
		break;

	case FR_TYPE_TIMEVAL:
		if (fr_timeval_from_str(&dst->datum.timeval, in) < 0) return -1;
		break;

	case FR_TYPE_FLOAT32:
	{
		float f;

		if (sscanf(in, "%f", &f) != 1) {
			fr_strerror_printf("Failed parsing \"%s\" as a float32", in);
			return -1;
		}
		dst->datum.float32 = f;
	}

	case FR_TYPE_FLOAT64:
	{
		double d;

		if (sscanf(in, "%lf", &d) != 1) {
			fr_strerror_printf("Failed parsing \"%s\" as a float64", in);
			return -1;
		}
		dst->datum.float64 = d;
	}
		break;

	case FR_TYPE_DATE:
	{
		/*
		 *	time_t may be 64 bits, while vp_date MUST be 32-bits.  We need an
		 *	intermediary variable to handle the conversions.
		 */
		time_t date;

		if (fr_time_from_str(&date, in) < 0) {
			fr_strerror_printf("failed to parse time string \"%s\"", in);
			return -1;
		}

		dst->datum.date = date;
	}

		break;

	case FR_TYPE_IFID:
		if (fr_inet_ifid_pton((void *) dst->datum.ifid, in) == NULL) {
			fr_strerror_printf("Failed to parse interface-id string \"%s\"", in);
			return -1;
		}
		break;

	case FR_TYPE_ETHERNET:
	{
		char const *c1, *c2, *cp;
		size_t p_len = 0;

		/*
		 *	Convert things which are obviously uint32s to Ethernet addresses
		 *
		 *	We assume the number is the bigendian representation of the
		 *	ethernet address.
		 */
		if (is_integer(in)) {
			uint64_t uint32 = htonll(atoll(in));

			memcpy(dst->datum.ether, &uint32, sizeof(dst->datum.ether));
			break;
		}

		cp = in;
		while (*cp) {
			if (cp[1] == ':') {
				c1 = hextab;
				c2 = memchr(hextab, tolower((int) cp[0]), 16);
				cp += 2;
			} else if ((cp[1] != '\0') && ((cp[2] == ':') || (cp[2] == '\0'))) {
				c1 = memchr(hextab, tolower((int) cp[0]), 16);
				c2 = memchr(hextab, tolower((int) cp[1]), 16);
				cp += 2;
				if (*cp == ':') cp++;
			} else {
				c1 = c2 = NULL;
			}
			if (!c1 || !c2 || (p_len >= sizeof(dst->datum.ether))) {
				fr_strerror_printf("failed to parse Ethernet address \"%s\"", in);
				return -1;
			}
			dst->datum.ether[p_len] = ((c1-hextab)<<4) + (c2-hextab);
			p_len++;
		}
	}
		break;

	/*
	 *	Crazy polymorphic (IPv4/IPv6) attribute src->dst_type for WiMAX.
	 *
	 *	We try and make is saner by replacing the original
	 *	da, with either an IPv4 or IPv6 da src->dst_type.
	 *
	 *	These are not dynamic da, and will have the same vendor
	 *	and attribute as the original.
	 */
	case FR_TYPE_COMBO_IP_ADDR:
	{
		if (fr_inet_pton(&dst->datum.ip, in, inlen, AF_UNSPEC, fr_hostname_lookups, true) < 0) return -1;
		switch (dst->datum.ip.af) {
		case AF_INET:
			ret = dict_attr_sizes[FR_TYPE_COMBO_IP_ADDR][0]; /* size of IPv4 address */
			*dst_type = FR_TYPE_IPV4_ADDR;
			break;

		case AF_INET6:
			*dst_type = FR_TYPE_IPV6_ADDR;
			ret = dict_attr_sizes[FR_TYPE_COMBO_IP_ADDR][1]; /* size of IPv6 address */
			break;

		default:
			fr_strerror_printf("Bad address family %i", dst->datum.ip.af);
			return -1;
		}
	}
		break;

	case FR_TYPE_BOOL:
		if ((strcmp(in, "yes") == 0) || strcmp(in, "true") == 0) {
			dst->datum.boolean = true;
		} else if ((strcmp(in, "no") == 0) || (strcmp(in, "false") == 0)) {
			dst->datum.boolean = false;
		} else {
			fr_strerror_printf("\"%s\" is not a valid boolean value", in);
			return -1;
		}
		break;

	case FR_TYPE_COMBO_IP_PREFIX:
		break;

	case FR_TYPE_VARIABLE_SIZE:		/* Should have been dealt with above */
	case FR_TYPE_STRUCTURAL:	/* Listed again to suppress compiler warnings */
	case FR_TYPE_BAD:
		fr_strerror_printf("Unknown attribute dst_type %d", *dst_type);
		return -1;
	}

finish:
	dst->datum.length = ret;
	dst->type = *dst_type;
	dst->tainted = tainted;

	/*
	 *	Fixup enumv
	 */
	dst->enumv = dst_enumv;
	dst->next = NULL;

	return 0;
}

/** Print one attribute value to a string
 *
 */
char *fr_value_box_asprint(TALLOC_CTX *ctx, fr_value_box_t const *data, char quote)
{
	char *p = NULL;

	if (!fr_cond_assert(data->type != FR_TYPE_INVALID)) return NULL;

	if (data->enumv) {
		fr_dict_enum_t const	*dv;

		dv = fr_dict_enum_by_value(NULL, data->enumv, data);
		if (dv) return talloc_typed_strdup(ctx, dv->alias);
	}

	switch (data->type) {
	case FR_TYPE_STRING:
	{
		size_t len, ret;

		if (!quote) {
			p = talloc_bstrndup(ctx, data->datum.strvalue, data->datum.length);
			if (!p) return NULL;
			talloc_set_type(p, char);
			return p;
		}

		/* Gets us the size of the buffer we need to alloc */
		len = fr_snprint_len(data->datum.strvalue, data->datum.length, quote);
		p = talloc_array(ctx, char, len);
		if (!p) return NULL;

		ret = fr_snprint(p, len, data->datum.strvalue, data->datum.length, quote);
		if (!fr_cond_assert(ret == (len - 1))) {
			talloc_free(p);
			return NULL;
		}
		break;
	}

	case FR_TYPE_OCTETS:
		p = talloc_array(ctx, char, 2 + 1 + data->datum.length * 2);
		if (!p) return NULL;
		p[0] = '0';
		p[1] = 'x';

		fr_bin2hex(p + 2, data->datum.octets, data->datum.length);
		p[2 + (data->datum.length * 2)] = '\0';
		break;

	/*
	 *	We need to use the proper inet_ntop functions for IP
	 *	addresses, else the output might not match output of
	 *	other functions, which makes testing difficult.
	 *
	 *	An example is tunneled ipv4 in ipv6 addresses.
	 */
	case FR_TYPE_IPV4_ADDR:
	case FR_TYPE_IPV4_PREFIX:
	{
		char buff[INET_ADDRSTRLEN  + 4]; // + /prefix

		buff[0] = '\0';
		fr_value_box_snprint(buff, sizeof(buff), data, '\0');

		p = talloc_typed_strdup(ctx, buff);
	}
	break;

	case FR_TYPE_IPV6_ADDR:
	case FR_TYPE_IPV6_PREFIX:
	{
		char buff[INET6_ADDRSTRLEN + 4]; // + /prefix

		buff[0] = '\0';
		fr_value_box_snprint(buff, sizeof(buff), data, '\0');

		p = talloc_typed_strdup(ctx, buff);
	}
	break;

	case FR_TYPE_IFID:
		p = talloc_typed_asprintf(ctx, "%x:%x:%x:%x",
					  (data->datum.ifid[0] << 8) | data->datum.ifid[1],
					  (data->datum.ifid[2] << 8) | data->datum.ifid[3],
					  (data->datum.ifid[4] << 8) | data->datum.ifid[5],
					  (data->datum.ifid[6] << 8) | data->datum.ifid[7]);
		break;

	case FR_TYPE_ETHERNET:
		p = talloc_typed_asprintf(ctx, "%02x:%02x:%02x:%02x:%02x:%02x",
					  data->datum.ether[0], data->datum.ether[1],
					  data->datum.ether[2], data->datum.ether[3],
					  data->datum.ether[4], data->datum.ether[5]);
		break;

	case FR_TYPE_BOOL:
		p = talloc_typed_strdup(ctx, data->datum.uint8 ? "yes" : "no");
		break;

	case FR_TYPE_UINT8:
		p = talloc_typed_asprintf(ctx, "%u", data->datum.uint8);
		break;

	case FR_TYPE_UINT16:
		p = talloc_typed_asprintf(ctx, "%u", data->datum.uint16);
		break;

	case FR_TYPE_UINT32:
		p = talloc_typed_asprintf(ctx, "%u", data->datum.uint32);
		break;

	case FR_TYPE_UINT64:
		p = talloc_typed_asprintf(ctx, "%" PRIu64, data->datum.uint64);
		break;

	case FR_TYPE_INT8:
		p = talloc_typed_asprintf(ctx, "%d", data->datum.int8);
		break;

	case FR_TYPE_INT16:
		p = talloc_typed_asprintf(ctx, "%d", data->datum.int16);
		break;

	case FR_TYPE_INT32:
		p = talloc_typed_asprintf(ctx, "%d", data->datum.int32);
		break;

	case FR_TYPE_INT64:
		p = talloc_typed_asprintf(ctx, "%" PRId64, data->datum.int64);
		break;

	case FR_TYPE_FLOAT32:
		p = talloc_typed_asprintf(ctx, "%f", data->datum.float32);
		break;

	case FR_TYPE_FLOAT64:
		p = talloc_typed_asprintf(ctx, "%g", data->datum.float64);
		break;

	case FR_TYPE_DATE:
	{
		time_t t;
		struct tm s_tm;

		t = data->datum.date;

		p = talloc_array(ctx, char, 64);
		strftime(p, 64, "%b %e %Y %H:%M:%S %Z",
			 localtime_r(&t, &s_tm));
		break;
	}

	case FR_TYPE_DATE_MILLISECONDS:
		p = talloc_typed_asprintf(ctx, "%" PRIu64, data->datum.date_milliseconds);
		break;

	case FR_TYPE_DATE_MICROSECONDS:
		p = talloc_typed_asprintf(ctx, "%" PRIu64, data->datum.date_microseconds);
		break;

	case FR_TYPE_DATE_NANOSECONDS:
		p = talloc_typed_asprintf(ctx, "%" PRIu64, data->datum.date_nanoseconds);
		break;

	case FR_TYPE_SIZE:
		p = talloc_typed_asprintf(ctx, "%zu", data->datum.size);
		break;

	case FR_TYPE_TIMEVAL:
		p = talloc_typed_asprintf(ctx, "%" PRIu64 ".%06" PRIu64,
					  (uint64_t)data->datum.timeval.tv_sec, (uint64_t)data->datum.timeval.tv_usec);
		break;


	case FR_TYPE_ABINARY:
#ifdef WITH_ASCEND_BINARY
		p = talloc_array(ctx, char, 128);
		if (!p) return NULL;
		print_abinary(p, 128, (uint8_t const *) &data->datum.filter, data->datum.length, 0);
		break;
#else
		  /* FALL THROUGH */
#endif

	/*
	 *	Don't add default here
	 */
	case FR_TYPE_COMBO_IP_ADDR:
	case FR_TYPE_COMBO_IP_PREFIX:
	case FR_TYPE_STRUCTURAL:
	case FR_TYPE_BAD:
		(void)fr_cond_assert(0);
		return NULL;
	}

	return p;
}

/** Print the value of an attribute to a string
 *
 * @note return value should be checked with is_truncated.
 * @note Will always \0 terminate unless outlen == 0.
 *
 * @param out Where to write the printed version of the attribute value.
 * @param outlen Length of the output buffer.
 * @param data to print.
 * @param quote char to escape in string output.
 * @return
 *	- The number of uint8s written to the out buffer.
 *	- A number >= outlen if truncation has occurred.
 */
size_t fr_value_box_snprint(char *out, size_t outlen, fr_value_box_t const *data, char quote)
{
	char		buf[1024];	/* Interim buffer to use with poorly behaved printing functions */
	char const	*a = NULL;
	char		*p = out;
	time_t		t;
	struct tm	s_tm;

	size_t		len = 0, freespace = outlen;

	if (!fr_cond_assert(data->type != FR_TYPE_INVALID)) return -1;

	if (!data) return 0;
	if (outlen == 0) return data->datum.length;

	*out = '\0';

	p = out;

	if (data->enumv) {
		fr_dict_enum_t const	*dv;

		dv = fr_dict_enum_by_value(NULL, data->enumv, data);
		if (dv) return strlcpy(out, dv->alias, outlen);
	}

	switch (data->type) {
	case FR_TYPE_STRING:

		/*
		 *	Ensure that WE add the quotation marks around the string.
		 */
		if (quote) {
			if (freespace < 3) return data->datum.length + 2;

			*p++ = quote;
			freespace--;

			len = fr_snprint(p, freespace, data->datum.strvalue, data->datum.length, quote);
			/* always terminate the quoted string with another quote */
			if (len >= (freespace - 1)) {
				/* Use out not p as we're operating on the entire buffer */
				out[outlen - 2] = (char) quote;
				out[outlen - 1] = '\0';
				return len + 2;
			}
			p += len;
			freespace -= len;

			*p++ = (char) quote;
			freespace--;
			*p = '\0';

			return len + 2;
		}

		return fr_snprint(out, outlen, data->datum.strvalue, data->datum.length, quote);

	case FR_TYPE_IPV4_ADDR:
	case FR_TYPE_IPV6_ADDR:
		a = fr_inet_ntop(buf, sizeof(buf), &data->datum.ip);
		len = strlen(buf);
		break;

	case FR_TYPE_IPV4_PREFIX:
	case FR_TYPE_IPV6_PREFIX:
		a = fr_inet_ntop_prefix(buf, sizeof(buf), &data->datum.ip);
		len = strlen(buf);
		break;

	case FR_TYPE_IFID:
		a = fr_inet_ifid_ntop(buf, sizeof(buf), data->datum.ifid);
		len = strlen(buf);
		break;

	case FR_TYPE_ETHERNET:
		return snprintf(out, outlen, "%02x:%02x:%02x:%02x:%02x:%02x",
				data->datum.ether[0], data->datum.ether[1],
				data->datum.ether[2], data->datum.ether[3],
				data->datum.ether[4], data->datum.ether[5]);

	case FR_TYPE_UINT8:
		return snprintf(out, outlen, "%u", data->datum.uint8);

	case FR_TYPE_UINT16:
		return snprintf(out, outlen, "%u", data->datum.uint16);

	case FR_TYPE_UINT32:
		return snprintf(out, outlen, "%u", data->datum.uint32);

	case FR_TYPE_UINT64:
		return snprintf(out, outlen, "%" PRIu64, data->datum.uint64);

	case FR_TYPE_INT8:
		return snprintf(out, outlen, "%d", data->datum.int8);

	case FR_TYPE_INT16:
		return snprintf(out, outlen, "%d", data->datum.int16);

	case FR_TYPE_INT32:
		return snprintf(out, outlen, "%d", data->datum.int32);

	case FR_TYPE_INT64:
		return snprintf(out, outlen, "%" PRId64, data->datum.int64);

	case FR_TYPE_FLOAT32:
		return snprintf(out, outlen, "%f", data->datum.float32);

	case FR_TYPE_FLOAT64:
		return snprintf(out, outlen, "%g", data->datum.float64);

	case FR_TYPE_DATE:
		t = data->datum.date;
		if (quote > 0) {
			len = strftime(buf, sizeof(buf) - 1, "%%%b %e %Y %H:%M:%S %Z%%", localtime_r(&t, &s_tm));
			buf[0] = (char) quote;
			buf[len - 1] = (char) quote;
			buf[len] = '\0';
		} else {
			len = strftime(buf, sizeof(buf), "%b %e %Y %H:%M:%S %Z", localtime_r(&t, &s_tm));
		}
		a = buf;
		break;

	case FR_TYPE_DATE_MILLISECONDS:
		return snprintf(out, outlen, "%" PRIu64, data->datum.date_milliseconds);

	case FR_TYPE_DATE_MICROSECONDS:
		return snprintf(out, outlen, "%" PRIu64, data->datum.date_microseconds);

	case FR_TYPE_DATE_NANOSECONDS:
		return snprintf(out, outlen, "%" PRIu64, data->datum.date_nanoseconds);

	case FR_TYPE_ABINARY:
#ifdef WITH_ASCEND_BINARY
		print_abinary(buf, sizeof(buf), (uint8_t const *) data->datum.filter, data->datum.length, quote);
		a = buf;
		len = strlen(buf);
		break;
#else
	/* FALL THROUGH */
#endif
	case FR_TYPE_OCTETS:
	case FR_TYPE_TLV:
	{
		size_t max;

		/* Return the number of uint8s we would have written */
		len = (data->datum.length * 2) + 2;
		if (freespace <= 1) {
			return len;
		}

		*out++ = '0';
		freespace--;

		if (freespace <= 1) {
			*out = '\0';
			return len;
		}
		*out++ = 'x';
		freespace--;

		if (freespace <= 2) {
			*out = '\0';
			return len;
		}

		/* Get maximum number of uint8s we can encode given freespace */
		max = ((freespace % 2) ? freespace - 1 : freespace - 2) / 2;
		fr_bin2hex(out, data->datum.octets,
			   ((size_t)data->datum.length > max) ? max : (size_t)data->datum.length);
	}
		return len;



	case FR_TYPE_SIZE:
		return snprintf(out, outlen, "%zu", data->datum.size);

	case FR_TYPE_TIMEVAL:
		len = snprintf(buf, sizeof(buf),  "%" PRIu64 ".%06" PRIu64,
			       (uint64_t)data->datum.timeval.tv_sec, (uint64_t)data->datum.timeval.tv_usec);
		a = buf;
		break;

	/*
	 *	Don't add default here
	 */
	case FR_TYPE_INVALID:
	case FR_TYPE_COMBO_IP_ADDR:
	case FR_TYPE_COMBO_IP_PREFIX:
	case FR_TYPE_EXTENDED:
	case FR_TYPE_LONG_EXTENDED:
	case FR_TYPE_EVS:
	case FR_TYPE_VSA:
	case FR_TYPE_VENDOR:
	case FR_TYPE_BOOL:
	case FR_TYPE_STRUCT:
	case FR_TYPE_MAX:
		(void)fr_cond_assert(0);
		*out = '\0';
		return 0;
	}

	if (a) strlcpy(out, a, outlen);

	return len;	/* Return the number of uint8s we would of written (for truncation detection) */
}
