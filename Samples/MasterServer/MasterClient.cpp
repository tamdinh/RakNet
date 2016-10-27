/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant 
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "MasterClient.h"
#include "RakPeerInterface.h"
#include "MessageIdentifiers.h"
#include "StringCompressor.h"
#include "GetTime.h"
#include <cstring>

// Uncomment this define for debugging printfs
#define _SHOW_MASTER_SERVER_PRINTF
#ifdef _SHOW_MASTER_SERVER_PRINTF
#include <cstdio>
#endif

MasterClient::MasterClient()
{
}

MasterClient::~MasterClient()
{
	ClearServerList();
}

bool MasterClient::Connect(char* host, int masterServerPort)
{
	localServer.Clear();
	listServer=serverListed=localServerModified=false;
    SystemAddress sa = UNASSIGNED_SYSTEM_ADDRESS;
    sa.FromStringExplicitPort(host, masterServerPort);
    localServer.connectionIdentifier = rakPeer->GetGuidFromSystemAddress(sa);
//	localServer.connectionIdentifier.port = rakPeer->GetInternalID().GetPort();
	ruleIdentifierList.Reset();

	return rakPeer->Connect(host, masterServerPort, 0, 0);
}

void MasterClient::Disconnect(void)
{
	if (IsConnected())
		DelistServer();

//	rakPeer->Disconnect(100);
    RakNet::RakPeerInterface::DestroyInstance(rakPeer);
}

bool MasterClient::IsConnected(void)
{
	unsigned short numberOfSystems;
	rakPeer->GetConnectionList(0, &numberOfSystems);
	return numberOfSystems==1;
}

void MasterClient::AddQueryRule(char *ruleIdentifier)
{
	if (ruleIdentifier && IsReservedRuleIdentifier(ruleIdentifier)==false)
		stringCompressor->EncodeString(ruleIdentifier, 256, &ruleIdentifierList);
}
void MasterClient::ClearQueryRules(void)
{
	ruleIdentifierList.Reset();
}
void MasterClient::QueryMasterServer(void)
{
	BitStream outgoingBitStream;
	// Request to the master server for the list of servers that contain at least one of the specified keys
	outgoingBitStream.Write((unsigned char)ID_QUERY_MASTER_SERVER);
	if (ruleIdentifierList.GetNumberOfBitsUsed()>0)
		outgoingBitStream.WriteBits(ruleIdentifierList.GetData(), ruleIdentifierList.GetNumberOfBitsUsed(), false);
    rakPeer->Send(&outgoingBitStream, HIGH_PRIORITY, RELIABLE_ORDERED, 0, UNASSIGNED_RAKNET_GUID, true);
}

void MasterClient::PingServers(void)
{
	unsigned serverIndex;

	for (serverIndex=0; serverIndex < gameServerList.serverList.Size(); serverIndex++)
	{
        SystemAddress sa;
        rakPeer->GetSystemAddressFromGuid(gameServerList.serverList[serverIndex]->connectionIdentifier);
		rakPeer->Ping(sa.ToString(),sa.GetPort(), false);
	}
}

void MasterClient::Update(RakPeerInterface *peer)
{
	BitStream outgoingBitStream;

	if (listServer && ((serverListed && localServerModified) || (serverListed==false)))
	{
		outgoingBitStream.Write((unsigned char)ID_MASTER_SERVER_SET_SERVER);
		SerializeServer(&localServer, &outgoingBitStream);
		rakPeer->Send(&outgoingBitStream, MEDIUM_PRIORITY, RELIABLE_ORDERED, 0, UNASSIGNED_RAKNET_GUID, true);
		serverListed=true;
		localServerModified=false;
	}
}

bool MasterClient::OnReceive(RakPeerInterface *peer, Packet *packet)
{
	switch(packet->data[0])
	{
	case ID_NO_FREE_INCOMING_CONNECTIONS:
		OnMasterServerFull();
		return false; // Do not absorb packet
	case ID_DISCONNECTION_NOTIFICATION:
		OnLostConnection();
		return false; // Do not absorb packet
	case ID_CONNECTION_LOST:
		OnLostConnection();
		return false; // Do not absorb packet
//	case ID_MODIFIED_PACKET:
//		OnModifiedPacket();
//		return false;
	case ID_CONNECTION_ATTEMPT_FAILED:
		OnConnectionAttemptFailed();
		return false; // Do not absorb packet
	case ID_MASTER_SERVER_UPDATE_SERVER:
		HandleServerListResponse(packet, false);
		return true; // Absorb packet
	case ID_MASTER_SERVER_SET_SERVER:
		HandleServerListResponse(packet, true);
		return true; // Absorb packet
	case ID_UNCONNECTED_PONG:
		HandlePong(packet);
		return false; // Absorb packet
	case ID_RELAYED_CONNECTION_NOTIFICATION:
		HandleRelayedConnectionNotification(packet);
		return true; // Absorb packet
	}

	return 0;
}

void MasterClient::ConnectionAttemptNotification(char *serverIP, unsigned short serverPort)
{
	if (serverIP==0)
		return;

	BitStream bitStream(23);
	bitStream.Write((unsigned char)ID_RELAYED_CONNECTION_NOTIFICATION);
    
    SystemAddress sa = rakPeer->GetSystemAddressFromGuid(localServer.connectionIdentifier);
	bitStream.Write(sa.GetPort()); // Your own game client port
	bitStream.Write(serverPort); // The game server you are connecting to port
	stringCompressor->EncodeString(serverIP, 22, &bitStream); // The game server IP you are connecting to
	rakPeer->Send(&bitStream, HIGH_PRIORITY, RELIABLE, 0, UNASSIGNED_RAKNET_GUID, true);
}
void MasterClient::ListServer(void)
{
	listServer=true;
}
void MasterClient::DelistServer(void)
{
	BitStream bitStream;
	listServer=false;
	if (serverListed)
	{
        SystemAddress sa = rakPeer->GetSystemAddressFromGuid(localServer.connectionIdentifier);
		bitStream.Write((unsigned char)ID_MASTER_SERVER_DELIST_SERVER);
		bitStream.Write(sa.GetPort());
        rakPeer->Send(&bitStream, HIGH_PRIORITY, RELIABLE_ORDERED, 0, UNASSIGNED_RAKNET_GUID, true);
		serverListed=false;
	}
}
void MasterClient::HandleServerListResponse(Packet *packet, bool overwriteExisting)
{
	int serverIndex;
	bool newServerAdded;
	unsigned short numberOfServers;
	GameServer *gameServer;
	RakNet::Time currentTime;
	BitStream inputBitStream(packet->data, packet->length, false);
	inputBitStream.IgnoreBits(8*sizeof(unsigned char));
	
	if (inputBitStream.ReadCompressed(numberOfServers)==false)
		return;

	currentTime=RakNet::GetTime();

	for (serverIndex=0; serverIndex < numberOfServers; serverIndex++)
	{
		gameServer = DeserializeServer(&inputBitStream);

		// Find the existing game server that matches this port/address.
		// If not found, then add it to the list.
		// else update it
		// If (overwriteExisting)
		// - Delete any fields that exist in the old and not in the new
		// Add any fields that exist in the new and do not exist in the old
		// Update any fields that exist in both
		// Unset the deletion mark
		gameServer=UpdateServerList(gameServer,overwriteExisting, &newServerAdded);
		if (newServerAdded)
		{
            SystemAddress sa = rakPeer->GetSystemAddressFromGuid(gameServer->connectionIdentifier).ToString();
			// Ping the new server
			rakPeer->Ping(sa.ToString(),
				sa.GetPort(), false);

			// Returns true if new server updated
			OnGameServerListAddition(gameServer);
		}
		else
		{
			// returns false if an existing server is modified
			OnGameServerListRuleUpdate(gameServer);
		}


	}

	// Any servers that were not updated on the last call to UpdateServerList
	// will have lastUpdateTime time as less than the current time
	// Delete those
	serverIndex=0;
	while (serverIndex < (int) gameServerList.serverList.Size())
	{
		if (gameServerList.serverList[serverIndex]->lastUpdateTime < currentTime)
		{
			delete gameServerList.serverList[serverIndex];
			gameServerList.serverList.RemoveAtIndex(serverIndex);
		}
		else
			serverIndex++;
	}

	OnGameServerListQueryComplete();
}

void MasterClient::HandleRelayedConnectionNotification(Packet *packet)
{
	RakNetGUID clientSystem;
	BitStream incomingBitStream(packet->data, packet->length, false);
	incomingBitStream.IgnoreBits(8*sizeof(unsigned char));
 
//	incomingBitStream.Read(packet->systemAddress.ToString());
//	incomingBitStream.Read(packet->systemAddress.GetPort());
	
	OnConnectionRequest(packet->systemAddress.ToString(), packet->systemAddress.GetPort());
}
void MasterClient::PostRule(char *ruleIdentifier, char *stringData, int intData)
{
	if (ruleIdentifier)
	{
		if (IsReservedRuleIdentifier(ruleIdentifier))
			return;

		localServerModified |= UpdateServerRule(&localServer, ruleIdentifier, stringData, intData);
	}	
}

void MasterClient::RemoveRule(char *ruleIdentifier)
{
	if (ruleIdentifier)
		localServerModified |= RemoveServerRule(&localServer, ruleIdentifier);
}

void MasterClient::OnLostConnection(void)
{
#ifdef _SHOW_MASTER_SERVER_PRINTF
	printf("Connection lost.\n");
#endif
}
void MasterClient::OnConnectionAttemptFailed(void)
{
#ifdef _SHOW_MASTER_SERVER_PRINTF
	printf("Connection attempt failed.\n");
#endif
}
void MasterClient::OnMasterServerFull(void)
{
#ifdef _SHOW_MASTER_SERVER_PRINTF
	printf("Server full.\n");
#endif
}
void MasterClient::OnModifiedPacket(void)
{
#ifdef _SHOW_MASTER_SERVER_PRINTF
	printf("Modified packet.\n");
#endif
}
void MasterClient::OnGameServerListAddition(GameServer *newServer)
{
#ifdef _SHOW_MASTER_SERVER_PRINTF
	printf("Server added.\n");
#endif
}
void MasterClient::OnGameServerListRuleUpdate(GameServer *updatedServer)
{
#ifdef _SHOW_MASTER_SERVER_PRINTF
	printf("Rules updated for a server.\n");
#endif
}
void MasterClient::OnGameServerListQueryComplete(void)
{
#ifdef _SHOW_MASTER_SERVER_PRINTF
	printf("Query complete.\n");
#endif
}
// Event when a game client wants to connect to our server
// You should call AdvertiseSystem to the passed IP and port from your game instance
void MasterClient::OnConnectionRequest(const char *clientIP, unsigned short clientPort)
{
#ifdef _SHOW_MASTER_SERVER_PRINTF
	printf("Master client indicates a connection request from %s:%i.\n", clientIP, clientPort);
#endif
	rakPeer->AdvertiseSystem((char*)clientIP, clientPort,0,0);
}
