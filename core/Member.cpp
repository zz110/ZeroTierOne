/*
 * Copyright (c)2013-2020 ZeroTier, Inc.
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file in the project's root directory.
 *
 * Change Date: 2025-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2.0 of the Apache License.
 */
/****/

#include <algorithm>

#include "Member.hpp"
#include "RuntimeEnvironment.hpp"
#include "Peer.hpp"
#include "Topology.hpp"

namespace ZeroTier {

Member::Member() :
	m_comRevocationThreshold(0),
	m_lastPushedCredentials(0),
	m_comAgreementLocalTimestamp(0),
	m_comAgreementRemoteTimestamp(0)
{
}

void Member::pushCredentials(const RuntimeEnvironment *RR, void *tPtr, const int64_t now, const SharedPtr< Peer > &to, const NetworkConfig &nconf)
{
	if (!nconf.com) // sanity check
		return;

#if 0
	SharedPtr<Buf> outp(new Buf());
	Protocol::Header &ph = outp->as<Protocol::Header>();

	unsigned int capPtr = 0,tagPtr = 0,cooPtr = 0;
	bool sendCom = true;
	bool complete = false;
	while (!complete) {
		ph.packetId = Protocol::getPacketId();
		to->address().copyTo(ph.destination);
		RR->identity.address().copyTo(ph.source);
		ph.flags = 0;
		ph.verb = Protocol::VERB_NETWORK_CREDENTIALS;

		int outl = sizeof(Protocol::Header);

		if (sendCom) {
			sendCom = false;
			outp->wO(outl,nconf.com);
		}
		outp->wI8(outl,0);

		if ((outl + ZT_CAPABILITY_MARSHAL_SIZE_MAX + 2) < ZT_PROTO_MAX_PACKET_LENGTH) {
			void *const capCountAt = outp->unsafeData + outl;
			outl += 2;
			unsigned int capCount = 0;
			while (capPtr < nconf.capabilityCount) {
				outp->wO(outl,nconf.capabilities[capPtr++]);
				++capCount;
				if ((outl + ZT_CAPABILITY_MARSHAL_SIZE_MAX) >= ZT_PROTO_MAX_PACKET_LENGTH)
					break;
			}
			Utils::storeBigEndian(capCountAt,(uint16_t)capCount);

			if ((outl + ZT_TAG_MARSHAL_SIZE_MAX + 4) < ZT_PROTO_MAX_PACKET_LENGTH) {
				void *const tagCountAt = outp->unsafeData + outl;
				outl += 2;
				unsigned int tagCount = 0;
				while (tagPtr < nconf.tagCount) {
					outp->wO(outl,nconf.tags[tagPtr++]);
					++tagCount;
					if ((outl + ZT_TAG_MARSHAL_SIZE_MAX) >= ZT_PROTO_MAX_PACKET_LENGTH)
						break;
				}
				Utils::storeBigEndian(tagCountAt,(uint16_t)tagCount);

				outp->wI16(outl,0); // no revocations sent here as these propagate differently

				if ((outl + ZT_CERTIFICATEOFOWNERSHIP_MARSHAL_SIZE_MAX + 2) < ZT_PROTO_MAX_PACKET_LENGTH) {
					void *const cooCountAt = outp->unsafeData + outl;
					outl += 2;
					unsigned int cooCount = 0;
					while (cooPtr < nconf.certificateOfOwnershipCount) {
						outp->wO(outl,nconf.certificatesOfOwnership[cooPtr++]);
						++cooCount;
						if ((outl + ZT_CERTIFICATEOFOWNERSHIP_MARSHAL_SIZE_MAX) >= ZT_PROTO_MAX_PACKET_LENGTH)
							break;
					}
					Utils::storeBigEndian(cooCountAt,(uint16_t)cooCount);

					complete = true;
				} else {
					outp->wI16(outl,0);
				}
			} else {
				outp->wI32(outl,0);
				outp->wI16(outl,0); // three zero 16-bit integers
			}
		} else {
			outp->wI64(outl,0); // four zero 16-bit integers
		}

		if (outl > (int)sizeof(Protocol::Header)) {
			outl = Protocol::compress(outp,outl);
			// TODO
		}
	}
#endif

	m_lastPushedCredentials = now;
}

void Member::clean(const NetworkConfig &nconf)
{
	m_cleanCredImpl< TagCredential >(nconf, m_remoteTags);
	m_cleanCredImpl< CapabilityCredential >(nconf, m_remoteCaps);
	m_cleanCredImpl< OwnershipCredential >(nconf, m_remoteCoos);
}

Member::AddCredentialResult Member::addCredential(const RuntimeEnvironment *RR, void *tPtr, const Identity &sourcePeerIdentity, const NetworkConfig &nconf, const MembershipCredential &com)
{
	const int64_t newts = com.timestamp();
	if (newts <= m_comRevocationThreshold) {
		RR->t->credentialRejected(tPtr, 0xd9992121, com.networkId(), sourcePeerIdentity, com.id(), com.timestamp(), ZT_CREDENTIAL_TYPE_COM, ZT_TRACE_CREDENTIAL_REJECTION_REASON_REVOKED);
		return ADD_REJECTED;
	}

	const int64_t oldts = m_com.timestamp();
	if (newts < oldts) {
		RR->t->credentialRejected(tPtr, 0xd9928192, com.networkId(), sourcePeerIdentity, com.id(), com.timestamp(), ZT_CREDENTIAL_TYPE_COM, ZT_TRACE_CREDENTIAL_REJECTION_REASON_OLDER_THAN_LATEST);
		return ADD_REJECTED;
	}
	if ((newts == oldts) && (m_com == com))
		return ADD_ACCEPTED_REDUNDANT;

	switch (com.verify(RR, tPtr)) {
		default:
			RR->t->credentialRejected(tPtr, 0x0f198241, com.networkId(), sourcePeerIdentity, com.id(), com.timestamp(), ZT_CREDENTIAL_TYPE_COM, ZT_TRACE_CREDENTIAL_REJECTION_REASON_INVALID);
			return Member::ADD_REJECTED;
		case Credential::VERIFY_OK:
			m_com = com;
			return ADD_ACCEPTED_NEW;
		case Credential::VERIFY_BAD_SIGNATURE:
			RR->t->credentialRejected(tPtr, 0xbaf0aaaa, com.networkId(), sourcePeerIdentity, com.id(), com.timestamp(), ZT_CREDENTIAL_TYPE_COM, ZT_TRACE_CREDENTIAL_REJECTION_REASON_SIGNATURE_VERIFICATION_FAILED);
			return ADD_REJECTED;
		case Credential::VERIFY_NEED_IDENTITY:
			return ADD_DEFERRED_FOR_WHOIS;
	}
}

// 3/5 of the credential types have identical addCredential() code
template< typename C >
static ZT_INLINE Member::AddCredentialResult _addCredImpl(
	Map< uint32_t, C > &remoteCreds,
	const Map< uint64_t, int64_t > &revocations,
	const RuntimeEnvironment *const RR,
	void *const tPtr,
	const Identity &sourcePeerIdentity,
	const NetworkConfig &nconf,
	const C &cred)
{
	typename Map< uint32_t, C >::const_iterator rc(remoteCreds.find(cred.id()));
	if (rc != remoteCreds.end()) {
		if (rc->second.timestamp() > cred.timestamp()) {
			RR->t->credentialRejected(tPtr, 0x40000001, nconf.networkId, sourcePeerIdentity, cred.id(), cred.timestamp(), C::credentialType(), ZT_TRACE_CREDENTIAL_REJECTION_REASON_OLDER_THAN_LATEST);
			return Member::ADD_REJECTED;
		}
		if (rc->second == cred)
			return Member::ADD_ACCEPTED_REDUNDANT;
	}

	typename Map< uint64_t, int64_t >::const_iterator rt(revocations.find(Member::credentialKey(C::credentialType(), cred.id())));
	if ((rt != revocations.end()) && (rt->second >= cred.timestamp())) {
		RR->t->credentialRejected(tPtr, 0x24248124, nconf.networkId, sourcePeerIdentity, cred.id(), cred.timestamp(), C::credentialType(), ZT_TRACE_CREDENTIAL_REJECTION_REASON_REVOKED);
		return Member::ADD_REJECTED;
	}

	switch (cred.verify(RR, tPtr)) {
		default:
			RR->t->credentialRejected(tPtr, 0x01feba012, nconf.networkId, sourcePeerIdentity, cred.id(), cred.timestamp(), C::credentialType(), ZT_TRACE_CREDENTIAL_REJECTION_REASON_INVALID);
			return Member::ADD_REJECTED;
		case 0:
			if (rc == remoteCreds.end())
				remoteCreds[cred.id()] = cred;
			return Member::ADD_ACCEPTED_NEW;
		case 1:
			return Member::ADD_DEFERRED_FOR_WHOIS;
	}
}

Member::AddCredentialResult Member::addCredential(const RuntimeEnvironment *RR, void *tPtr, const Identity &sourcePeerIdentity, const NetworkConfig &nconf, const TagCredential &tag)
{ return _addCredImpl< TagCredential >(m_remoteTags, m_revocations, RR, tPtr, sourcePeerIdentity, nconf, tag); }

Member::AddCredentialResult Member::addCredential(const RuntimeEnvironment *RR, void *tPtr, const Identity &sourcePeerIdentity, const NetworkConfig &nconf, const CapabilityCredential &cap)
{ return _addCredImpl< CapabilityCredential >(m_remoteCaps, m_revocations, RR, tPtr, sourcePeerIdentity, nconf, cap); }

Member::AddCredentialResult Member::addCredential(const RuntimeEnvironment *RR, void *tPtr, const Identity &sourcePeerIdentity, const NetworkConfig &nconf, const OwnershipCredential &coo)
{ return _addCredImpl< OwnershipCredential >(m_remoteCoos, m_revocations, RR, tPtr, sourcePeerIdentity, nconf, coo); }

Member::AddCredentialResult Member::addCredential(const RuntimeEnvironment *RR, void *tPtr, const Identity &sourcePeerIdentity, const NetworkConfig &nconf, const RevocationCredential &rev)
{
	int64_t *rt;
	switch (rev.verify(RR, tPtr)) {
		default:
			RR->t->credentialRejected(tPtr, 0x938fffff, nconf.networkId, sourcePeerIdentity, rev.id(), 0, ZT_CREDENTIAL_TYPE_REVOCATION, ZT_TRACE_CREDENTIAL_REJECTION_REASON_INVALID);
			return ADD_REJECTED;
		case 0: {
			const ZT_CredentialType ct = rev.typeBeingRevoked();
			switch (ct) {
				case ZT_CREDENTIAL_TYPE_COM:
					if (rev.threshold() > m_comRevocationThreshold) {
						m_comRevocationThreshold = rev.threshold();
						return ADD_ACCEPTED_NEW;
					}
					return ADD_ACCEPTED_REDUNDANT;
				case ZT_CREDENTIAL_TYPE_CAPABILITY:
				case ZT_CREDENTIAL_TYPE_TAG:
				case ZT_CREDENTIAL_TYPE_COO:
					rt = &(m_revocations[credentialKey(ct, rev.credentialId())]);
					if (*rt < rev.threshold()) {
						*rt = rev.threshold();
						m_comRevocationThreshold = rev.threshold();
						return ADD_ACCEPTED_NEW;
					}
					return ADD_ACCEPTED_REDUNDANT;
				default:
					RR->t->credentialRejected(tPtr, 0x0bbbb1a4, nconf.networkId, sourcePeerIdentity, rev.id(), 0, ZT_CREDENTIAL_TYPE_REVOCATION, ZT_TRACE_CREDENTIAL_REJECTION_REASON_INVALID);
					return ADD_REJECTED;
			}
		}
		case 1:
			return ADD_DEFERRED_FOR_WHOIS;
	}
}

bool Member::m_isUnspoofableAddress(const NetworkConfig &nconf, const InetAddress &ip) const noexcept
{
	return (
		ip.isV6() &&
		nconf.ndpEmulation() &&
		(
			(ip == InetAddress::makeIpv66plane(nconf.networkId, m_com.issuedTo().address)) ||
			(ip == InetAddress::makeIpv6rfc4193(nconf.networkId, m_com.issuedTo().address))
		)
	);
}

} // namespace ZeroTier