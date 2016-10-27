/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant 
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "MasterServer.h"
#include "RakPeerInterface.h"
#include "BitStream.h"
#include "StringCompressor.h"
#include "GetTime.h"
#include "RakNetStatistics.h"
#include "MessageIdentifiers.h"

// Uncomment this define for debugging printfs
#define _SHOW_MASTER_SERVER_PRINTF
#ifdef _SHOW_MASTER_SERVER_PRINTF
#include <cstdio>
#endif

MasterServer::MasterServer()
{
}

MasterServer::~MasterServer()
{
	ClearServerList();
}

void MasterServer::Update(RakPeerInterface *peer)
{	
	unsigned serverIndex;
    RakNet::Time time;
	// TODO - should have multiple listing security

	time = RakNet::GetTime();

	serverIndex=0;
	while (serverIndex < gameServerList.serverList.Size())
	{
		if (time >= gameServerList.serverList[serverIndex]->nextPingTime)
		{
			if (gameServerList.serverList[serverIndex]->failedPingResponses>=NUMBER_OF_MISSED_PINGS_TO_DROP)
			{
				#ifdef _SHOW_MASTER_SERVER_PRINTF
				printf("Deleting %s for lack of ping response.\n", (char*)rakPeer->GetSystemAddressFromGuid( gameServerList.serverList[serverIndex]->connectionIdentifier).ToString() );
				#endif
				gameServerList.serverList[serverIndex]->Clear();
				delete gameServerList.serverList[serverIndex];
				gameServerList.serverList.RemoveAtIndex(serverIndex);
			}
            
			else
			{
				gameServerList.serverList[serverIndex]->nextPingTime = time + KEEP_ALIVE_PING_FREQUENCY;
            
                if (rakPeer->GetIndexFromSystemAddress(rakPeer->GetSystemAddressFromGuid( gameServerList.serverList[serverIndex]->connectionIdentifier)) == -1 )
				{
					rakPeer->Ping(rakPeer->GetSystemAddressFromGuid( gameServerList.serverList[serverIndex]->connectionIdentifier));

//                    rakPeer->Ping((char*)rakPeer->PlayerIDToDottedIP(gameServerList.serverList[serverIndex]->connectionIdentifier),
//                                  gameServerList.serverList[serverIndex]->connectionIdentifier.port, false);
                   
					gameServerList.serverList[serverIndex]->failedPingResponses++;
#ifdef _SHOW_MASTER_SERVER_PRINTF
					printf("Pinging %s. Waiting on %i repl(ies) so far.\n", rakPeer->GetSystemAddressFromGuid(gameServerList.serverList[serverIndex]->connectionIdentifier).ToString(),gameServerList.serverList[serverIndex]->failedPingResponses);
#endif
				}
				else
				{
#ifdef _SHOW_MASTER_SERVER_PRINTF
                    printf("Not pinging %s since they are currently connected.\n",  rakPeer->GetSystemAddressFromGuid(gameServerList.serverList[serverIndex]->connectionIdentifier).ToString());
                           
#endif
				}
				serverIndex++;
			}
		}
		else
			serverIndex++;
	}
}

bool MasterServer::OnReceive(RakPeerInterface *peer, Packet *packet)
{

	RakNetStatistics *rss;
    RakNet:Time connectionTime, time;
//    RakNet:Time time;
	unsigned serverIndex;
    
	time = RakNet::GetTime();

	// Quick and dirty flood attack security:
	// If a client has been connected for more than 5 seconds,
	// and has sent more than 1000 bytes per second on average then ban them
	rss = rakPeer->GetStatistics(packet->systemAddress , rss);
	if (rss)
	{
        
		connectionTime=time-rss->connectionStartTime;
       
		if (connectionTime > FLOOD_ATTACK_CHECK_DELAY &&
			(float)( rss->runningTotal[ACTUAL_BYTES_RECEIVED]/8) / (float) connectionTime > FLOOD_ATTACK_BYTES_PER_MS)
		{
			rakPeer->CloseConnection(packet->guid, true,0);
#ifdef _SHOW_MASTER_SERVER_PRINTF
			printf("%s banned for session due to for flood attack\n", rakPeer->GetSystemAddressFromGuid(packet->guid).ToString());
#endif
			rakPeer->AddToBanList(rakPeer->GetSystemAddressFromGuid(packet->guid).ToString());

			// Find all servers with this IP and kill them.
			serverIndex=0;
			while (serverIndex < gameServerList.serverList.Size())
			{
				if (rakPeer->GetSystemAddressFromGuid(gameServerList.serverList[serverIndex]->connectionIdentifier).EqualsExcludingPort(rakPeer->GetSystemAddressFromGuid( packet->guid ) ) )
				{
					delete gameServerList.serverList[serverIndex];
					gameServerList.serverList.RemoveAtIndex(serverIndex);
				}
				else
					serverIndex++;
			}
		}
	}

	switch(packet->data[0])
	{
	case ID_QUERY_MASTER_SERVER:
		HandleQuery(packet);
		return true;
	case ID_MASTER_SERVER_DELIST_SERVER:
		HandleDelistServer(packet);
		return true;
	case ID_MASTER_SERVER_SET_SERVER:
		HandleUpdateServer(packet);
		return true;
	case ID_UNCONNECTED_PONG:
		HandlePong(packet);
		return false;
	case ID_RELAYED_CONNECTION_NOTIFICATION:
		HandleRelayedConnectionNotification(packet);
		return true;
	}

	return false; // Absorb packet
}

bool MasterServer::PropagateToGame(Packet *packet) const
{
	unsigned char packetIdentifier = packet->data[ 0 ];
	
	return packetIdentifier!=ID_QUERY_MASTER_SERVER &&
		packetIdentifier!=ID_MASTER_SERVER_DELIST_SERVER &&
		packetIdentifier!=ID_MASTER_SERVER_SET_SERVER &&
		packetIdentifier!=ID_RELAYED_CONNECTION_NOTIFICATION;
}

void MasterServer::HandleDelistServer(Packet *packet)
{
//	RakNetGUID serverPlayerID;
	int existingServerIndex;
//	BitStream bitStream(packet->data, packet->length, false);
//
//	bitStream.IgnoreBits(sizeof(unsigned char)*8); // Ignore the packet type enum

//    unsigned short port;
//	bitStream.Read(port);
//    this->rakPeer->GetSystemAddressFromGuid(serverPlayerID).SetPortHostOrder(port);
//    
//	serverPlayerID.binaryAddress = packet->playerId.binaryAddress;

	existingServerIndex = gameServerList.GetIndexByPlayerID(packet->guid);
    
	if (existingServerIndex>=0)
	{
		gameServerList.serverList[existingServerIndex]->Clear();
		delete gameServerList.serverList[existingServerIndex];
		gameServerList.serverList.RemoveAtIndex(existingServerIndex);
	}
	//else
		// Server does not already exist

	#ifdef _SHOW_MASTER_SERVER_PRINTF
	printf("%i servers on the. list\n", gameServerList.serverList.Size());
	#endif
    
}

void MasterServer::HandleQuery(Packet *packet)
{
	DataStructures::List<GameServer*> serversWithKeysList;
	char ruleIdentifier[256];
	unsigned index, serverIndex;
	int key;
	bool queryAll;
	BitStream outputBitStream;
	BitStream compressedString(packet->data, packet->length, false);
	compressedString.IgnoreBits(8*sizeof(unsigned char));

	queryAll=true;

	while (compressedString.GetNumberOfUnreadBits()>0)
	{
		// Generate a list of the indices of the servers that have one or more of the specified keys.
		stringCompressor->DecodeString(ruleIdentifier, 256, &compressedString);
		if (ruleIdentifier[0]==0)
			// If we fail to read the first string, queryAll remains true.
			break;
		
		queryAll=false;

		if (IsReservedRuleIdentifier(ruleIdentifier))
			continue;

		for (index=0; index < gameServerList.serverList.Size(); index++)
		{
			if (gameServerList.serverList[index]->connectionIdentifier==UNASSIGNED_RAKNET_GUID)
				continue;

			if (gameServerList.serverList[index]->FindKey(ruleIdentifier))
			{
				serverIndex=serversWithKeysList.GetIndexOf(gameServerList.serverList[index]);
				if (serverIndex==MAX_UNSIGNED_LONG)
				{
					gameServerList.serverList[index]->numberOfKeysFound=1;
					serversWithKeysList.Insert(gameServerList.serverList[index],__FILE__,__LINE__);
				}
				else
				{
					serversWithKeysList[serverIndex]->numberOfKeysFound++;
				}
			}
		}
	}
	
	// Write the packet id
	if (queryAll)
		outputBitStream.Write((unsigned char) ID_MASTER_SERVER_SET_SERVER);
	else
		outputBitStream.Write((unsigned char) ID_MASTER_SERVER_UPDATE_SERVER);
	if (queryAll)
	{
		// Write the number of servers
		outputBitStream.WriteCompressed((unsigned short)gameServerList.serverList.Size());

		for (index=0; index < gameServerList.serverList.Size(); index++)
		{
			// Write the whole server
			SerializeServer(gameServerList.serverList[index], &outputBitStream);	
		}		
	}
	else
	{
		compressedString.ResetReadPointer();
		compressedString.IgnoreBits(8*sizeof(unsigned char));

		// Write the number of servers with requested keys
		outputBitStream.WriteCompressed((unsigned short)serversWithKeysList.Size());

		// For each server, write the header which consists of the IP/PORT.
		// Then go through the list of requested keys and write those
		for (index=0; index < serversWithKeysList.Size(); index++)
		{
			SerializePlayerID(&(serversWithKeysList[index]->connectionIdentifier), &outputBitStream);

			outputBitStream.WriteCompressed((unsigned short)serversWithKeysList[index]->numberOfKeysFound);
			while (compressedString.GetNumberOfUnreadBits()>0)
			{
				// Generate a list of the indices of the servers that have one or more of the specified keys.
				stringCompressor->DecodeString(ruleIdentifier, 256, &compressedString);
				if (ruleIdentifier[0]==0)
					break;
				if (IsReservedRuleIdentifier(ruleIdentifier))
					continue;

				serversWithKeysList[index]->FindKey(ruleIdentifier);
				key=serversWithKeysList[index]->keyIndex;
				if (key>=0)
					SerializeRule(serversWithKeysList[index]->serverRules[key], &outputBitStream);
			}
		}
	}

	rakPeer->Send(&outputBitStream, MEDIUM_PRIORITY, RELIABLE, 0, packet->guid, false);
}

void MasterServer::HandleUpdateServer(Packet *packet)
{
	GameServer *gameServer;
	bool newServerAdded;
	BitStream incomingBitStream(packet->data, packet->length, false);
	incomingBitStream.IgnoreBits(8*sizeof(unsigned char));

	gameServer = DeserializeServer(&incomingBitStream);
    
	gameServer->connectionIdentifier = packet->guid;

	UpdateServerList(gameServer, true, &newServerAdded);

	if (newServerAdded)
	{
		#ifdef _SHOW_MASTER_SERVER_PRINTF
		printf("Server added. %i servers on the list\n", gameServerList.serverList.Size());
		#endif
		gameServer->originationId = packet->guid;
	}
	#ifdef _SHOW_MASTER_SERVER_PRINTF
	else
		printf("Server updated. %i servers on the list\n", gameServerList.serverList.Size());
	#endif
}

void MasterServer::OnModifiedPacket(void)
{
#ifdef _SHOW_MASTER_SERVER_PRINTF
	printf("Modified packet.\n");
#endif
}

void MasterServer::HandleRelayedConnectionNotification(Packet *packet)
{
	char str[22];
	unsigned short clientGamePort, serverGamePort;
	BitStream incomingBitStream(packet->data, packet->length, false);
	incomingBitStream.IgnoreBits(8*sizeof(unsigned char));
	incomingBitStream.Read(clientGamePort);
	incomingBitStream.Read(serverGamePort);
	if (!stringCompressor->DecodeString(str, 22, &incomingBitStream))
		return;

	if (str[0]==0)
		return;

	BitStream outgoingBitStream;
	outgoingBitStream.Write((unsigned char)ID_RELAYED_CONNECTION_NOTIFICATION);
	// Assumes the game client is on the same computer as the master client
	outgoingBitStream.Write(packet->systemAddress.ToString()); // This is the public IP, which the sender doesn't necessarily know
	outgoingBitStream.Write(clientGamePort);

	RakNetGUID targetID;
    SystemAddress sa;
    sa.FromStringExplicitPort(str, serverGamePort);
    targetID = rakPeer->GetGuidFromSystemAddress(sa);
    
	// Given the IP and port of the game system, give me the index into the game server list
	int serverIndex = gameServerList.GetIndexByPlayerID(targetID);

	if (serverIndex>=0)
	{
		#ifdef _SHOW_MASTER_SERVER_PRINTF
		printf("ID_RELAYED_CONNECTION_NOTIFICATION sent to %s:%i from %s:%i\n", str, serverGamePort, packet->systemAddress.ToString(), packet->systemAddress.GetPort());
		#endif
		rakPeer->Send(&outgoingBitStream, HIGH_PRIORITY, RELIABLE, 0, gameServerList.serverList[serverIndex]->originationId, false);
	}
	else
	{
#ifdef _SHOW_MASTER_SERVER_PRINTF
		printf("ID_RELAYED_CONNECTION_NOTIFICATION not sent to %s:%i from %s:%i.\nMaster server does not know about target system.\n", str, serverGamePort, packet->systemAddress.ToString(), packet->systemAddress.GetPort());
#endif
	}

	
}
