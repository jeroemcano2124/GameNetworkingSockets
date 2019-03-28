//====== Copyright Valve Corporation, All rights reserved. ====================

#include <time.h>

#include <steam/isteamnetworkingsockets.h>
#include "steamnetworkingsockets_connections.h"
#include "steamnetworkingsockets_lowlevel.h"
#include "csteamnetworkingsockets.h"
#include "crypto.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#ifdef __GNUC__
	// error: assuming signed overflow does not occur when assuming that (X + c) < X is always false [-Werror=strict-overflow]
	// current steamrt:scout gcc "g++ (SteamRT 4.8.4-1ubuntu15~12.04+steamrt1.2+srt1) 4.8.4" requires this at the top due to optimizations
	#pragma GCC diagnostic ignored "-Wstrict-overflow"
#endif

// Put everything in a namespace, so we don't violate the one definition rule
namespace SteamNetworkingSocketsLib {

const int k_nMaxRecentLocalConnectionIDs = 256;
static CUtlVectorFixed<uint16,k_nMaxRecentLocalConnectionIDs> s_vecRecentLocalConnectionIDs;

/// Check if we've sent a "spam reply", meaning a reply to an incoming
/// message that could be random spoofed garbage.  Returns false if we've
/// recently sent one and cannot send any more right now without risking
/// being taken advantage of.  Returns true if we haven't sent too many
/// such packets recently, and it's OK to send one now.  (If true is returned,
/// it's assumed that you will send one.)
bool BCheckGlobalSpamReplyRateLimit( SteamNetworkingMicroseconds usecNow )
{
	static SteamNetworkingMicroseconds s_usecLastSpamReplySent;
	if ( s_usecLastSpamReplySent + k_nMillion/4 > usecNow )
		return false;
	s_usecLastSpamReplySent = usecNow;
	return true;
}

/// Replace internal states that are not visible outside of the API with
/// the corresponding state that we show the the application.
inline ESteamNetworkingConnectionState CollapseConnectionStateToAPIState( ESteamNetworkingConnectionState eState )
{
	// All the hidden internal states are assigned negative values
	if ( eState < 0 )
		return k_ESteamNetworkingConnectionState_None;
	return eState;
}

struct TrustedKey
{
	typedef char KeyData[33];
	TrustedKey( uint64 id, const KeyData &data ) : m_id( id )
	{
		m_key.SetRawDataWithoutWipingInput( &data[0], sizeof(KeyData)-1 );
	}
	const uint64 m_id;
	CECSigningPublicKey m_key;

	#ifdef DBGFLAG_VALIDATE
		void Validate( CValidator &validator, const char *pchName ) const
		{
			ValidateObj( m_key );
		}
	#endif
};

// !KLUDGE! For now, we only have one trusted CA key.
// Note that it's important to burn this key into the source code,
// *not* load it from a file.  Our threat model for eavesdropping/tampering
// includes the player!  Everything outside of this process is untrusted.
// Obviously they can tamper with the process or modify the executable,
// but that puts them into VAC territory.
const TrustedKey s_arTrustedKeys[1] = {
	{ 18220590129359924542llu, "\x9a\xec\xa0\x4e\x17\x51\xce\x62\x68\xd5\x69\x00\x2c\xa1\xe1\xfa\x1b\x2d\xbc\x26\xd3\x6b\x4e\xa3\xa0\x08\x3a\xd3\x72\x82\x9b\x84" }
};

// Hack code used to generate C++ code to add a new CA key to the table above
//void KludgePrintPublicKey()
//{
//	CECSigningPublicKey key;
//	char *x = strdup( "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIJrsoE4XUc5iaNVpACyh4fobLbwm02tOo6AIOtNygpuE" );
//	DbgVerify( key.LoadFromAndWipeBuffer( x, strlen(x) ) );
//	CUtlStringBuilder bufText;
//	for ( uint32 i = 0 ; i < key.GetLength() ; ++i )
//	{
//		bufText.AppendFormat("\\x%02x", key.GetData()[i] );
//	}
//	SHA256Digest_t digest;
//	CCrypto::GenerateSHA256Digest( key.GetData(), key.GetLength(), &digest );
//	SpewWarning( "TrustedKey( %llullu, \"%s\" )\n", LittleQWord( *(uint64*)&digest ), bufText.String() );
//}

/////////////////////////////////////////////////////////////////////////////
//
// Message storage
//
/////////////////////////////////////////////////////////////////////////////

void CSteamNetworkingMessage::DefaultFreeData( SteamNetworkingMessage_t *pMsg )
{
	free( pMsg->m_pData );
}


void SteamNetworkingMessage_t_Release( SteamNetworkingMessage_t *pIMsg )
{
	CSteamNetworkingMessage *pMsg = static_cast<CSteamNetworkingMessage *>( pIMsg );

	// Free up the buffer, if we have one
	if ( pMsg->m_pData )
	{
		(*pMsg->m_pfnFreeData)( pMsg );
		pMsg->m_pData = nullptr;
	}

	// We must not currently be in any queue.  In fact, our parent
	// might have been destroyed.
	Assert( !pMsg->m_linksSameConnection.m_pQueue );
	Assert( !pMsg->m_linksSameConnection.m_pPrev );
	Assert( !pMsg->m_linksSameConnection.m_pNext );
	Assert( !pMsg->m_linksSecondaryQueue.m_pQueue );
	Assert( !pMsg->m_linksSecondaryQueue.m_pPrev );
	Assert( !pMsg->m_linksSecondaryQueue.m_pNext );

	// Self destruct
	// FIXME Should avoid this dynamic memory call with some sort of pooling
	delete pMsg;
}

CSteamNetworkingMessage *CSteamNetworkingMessage::New( CSteamNetworkConnectionBase *pParent, uint32 cbSize, int64 nMsgNum, SteamNetworkingMicroseconds usecNow )
{
	// FIXME Should avoid this dynamic memory call with some sort of pooling
	CSteamNetworkingMessage *pMsg = new CSteamNetworkingMessage;

	if ( pParent )
	{
		pMsg->m_sender = pParent->m_identityRemote;
		pMsg->m_conn = pParent->m_hConnectionSelf;
		pMsg->m_nConnUserData = pParent->GetUserData();
	}
	else
	{
		memset( &pMsg->m_sender, 0, sizeof(pMsg->m_sender) );
		pMsg->m_conn = k_HSteamNetConnection_Invalid;
		pMsg->m_nConnUserData = 0;
	}
	pMsg->m_pData = malloc( cbSize );
	pMsg->m_cbSize = cbSize;
	pMsg->m_nChannel = -1;
	pMsg->m_usecTimeReceived = usecNow;
	pMsg->m_nMessageNumber = nMsgNum;
	pMsg->m_pfnFreeData = CSteamNetworkingMessage::DefaultFreeData;
	pMsg->m_pfnRelease = SteamNetworkingMessage_t_Release;
	return pMsg;
}

void CSteamNetworkingMessage::LinkToQueueTail( Links CSteamNetworkingMessage::*pMbrLinks, SteamNetworkingMessageQueue *pQueue )
{
	// Locate previous link that should point to us.
	// Does the queue have anything in it?
	if ( pQueue->m_pLast )
	{
		Assert( pQueue->m_pFirst );
		Assert( !(pQueue->m_pLast->*pMbrLinks).m_pNext );
		(pQueue->m_pLast->*pMbrLinks).m_pNext = this;
	}
	else
	{
		Assert( !pQueue->m_pFirst );
		pQueue->m_pFirst = this;
	}

	// Link back to the previous guy, if any
	(this->*pMbrLinks).m_pPrev = pQueue->m_pLast;

	// We're last in the list, nobody after us
	(this->*pMbrLinks).m_pNext = nullptr;
	pQueue->m_pLast = this;

	// Remember what queue we're in
	(this->*pMbrLinks).m_pQueue = pQueue;
}

void CSteamNetworkingMessage::UnlinkFromQueue( Links CSteamNetworkingMessage::*pMbrLinks )
{
	Links &links = this->*pMbrLinks;
	if ( links.m_pQueue == nullptr )
		return;
	SteamNetworkingMessageQueue &q = *links.m_pQueue;

	// Unlink from previous
	if ( links.m_pPrev )
	{
		Assert( q.m_pFirst != this );
		Assert( (links.m_pPrev->*pMbrLinks).m_pNext == this );
		(links.m_pPrev->*pMbrLinks).m_pNext = links.m_pNext;
	}
	else
	{
		Assert( q.m_pFirst == this );
		q.m_pFirst = links.m_pNext;
	}

	// Unlink from next
	if ( links.m_pNext )
	{
		Assert( q.m_pLast != this );
		Assert( (links.m_pNext->*pMbrLinks).m_pPrev == this );
		(links.m_pNext->*pMbrLinks).m_pPrev = links.m_pPrev;
	}
	else
	{
		Assert( q.m_pLast == this );
		q.m_pLast = links.m_pPrev;
	}

	// Clear links
	links.m_pQueue = nullptr;
	links.m_pPrev = nullptr;
	links.m_pNext = nullptr;
}

void CSteamNetworkingMessage::Unlink()
{
	// Unlink from any queues we are in
	UnlinkFromQueue( &CSteamNetworkingMessage::m_linksSameConnection );
	UnlinkFromQueue( &CSteamNetworkingMessage::m_linksSecondaryQueue );
}

void SteamNetworkingMessageQueue::PurgeMessages()
{

	while ( !IsEmpty() )
	{
		CSteamNetworkingMessage *pMsg = m_pFirst;
		pMsg->Unlink();
		Assert( m_pFirst != pMsg );
		pMsg->Release();
	}
}

int SteamNetworkingMessageQueue::RemoveMessages( SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages )
{
	int nMessagesReturned = 0;

	while ( !IsEmpty() && nMessagesReturned < nMaxMessages )
	{
		// Locate message, put into caller's list
		CSteamNetworkingMessage *pMsg = m_pFirst;
		ppOutMessages[nMessagesReturned++] = pMsg;

		// Unlink from all queues
		pMsg->Unlink();

		// That should have unlinked from *us*, so it shouldn't be in our queue anymore
		Assert( m_pFirst != pMsg );
	}

	return nMessagesReturned;
}

/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkListenSocketBase
//
/////////////////////////////////////////////////////////////////////////////

CSteamNetworkListenSocketBase::CSteamNetworkListenSocketBase( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface )
: m_pSteamNetworkingSocketsInterface( pSteamNetworkingSocketsInterface )
{
	m_hListenSocketSelf = k_HSteamListenSocket_Invalid;
	m_connectionConfig.Init( &pSteamNetworkingSocketsInterface->m_connectionConfig );
}

CSteamNetworkListenSocketBase::~CSteamNetworkListenSocketBase()
{
	AssertMsg( m_mapChildConnections.Count() == 0 && !m_queueRecvMessages.m_pFirst && !m_queueRecvMessages.m_pLast, "Destroy() not used properly" );
}

void CSteamNetworkListenSocketBase::Destroy()
{

	// Destroy all child connections
	FOR_EACH_HASHMAP( m_mapChildConnections, h )
	{
		CSteamNetworkConnectionBase *pChild = m_mapChildConnections[ h ];
		Assert( pChild->m_pParentListenSocket == this );
		Assert( pChild->m_hSelfInParentListenSocketMap == h );

		int n = m_mapChildConnections.Count();
		pChild->Destroy();
		Assert( m_mapChildConnections.Count() == n-1 );
	}

	// Self destruct
	delete this;
}

bool CSteamNetworkListenSocketBase::APIGetAddress( SteamNetworkingIPAddr *pAddress )
{
	// Base class doesn't know
	return false;
}

int CSteamNetworkListenSocketBase::APIReceiveMessages( SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages )
{
	return m_queueRecvMessages.RemoveMessages( ppOutMessages, nMaxMessages );
}

void CSteamNetworkListenSocketBase::AddChildConnection( CSteamNetworkConnectionBase *pConn )
{
	Assert( pConn->m_pParentListenSocket == nullptr );
	Assert( pConn->m_hSelfInParentListenSocketMap == -1 );
	Assert( pConn->m_hConnectionSelf == k_HSteamNetConnection_Invalid );

	RemoteConnectionKey_t key{ pConn->m_identityRemote, pConn->m_unConnectionIDRemote };
	Assert( m_mapChildConnections.Find( key ) == m_mapChildConnections.InvalidIndex() );

	// Setup linkage
	pConn->m_pParentListenSocket = this;
	pConn->m_hSelfInParentListenSocketMap = m_mapChildConnections.Insert( key, pConn );

	// Connection configuration will inherit from us
	pConn->m_connectionConfig.Init( &m_connectionConfig );
}

void CSteamNetworkListenSocketBase::AboutToDestroyChildConnection( CSteamNetworkConnectionBase *pConn )
{
	Assert( pConn->m_pParentListenSocket == this );
	int hChild = pConn->m_hSelfInParentListenSocketMap;

	pConn->m_pParentListenSocket = nullptr;
	pConn->m_hSelfInParentListenSocketMap = -1;

	if ( m_mapChildConnections[ hChild ] == pConn )
	{
		 m_mapChildConnections[ hChild ] = nullptr; // just for kicks
		 m_mapChildConnections.RemoveAt( hChild );
	}
	else
	{
		AssertMsg( false, "Listen socket child list corruption!" );
		FOR_EACH_HASHMAP( m_mapChildConnections, h )
		{
			if ( m_mapChildConnections[h] == pConn )
				m_mapChildConnections.RemoveAt(h);
		}
	}
}

/////////////////////////////////////////////////////////////////////////////
//
// Abstract connection classes
//
/////////////////////////////////////////////////////////////////////////////

CSteamNetworkConnectionBase::CSteamNetworkConnectionBase( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface )
: m_pSteamNetworkingSocketsInterface( pSteamNetworkingSocketsInterface )
{
	m_hConnectionSelf = k_HSteamNetConnection_Invalid;
	//m_nVirtualPort = -1;
	m_nUserData = -1;
	m_eConnectionState = k_ESteamNetworkingConnectionState_None;
	m_usecWhenEnteredConnectionState = 0;
	m_usecWhenSentConnectRequest = 0;
	m_ulHandshakeRemoteTimestamp = 0;
	m_usecWhenReceivedHandshakeRemoteTimestamp = 0;
	m_eEndReason = k_ESteamNetConnectionEnd_Invalid;
	m_szEndDebug[0] = '\0';
	memset( &m_identityLocal, 0, sizeof(m_identityLocal) );
	memset( &m_identityRemote, 0, sizeof(m_identityRemote) );
	m_unConnectionIDLocal = 0;
	m_unConnectionIDRemote = 0;
	m_pParentListenSocket = nullptr;
	m_hSelfInParentListenSocketMap = -1;
	m_pMessagesInterface = nullptr;
	m_pMessagesSession = nullptr;
	m_bCertHasIdentity = false;
	m_bCryptKeysValid = false;
	memset( m_szAppName, 0, sizeof( m_szAppName ) );
	memset( m_szDescription, 0, sizeof( m_szDescription ) );

	// Initialize configuration using parent interface for now.
	m_connectionConfig.Init( &m_pSteamNetworkingSocketsInterface->m_connectionConfig );
}

CSteamNetworkConnectionBase::~CSteamNetworkConnectionBase()
{
	Assert( m_hConnectionSelf == k_HSteamNetConnection_Invalid );
	Assert( m_eConnectionState == k_ESteamNetworkingConnectionState_Dead );
	Assert( m_queueRecvMessages.IsEmpty() );
	Assert( m_pParentListenSocket == nullptr );
	Assert( m_pMessagesSession == nullptr );
}

void CSteamNetworkConnectionBase::Destroy()
{

	// Make sure all resources have been freed, etc
	FreeResources();

	// Self destruct NOW
	delete this;
}

void CSteamNetworkConnectionBase::QueueDestroy()
{
	FreeResources();

	// We'll delete ourselves from within Think();
	SetNextThinkTime( SteamNetworkingSockets_GetLocalTimestamp() );
}

void CSteamNetworkConnectionBase::FreeResources()
{
	// Make sure we're marked in the dead state, and also if we were in an
	// API-visible state, this will queue the state change notification
	// while we still know who our listen socket is (if any).
	SetState( k_ESteamNetworkingConnectionState_Dead, SteamNetworkingSockets_GetLocalTimestamp() );

	// We should be detatched from any mesages session!
	Assert( m_pMessagesSession == nullptr );

	// Discard any messages that weren't retrieved
	m_queueRecvMessages.PurgeMessages();

	// Detach from the listen socket that owns us, if any
	if ( m_pParentListenSocket )
		m_pParentListenSocket->AboutToDestroyChildConnection( this );

	// Remove from global connection list
	if ( m_hConnectionSelf != k_HSteamNetConnection_Invalid )
	{
		int idx = g_mapConnections.Find( uint16( m_hConnectionSelf ) );
		if ( idx == g_mapConnections.InvalidIndex() || g_mapConnections[ idx ] != this )
		{
			AssertMsg( false, "Connection list bookeeping corruption" );
			FOR_EACH_HASHMAP( g_mapConnections, i )
			{
				if ( g_mapConnections[i] == this )
					g_mapConnections.RemoveAt( i );
			}
		}
		else
		{
			g_mapConnections[ idx ] = nullptr; // Just for grins
			g_mapConnections.RemoveAt( idx );
		}

		m_hConnectionSelf = k_HSteamNetConnection_Invalid;
	}

	// Make sure and clean out crypto keys and such now
	ClearCrypto();

	// Save connection ID so we avoid using the same thing in the very near future.
	if ( m_unConnectionIDLocal )
	{
		// Trim history to max.  If we're really cycling through connections fast, this
		// history won't be very useful, but that should be an extremely rare edge case,
		// and the worst thing that happens is that we have a higher chance of reusing
		// a connection ID that shares the same bottom 16 bits.
		while ( s_vecRecentLocalConnectionIDs.Count() >= k_nMaxRecentLocalConnectionIDs )
			s_vecRecentLocalConnectionIDs.Remove( 0 );
		s_vecRecentLocalConnectionIDs.AddToTail( (uint16)m_unConnectionIDLocal );

		// Clear it, since this function should be idempotent
		m_unConnectionIDLocal = 0;
	}
}

bool CSteamNetworkConnectionBase::BInitConnection( SteamNetworkingMicroseconds usecNow, SteamDatagramErrMsg &errMsg )
{
	// We make sure the lower 16 bits are unique.  Make sure we don't have too many connections.
	// This definitely could be relaxed, but honestly we don't expect this library to be used in situations
	// where you need that many connections.
	if ( g_mapConnections.Count() >= 0x1fff )
	{
		V_strcpy_safe( errMsg, "Too many connections." );
		return false;
	}

	// Select random connection ID, and make sure it passes certain sanity checks
	Assert( m_unConnectionIDLocal == 0 );
	int tries = 0;
	for (;;) {
		if ( ++tries > 10000 )
		{
			V_strcpy_safe( errMsg, "Unable to find unique connection ID" );
			return false;
		}
		CCrypto::GenerateRandomBlock( &m_unConnectionIDLocal, sizeof(m_unConnectionIDLocal) );

		// Make sure neither half is zero
		if ( ( m_unConnectionIDLocal & 0xffff ) == 0 )
			continue;
		if ( ( m_unConnectionIDLocal & 0xffff0000 ) == 0 )
			continue;

		// Check recent connections
		if ( s_vecRecentLocalConnectionIDs.HasElement( (uint16)m_unConnectionIDLocal ) )
			continue;

		// Check active connections
		if ( g_mapConnections.HasElement( (uint16)m_unConnectionIDLocal ) )
			continue;

		// This one's good
		break;
	}

	Assert( m_hConnectionSelf == k_HSteamNetConnection_Invalid );

	Assert( m_pParentListenSocket == nullptr || m_pSteamNetworkingSocketsInterface == m_pParentListenSocket->m_pSteamNetworkingSocketsInterface );

	// We need to know who we are
	if ( m_identityLocal.IsInvalid() )
	{
		if ( !m_pSteamNetworkingSocketsInterface->GetIdentity( &m_identityLocal ) )
		{
			V_strcpy_safe( errMsg, "We don't know our local identity." );
			return false;
		}
	}

	m_eEndReason = k_ESteamNetConnectionEnd_Invalid;
	m_szEndDebug[0] = '\0';
	m_statsEndToEnd.Init( usecNow, true ); // Until we go connected don't try to send acks, etc

	// Let's use the the connection ID as the connection handle.  It's random, not reused
	// within a short time interval, and we print it in our debugging in places, and you
	// can see it on the wire for debugging.  In the past we has a "clever" method of
	// assigning the handle that had some cute performance tricks for lookups and
	// guaranteeing handles wouldn't be reused.  But making it be the same as the
	// ConnectionID is probably just more useful and less confusing.
	m_hConnectionSelf = m_unConnectionIDLocal;

	// Add it to our table of active sockets.
	g_mapConnections.Insert( int16( m_hConnectionSelf ), this );

	// Make sure a description has been set for debugging purposes
	SetDescription();

	// Clear everything out
	ClearCrypto();

	// Switch connection state, queue state change notifications.
	SetState( k_ESteamNetworkingConnectionState_Connecting, usecNow );

	// Take action to start obtaining a cert, or if we already have one, then set it now
	InitConnectionCrypto( usecNow );

	// Queue us to think ASAP.
	SetNextThinkTime( usecNow );

	return true;
}

void CSteamNetworkConnectionBase::SetAppName( const char *pszName )
{
	V_strcpy_safe( m_szAppName, pszName ? pszName : "" );

	// Re-calculate description
	SetDescription();
}

void CSteamNetworkConnectionBase::SetDescription()
{
	ConnectionTypeDescription_t szTypeDescription;
	GetConnectionTypeDescription( szTypeDescription );

	if ( m_szAppName[0] )
		V_sprintf_safe( m_szDescription, "#%u %s '%s'", m_unConnectionIDLocal, szTypeDescription, m_szAppName );
	else
		V_sprintf_safe( m_szDescription, "#%u %s", m_unConnectionIDLocal, szTypeDescription );
}

void CSteamNetworkConnectionBase::InitConnectionCrypto( SteamNetworkingMicroseconds usecNow )
{
	BThinkCryptoReady( usecNow );
}

void CSteamNetworkConnectionBase::ClearCrypto()
{
	m_msgCertRemote.Clear();
	m_msgCryptRemote.Clear();

	m_keyExchangePrivateKeyLocal.Wipe();
	m_msgCryptLocal.Clear();
	m_msgSignedCryptLocal.Clear();

	m_bCertHasIdentity = false;
	m_bCryptKeysValid = false;
	m_cryptContextSend.Wipe();
	m_cryptContextRecv.Wipe();
	m_cryptIVSend.Wipe();
	m_cryptIVRecv.Wipe();
}

bool CSteamNetworkConnectionBase::RecvNonDataSequencedPacket( int64 nPktNum, SteamNetworkingMicroseconds usecNow )
{

	// Let SNP know when we received it, so we can track loss events and send acks
	if ( SNP_RecordReceivedPktNum( nPktNum, usecNow, false ) )
	{

		// And also the general purpose sequence number/stats tracker
		// for the end-to-end flow.
		m_statsEndToEnd.TrackProcessSequencedPacket( nPktNum, usecNow, 0 );
	}

	return true;
}

bool CSteamNetworkConnectionBase::BThinkCryptoReady( SteamNetworkingMicroseconds usecNow )
{
	Assert( GetState() == k_ESteamNetworkingConnectionState_Connecting );

	// Do we already have a cert?
	if ( m_msgSignedCertLocal.has_cert() )
		return true;

	// If we are using an anonymous identity, then always use self-signed.
	// CA's should never issue a certificate for this identity, because that
	// is meaningless.  No peer should ever honor such a certificate.
	if ( m_identityLocal.IsLocalHost() )
	{
		InitLocalCryptoWithUnsignedCert();
		return true;
	}

	// Already have a a signed cert?
	if ( m_pSteamNetworkingSocketsInterface->m_msgSignedCert.has_ca_signature() )
	{

		// Use it!
		InitLocalCrypto( m_pSteamNetworkingSocketsInterface->m_msgSignedCert, m_pSteamNetworkingSocketsInterface->m_keyPrivateKey, m_pSteamNetworkingSocketsInterface->BCertHasIdentity() );
		return true;
	}

	// Check if we have intentionally disabled auth
	// !KLUDGE! This is not exactly the right test, since we're checking a
	// connection-type-specific convar and this is generic connection code.
	// might want to revisit this and make BAllowLocalUnsignedCert return
	// slightly more nuanced return value that distinguishes between
	// "Don't even try" from "try, but continue if we fail"
	if ( BAllowLocalUnsignedCert() && m_connectionConfig.m_IP_AllowWithoutAuth.Get() )
	{
		InitLocalCryptoWithUnsignedCert();
		return true;
	}

	// Otherwise, we don't have a signed cert (yet?).  Try (again?) to get one.
	// If this fails (either immediately, or asynchronously), we will
	// get a CertFailed call with the appropriate code, and we can decide
	// what we want to do.
	#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_InternalError, "Need a cert authority!" );
		Assert( false );
	#else
		m_pSteamNetworkingSocketsInterface->AsyncCertRequest();
	#endif
	return false;
}

void CSteamNetworkConnectionBase::InterfaceGotCert()
{
	// Make sure we care about this
	if ( GetState() != k_ESteamNetworkingConnectionState_Connecting )
		return;
	if ( BHasLocalCert() )
		return;

	// Setup with this cert
	InitLocalCrypto( m_pSteamNetworkingSocketsInterface->m_msgSignedCert, m_pSteamNetworkingSocketsInterface->m_keyPrivateKey, m_pSteamNetworkingSocketsInterface->BCertHasIdentity() );

	// Don't check state machine now, let's just schedule immediate wake up to deal with it
	SetNextThinkTime( SteamNetworkingSockets_GetLocalTimestamp() );
}

void CSteamNetworkConnectionBase::InitLocalCrypto( const CMsgSteamDatagramCertificateSigned &msgSignedCert, const CECSigningPrivateKey &keyPrivate, bool bCertHasIdentity )
{
	Assert( msgSignedCert.has_cert() );
	Assert( keyPrivate.IsValid() );

	// Save off the signed certificate
	m_msgSignedCertLocal = msgSignedCert;
	m_bCertHasIdentity = bCertHasIdentity;

	// Set protocol version
	m_msgCryptLocal.set_protocol_version( k_nCurrentProtocolVersion );

	// Generate a keypair for key exchange
	CECKeyExchangePublicKey publicKeyLocal;
	CCrypto::GenerateKeyExchangeKeyPair( &publicKeyLocal, &m_keyExchangePrivateKeyLocal );
	m_msgCryptLocal.set_key_type( CMsgSteamDatagramSessionCryptInfo_EKeyType_CURVE25519 );
	publicKeyLocal.GetRawDataAsStdString( m_msgCryptLocal.mutable_key_data() );

	// Generate some more randomness for the secret key
	uint64 crypt_nonce;
	CCrypto::GenerateRandomBlock( &crypt_nonce, sizeof(crypt_nonce) );
	m_msgCryptLocal.set_nonce( crypt_nonce );

	// Serialize and sign the crypt key with the private key that matches this cert
	m_msgSignedCryptLocal.set_info( m_msgCryptLocal.SerializeAsString() );
	CryptoSignature_t sig;
	keyPrivate.GenerateSignature( m_msgSignedCryptLocal.info().c_str(), m_msgSignedCryptLocal.info().length(), &sig );
	m_msgSignedCryptLocal.set_signature( &sig, sizeof(sig) );
}

void CSteamNetworkConnectionBase::InitLocalCryptoWithUnsignedCert()
{

	// Generate a keypair
	CECSigningPrivateKey keyPrivate;
	CECSigningPublicKey keyPublic;
	CCrypto::GenerateSigningKeyPair( &keyPublic, &keyPrivate );

	// Generate a cert
	CMsgSteamDatagramCertificate msgCert;
	keyPublic.GetRawDataAsStdString( msgCert.mutable_key_data() );
	msgCert.set_key_type( CMsgSteamDatagramCertificate_EKeyType_ED25519 );
	SteamNetworkingIdentityToProtobuf( m_identityLocal, msgCert, identity, legacy_steam_id );
	msgCert.set_app_id( m_pSteamNetworkingSocketsInterface->m_nAppID );

	// Should we set an expiry?  I mean it's unsigned, so it has zero value, so probably not
	//s_msgCertLocal.set_time_created( );

	// Serialize into "signed" message type, although we won't actually sign it.
	CMsgSteamDatagramCertificateSigned msgSignedCert;
	msgSignedCert.set_cert( msgCert.SerializeAsString() );

	// Standard init, as if this were a normal cert
	InitLocalCrypto( msgSignedCert, keyPrivate, true );
}

void CSteamNetworkConnectionBase::CertRequestFailed( ESteamNetConnectionEnd nConnectionEndReason, const char *pszMsg )
{

	// Make sure we care about this
	if ( GetState() != k_ESteamNetworkingConnectionState_Connecting )
		return;
	if ( BHasLocalCert() )
		return;

	// Do we require a signed cert?
	if ( !BAllowLocalUnsignedCert() )
	{
		// This is fatal
		SpewWarning( "Connection %u cannot use self-signed cert; failing connection.\n", m_unConnectionIDLocal );
		ConnectionState_ProblemDetectedLocally( nConnectionEndReason, "Cert failure: %s", pszMsg );
		return;
	}

	SpewWarning( "Connection %u is continuing with self-signed cert.\n", m_unConnectionIDLocal );
	InitLocalCryptoWithUnsignedCert();

	// Schedule immediate wake up to check on state machine
	SetNextThinkTime( SteamNetworkingSockets_GetLocalTimestamp() );
}

bool CSteamNetworkConnectionBase::BRecvCryptoHandshake( const CMsgSteamDatagramCertificateSigned &msgCert, const CMsgSteamDatagramSessionCryptInfoSigned &msgSessionInfo, bool bServer )
{

	// Have we already done key exchange?
	if ( m_bCryptKeysValid )
	{
		// FIXME - Probably should check that they aren't changing any keys.
		return true;
	}

	// Make sure we have what we need
	if ( !msgCert.has_cert() || !msgSessionInfo.has_info() )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCrypt, "Crypto handshake missing cert or session data" );
		return false;
	}

	// Deserialize the cert
	if ( !m_msgCertRemote.ParseFromString( msgCert.cert() ) )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCrypt, "Cert failed protobuf decode" );
		return false;
	}

	// Identity public key
	CECSigningPublicKey keySigningPublicKeyRemote;
	if ( m_msgCertRemote.key_type() != CMsgSteamDatagramCertificate_EKeyType_ED25519 )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCrypt, "Unsupported identity key type" );
		return false;
	}
	if ( !keySigningPublicKeyRemote.SetRawDataWithoutWipingInput( m_msgCertRemote.key_data().c_str(), m_msgCertRemote.key_data().length() ) )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCrypt, "Cert has invalid identity key" );
		return false;
	}

	// We need a cert.  If we don't have one by now, then we might try generating one
	if ( m_msgSignedCertLocal.has_cert() )
	{
		Assert( m_msgCryptLocal.has_nonce() );
		Assert( m_msgCryptLocal.has_key_data() );
		Assert( m_msgCryptLocal.has_key_type() );
	}
	else
	{
		if ( !BAllowLocalUnsignedCert() )
		{
			// Derived class / calling code should check for this and handle it better and fail
			// earlier with a more specific error message.  (Or allow self-signed certs)
			//ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_InternalError, "We don't have cert, and self-signed certs not allowed" );
			//return false;
			SpewWarning( "We don't have cert, and unsigned certs are not supposed to be allowed here.  Continuing anyway temporarily." );
		}

		// Proceed with an unsigned cert
		InitLocalCryptoWithUnsignedCert();
	}

	// If cert has an App ID restriction, then it better match our App
	if ( m_msgCertRemote.has_app_id() )
	{
		if ( m_msgCertRemote.app_id() != m_pSteamNetworkingSocketsInterface->m_nAppID )
		{
			ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCert, "Cert is for AppID %u instead of %u", m_msgCertRemote.app_id(), m_pSteamNetworkingSocketsInterface->m_nAppID );
			return false;
		}
	}

	// Special cert for gameservers in our data center?
	if ( m_msgCertRemote.gameserver_datacenter_ids_size()>0 && msgCert.has_ca_signature() )
	{
		if ( !m_identityRemote.GetSteamID().BAnonGameServerAccount() )
		{
			ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCert, "Certs restricted data center are for anon GS only.  Not %s", SteamNetworkingIdentityRender( m_identityRemote ).c_str() );
			return false;
		}
	}
	else
	{
		if ( !m_msgCertRemote.has_app_id() )
		{
			ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCert, "Cert must be bound to an AppID." );
			return false;
		}
		SteamNetworkingIdentity identityCert;
		SteamDatagramErrMsg errMsg;
		if ( SteamNetworkingIdentityFromCert( identityCert, m_msgCertRemote, errMsg ) <= 0 )
		{
			ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCert, "Bad cert identity.  %s", errMsg );
			return false;
		}

		if ( !( identityCert == m_identityRemote ) )
		{
			if ( identityCert.IsLocalHost() && !msgCert.has_ca_signature() )
			{
				// Special case for an unsigned, anonymous logon.  We've remapped their identity
				// to their real IP already.  Allow this.
			}
			else
			{
				ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCert, "Cert was issued to %s, not %s",
					SteamNetworkingIdentityRender( identityCert ).c_str(), SteamNetworkingIdentityRender( m_identityRemote ).c_str() );
				return false;
			}
		}
	}

	// Check if they are presenting a signature, then check it
	if ( msgCert.has_ca_signature() )
	{
		// Scan list of trusted CA keys
		bool bTrusted = false;
		for ( const TrustedKey &k: s_arTrustedKeys )
		{
			if ( msgCert.ca_key_id() != k.m_id )
				continue;
			if (
				msgCert.ca_signature().length() == sizeof(CryptoSignature_t)
				&& k.m_key.VerifySignature( msgCert.cert().c_str(), msgCert.cert().length(), *(const CryptoSignature_t *)msgCert.ca_signature().c_str() ) )
			{
				bTrusted = true;
				break;
			}
			ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCert, "Invalid cert signature" );
			return false;
		}
		if ( !bTrusted )
		{
			ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCert, "Cert signed with key %llu; not in trusted list", (uint64) msgCert.ca_key_id() );
			return false;
		}

		long rtNow = m_pSteamNetworkingSocketsInterface->GetTimeSecure();

		// Make sure hasn't expired.  All signed certs without an expiry should be considered invalid!
		// For unsigned certs, there's no point in checking the expiry, since anybody who wanted
		// to do bad stuff could just change it, we have no protection against tampering.
		long rtExpiry = m_msgCertRemote.time_expiry();
		if ( rtNow > rtExpiry )
		{
			//ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCert, msg );
			//return false;
			SpewWarning( "Cert failure: Cert expired %ld secs ago at %ld\n", rtNow-rtExpiry, rtExpiry );
		}

		// Let derived class check for particular auth/crypt requirements
		if ( !BCheckRemoteCert() )
		{
			Assert( GetState() == k_ESteamNetworkingConnectionState_ProblemDetectedLocally );
			return false;
		}

	}
	else
	{
		ERemoteUnsignedCert eAllow = AllowRemoteUnsignedCert();
		if ( eAllow == k_ERemoteUnsignedCert_AllowWarn )
		{
			SpewMsg( "[%s] Remote host is using an unsigned cert.  Allowing connection, but it's not secure!\n", GetDescription() );
		}
		else if ( eAllow != k_ERemoteUnsignedCert_Allow )
		{
			// Caller might have switched the state and provided a specific message.
			// if not, we'll do that for them
			if ( GetState() != k_ESteamNetworkingConnectionState_ProblemDetectedLocally )
			{
				Assert( GetState() == k_ESteamNetworkingConnectionState_Connecting );
				ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCert, "Unsigned certs are not allowed" );
			}
			return false;
		}
	}

	// Deserialize crypt info
	if ( !m_msgCryptRemote.ParseFromString( msgSessionInfo.info() ) )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCrypt, "Crypt info failed protobuf decode" );
		return false;
	}

	// Protocol version
	if ( m_msgCryptRemote.protocol_version() < k_nMinRequiredProtocolVersion )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadProtocolVersion, "Peer is running old software and needs to be updated.  (V%u, >=V%u is required)",
			m_msgCryptRemote.protocol_version(), k_nMinRequiredProtocolVersion );
		return false;
	}

	// Did they already send a protocol version in an earlier message?  If so, it needs to match.
	if ( m_statsEndToEnd.m_nPeerProtocolVersion != 0 && m_statsEndToEnd.m_nPeerProtocolVersion != m_msgCryptRemote.protocol_version() )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadProtocolVersion, "Claiming protocol V%u now, but earlier was using V%u",m_msgCryptRemote.protocol_version(), m_statsEndToEnd.m_nPeerProtocolVersion );
		return false;
	}
	m_statsEndToEnd.m_nPeerProtocolVersion = m_msgCryptRemote.protocol_version();

	// Key exchange public key
	CECKeyExchangePublicKey keyExchangePublicKeyRemote;
	if ( m_msgCryptRemote.key_type() != CMsgSteamDatagramSessionCryptInfo_EKeyType_CURVE25519 )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCrypt, "Unsupported DH key type" );
		return false;
	}
	if ( !keyExchangePublicKeyRemote.SetRawDataWithoutWipingInput( m_msgCryptRemote.key_data().c_str(), m_msgCryptRemote.key_data().length() ) )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCrypt, "Invalid DH key" );
		return false;
	}

	// Diffie�Hellman key exchange to get "premaster secret"
	AutoWipeFixedSizeBuffer<sizeof(SHA256Digest_t)> premasterSecret;
	if ( !CCrypto::PerformKeyExchange( m_keyExchangePrivateKeyLocal, keyExchangePublicKeyRemote, &premasterSecret.m_buf ) )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCrypt, "Key exchange failed" );
		return false;
	}
	//SpewMsg( "%s premaster: %02x%02x%02x%02x\n", bServer ? "Server" : "Client", premasterSecret.m_buf[0], premasterSecret.m_buf[1], premasterSecret.m_buf[2], premasterSecret.m_buf[3] );

	// We won't need this again, so go ahead and discard it now.
	m_keyExchangePrivateKeyLocal.Wipe();

	//
	// HMAC Key derivation function.
	//
	// https://tools.ietf.org/html/rfc5869
	// https://docs.google.com/document/d/1g5nIXAIkN_Y-7XJW5K45IblHd_L2f5LTaDUDwvZ5L6g/edit - Google QUIC as of 4/26/2017
	//

	//
	// 1. Extract: take premaster secret from key exchange and mix it so that it's evenly distributed, producing Pseudorandom key ("PRK")
	//
	uint64 salt[2] = { LittleQWord( m_msgCryptRemote.nonce() ), LittleQWord( m_msgCryptLocal.nonce() ) };
	if ( bServer )
		std::swap( salt[0], salt[1] );
	AutoWipeFixedSizeBuffer<sizeof(SHA256Digest_t)> prk;
	CCrypto::GenerateHMAC256( (const uint8 *)salt, sizeof(salt), premasterSecret.m_buf, premasterSecret.k_nSize, &prk.m_buf );
	premasterSecret.Wipe();

	//
	// 2. Expand: Use PRK as seed to generate all the different keys we need, mixing with connection-specific context
	//

	AutoWipeFixedSizeBuffer<32> cryptKeySend;
	AutoWipeFixedSizeBuffer<32> cryptKeyRecv;
	COMPILE_TIME_ASSERT( sizeof( cryptKeyRecv ) == sizeof(SHA256Digest_t) );
	COMPILE_TIME_ASSERT( sizeof( cryptKeySend ) == sizeof(SHA256Digest_t) );
	COMPILE_TIME_ASSERT( sizeof( m_cryptIVRecv ) <= sizeof(SHA256Digest_t) );
	COMPILE_TIME_ASSERT( sizeof( m_cryptIVSend ) <= sizeof(SHA256Digest_t) );

	uint8 *expandOrder[4] = { cryptKeySend.m_buf, cryptKeyRecv.m_buf, m_cryptIVSend.m_buf, m_cryptIVRecv.m_buf };
	int expandSize[4] = { cryptKeySend.k_nSize, cryptKeyRecv.k_nSize, m_cryptIVSend.k_nSize, m_cryptIVRecv.k_nSize };
	const std::string *context[4] = { &msgCert.cert(), &m_msgSignedCertLocal.cert(), &msgSessionInfo.info(), &m_msgSignedCryptLocal.info() };
	uint32 unConnectionIDContext[2] = { LittleDWord( m_unConnectionIDLocal ), LittleDWord( m_unConnectionIDRemote ) };

	// Make sure that both peers do things the same, so swap "local" and "remote" on one side arbitrarily.
	if ( bServer )
	{
		std::swap( expandOrder[0], expandOrder[1] );
		std::swap( expandOrder[2], expandOrder[3] );
		std::swap( expandSize[0], expandSize[1] ); // Actually NOP, but makes me feel better
		std::swap( expandSize[2], expandSize[3] );
		std::swap( context[0], context[1] );
		std::swap( context[2], context[3] );
		std::swap( unConnectionIDContext[0], unConnectionIDContext[1] );
	}
	//SpewMsg( "%s unConnectionIDContext = [ %u, %u ]\n", bServer ? "Server" : "Client", unConnectionIDContext[0], unConnectionIDContext[1] );

	// Generate connection "context" buffer
	CUtlBuffer bufContext( 0, (int)( sizeof(SHA256Digest_t) + sizeof(unConnectionIDContext) + 64 + context[0]->length() + context[1]->length() + context[2]->length() + context[3]->length() ), 0 );
	bufContext.SeekPut( CUtlBuffer::SEEK_HEAD, sizeof(SHA256Digest_t) );
	uint8 *pStart = (uint8 *)bufContext.PeekPut();

	// Write connection ID(s) into context buffer
	bufContext.Put( unConnectionIDContext, sizeof(unConnectionIDContext) );

	bufContext.Put( "Steam datagram", 14 );
	for ( const std::string *c: context )
		bufContext.Put( c->c_str(), (int)c->length() );

	// Now extract the keys according to the method in the RFC
	uint8 *pLastByte = (uint8 *)bufContext.PeekPut();
	SHA256Digest_t expandTemp;
	for ( int idxExpand = 0 ; idxExpand < 4 ; ++idxExpand )
	{
		*pLastByte = idxExpand+1;
		CCrypto::GenerateHMAC256( pStart, pLastByte - pStart + 1, prk.m_buf, prk.k_nSize, &expandTemp );
		V_memcpy( expandOrder[ idxExpand ], &expandTemp, expandSize[ idxExpand ] );

		//SpewMsg( "%s key %d: %02x%02x%02x%02x\n", bServer ? "Server" : "Client", idxExpand, expandTemp[0], expandTemp[1], expandTemp[2], expandTemp[3] );

		// Copy previous digest to use in generating the next one
		pStart = (uint8 *)bufContext.Base();
		V_memcpy( pStart, &expandTemp, sizeof(SHA256Digest_t) );
	}

	// Set encryption keys into the contexts, and set parameters
	if (
		!m_cryptContextSend.Init( cryptKeySend.m_buf, cryptKeySend.k_nSize, m_cryptIVSend.k_nSize, k_cbSteamNetwokingSocketsEncrytionTagSize )
		|| !m_cryptContextRecv.Init( cryptKeyRecv.m_buf, cryptKeyRecv.k_nSize, m_cryptIVRecv.k_nSize, k_cbSteamNetwokingSocketsEncrytionTagSize ) )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCrypt, "Error initializing crypto" );
		return false;
	}

	//
	// Tidy up key droppings
	//
	SecureZeroMemory( bufContext.Base(), bufContext.SizeAllocated() );
	SecureZeroMemory( expandTemp, sizeof(expandTemp) );

	// Make sure the connection description is set.
	// This is often called after we know who the remote host is
	SetDescription();

	// We're ready
	m_bCryptKeysValid = true;
	return true;
}

bool CSteamNetworkConnectionBase::BAllowLocalUnsignedCert() const
{
	// Base class will assume this is OK.  Derived connection
	// types can override.
	return true;
}

CSteamNetworkConnectionBase::ERemoteUnsignedCert CSteamNetworkConnectionBase::AllowRemoteUnsignedCert()
{
	// !KLUDGE! For now, assume this is OK, but warn about it.  We need to make this configurable and lock it down
	return k_ERemoteUnsignedCert_AllowWarn;
}

bool CSteamNetworkConnectionBase::BCheckRemoteCert()
{

	// No additional checks at the base class
	return true;
}

void CSteamNetworkConnectionBase::SetUserData( int64 nUserData )
{
	m_nUserData = nUserData;

	// Change user data on all messages that haven't been pulled out
	// of the queue yet.  This way we don't expose the client to weird
	// race conditions where they create a connection, and before they
	// are able to install their user data, some messages come in
	for ( CSteamNetworkingMessage *m = m_queueRecvMessages.m_pFirst ; m ; m = m->m_linksSameConnection.m_pNext )
	{
		Assert( m->GetConnection() == m_hConnectionSelf );
		m->SetConnectionUserData( m_nUserData );
	}
}

void CSteamNetworkConnectionBase::PopulateConnectionInfo( SteamNetConnectionInfo_t &info ) const
{
	info.m_eState = CollapseConnectionStateToAPIState( m_eConnectionState );
	info.m_hListenSocket = m_pParentListenSocket ? m_pParentListenSocket->m_hListenSocketSelf : k_HSteamListenSocket_Invalid;
	NetAdrToSteamNetworkingIPAddr( info.m_addrRemote, m_netAdrRemote );
	info.m_idPOPRemote = 0;
	info.m_idPOPRelay = 0;
	info.m_identityRemote = m_identityRemote;
	info.m_nUserData = m_nUserData;
	info.m_eEndReason = m_eEndReason;
	V_strcpy_safe( info.m_szEndDebug, m_szEndDebug );
	V_strcpy_safe( info.m_szConnectionDescription, m_szDescription );
}

void CSteamNetworkConnectionBase::APIGetQuickConnectionStatus( SteamNetworkingQuickConnectionStatus &stats )
{
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	stats.m_eState = CollapseConnectionStateToAPIState( m_eConnectionState );
	stats.m_nPing = m_statsEndToEnd.m_ping.m_nSmoothedPing;
	if ( m_statsEndToEnd.m_flInPacketsDroppedPct >= 0.0f )
	{
		Assert( m_statsEndToEnd.m_flInPacketsWeirdSequencePct >= 0.0f );
		stats.m_flConnectionQualityLocal = 1.0f - m_statsEndToEnd.m_flInPacketsDroppedPct - m_statsEndToEnd.m_flInPacketsWeirdSequencePct;
		Assert( stats.m_flConnectionQualityLocal >= 0.0f );
	}
	else
	{
		stats.m_flConnectionQualityLocal = -1.0f;
	}

	// FIXME - Can SNP give us a more up-to-date value from the feedback packet?
	if ( m_statsEndToEnd.m_latestRemote.m_flPacketsDroppedPct >= 0.0f )
	{
		Assert( m_statsEndToEnd.m_latestRemote.m_flPacketsWeirdSequenceNumberPct >= 0.0f );
		stats.m_flConnectionQualityRemote = 1.0f - m_statsEndToEnd.m_latestRemote.m_flPacketsDroppedPct - m_statsEndToEnd.m_latestRemote.m_flPacketsWeirdSequenceNumberPct;
		Assert( stats.m_flConnectionQualityRemote >= 0.0f );
	}
	else
	{
		stats.m_flConnectionQualityRemote = -1.0f;
	}

	// Actual current data rates
	stats.m_flOutPacketsPerSec = m_statsEndToEnd.m_sent.m_packets.m_flRate;
	stats.m_flOutBytesPerSec = m_statsEndToEnd.m_sent.m_bytes.m_flRate;
	stats.m_flInPacketsPerSec = m_statsEndToEnd.m_recv.m_packets.m_flRate;
	stats.m_flInBytesPerSec = m_statsEndToEnd.m_recv.m_bytes.m_flRate;
	SNP_PopulateQuickStats( stats, usecNow );
}

void CSteamNetworkConnectionBase::APIGetDetailedConnectionStatus( SteamNetworkingDetailedConnectionStatus &stats, SteamNetworkingMicroseconds usecNow )
{
	stats.Clear();
	PopulateConnectionInfo( stats.m_info );

	// Copy end-to-end stats
	m_statsEndToEnd.GetLinkStats( stats.m_statsEndToEnd, usecNow );

	// Congestion control and bandwidth estimation
	SNP_PopulateDetailedStats( stats.m_statsEndToEnd );
}

EResult CSteamNetworkConnectionBase::APISendMessageToConnection( const void *pData, uint32 cbData, int nSendFlags )
{

	// Check connection state
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_Linger:
		case k_ESteamNetworkingConnectionState_Dead:
		default:
			AssertMsg( false, "Why are making API calls on this connection?" );
			return k_EResultInvalidState;

		case k_ESteamNetworkingConnectionState_Connecting:
		case k_ESteamNetworkingConnectionState_FindingRoute:
			if ( nSendFlags & k_nSteamNetworkingSend_NoDelay )
				return k_EResultIgnored;
			break;

		case k_ESteamNetworkingConnectionState_Connected:
			break;

		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			return k_EResultNoConnection;
	}

	// Connection-type specific logic
	return _APISendMessageToConnection( pData, cbData, nSendFlags );
}

EResult CSteamNetworkConnectionBase::_APISendMessageToConnection( const void *pData, uint32 cbData, int nSendFlags )
{

	// Message too big?
	if ( cbData > k_cbMaxSteamNetworkingSocketsMessageSizeSend )
	{
		AssertMsg2( false, "Message size %d is too big.  Max is %d", cbData, k_cbMaxSteamNetworkingSocketsMessageSizeSend );
		return k_EResultInvalidParam;
	}

	// Pass to reliability layer
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();
	return SNP_SendMessage( usecNow, pData, cbData, nSendFlags );
}


EResult CSteamNetworkConnectionBase::APIFlushMessageOnConnection()
{

	// Check connection state
	switch ( GetState() )
	{
	case k_ESteamNetworkingConnectionState_None:
	case k_ESteamNetworkingConnectionState_FinWait:
	case k_ESteamNetworkingConnectionState_Linger:
	case k_ESteamNetworkingConnectionState_Dead:
	default:
		AssertMsg( false, "Why are making API calls on this connection?" );
		return k_EResultInvalidState;

	case k_ESteamNetworkingConnectionState_Connecting:
	case k_ESteamNetworkingConnectionState_FindingRoute:
	case k_ESteamNetworkingConnectionState_Connected:
		break;

	case k_ESteamNetworkingConnectionState_ClosedByPeer:
	case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		return k_EResultNoConnection;
	}

	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();
	return SNP_FlushMessage( usecNow );
}

int CSteamNetworkConnectionBase::APIReceiveMessages( SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages )
{
	return m_queueRecvMessages.RemoveMessages( ppOutMessages, nMaxMessages );
}

int64 CSteamNetworkConnectionBase::DecryptDataChunk( uint16 nWireSeqNum, int cbPacketSize, const void *pChunk, int cbChunk, void *pDecrypted, uint32 &cbDecrypted, SteamNetworkingMicroseconds usecNow )
{
	Assert( m_bCryptKeysValid );
	Assert( cbDecrypted >= k_cbSteamNetworkingSocketsMaxPlaintextPayloadRecv );

	// Track flow, even if we end up discarding this
	m_statsEndToEnd.TrackRecvPacket( cbPacketSize, usecNow );

	// Get the full end-to-end packet number, check if we should process it
	int64 nFullSequenceNumber = m_statsEndToEnd.ExpandWirePacketNumberAndCheck( nWireSeqNum );
	if ( nFullSequenceNumber <= 0 )
		return 0;

	// Adjust the IV by the packet number
	*(uint64 *)&m_cryptIVRecv.m_buf += LittleQWord( nFullSequenceNumber );
	//SpewMsg( "Recv decrypt IV %llu + %02x%02x%02x%02x, key %02x%02x%02x%02x\n", *(uint64 *)&m_cryptIVRecv.m_buf, m_cryptIVRecv.m_buf[8], m_cryptIVRecv.m_buf[9], m_cryptIVRecv.m_buf[10], m_cryptIVRecv.m_buf[11], m_cryptKeyRecv.m_buf[0], m_cryptKeyRecv.m_buf[1], m_cryptKeyRecv.m_buf[2], m_cryptKeyRecv.m_buf[3] );

	// Decrypt the chunk and check the auth tag
	bool bDecryptOK = m_cryptContextRecv.Decrypt(
		pChunk, cbChunk, // encrypted
		m_cryptIVRecv.m_buf, // IV
		pDecrypted, &cbDecrypted, // output
		nullptr, 0 // no AAD
	);

	// Restore the IV to the base value
	*(uint64 *)&m_cryptIVRecv.m_buf -= LittleQWord( nFullSequenceNumber );
	
	// Did decryption fail?
	if ( !bDecryptOK ) {

		// Just drop packet.
		// The assumption is that we either have a bug or some weird thing,
		// or that somebody is spoofing / tampering.  If it's the latter
		// we don't want to magnify the impact of their efforts
		SpewWarningRateLimited( usecNow, "[%s] Packet data chunk failed to decrypt!  Could be tampering/spoofing or a bug.", GetDescription() );
		return 0;
	}

	//SpewVerbose( "Connection %u recv seqnum %lld (gap=%d) sz=%d %02x %02x %02x %02x\n", m_unConnectionID, unFullSequenceNumber, nGap, cbDecrypted, arDecryptedChunk[0], arDecryptedChunk[1], arDecryptedChunk[2], arDecryptedChunk[3] );

	// OK, we have high confidence that this packet is actually from our peer and has not
	// been tampered with.  Check the gap.  If it's too big, that means we are risking losing
	// our ability to keep the sequence numbers in sync on each end.  This is a relatively
	// large number of outstanding packets.  We should never have this many packets
	// outstanding unacknowledged.  When we stop getting acks we should reduce our packet rate.
	// This isn't really a practical limitation, but it is a theoretical limitation if the
	// bandwidth is extremely high relatively to the latency.
	//
	// Even if the packets are on average only half full (~600 bytes), 16k packets is
	// around 9MB of data.  We probably don't want to have this amount of un-acked data
	// in our buffers, anyway.  If the packets are tiny it would be less, but a
	// a really high packet rate of tiny packets is not a good idea anyway.  Use bigger packets
	// with a lower rate.  If the app is really trying to fill the pipe and blasting a large
	// amount of data (and not forcing us to send small packets), then our code should be sending
	// mostly full packets, which means that this is closer to a gap of around ~18MB.
	int64 nGap = nFullSequenceNumber - m_statsEndToEnd.m_nMaxRecvPktNum;
	if ( nGap > 0x4000 )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_Generic,
			"Pkt number lurch by %lld; %04x->%04x",
			(long long)nGap, (uint16)m_statsEndToEnd.m_nMaxRecvPktNum, nWireSeqNum);
		return 0;
	}

	// Decrypted ok
	return nFullSequenceNumber;
}

bool CSteamNetworkConnectionBase::ProcessPlainTextDataChunk( int64 nFullSequenceNumber, const void *pDecrypted, uint32 cbDecrypted, int usecTimeSinceLast, SteamNetworkingMicroseconds usecNow )
{

	// Pass on to reassembly/reliability layer.  It may instruct us to act like we never received this
	// packet
	if ( !SNP_RecvDataChunk( nFullSequenceNumber, pDecrypted, cbDecrypted, usecNow ) )
	{
		SpewDebug( "[%s] discarding pkt %lld\n", GetDescription(), (long long)nFullSequenceNumber );
		return false;
	}

	// Packet is OK.  Track end-to-end flow.
	m_statsEndToEnd.TrackProcessSequencedPacket( nFullSequenceNumber, usecNow, usecTimeSinceLast );
	return true;
}

void CSteamNetworkConnectionBase::APICloseConnection( int nReason, const char *pszDebug, bool bEnableLinger )
{

	// If we already know the reason for the problem, we should ignore theirs
	if ( m_eEndReason == k_ESteamNetConnectionEnd_Invalid || GetState() == k_ESteamNetworkingConnectionState_Connecting || GetState() == k_ESteamNetworkingConnectionState_FindingRoute || GetState() == k_ESteamNetworkingConnectionState_Connected )
	{
		if ( nReason == 0 )
		{
			nReason = k_ESteamNetConnectionEnd_App_Generic;
		}
		else if ( nReason < k_ESteamNetConnectionEnd_App_Min || nReason > k_ESteamNetConnectionEnd_AppException_Max )
		{
			// Use a special value so that we can detect if people have this bug in our analytics
			nReason = k_ESteamNetConnectionEnd_App_Max;
			pszDebug = "Invalid numeric reason code";
		}

		m_eEndReason = ESteamNetConnectionEnd( nReason );
		if ( m_szEndDebug[0] == '\0' )
		{
			if ( pszDebug == nullptr || *pszDebug == '\0' )
			{
				if ( nReason >= k_ESteamNetConnectionEnd_AppException_Min )
				{
					pszDebug = "Application closed connection in an unusual way";
				}
				else
				{
					pszDebug = "Application closed connection";
				}
			}
			V_strcpy_safe( m_szEndDebug, pszDebug );
		}
	}

	// Check our state
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_Linger:
		default:
			Assert( false );
			return;

		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		case k_ESteamNetworkingConnectionState_Connecting:
		case k_ESteamNetworkingConnectionState_FindingRoute:
			ConnectionState_FinWait();
			break;

		case k_ESteamNetworkingConnectionState_Connected:
			if ( bEnableLinger )
			{
				SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();
				SetState( k_ESteamNetworkingConnectionState_Linger, usecNow );
				CheckConnectionStateAndSetNextThinkTime( usecNow );
			}
			else
			{
				ConnectionState_FinWait();
			}
			break;
	}
}

void CSteamNetworkConnectionBase::SetState( ESteamNetworkingConnectionState eNewState, SteamNetworkingMicroseconds usecNow )
{
	if ( eNewState == m_eConnectionState )
		return;
	ESteamNetworkingConnectionState eOldState = m_eConnectionState;
	m_eConnectionState = eNewState;

	// Remember when we entered this state
	m_usecWhenEnteredConnectionState = usecNow;

	// Give derived classes get a chance to take action on state changes
	ConnectionStateChanged( eOldState );
}

void CSteamNetworkConnectionBase::ReceivedMessage( const void *pData, int cbData, int64 nMsgNum, SteamNetworkingMicroseconds usecNow )
{
//	// !TEST! Enable this during connection test to trap bogus messages earlier
//	#if 1
//		struct TestMsg
//		{
//			int64 m_nMsgNum;
//			bool m_bReliable;
//			int m_cbSize;
//			uint8 m_data[ 20*1000 ];
//		};
//		const TestMsg *pTestMsg = (const TestMsg *)pData;
//
//		// Size makes sense?
//		Assert( sizeof(*pTestMsg) - sizeof(pTestMsg->m_data) + pTestMsg->m_cbSize == cbData );
//	#endif

	SpewType( m_connectionConfig.m_LogLevel_Message.Get(), "[%s] RecvMessage MsgNum=%lld sz=%d\n",
		GetDescription(),
		(long long)nMsgNum,
		cbData );

	// Special case for internal connections used by Messages interface
	if ( m_pMessagesInterface )
	{
		// Are we still associated with our session?
		if ( !m_pMessagesSession )
		{
			// How did we get here?  We should be closed, and once closed,
			// we should not receive any more messages
			AssertMsg2( false, "Received message for connection %s associated with Messages interface, but no session.  Connection state is %d", GetDescription(), (int)GetState() );
		}
		else if ( m_pMessagesSession->m_pConnection != this )
		{
			AssertMsg2( false, "Connection/session linkage bookkeeping bug!  %s state %d", GetDescription(), (int)GetState() );
		}
		else
		{
			m_pMessagesSession->ReceivedMessage( pData, cbData, nMsgNum, usecNow );
		}
		return;
	}

	// Create a message
	CSteamNetworkingMessage *pMsg = CSteamNetworkingMessage::New( this, cbData, nMsgNum, usecNow );

	// Add to end of my queue.
	pMsg->LinkToQueueTail( &CSteamNetworkingMessage::m_linksSameConnection, &m_queueRecvMessages );

	// If we are an inbound, accepted connection, link into the listen socket's queue
	if ( m_pParentListenSocket )
		pMsg->LinkToQueueTail( &CSteamNetworkingMessage::m_linksSecondaryQueue, &m_pParentListenSocket->m_queueRecvMessages );

	// Copy the data
	memcpy( const_cast<void*>( pMsg->GetData() ), pData, cbData );
}

void CSteamNetworkConnectionBase::ConnectionStateChanged( ESteamNetworkingConnectionState eOldState )
{

	// Post a notification when certain state changes occur.  Note that
	// "internal" state changes, where the connection is effectively closed
	// from the application's perspective, are not relevant
	ESteamNetworkingConnectionState eOldAPIState = CollapseConnectionStateToAPIState( eOldState );
	ESteamNetworkingConnectionState eNewAPIState = CollapseConnectionStateToAPIState( GetState() );

	// Internal connection used by the higher-level messages interface?
	if ( m_pMessagesInterface )
	{
		// Are we still associated with our session?
		if ( m_pMessagesSession )
		{
			// How did we get here?  We should be closed!
			if ( m_pMessagesSession->m_pConnection != this )
			{
				AssertMsg2( false, "Connection/session linkage bookkeeping bug!  %s state %d", GetDescription(), (int)GetState() );
			}
			else
			{
				m_pMessagesSession->ConnectionStateChanged( eOldAPIState, eNewAPIState );
			}
		}
		else
		{
			// We should only detach after being closed or destroyed.
			AssertMsg2( GetState() == k_ESteamNetworkingConnectionState_FinWait || GetState() == k_ESteamNetworkingConnectionState_Dead || GetState() == k_ESteamNetworkingConnectionState_None,
				"Connection %s has detatched from messages session, but is in state %d", GetDescription(), (int)GetState() );
		}
	}
	else
	{

		// Ordinary connection.  Check for posting callback, if connection state has changed from
		// an API perspective
		if ( eOldAPIState != eNewAPIState )
		{
			PostConnectionStateChangedCallback( eOldAPIState, eNewAPIState );
		}
	}

	// Any time we switch into a state that is closed from an API perspective,
	// discard any unread received messages
	if ( eNewAPIState == k_ESteamNetworkingConnectionState_None )
		m_queueRecvMessages.PurgeMessages();

	// Check crypto state
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_ClosedByPeer:

			// Clear out any secret state, since we can't use it anymore anyway.
			ClearCrypto();

			// And let stats tracking system know that it shouldn't
			// expect to be able to get stuff acked, etc
			m_statsEndToEnd.SetDisconnected( true, m_usecWhenEnteredConnectionState );
			break;

		case k_ESteamNetworkingConnectionState_Linger:
			// Don't bother trading stats back and forth with peer,
			// the only message we will send to them is "connection has been closed"
			m_statsEndToEnd.SetDisconnected( true, m_usecWhenEnteredConnectionState );
			break;

		case k_ESteamNetworkingConnectionState_Connected:
		case k_ESteamNetworkingConnectionState_FindingRoute:

			// Key exchange should be complete
			Assert( m_bCryptKeysValid );
			m_statsEndToEnd.SetDisconnected( false, m_usecWhenEnteredConnectionState );
			break;

		case k_ESteamNetworkingConnectionState_Connecting:

			// If we've completed key exchange, then we should be connected
			Assert( !m_bCryptKeysValid );

			// And we shouldn't mark stats object as ready until we go connecteded
			Assert( m_statsEndToEnd.IsDisconnected() );
			break;
	}
}

void CSteamNetworkConnectionBase::PostConnectionStateChangedCallback( ESteamNetworkingConnectionState eOldAPIState, ESteamNetworkingConnectionState eNewAPIState )
{
	SteamNetConnectionStatusChangedCallback_t c;
	PopulateConnectionInfo( c.m_info );
	c.m_eOldState = eOldAPIState;
	c.m_hConn = m_hConnectionSelf;
	m_pSteamNetworkingSocketsInterface->QueueCallback( c );
}

void CSteamNetworkConnectionBase::ConnectionState_ProblemDetectedLocally( ESteamNetConnectionEnd eReason, const char *pszFmt, ... )
{
	va_list ap;

	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();
	Assert( eReason > k_ESteamNetConnectionEnd_AppException_Max );
	Assert( pszFmt && *pszFmt );
	if ( m_eEndReason == k_ESteamNetConnectionEnd_Invalid || GetState() == k_ESteamNetworkingConnectionState_Linger )
	{
		m_eEndReason = eReason;
		va_start(ap, pszFmt);
		V_vsprintf_safe( m_szEndDebug, pszFmt, ap );
		va_end(ap);
	}

	// Check our state
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		default:
			Assert( false );
			return;

		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_ClosedByPeer:
			// Don't do anything
			break;

		case k_ESteamNetworkingConnectionState_Linger:
			ConnectionState_FinWait();
			return;

		case k_ESteamNetworkingConnectionState_Connecting:
		case k_ESteamNetworkingConnectionState_FindingRoute:
		case k_ESteamNetworkingConnectionState_Connected:
			SetState( k_ESteamNetworkingConnectionState_ProblemDetectedLocally, usecNow );
			break;
	}

	CheckConnectionStateAndSetNextThinkTime( usecNow );
}

void CSteamNetworkConnectionBase::ConnectionState_FinWait()
{
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	// Check our state
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		default:
			Assert( false );
			return;

		case k_ESteamNetworkingConnectionState_FinWait:
			break;

		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		case k_ESteamNetworkingConnectionState_Linger:
		case k_ESteamNetworkingConnectionState_Connecting:
		case k_ESteamNetworkingConnectionState_FindingRoute:
		case k_ESteamNetworkingConnectionState_Connected:
			SetState( k_ESteamNetworkingConnectionState_FinWait, usecNow );
			CheckConnectionStateAndSetNextThinkTime( usecNow );
			break;
	}
}

void CSteamNetworkConnectionBase::ConnectionState_ClosedByPeer( int nReason, const char *pszDebug )
{

	// Check our state
	switch ( m_eConnectionState )
	{
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		default:
			Assert( false );
			return;

		case k_ESteamNetworkingConnectionState_FinWait:
			// Keep hanging out until the fin wait time is up
			break;

		case k_ESteamNetworkingConnectionState_Linger:
			// Hang out to gracefully handle any last stray packets,
			// clean up relay sessions, etc.
			ConnectionState_FinWait();
			break;

		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			// Just ignore this.  We detected a problem, but now the peer
			// is also trying to close the connection.  In any case, we
			// need to wait for the client code to close the handle
			break;

		case k_ESteamNetworkingConnectionState_ClosedByPeer:
			// We already knew this, we're just waiting for
			// the client code to clean up the handle.
			break;

		case k_ESteamNetworkingConnectionState_Connecting:
		case k_ESteamNetworkingConnectionState_FindingRoute:
		case k_ESteamNetworkingConnectionState_Connected:

			if ( pszDebug && *pszDebug )
				V_strcpy_safe( m_szEndDebug, pszDebug );
			else if ( m_szEndDebug[0] == '\0' )
				V_strcpy_safe( m_szEndDebug, "The remote host closed the connection." );
			m_eEndReason = ESteamNetConnectionEnd( nReason );
			SetState( k_ESteamNetworkingConnectionState_ClosedByPeer, SteamNetworkingSockets_GetLocalTimestamp() );
			break;
	}
}

void CSteamNetworkConnectionBase::ConnectionState_Connected( SteamNetworkingMicroseconds usecNow )
{
	// Check our state
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_Linger:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		default:
			Assert( false );
			break;

		case k_ESteamNetworkingConnectionState_Connecting:
		case k_ESteamNetworkingConnectionState_FindingRoute:
			{
				// We must receive a packet in order to be connected!
				Assert( m_statsEndToEnd.m_usecTimeLastRecv > 0 );

				SetState( k_ESteamNetworkingConnectionState_Connected, usecNow );

				SNP_InitializeConnection( usecNow );
			}

			break;

		case k_ESteamNetworkingConnectionState_Connected:
			break;
	}

	// Make sure if we have any data already queued, that we start sending it out ASAP
	CheckConnectionStateAndSetNextThinkTime( usecNow );
}

void CSteamNetworkConnectionBase::ConnectionState_FindingRoute( SteamNetworkingMicroseconds usecNow )
{
	// Check our state, we really should only transition into this state from one state.
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_Linger:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		case k_ESteamNetworkingConnectionState_Connected:
		default:
			Assert( false );
			break;

		case k_ESteamNetworkingConnectionState_Connecting:
			SetState( k_ESteamNetworkingConnectionState_FindingRoute, usecNow );
			break;

		case k_ESteamNetworkingConnectionState_FindingRoute:
			break;
	}

	// Make sure if we have any data already queued, that we start sending it out ASAP
	CheckConnectionStateAndSetNextThinkTime( usecNow );
}

void CSteamNetworkConnectionBase::Think( SteamNetworkingMicroseconds usecNow )
{
	// If we queued ourselves for deletion, now is a safe time to do it.
	// Self destruct!
	if ( m_eConnectionState == k_ESteamNetworkingConnectionState_Dead )
	{
		delete this;
		return;
	}

	// CheckConnectionStateAndSetNextThinkTime does all the work of examining the current state
	// and deciding what to do.  But it should be safe to call at any time, whereas Think()
	// has a fixed contract: it should only be called by the thinker framework.
	CheckConnectionStateAndSetNextThinkTime( usecNow );
}

void CSteamNetworkConnectionBase::CheckConnectionStateAndSetNextThinkTime( SteamNetworkingMicroseconds usecNow )
{
	// Assume a default think interval just to make sure we check in periodically
	SteamNetworkingMicroseconds usecMinNextThinkTime = usecNow + k_nMillion;
	SteamNetworkingMicroseconds usecMaxNextThinkTime = usecMinNextThinkTime + 100*1000;

	auto UpdateMinThinkTime = [&]( SteamNetworkingMicroseconds usecTime, int msTol ) {
		if ( usecTime < usecMinNextThinkTime )
			usecMinNextThinkTime = usecTime;
		SteamNetworkingMicroseconds usecEnd = usecTime + msTol*1000;
		#ifdef _MSC_VER // Fix warning about optimization assuming no overflow
		Assert( usecEnd > usecTime );
		#endif
		if ( usecEnd < usecMaxNextThinkTime )
			usecMaxNextThinkTime = usecEnd;
	};

	// Check our state
	switch ( m_eConnectionState )
	{
		case k_ESteamNetworkingConnectionState_Dead:
			// This really shouldn't happen.  But if it does....
			// We can't be sure that it's safe to delete us now.
			// Just queue us for deletion ASAP.
			Assert( false );
			SetNextThinkTime( usecNow );
			return;

		case k_ESteamNetworkingConnectionState_None:
		default:
			// WAT
			Assert( false );
			return;

		case k_ESteamNetworkingConnectionState_FinWait:
		{
			// Timeout?
			SteamNetworkingMicroseconds usecTimeout = m_usecWhenEnteredConnectionState + k_usecFinWaitTimeout;
			if ( usecNow >= usecTimeout )
			{
				QueueDestroy();
				return;
			}

			// It's not time yet, make sure we get our callback when it's time.
			EnsureMinThinkTime( usecTimeout );
		}
		return;

		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		case k_ESteamNetworkingConnectionState_ClosedByPeer:
			// We don't send any data packets or keepalives in this state.
			// We're just waiting for the client API to close us.
			return;

		case k_ESteamNetworkingConnectionState_FindingRoute:
		case k_ESteamNetworkingConnectionState_Connecting:
		{

			// Timeout?
			SteamNetworkingMicroseconds usecTimeout = m_usecWhenEnteredConnectionState + (SteamNetworkingMicroseconds)m_connectionConfig.m_TimeoutInitial.Get()*1000;
			if ( usecNow >= usecTimeout )
			{
				// Check if the application just didn't ever respond, it's probably a bug.
				// We should squawk about this and let them know.
				if ( m_eConnectionState != k_ESteamNetworkingConnectionState_FindingRoute && m_pParentListenSocket )
				{
					if ( m_pMessagesSession )
					{
						ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_Timeout, "%s", "App did not respond to Messages session request in time, discarding." );
					}
					else
					{
						AssertMsg( false, "Application didn't accept or close incoming connection in a reasonable amount of time.  This is probably a bug." );
						ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_Timeout, "%s", "App didn't accept or close incoming connection in time." );
					}
				}
				else
				{
					ConnectionTimedOut( usecNow );
				}
				AssertMsg( GetState() == k_ESteamNetworkingConnectionState_ProblemDetectedLocally, "ConnectionTimedOut didn't do what it is supposed to!" );
				return;
			}

			if ( m_pParentListenSocket || m_eConnectionState == k_ESteamNetworkingConnectionState_FindingRoute )
			{
				UpdateMinThinkTime( usecTimeout, +10 );
			}
			else
			{

				SteamNetworkingMicroseconds usecRetry = usecNow + k_nMillion/20;

				// Do we have all of our crypt stuff ready?
				if ( BThinkCryptoReady( usecNow ) )
				{

					// Time to try to send an end-to-end connection?  If we cannot send packets now, then we
					// really ought to be called again if something changes, but just in case we don't, set a
					// reasonable polling interval.
					if ( BCanSendEndToEndConnectRequest() )
					{
						usecRetry = m_usecWhenSentConnectRequest + k_usecConnectRetryInterval;
						if ( usecNow >= usecRetry )
						{
							SendEndToEndConnectRequest( usecNow ); // don't return true from within BCanSendEndToEndPackets if you can't do this!
							m_usecWhenSentConnectRequest = usecNow;
							usecRetry = m_usecWhenSentConnectRequest + k_usecConnectRetryInterval;
						}
					}
				}

				UpdateMinThinkTime( usecRetry, +5 );
			}
		} break;

		case k_ESteamNetworkingConnectionState_Linger:

			// Have we sent everything we wanted to?
			if ( m_senderState.m_messagesQueued.empty() && m_senderState.m_unackedReliableMessages.empty() )
			{
				// Close the connection ASAP
				ConnectionState_FinWait();
				return;
			}

		// |
		// | otherwise, fall through
		// V
		case k_ESteamNetworkingConnectionState_Connected:
		{
			if ( BCanSendEndToEndData() )
			{
				SteamNetworkingMicroseconds usecNextThinkSNP = SNP_ThinkSendState( usecNow );
				AssertMsg1( usecNextThinkSNP > usecNow, "SNP next think time must be in in the future.  It's %lldusec in the past", (long long)( usecNow - usecNextThinkSNP ) );

				// Set a pretty tight tolerance if SNP wants to wake up at a certain time.
				if ( usecNextThinkSNP < k_nThinkTime_Never )
					UpdateMinThinkTime( usecNextThinkSNP, +1 );
			}
			else
			{
				UpdateMinThinkTime( usecNow + 20*1000, +5 );
			}
		} break;
	}

	// Update stats
	m_statsEndToEnd.Think( usecNow );

	// Check for sending keepalives or probing a connection that appears to be timing out
	if ( m_eConnectionState != k_ESteamNetworkingConnectionState_Connecting && m_eConnectionState != k_ESteamNetworkingConnectionState_FindingRoute )
	{
		Assert( m_statsEndToEnd.m_usecTimeLastRecv > 0 ); // How did we get connected without receiving anything end-to-end?

		SteamNetworkingMicroseconds usecEndToEndConnectionTimeout = m_statsEndToEnd.m_usecTimeLastRecv + (SteamNetworkingMicroseconds)m_connectionConfig.m_TimeoutConnected.Get()*1000;
		if ( usecNow >= usecEndToEndConnectionTimeout )
		{
			if ( m_statsEndToEnd.m_nReplyTimeoutsSinceLastRecv >= 4 || !BCanSendEndToEndData() )
			{
				ConnectionTimedOut( usecNow );
				AssertMsg( GetState() == k_ESteamNetworkingConnectionState_ProblemDetectedLocally, "ConnectionTimedOut didn't do what it is supposed to!" );
				return;
			}
			// The timeout time has expired, but we haven't marked enough packets as dropped yet?
			// Hm, this is weird, probably our aggressive pinging code isn't working or something.
			// In any case, just check in a bit
			UpdateMinThinkTime( usecNow + 100*1000, +100 );
		}
		else
		{
			UpdateMinThinkTime( usecEndToEndConnectionTimeout, +100 );
		}

		// Check for keepalives of varying urgency.
		// Ping aggressively because connection appears to be timing out?
		if ( m_statsEndToEnd.m_nReplyTimeoutsSinceLastRecv > 0 )
		{
			SteamNetworkingMicroseconds usecSendAggressivePing = Max( m_statsEndToEnd.m_usecTimeLastRecv, m_statsEndToEnd.m_usecLastSendPacketExpectingImmediateReply ) + k_usecAggressivePingInterval;
			if ( usecNow >= usecSendAggressivePing )
			{
				if ( BCanSendEndToEndData() )
				{
					if ( m_statsEndToEnd.m_nReplyTimeoutsSinceLastRecv == 1 )
						SpewVerbose( "[%s] Reply timeout, last recv %.1fms ago.  Sending keepalive.\n", GetDescription(), ( usecNow - m_statsEndToEnd.m_usecTimeLastRecv ) * 1e-3 );
					else
						SpewMsg( "[%s] %d reply timeouts, last recv %.1fms ago.  Sending keepalive.\n", GetDescription(), m_statsEndToEnd.m_nReplyTimeoutsSinceLastRecv, ( usecNow - m_statsEndToEnd.m_usecTimeLastRecv ) * 1e-3 );
					Assert( m_statsEndToEnd.BNeedToSendPingImmediate( usecNow ) ); // Make sure logic matches
					SendEndToEndStatsMsg( k_EStatsReplyRequest_Immediate, usecNow, "E2ETimingOutKeepalive" );
					AssertMsg( !m_statsEndToEnd.BNeedToSendPingImmediate( usecNow ), "SendEndToEndStatsMsg didn't do its job!" );
					Assert( m_statsEndToEnd.m_usecInFlightReplyTimeout != 0 );
				}
				else
				{
					// Nothing we can do right now.  Just check back in a little bit.
					UpdateMinThinkTime( usecNow+20*1000, +5 );
				}
			}
			else
			{
				UpdateMinThinkTime( usecSendAggressivePing, +20 );
			}
		}

		// Ordinary keepalive?
		if ( m_statsEndToEnd.m_usecInFlightReplyTimeout == 0 )
		{
			// FIXME We really should be a lot better here with an adaptive keepalive time.  If they have been
			// sending us a steady stream of packets, we could expect it to continue at a high rate, so that we
			// can begin to detect a dropped connection much more quickly.  But if the connection is mostly idle, we want
			// to make sure we use a relatively long keepalive.
			SteamNetworkingMicroseconds usecSendKeepalive = m_statsEndToEnd.m_usecTimeLastRecv+k_usecKeepAliveInterval;
			if ( usecNow >= usecSendKeepalive )
			{
				if ( BCanSendEndToEndData() )
				{
					Assert( m_statsEndToEnd.BNeedToSendKeepalive( usecNow ) ); // Make sure logic matches
					SendEndToEndStatsMsg( k_EStatsReplyRequest_DelayedOK, usecNow, "E2EKeepalive" );
					AssertMsg( !m_statsEndToEnd.BNeedToSendKeepalive( usecNow ), "SendEndToEndStatsMsg didn't do its job!" );
				}
				else
				{
					// Nothing we can do right now.  Just check back in a little bit.
					UpdateMinThinkTime( usecNow+20*1000, +5 );
				}
			}
			else
			{
				// Not right now, but schedule a wakeup call to do it
				UpdateMinThinkTime( usecSendKeepalive, +100 );
			}
		}
	}

	// Scheduled think time must be in the future.  If some code is setting a think time for right now,
	// then it should have just done it.
	if ( usecMinNextThinkTime <= usecNow )
	{
		AssertMsg1( false, "Scheduled next think time must be in in the future.  It's %lldusec in the past", (long long)( usecNow - usecMinNextThinkTime ) );
		usecMinNextThinkTime = usecNow + 1000;
		usecMaxNextThinkTime = usecMinNextThinkTime + 2000;
	}

	// Hook for derived class to do its connection-type-specific stuff
	ThinkConnection( usecNow );

	// Schedule next time to think, if derived class didn't request an earlier
	// wakeup call.  We ask that we not be woken up early, because none of the code
	// above who is setting this timeout will trigger, and we'll just go back to
	// sleep again.  So better to be just a tiny bit late than a tiny bit early.
	Assert( usecMaxNextThinkTime >= usecMinNextThinkTime+1000 );
	EnsureMinThinkTime( usecMinNextThinkTime, (usecMaxNextThinkTime-usecMinNextThinkTime)/1000 );
}

void CSteamNetworkConnectionBase::ThinkConnection( SteamNetworkingMicroseconds usecNow )
{
}

void CSteamNetworkConnectionBase::ConnectionTimedOut( SteamNetworkingMicroseconds usecNow )
{
	ESteamNetConnectionEnd nReasonCode;
	ConnectionEndDebugMsg msg;

	// Set some generic defaults using our base class version, so
	// this function will work even if the derived class forgets to
	// call the base class.
	CSteamNetworkConnectionBase::GuessTimeoutReason( nReasonCode, msg, usecNow );

	// Check if connection has a more enlightened understanding of what's wrong
	GuessTimeoutReason( nReasonCode, msg, usecNow );

	// Switch connection state
	ConnectionState_ProblemDetectedLocally( nReasonCode, "%s", msg );
}

void CSteamNetworkConnectionBase::GuessTimeoutReason( ESteamNetConnectionEnd &nReasonCode, ConnectionEndDebugMsg &msg, SteamNetworkingMicroseconds usecNow )
{
	NOTE_UNUSED( usecNow );

	nReasonCode = k_ESteamNetConnectionEnd_Misc_Timeout;
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_Connecting:
			V_strcpy_safe( msg, "Timed out attempting to connect" );
			break;

		case k_ESteamNetworkingConnectionState_FindingRoute:
			V_strcpy_safe( msg, "Timed out attempting to negotiate rendezvous" );
			break;

		default:
			V_strcpy_safe( msg, "Connection dropped" );
			break;
	}
}

void CSteamNetworkConnectionBase::UpdateSpeeds( int nTXSpeed, int nRXSpeed )
{
	m_statsEndToEnd.UpdateSpeeds( nTXSpeed, nRXSpeed );
}

/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkConnectionPipe
//
/////////////////////////////////////////////////////////////////////////////

bool CSteamNetworkConnectionPipe::APICreateSocketPair( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface, CSteamNetworkConnectionPipe *pConn[2], const SteamNetworkingIdentity pIdentity[2] )
{
	SteamDatagramErrMsg errMsg;
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	pConn[1] = new CSteamNetworkConnectionPipe( pSteamNetworkingSocketsInterface, pIdentity[0] );
	pConn[0] = new CSteamNetworkConnectionPipe( pSteamNetworkingSocketsInterface, pIdentity[1] );
	if ( !pConn[0] || !pConn[1] )
	{
failed:
		delete pConn[0]; pConn[0] = nullptr;
		delete pConn[1]; pConn[1] = nullptr;
		return false;
	}

	pConn[0]->m_pPartner = pConn[1];
	pConn[1]->m_pPartner = pConn[0];

	// Do generic base class initialization
	for ( int i = 0 ; i < 2 ; ++i )
	{
		if ( !pConn[i]->BInitConnection( usecNow, errMsg ) )
			goto failed;

		// Slam in a really large SNP rate
		int nRate = 0x10000000;
		pConn[i]->m_connectionConfig.m_SendRateMin.Set( nRate );
		pConn[i]->m_connectionConfig.m_SendRateMax.Set( nRate );
	}

	// Exchange some dummy "connect" packets so that all of our internal variables
	// (and ping) look as realistic as possible
	pConn[0]->FakeSendStats( usecNow, 0 );
	pConn[1]->FakeSendStats( usecNow, 0 );

	// Tie the connections to each other, and mark them as connected
	for ( int i = 0 ; i < 2 ; ++i )
	{
		CSteamNetworkConnectionPipe *p = pConn[i];
		CSteamNetworkConnectionPipe *q = pConn[1-i];
		p->m_identityRemote = q->m_identityLocal;
		p->m_unConnectionIDRemote = q->m_unConnectionIDLocal;
		if ( !p->BRecvCryptoHandshake( q->m_msgSignedCertLocal, q->m_msgSignedCryptLocal, i==0 ) )
		{
			AssertMsg( false, "BRecvCryptoHandshake failed creating localhost socket pair" );
			goto failed;
		}
		p->ConnectionState_Connected( usecNow );
	}

	return true;
}

CSteamNetworkConnectionPipe::CSteamNetworkConnectionPipe( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface, const SteamNetworkingIdentity &identity )
: CSteamNetworkConnectionBase( pSteamNetworkingSocketsInterface )
, m_pPartner( nullptr )
{
	m_identityLocal = identity;
}

CSteamNetworkConnectionPipe::~CSteamNetworkConnectionPipe()
{
	Assert( !m_pPartner );
}

void CSteamNetworkConnectionPipe::GetConnectionTypeDescription( ConnectionTypeDescription_t &szDescription ) const
{
	V_strcpy_safe( szDescription, "pipe" );
}

CSteamNetworkConnectionBase::ERemoteUnsignedCert CSteamNetworkConnectionPipe::AllowRemoteUnsignedCert()
{
	// It's definitely us, and we trust ourselves, right?
	return k_ERemoteUnsignedCert_Allow;
}

void CSteamNetworkConnectionPipe::InitConnectionCrypto( SteamNetworkingMicroseconds usecNow )
{
	InitLocalCryptoWithUnsignedCert();
}

EResult CSteamNetworkConnectionPipe::_APISendMessageToConnection( const void *pData, uint32 cbData, int nSendFlags )
{
	if ( !m_pPartner )
	{
		// Caller should have checked the connection at a higher level, so this is a bug
		AssertMsg( false, "No partner pipe?" );
		return k_EResultFail;
	}
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	// Fake a bunch of stats
	FakeSendStats( usecNow, cbData );

	int64 nMsgNum = ++m_senderState.m_nLastSentMsgNum;

	// Pass directly to our partner
	m_pPartner->ReceivedMessage( pData, cbData, nMsgNum, usecNow );

	return k_EResultOK;
}

void CSteamNetworkConnectionPipe::FakeSendStats( SteamNetworkingMicroseconds usecNow, int cbPktSize )
{
	if ( !m_pPartner )
		return;

	// Get the next packet number we would have sent
	uint16 nSeqNum = m_statsEndToEnd.ConsumeSendPacketNumberAndGetWireFmt( usecNow );

	// And the peer receiving it immediately.  And assume every packet represents
	// a ping measurement.
	int64 nPktNum = m_pPartner->m_statsEndToEnd.ExpandWirePacketNumberAndCheck( nSeqNum );
	Assert( nPktNum+1 == m_statsEndToEnd.m_nNextSendSequenceNumber );
	m_pPartner->m_statsEndToEnd.TrackProcessSequencedPacket( nPktNum, usecNow, -1 );
	m_pPartner->m_statsEndToEnd.TrackRecvPacket( cbPktSize, usecNow );
	m_pPartner->m_statsEndToEnd.m_ping.ReceivedPing( 0, usecNow );

	// Fake sending stats
	m_statsEndToEnd.TrackSentPacket( cbPktSize );
}

void CSteamNetworkConnectionPipe::SendEndToEndStatsMsg( EStatsReplyRequest eRequest, SteamNetworkingMicroseconds usecNow, const char *pszReason )
{
	NOTE_UNUSED( eRequest );
	NOTE_UNUSED( pszReason );

	if ( !m_pPartner )
	{
		Assert( false );
		return;
	}

	// Fake sending us a ping request
	m_statsEndToEnd.TrackSentPingRequest( usecNow, false );
	FakeSendStats( usecNow, 0 );

	// Fake partner receiving it
	m_pPartner->m_statsEndToEnd.PeerAckedLifetime( usecNow );
	m_pPartner->m_statsEndToEnd.PeerAckedInstantaneous( usecNow );

	// ...and sending us a reply immediately
	m_pPartner->FakeSendStats( usecNow, 0 );

	// ... and us receiving it immediately
	m_pPartner->m_statsEndToEnd.PeerAckedLifetime( usecNow );
	m_pPartner->m_statsEndToEnd.PeerAckedInstantaneous( usecNow );
}

bool CSteamNetworkConnectionPipe::BCanSendEndToEndConnectRequest() const
{
	// We're never not connected, so nobody should ever need to ask this question
	AssertMsg( false, "Shouldn't need to ask this question" );
	return false;
}

bool CSteamNetworkConnectionPipe::BCanSendEndToEndData() const
{
	Assert( m_pPartner );
	return m_pPartner != nullptr;
}

void CSteamNetworkConnectionPipe::SendEndToEndConnectRequest( SteamNetworkingMicroseconds usecNow )
{
	AssertMsg( false, "Inconceivable!" );
}

EResult CSteamNetworkConnectionPipe::APIAcceptConnection()
{
	AssertMsg( false, "Inconceivable!" );
	return k_EResultFail;
}

bool CSteamNetworkConnectionPipe::SendDataPacket( SteamNetworkingMicroseconds usecNow )
{
	AssertMsg( false, "CSteamNetworkConnectionPipe connections shouldn't try to send 'packets'!" );
	return false;
}

int CSteamNetworkConnectionPipe::SendEncryptedDataChunk( const void *pChunk, int cbChunk, SendPacketContext_t &ctx )
{
	AssertMsg( false, "CSteamNetworkConnectionPipe connections shouldn't try to send 'packets'!" );
	return -1;
}

void CSteamNetworkConnectionPipe::ConnectionStateChanged( ESteamNetworkingConnectionState eOldState )
{
	CSteamNetworkConnectionBase::ConnectionStateChanged( eOldState );

	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_FindingRoute:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: // What local "problem" could we have detected??
		default:
			Assert( false );
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_Linger:
			if ( m_pPartner )
			{
				CSteamNetworkConnectionPipe *pPartner = m_pPartner;
				m_pPartner = nullptr; // clear pointer now, to prevent recursion
				pPartner->ConnectionState_ClosedByPeer( m_eEndReason, m_szEndDebug );
			}
			break;

		case k_ESteamNetworkingConnectionState_Connecting:
		case k_ESteamNetworkingConnectionState_Connected:
			Assert( m_pPartner );
			break;

		case k_ESteamNetworkingConnectionState_ClosedByPeer:

			// If we have a partner, they should be the ones initiating this.
			// (In the code directly above.)
			if ( m_pPartner )
			{
				Assert( CollapseConnectionStateToAPIState( m_pPartner->GetState() ) == k_ESteamNetworkingConnectionState_None );
				Assert( m_pPartner->m_pPartner == nullptr );
				m_pPartner = nullptr;
			}
			break;
	}
}

void CSteamNetworkConnectionPipe::PostConnectionStateChangedCallback( ESteamNetworkingConnectionState eOldAPIState, ESteamNetworkingConnectionState eNewAPIState )
{
	// Don't post any callbacks for the initial transitions.
	if ( eNewAPIState == k_ESteamNetworkingConnectionState_Connecting || eNewAPIState == k_ESteamNetworkingConnectionState_Connected )
		return;

	// But post callbacks for these guys
	CSteamNetworkConnectionBase::PostConnectionStateChangedCallback( eOldAPIState, eNewAPIState );
}


#ifdef DBGFLAG_VALIDATE
void CSteamNetworkConnectionBase::ValidateStatics( CValidator &validator )
{
	for ( const TrustedKey &trustedKey: s_arTrustedKeys )
	{
		ValidateObj( trustedKey );
	}
}
#endif

} // namespace SteamNetworkingSocketsLib
