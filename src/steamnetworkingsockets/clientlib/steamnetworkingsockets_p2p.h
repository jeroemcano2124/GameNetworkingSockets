//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef STEAMNETWORKINGSOCKETS_P2P_H
#define STEAMNETWORKINGSOCKETS_P2P_H
#pragma once

#include <steam/steamnetworkingcustomsignaling.h>
#include "steamnetworkingsockets_connections.h"
#include "csteamnetworkingsockets.h"

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
	#include <steamdatagram_messages_sdr.pb.h>
#endif

class CMsgSteamDatagramConnectRequest;

namespace SteamNetworkingSocketsLib {

/// Special disconnection reason code that is used in signals
/// to indicate "no connection"
const uint32 k_ESteamNetConnectionEnd_Internal_P2PNoConnection = 9999;

// If we are the "controlled" agent, add this penalty to routes
// other than the one that are not the one the controlling agent
// has selected
constexpr int k_nRoutePenaltyNotNominated = 100;
constexpr int k_nRoutePenaltyNeedToConfirmConnectivity = 10000;
constexpr int k_nRoutePenaltyNotLan = 10; // Any route that appears to be a LAN route gets a bonus.  (Actually, all others are penalized)
constexpr int k_nRoutePenaltyNotSelectedOverride = 4000;

// Values for P2PTRansportOverride config value
constexpr int k_nP2P_TransportOverride_None = 0;
constexpr int k_nP2P_TransportOverride_SDR = 1;
constexpr int k_nP2P_TransportOverride_ICE = 2;

constexpr int k_nICECloseCode_Local_NotCompiled = k_ESteamNetConnectionEnd_Local_Max;
constexpr int k_nICECloseCode_Local_UserNotEnabled = k_ESteamNetConnectionEnd_Local_Max-1;
constexpr int k_nICECloseCode_Aborted = k_ESteamNetConnectionEnd_Local_Max-2;
constexpr int k_nICECloseCode_Remote_NotEnabled = k_ESteamNetConnectionEnd_Remote_Max;

class CConnectionTransportP2PSDR;
class CConnectionTransportP2PICE;

//-----------------------------------------------------------------------------
/// Listen socket for peer-to-peer connections relayed through through SDR network
/// We can only do this on platforms where this is some sort of "default" signaling
/// mechanism

class CSteamNetworkListenSocketP2P final : public CSteamNetworkListenSocketBase
{
public:
	CSteamNetworkListenSocketP2P( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface );

	// CSteamNetworkListenSocketBase overrides
	virtual bool BSupportsSymmetricMode() override { return true; }

	/// Setup
	bool BInit( int nLocalVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions, SteamDatagramErrMsg &errMsg );

	inline int LocalVirtualPort() const
	{
		Assert( m_connectionConfig.m_LocalVirtualPort.IsLocked() );
		return m_connectionConfig.m_LocalVirtualPort.m_data;
	}

private:
	virtual ~CSteamNetworkListenSocketP2P(); // hidden destructor, don't call directly.  Use Destroy()
};

/// Mixin base class for different P2P transports.
class CConnectionTransportP2PBase
{
public:
	// Virtual base classes.  (We don't directly derive, since we are a mixin,
	// but all classes that derive from us will derive from these base classes.)
	CConnectionTransport *const m_pSelfAsConnectionTransport;
	IThinker *const m_pSelfAsThinker;

	const char *const m_pszP2PTransportDebugName;

	/// True if we need to take aggressive action to confirm
	/// end-to-end connectivity.  This will be the case when
	/// doing initial route finding, or if we aren't sure about
	/// end-to-end connectivity because we lost all of our
	/// sessions, etc.  Once we get some data packets, we set
	/// this flag to false.
	bool m_bNeedToConfirmEndToEndConnectivity;

	// Some basic stats tracking about ping times.  Currently these only track the pings
	// explicitly sent at this layer.  Ideally we would hook into the SNP code, because
	// almost every data packet we send contains ping-related information.
	PingTrackerForRouteSelection m_pingEndToEnd;
	SteamNetworkingMicroseconds m_usecEndToEndInFlightReplyTimeout;
	int m_nReplyTimeoutsSinceLastRecv;
	int m_nKeepTryingToPingCounter;
	SteamNetworkingMicroseconds m_usecWhenSelected; // nonzero if we are the current transport
	SteamNetworkingMicroseconds m_usecTimeSelectedAccumulator; // How much time have we spent selected, not counting the current activation

	SteamNetworkingMicroseconds CalcTotalTimeSelected( SteamNetworkingMicroseconds usecNow ) const;

	struct P2PRouteQualityMetrics
	{
		// Scores based only on ping times.
		int m_nScoreCurrent;
		int m_nScoreMin;
		int m_nScoreMax;

		// Sum of all penalties
		int m_nTotalPenalty;

		// Number of recent valid ping collection intervals.
		// (See PingTrackerForRouteSelection)
		int m_nBucketsValid;

		inline void SetInvalid()
		{
			m_nScoreCurrent = k_nRouteScoreHuge;
			m_nScoreMin = k_nRouteScoreHuge;
			m_nScoreMax = k_nRouteScoreHuge;
			m_nTotalPenalty = 0;
			m_nBucketsValid = 0;
		}

	};
	P2PRouteQualityMetrics m_routeMetrics;

	void P2PTransportTrackRecvEndToEndPacket( SteamNetworkingMicroseconds usecNow )
	{
		m_usecEndToEndInFlightReplyTimeout = 0;
		m_nReplyTimeoutsSinceLastRecv = 0;
	}
	void P2PTransportTrackSentEndToEndPingRequest( SteamNetworkingMicroseconds usecNow, bool bAllowDelayedReply );
	void P2PTransportThink( SteamNetworkingMicroseconds usecNow );
	void P2PTransportEndToEndConnectivityConfirmed( SteamNetworkingMicroseconds usecNow );
	void P2PTransportEndToEndConnectivityNotConfirmed( SteamNetworkingMicroseconds usecNow );

	// Populate m_routeMetrics.  If we're not really available, then the metrics should be set to a huge score
	virtual void P2PTransportUpdateRouteMetrics( SteamNetworkingMicroseconds usecNow ) = 0;

protected:
	CConnectionTransportP2PBase( const char *pszDebugName, CConnectionTransport *pSelfBase, IThinker *pSelfThinker );

	// Shortcut to get connection and upcast
	CSteamNetworkConnectionP2P &Connection() const;
};

/// A peer-to-peer connection that can use different types of underlying transport
class CSteamNetworkConnectionP2P final : public CSteamNetworkConnectionBase
{
public:
	CSteamNetworkConnectionP2P( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface );

	/// Start connecting to a remote peer at the specified virtual port
	bool BInitConnect(
		ISteamNetworkingConnectionCustomSignaling *pSignaling,
		const SteamNetworkingIdentity *pIdentityRemote, int nRemoteVirtualPort,
		int nOptions, const SteamNetworkingConfigValue_t *pOptions,
		CSteamNetworkConnectionP2P **pOutMatchingSymmetricConnection,
		SteamDatagramErrMsg &errMsg
	);

	/// Begin accepting a P2P connection
	bool BBeginAccept(
		const CMsgSteamNetworkingP2PRendezvous_ConnectRequest &msgConnectRequest,
		SteamDatagramErrMsg &errMsg,
		SteamNetworkingMicroseconds usecNow
	);

	/// Called on a connection that we initiated, when we have a matching symmetric incoming connection,
	/// and we need to change the role of our connection to be "server"
	void ChangeRoleToServerAndAccept( const CMsgSteamNetworkingP2PRendezvous &msg, SteamNetworkingMicroseconds usecNow );

	// CSteamNetworkConnectionBase overrides
	virtual void FreeResources() override;
	virtual EResult AcceptConnection( SteamNetworkingMicroseconds usecNow ) override;
	virtual void GetConnectionTypeDescription( ConnectionTypeDescription_t &szDescription ) const override;
	virtual void ThinkConnection( SteamNetworkingMicroseconds usecNow ) override;
	virtual SteamNetworkingMicroseconds ThinkConnection_ClientConnecting( SteamNetworkingMicroseconds usecNow ) override;
	virtual void DestroyTransport() override;
	virtual CSteamNetworkConnectionP2P *AsSteamNetworkConnectionP2P() override;
	virtual void ConnectionStateChanged( ESteamNetworkingConnectionState eOldState ) override;
	virtual void ProcessSNPPing( int msPing, RecvPacketContext_t &ctx ) override;
	virtual bool BSupportsSymmetricMode() override;

	void SendConnectOKSignal( SteamNetworkingMicroseconds usecNow );
	void SendConnectionClosedSignal( SteamNetworkingMicroseconds usecNow );
	void SendNoConnectionSignal( SteamNetworkingMicroseconds usecNow );

	void ScheduleSendSignal( const char *pszReason );
	void QueueSignalReliableMessage( CMsgSteamNetworkingP2PRendezvous_ReliableMessage &&msg, const char *pszDebug );

	/// Given a partially-completed CMsgSteamNetworkingP2PRendezvous, finish filling out
	/// the required fields, and send it to the peer via the signaling mechanism
	void SetRendezvousCommonFieldsAndSendSignal( CMsgSteamNetworkingP2PRendezvous &msg, SteamNetworkingMicroseconds usecNow, const char *pszDebugReason );

	bool ProcessSignal( const CMsgSteamNetworkingP2PRendezvous &msg, SteamNetworkingMicroseconds usecNow );
	void ProcessSignal_ConnectOK( const CMsgSteamNetworkingP2PRendezvous_ConnectOK &msgConnectOK, SteamNetworkingMicroseconds usecNow );

	// Return true if we are the "controlling" peer, in the ICE sense of the term.
	// That is, the agent who will primarily make the route decisions, with the
	// controlled agent accepting whatever routing decisions are made, when possible.
	inline bool IsControllingAgent() const
	{
		// For now, the "server" will always be the controlling agent.
		// This is the opposite of the ICE convention, but we had some
		// reasons for the initial use case to do it this way.  We can
		// plumb through role negotiation if we need to change this.
		return m_bConnectionInitiatedRemotely;
	}

	/// Virtual port on the remote host.  If connection was initiated locally, this will always be valid.
	/// If initiated remotely, we don't need to know except for the purpose of purposes of symmetric connection
	/// matching.  If the peer didn't specify when attempting to connect, we will assume that it is the same
	/// as the local virtual port.
	int m_nRemoteVirtualPort;

	/// local virtual port is a configuration option
	inline int LocalVirtualPort() const { return m_connectionConfig.m_LocalVirtualPort.Get(); }

	/// Handle to our entry in g_mapIncomingP2PConnections, or -1 if we're not in the map
	int m_idxMapP2PConnectionsByRemoteInfo;

	/// How to send signals to the remote host for this
	ISteamNetworkingConnectionCustomSignaling *m_pSignaling;

	//
	// Different transports
	//

	// Steam datagram relay
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
		CConnectionTransportP2PSDR *m_pTransportP2PSDR;
		CMsgSteamNetworkingP2PSDRRoutingSummary m_msgSDRRoutingSummary;
	#endif

	// ICE (direct NAT punch)
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE

		// ICE transport that we are using, if any
		CConnectionTransportP2PICE *m_pTransportICE;

		// If ICE transport needs to self-destruct, we move it here, and clear
		// m_pTransportICE.  Then it will be deleted at a safe time.
		CConnectionTransportP2PICE *m_pTransportICEPendingDelete;

		// When we receive a connection from peer, we need to wait for the app
		// to accept it.  During that time we may need to pend any ICE messages
		std_vector<CMsgICERendezvous> m_vecPendingICEMessages;

		// Summary of connection.  Note in particular that the failure reason (if any)
		// is here.
		CMsgSteamNetworkingICESessionSummary m_msgICESessionSummary;

		// Detailed failure reason string.
		ConnectionEndDebugMsg m_szICECloseMsg;

		void ICEFailed( int nReasonCode, const char *pszReason );
		inline int GetICEFailureCode() const { return m_msgICESessionSummary.failure_reason_code(); }
		void GuessICEFailureReason( ESteamNetConnectionEnd &nReasonCode, ConnectionEndDebugMsg &msg, SteamNetworkingMicroseconds usecNow );
	#else
		inline int GetICEFailureCode() const { return k_nICECloseCode_Local_NotCompiled; }
	#endif

	/// Sometimes it's nice to have all existing options in a list
	vstd::small_vector< CConnectionTransportP2PBase *, 3 > m_vecAvailableTransports;

	/// Currently selected transport.
	/// Always the same as m_pTransport, but as CConnectionTransportP2PBase
	CConnectionTransportP2PBase *m_pCurrentTransportP2P;

	/// Which transport does it look like our peer is using?
	CConnectionTransportP2PBase *m_pPeerSelectedTransport;
	void SetPeerSelectedTransport( CConnectionTransportP2PBase *pPeerSelectedTransport )
	{
		if ( m_pPeerSelectedTransport != pPeerSelectedTransport )
		{
			m_pPeerSelectedTransport = pPeerSelectedTransport;
			PeerSelectedTransportChanged();
		}
	}

	bool BInitSDR( SteamNetworkingErrMsg &errMsg );

	// Check if user permissions for the remote host are allowed, then
	// create ICE.  Also, if the connection was initiated remotely,
	// we will create an offer
	void CheckInitICE();

	// Check if we pended ICE deletion, then do so now
	void CheckCleanupICE();

	// If we don't already have a failure code for ice, set one now.
	void EnsureICEFailureReasonSet( SteamNetworkingMicroseconds usecNow );

	//
	// Transport evaluation and selection
	//

	SteamNetworkingMicroseconds m_usecWhenStartedFindingRoute;

	SteamNetworkingMicroseconds m_usecNextEvaluateTransport;

	/// True if we should be "sticky" to the current transport.
	/// When major state changes happen, we clear this flag
	/// and evaluate from scratch with no stickiness
	bool m_bTransportSticky;

	void ThinkSelectTransport( SteamNetworkingMicroseconds usecNow );
	void TransportEndToEndConnectivityChanged( CConnectionTransportP2PBase *pTransportP2P, SteamNetworkingMicroseconds usecNow );
	void SelectTransport( CConnectionTransportP2PBase *pTransport, SteamNetworkingMicroseconds usecNow );

	void UpdateTransportSummaries( SteamNetworkingMicroseconds usecNow );

	// FIXME - UDP transport for LAN discovery, so P2P works without any signaling

	inline int LogLevel_P2PRendezvous() const { return m_connectionConfig.m_LogLevel_P2PRendezvous.Get(); }

	static CSteamNetworkConnectionP2P *FindDuplicateConnection( CSteamNetworkingSockets *pInterfaceLocal, int nLocalVirtualPort, const SteamNetworkingIdentity &identityRemote, int nRemoteVirtualPort, bool bOnlySymmetricConnections, CSteamNetworkConnectionP2P *pIgnore );

	bool BEnsureInP2PConnectionMapByRemoteInfo( SteamDatagramErrMsg &errMsg );

private:
	virtual ~CSteamNetworkConnectionP2P(); // hidden destructor, don't call directly.  Use ConnectionDestroySelfNow

	/// Shared init
	bool BInitP2PConnectionCommon( SteamNetworkingMicroseconds usecNow, int nOptions, const SteamNetworkingConfigValue_t *pOptions, SteamDatagramErrMsg &errMsg );

	struct OutboundMessage
	{
		uint32 m_nID;
		int m_cbSerialized;
		SteamNetworkingMicroseconds m_usecRTO; // Retry timeout
		CMsgSteamNetworkingP2PRendezvous_ReliableMessage m_msg;
	};
	std_vector< OutboundMessage > m_vecUnackedOutboundMessages; // outbound messages that have not been acked

	const char *m_pszNeedToSendSignalReason;
	SteamNetworkingMicroseconds m_usecSendSignalDeadline;
	uint32 m_nLastSendRendesvousMessageID;
	uint32 m_nLastRecvRendesvousMessageID;

	// Really destroy ICE now
	void DestroyICENow();

	void PeerSelectedTransportChanged();
};

inline CSteamNetworkConnectionP2P &CConnectionTransportP2PBase::Connection() const
{
	return *assert_cast<CSteamNetworkConnectionP2P *>( &m_pSelfAsConnectionTransport->m_connection );
}

} // namespace SteamNetworkingSocketsLib

#endif // STEAMNETWORKINGSOCKETS_P2P_H
