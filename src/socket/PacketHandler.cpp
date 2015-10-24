/*
 * PacketHandler.cpp
 *
 *  Created on: Feb 7, 2012
 *      Author: Jonas Kunze (kunze.jonas@gmail.com)
 */

#include "PacketHandler.h"

#include <tbb/task.h>
#include <tbb/tick_count.h>
#include <tbb/tbb_thread.h>
#include <linux/pf_ring.h>
#include <net/ethernet.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <sys/types.h>
#include <algorithm>
#include <cstdbool>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <queue>
#include <thread>

#include <exceptions/UnknownCREAMSourceIDFound.h>
#include <exceptions/UnknownSourceIDFound.h>
#include <l0/MEP.h>
#include <l0/MEPFragment.h>
#include <LKr/L1DistributionHandler.h>
#include <LKr/LkrFragment.h>
#include "../options/MyOptions.h"
#include <structs/Event.h>
#include <structs/Network.h>
#include <socket/EthernetUtils.h>
#include <socket/NetworkHandler.h>
#include <eventBuilding/SourceIDManager.h>
#include <boost/timer/timer.hpp>
#include <options/Logging.h>
#include <monitoring/BurstIdHandler.h>

#include "HandleFrameTask.h"

namespace na62 {


uint_fast16_t PacketHandler::L0_Port;
uint_fast16_t PacketHandler::CREAM_Port;
uint_fast16_t PacketHandler::STRAW_PORT;
uint_fast32_t PacketHandler::MyIP;


uint PacketHandler::NUMBER_OF_EBS = 0;

PacketHandler::PacketHandler(int threadNum) :
		threadNum_(threadNum), running_(true) {
	NUMBER_OF_EBS = Options::GetInt(OPTION_NUMBER_OF_EBS);
}

PacketHandler::~PacketHandler() {
}

void PacketHandler::thread() {
	const bool activePolling = Options::GetBool(OPTION_ACTIVE_POLLING);
	const uint pollDelay = Options::GetDouble(OPTION_POLLING_DELAY);

	const uint maxAggregationMicros = Options::GetInt(
	OPTION_MAX_AGGREGATION_TIME);

	const uint minUsecBetweenL1Requests = Options::GetInt(
	OPTION_MIN_USEC_BETWEEN_L1_REQUESTS);

	uint sleepMicros = Options::GetInt(OPTION_POLLING_SLEEP_MICROS);

	const uint framesToBeGathered = Options::GetInt(
	OPTION_MAX_FRAME_AGGREGATION);

	//boost::timer::cpu_timer sendTimer;

	while (running_) {
		u_char* data;
		uint_fast16_t bytesReceived = NetworkHandler::GetNextFrame(threadNum_,
		false /*active polling*/, data);

		DataContainer container = new DataContainer(data, bytesReceived)

		if (bytesReceived > 0) {
			//Process Frame
			try {
				UDP_HDR* hdr = (UDP_HDR*) data;
				const uint_fast16_t etherType = /*ntohs*/(hdr->eth.ether_type);
				const uint_fast8_t ipProto = hdr->ip.protocol;
				uint_fast16_t destPort = ntohs(hdr->udp.dest);
				const uint_fast32_t dstIP = hdr->ip.daddr;

				/*
				 * Check if we received an ARP request
				 * TODO Check lengths of items (hdr, buff, etc.)
				 *
				 *
				 */
				if (etherType != 0x0008/*ETHERTYPE_IP*/|| ipProto != IPPROTO_UDP) {
					if (etherType == 0x0608/*ETHERTYPE_ARP*/) {
						u_int16_t pktLen = bytesReceived;
						char buff[64];
						char* pbuff = buff;
						memcpy(pbuff, data, pktLen);
						std::stringstream AAARP;
						AAARP << "ARP Request FromRouter" << pktLen << " ";
						for (int i = 0; i < pktLen; i++)
							AAARP << std::hex << ((char) (*(pbuff + i)) & 0xFF)
									<< " ";
						//				LOG_INFO << AAARP.str() << ENDL;

						// this will delete the data
						processARPRequest(
								reinterpret_cast<ARP_HDR*>(data));
						return;
					} else {
						// Just ignore this frame as it's neither IP nor ARP
						//container.free();
						return;
					}
				}

				/*
				 * Check checksum errors
				 */
				if (!checkFrame(hdr, bytesReceived)) {
					return;
				}

				/*
				 * Check if we are really the destination of the IP datagram
				 */
				if (MyIP != dstIP) {
					return;
				}

				if (hdr->isFragment()) {
					container = FragmentStore::addFragment(
							std::move(container));
					if (container.data == nullptr) {
						return;
					}
					hdr = reinterpret_cast<UDP_HDR*>(container.data);
					destPort = ntohs(hdr->udp.dest);
				}

				const char * UDPPayload = container.data + sizeof(UDP_HDR);
				const uint_fast16_t & UdpDataLength = ntohs(hdr->udp.len)
						- sizeof(udphdr);

				/*
				 *  Now let's see what's insight the packet
				 */
				if (destPort == L0_Port) { ////////////////////////////////////////////////// L0 Data //////////////////////////////////////////////////
					/*
					 * L0 Data
					 * Length is hdr->ip.tot_len-sizeof(udphdr) and not container.length because of ethernet padding bytes!
					 */
					l0::MEP* mep = new l0::MEP(UDPPayload, UdpDataLength,
							container.data);

					uint sourceNum = SourceIDManager::sourceIDToNum(
							mep->getSourceID());

					MEPsReceivedBySourceNum_[sourceNum].fetch_add(1,
							std::memory_order_relaxed);
					BytesReceivedBySourceNum_[sourceNum].fetch_add(
							container.length, std::memory_order_relaxed);

					for (uint i = 0; i != mep->getNumberOfFragments(); i++) {
						// Add every fragment
						//				if (EventPool::getPoolSize()
						//						> mep->getFragment(i)->getEventNumber()) {
						//					EventPool::getL0PacketCounter()[mep->getFragment(i)->getEventNumber()].fetch_add(
						//							1, std::memory_order_relaxed);
						//				}
						L1Builder::buildEvent(mep->getFragment(i), burstID_);
					}
					/*
					 * Setup L1 block if L1 is active copying informations from L0TP MEps
					 */
					if (mep->getSourceID() == SOURCE_ID_L0TP) {
						if (SourceIDManager::isL1Active()) {
							uint16_t mep_factor = mep->getNumberOfFragments();
							uint32_t L1EventLength = sizeof(L1_BLOCK) + 8; //event length in bytes
							uint32_t L1BlockLength = mep_factor * L1EventLength
									+ 8; //L1 block length in bytes
							char * L1Data = new char[L1BlockLength
									+ sizeof(UDP_HDR)]; //include UDP header
							l0::MEP_HDR * L1Hdr = (l0::MEP_HDR *) (L1Data
									+ sizeof(UDP_HDR));

							L1Hdr->firstEventNum = mep->getFirstEventNum();
							L1Hdr->sourceID = SOURCE_ID_L1;
							L1Hdr->mepLength = L1BlockLength;
							L1Hdr->eventCount = mep_factor;
							L1Hdr->sourceSubID = 0;

							char * L1Event = L1Data + sizeof(UDP_HDR) + 8;
							l0::MEPFragment * L1Fragment;
							for (uint i = 0; i != mep_factor; i++) {
								L1Fragment = mep->getFragment(i);
								memcpy(L1Event,
										L1Fragment->getDataWithMepHeader(), 8);
								uint temp;
								temp = *(uint *) (L1Event) & 0xffff0000;
								temp |= L1EventLength;
								*(uint *) (L1Event) = temp;
								L1Event += L1EventLength;
							}

							const uint_fast16_t & L1DataLength = L1BlockLength;

							l0::MEP* mep_L1 = new l0::MEP(
									L1Data + sizeof(UDP_HDR), L1DataLength,
									L1Data);
							uint sourceNum = SourceIDManager::sourceIDToNum(
									mep_L1->getSourceID());

							MEPsReceivedBySourceNum_[sourceNum].fetch_add(1,
									std::memory_order_relaxed);
							BytesReceivedBySourceNum_[sourceNum].fetch_add(
									L1BlockLength + sizeof(UDP_HDR),
									std::memory_order_relaxed);
							for (uint i = 0;
									i != mep_L1->getNumberOfFragments(); i++) {
								// Add every fragment
								L1Builder::buildEvent(mep_L1->getFragment(i),
										burstID_);
							}
						}
						if (SourceIDManager::isL2Active()) {
							uint16_t mep_factor = mep->getNumberOfFragments();
							uint32_t L2EventLength = sizeof(L2_BLOCK) + 8; //event length in bytes
							uint32_t L2BlockLength = mep_factor * L2EventLength
									+ 8; //L2 block length in bytes
							char * L2Data = new char[L2BlockLength
									+ sizeof(UDP_HDR)]; //include UDP header
							l0::MEP_HDR * L2Hdr = (l0::MEP_HDR *) (L2Data
									+ sizeof(UDP_HDR));

							L2Hdr->firstEventNum = mep->getFirstEventNum();
							L2Hdr->sourceID = SOURCE_ID_L2;
							L2Hdr->mepLength = L2BlockLength;
							L2Hdr->eventCount = mep_factor;
							L2Hdr->sourceSubID = 0;

							char * L2Event = L2Data + sizeof(UDP_HDR) + 8;
							l0::MEPFragment * L2Fragment;
							for (uint i = 0; i != mep_factor; i++) {
								L2Fragment = mep->getFragment(i);
								memcpy(L2Event,
										L2Fragment->getDataWithMepHeader(), 8);
								uint temp;
								temp = *(uint *) (L2Event) & 0xffff0000;
								temp |= L2EventLength;
								*(uint *) (L2Event) = temp;
								L2Event += L2EventLength;
							}

							const uint_fast16_t & L2DataLength = L2BlockLength;

							l0::MEP* mep_L2 = new l0::MEP(
									L2Data + sizeof(UDP_HDR), L2DataLength,
									L2Data);
							uint sourceNum = SourceIDManager::sourceIDToNum(
									mep_L2->getSourceID());

							MEPsReceivedBySourceNum_[sourceNum].fetch_add(1,
									std::memory_order_relaxed);
							BytesReceivedBySourceNum_[sourceNum].fetch_add(
									L2BlockLength + sizeof(UDP_HDR),
									std::memory_order_relaxed);

							for (uint i = 0;
									i != mep_L2->getNumberOfFragments(); i++) {
								// Add every fragment
								L1Builder::buildEvent(mep_L2->getFragment(i),
										burstID_);
							}
						}
						if (SourceIDManager::isNSTDActive()) {
							uint16_t mep_factor = mep->getNumberOfFragments();
							uint32_t NSTDEventLength = sizeof(L2_BLOCK) + 8; //event length in bytes
							uint32_t NSTDBlockLength = mep_factor
									* NSTDEventLength + 8; //L2 block length in bytes
							char * NSTDData = new char[NSTDBlockLength
									+ sizeof(UDP_HDR)]; //include UDP header
							l0::MEP_HDR * NSTDHdr = (l0::MEP_HDR *) (NSTDData
									+ sizeof(UDP_HDR));

							NSTDHdr->firstEventNum = mep->getFirstEventNum();
							NSTDHdr->sourceID = SOURCE_ID_NSTD;
							NSTDHdr->mepLength = NSTDBlockLength;
							NSTDHdr->eventCount = mep_factor;
							NSTDHdr->sourceSubID = 0;

							char * NSTDEvent = NSTDData + sizeof(UDP_HDR) + 8;
							l0::MEPFragment * NSTDFragment;
							for (uint i = 0; i != mep_factor; i++) {
								NSTDFragment = mep->getFragment(i);
								memcpy(NSTDEvent,
										NSTDFragment->getDataWithMepHeader(),
										8);
								uint temp;
								temp = *(uint *) (NSTDEvent) & 0xffff0000;
								temp |= NSTDEventLength;
								*(uint *) (NSTDEvent) = temp;
								NSTDEvent += NSTDEventLength;
							}

							const uint_fast16_t & NSTDDataLength =
									NSTDBlockLength;

							l0::MEP* mep_NSTD = new l0::MEP(
									NSTDData + sizeof(UDP_HDR), NSTDDataLength,
									NSTDData);
							uint sourceNum = SourceIDManager::sourceIDToNum(
									mep_NSTD->getSourceID());

							MEPsReceivedBySourceNum_[sourceNum].fetch_add(1,
									std::memory_order_relaxed);
							BytesReceivedBySourceNum_[sourceNum].fetch_add(
									NSTDBlockLength + sizeof(UDP_HDR),
									std::memory_order_relaxed);

							for (uint i = 0;
									i != mep_NSTD->getNumberOfFragments();
									i++) {
								// Add every fragment
								L1Builder::buildEvent(mep_NSTD->getFragment(i),
										burstID_);
							}
						}

					}
				} else if (destPort == CREAM_Port) { ////////////////////////////////////////////////// CREAM Data //////////////////////////////////////////////////
					cream::LkrFragment* fragment = new cream::LkrFragment(
							UDPPayload, UdpDataLength, container.data);

					//fragment

					MEPsReceivedBySourceNum_[highestSourceNum_].fetch_add(1,
							std::memory_order_relaxed);

					BytesReceivedBySourceNum_[highestSourceNum_].fetch_add(
							container.length, std::memory_order_relaxed);

					//			if (EventPool::getPoolSize() > fragment->getEventNumber()) {
					//				EventPool::getCREAMPacketCounter()[fragment->getEventNumber()].fetch_add(
					//						1, std::memory_order_relaxed);
					//			}
					L2Builder::buildEvent(fragment);
				} else if (destPort == STRAW_PORT) { ////////////////////////////////////////////////// STRAW Data //////////////////////////////////////////////////
					StrawReceiver::processFrame(std::move(container), burstID_);
				} else {
					/*
					 * Packet with unknown UDP port received
					 */
					LOG_ERROR<<"Packet with unknown UDP port received: " << destPort << ENDL;
				}
			} catch (UnknownSourceIDFound const& e) {
			} catch (UnknownCREAMSourceIDFound const&e) {
			} catch (NA62Error const& e) {
			}

		}

	}
	finish: LOG_INFO<<"Stopping PacketHandler thread " << threadNum_
	<< ENDL;
}
bool PacketHandler::checkFrame(UDP_HDR* hdr, uint_fast16_t length) {
	/*
	 * Check IP-Header
	 */
	//				if (!EthernetUtils::CheckData((char*) &hdr->ip, sizeof(iphdr))) {
	//					LOG_ERROR << "Packet with broken IP-checksum received");
	//					container.free();
	//					continue;
	//				}
	if (hdr->isFragment()) {
		return true;
	}

	if (ntohs(hdr->ip.tot_len) + sizeof(ether_header) != length) {
		/*
		 * Does not need to be equal because of ethernet padding
		 */
		if (ntohs(hdr->ip.tot_len) + sizeof(ether_header) > length) {
			LOG_ERROR<<
			"Received IP-Packet with less bytes than ip.tot_len field! " <<
			(ntohs(hdr->ip.tot_len) + sizeof(ether_header) ) << ":"<<length << ENDL;
			return false;
		}
	}

	/*
	 * Does not need to be equal because of ethernet padding
	 */
	if (ntohs(hdr->udp.len) + sizeof(ether_header) + sizeof(iphdr) > length) {
		LOG_ERROR<<"Received UDP-Packet with less bytes than udp.len field! "<<(ntohs(hdr->udp.len) + sizeof(ether_header) + sizeof(iphdr)) <<":"<<length;
		return false;
	}

	//				/*
	//				 * Check UDP checksum
	//				 */
	//				if (!EthernetUtils::CheckUDP(hdr, (const char *) (&hdr->udp) + sizeof(udphdr), ntohs(hdr->udp.len) - sizeof(udphdr))) {
	//					LOG_ERROR << "Packet with broken UDP-checksum received" ) << ENDL;
	//					container.free();
	//					continue;
	//				}
	return true;
}
void PacketHandler::processARPRequest(ARP_HDR* arp) {
	/*
	 * Look for ARP requests asking for my IP
	 */
	if (arp->targetIPAddr == NetworkHandler::GetMyIP()) { // This is asking for me
		DataContainer responseArp = EthernetUtils::GenerateARPv4(
				NetworkHandler::GetMyMac().data(), arp->sourceHardwAddr,
				NetworkHandler::GetMyIP(), arp->sourceIPAddr,
				ARPOP_REPLY);
		u_int16_t pktLen = responseArp.length;
		char buff[64];
		char* pbuff = buff;
		memcpy(pbuff, responseArp.data, pktLen);
		std::stringstream AAARP;
		AAARP << "ARP Response FarmToRouter" << pktLen << " ";
		for (int i = 0; i < pktLen; i++)
			AAARP << std::hex << ((char) (*(pbuff + i)) & 0xFF) << " ";
//		LOG_INFO << AAARP.str() << ENDL;
		NetworkHandler::AsyncSendFrame(std::move(responseArp));
	}
}
}
/* namespace na62 */
