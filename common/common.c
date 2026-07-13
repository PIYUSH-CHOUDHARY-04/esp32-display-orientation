/**
 * @file common.c
 * @ingroup shared
 * @brief Checksum generation and verification for the wire protocol.
 *
 * Compiled into BOTH the PC daemon and the ESP firmware, from the same source, which is what
 * guarantees the two sides compute the checksum identically. A protocol where each end had its own
 * implementation would work right up until somebody fixed a bug in one of them.
 *
 * ### The algorithm
 * The internet checksum (RFC 1071), the same one IP, UDP and TCP use:
 * -# sum every byte of the packet, treating the checksum field itself as zero;
 * -# fold the carries back in until the sum fits in 16 bits;
 * -# take the one's complement.
 *
 * Chosen because it is cheap enough to run on the ESP32 for every packet, and because the folding
 * step makes it order-independent -- a transposition of bytes still changes the result, which a
 * plain truncating sum would not always catch.
 *
 * @warning This detects CORRUPTION, not TAMPERING. It is a 16-bit checksum with a published
 *          algorithm: anyone who can modify a packet can trivially recompute a valid checksum for
 *          it. That is fine here, because the protocol does not rely on it for security -- the
 *          credentials are protected by AES-256-GCM, whose authentication tag is what actually
 *          resists forgery. The checksum exists to catch a truncated read or an interleaved buffer,
 *          which is a much more likely failure on a local Wi-Fi link than an attacker.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "common.h"

/**
 * @brief Compute the internet checksum over a packet, treating the checksum field as zero.
 *
 * The shared core of ::proto_generate_chksum and ::proto_verify_chksum. Both need the identical
 * computation, and keeping it in one place is not merely tidy -- two copies of a checksum algorithm
 * that must agree forever is a bug waiting for the day somebody changes one of them. The symptom
 * would be every packet failing verification, and the cause would be invisible in either function
 * read on its own.
 *
 * ### Why the checksum field is skipped
 * The checksum covers the header, and the checksum field is IN the header. Including it would make
 * the value depend on itself. Both sides therefore skip those two bytes: the sender because the
 * field is not yet written, the receiver because it must reproduce exactly what the sender computed.
 *
 * @c offsetof is used rather than a hardcoded index so the skip follows the field if the header is
 * ever reordered.
 *
 * ### Bounding the length
 * @p len derives from @c pkt->header.length, which arrives FROM THE WIRE and has been checked by
 * nothing at the point this runs -- checking it is what this function exists to enable. So it cannot
 * be trusted as a loop bound.
 *
 * A packet is @c sizeof(proto_packet_t) bytes; on this build, 157. A @c length field claiming 5000
 * makes the loop read some 4.8 kilobytes past the end of that buffer. Anything from roughly 150 up
 * to 65527 overruns; only the very top of the range wraps back round to something harmless, which is
 * exactly the sort of accident that makes an unbounded loop look safe under casual testing.
 *
 * ::__chksum_len__ therefore clamps it to the real packet size. An over-long length then produces a
 * checksum computed over the wrong number of bytes, which fails verification -- and a rejected packet
 * is the correct outcome, as well as being vastly preferable to reading memory that is not ours.
 *
 * @param pkt The packet.
 * @param len Number of bytes to sum, already clamped by the caller.
 * @return The one's-complement folded sum.
 */
static uint16_t __checksum_bytes__(const proto_packet_t* pkt, size_t len){
	const uint8_t* data = (const uint8_t*)pkt;
	uint32_t chksum = 0;

	for(size_t i = 0; i < len; i++){
		if(i == offsetof(proto_hdr_t, chksum) || i == offsetof(proto_hdr_t, chksum) + 1){
			continue;
		}

		chksum += data[i];
	}

	/*
	 * Fold the carries back into the low 16 bits. A loop rather than a single add, because folding
	 * can itself produce a carry.
	 */
	while(chksum >> 16){
		chksum = (chksum & 0xFFFF) + (chksum >> 16);
	}

	return (uint16_t)(~chksum);
}

/**
 * @brief Work out how many bytes of @p pkt the checksum covers, bounded by the real packet size.
 *
 * @c header.length is attacker-reachable and unvalidated at this point, so it is treated as a hint
 * rather than a fact. The true ceiling is @c sizeof(proto_packet_t): nothing this protocol can send
 * is larger, so a length claiming otherwise is corrupt by definition.
 *
 * @note The addition is done in @c size_t, not in the @c uint16_t the field is declared as. In
 *       16-bit arithmetic @c 0xFFFF @c + @c 8 wraps to 7 -- so the original code silently
 *       checksummed seven bytes of a packet that claimed to be sixty-four kilobytes, and passed. The
 *       wrap concealed the bug rather than preventing it: the lengths that DID overrun were the
 *       unremarkable ones in between.
 *
 * @param pkt The packet.
 * @return The number of bytes to checksum, never more than @c sizeof(proto_packet_t).
 */
static size_t __chksum_len__(const proto_packet_t* pkt){
	size_t len = (size_t)pkt->header.length + sizeof(proto_hdr_t);

	if(len > sizeof(proto_packet_t)){
		len = sizeof(proto_packet_t);
	}

	return len;
}

/**
 * @brief Compute the checksum for an outgoing packet.
 *
 * @note Must be called LAST, once every other field -- @c magic, @c msg_id, @c length, and the whole
 *       payload -- is final. The checksum covers all of them, so computing it earlier checksums a
 *       half-built packet, and the receiver rejects the result. This is an easy mistake to make when
 *       adding a field to the header, and it fails at the far end rather than where it was caused.
 *
 * @note The parameter and return contract lives with the DECLARATION, in common.h -- not here. That
 *       is where a caller looks, and duplicating it in both places would mean one contract with two
 *       copies free to drift apart. Doxygen also merges the two blocks and then reports the
 *       duplication, which is a fair complaint.
 */
uint16_t proto_generate_chksum(const proto_packet_t* pkt){
	return __checksum_bytes__(pkt, __chksum_len__(pkt));
}

/**
 * @brief Verify the checksum of a received packet.
 *
 * Recomputes the checksum the same way the sender did -- same skipped field, same fold, same
 * complement -- and compares.
 *
 * @warning Call this BEFORE reading anything out of the payload. ::msg_payload_u is a union whose
 *          members overlap in memory, so a corrupt @c msg_id does not produce an error: it produces
 *          a confident misinterpretation of the same bytes as an entirely different message type.
 *          The checksum is the only thing standing between a truncated read and the receiver acting
 *          on nonsense.
 *
 * @note The parameter and return contract lives with the DECLARATION, in common.h -- not here.
 */
bool proto_verify_chksum(const proto_packet_t* pkt){
	uint16_t received = pkt->header.chksum;
	uint16_t calculated = __checksum_bytes__(pkt, __chksum_len__(pkt));

	return (calculated == received);
}
