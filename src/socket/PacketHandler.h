/*
 * PacketHandler.h
 *
 *  Created on: Feb 7, 2012
 *      Author: Jonas Kunze (kunze.jonas@gmail.com)
 */

#pragma once
#ifndef PACKETHANDLER_H_
#define PACKETHANDLER_H_

#include <sys/types.h>
#include <atomic>
#include <cstdint>
#include <vector>
#include <iostream>
#include <utils/AExecutable.h>
#include <boost/timer/timer.hpp>
#include <eventBuilding/EventPool.h>
#include <eventBuilding/Event.h>

namespace na62 {
struct DataContainer;

class PacketHandler: public AExecutable {

public:
	PacketHandler(int threadNum);
	virtual ~PacketHandler();

	void stopRunning() {
		running_ = false;
	}

	static std::atomic<uint> spins_;
	static std::atomic<uint> sleeps_;
	static boost::timer::cpu_timer sendTimer;

	/*
	 * Number of times a HandleFrameTask object has been created and enqueued
	 */

private:

	static uint_fast16_t L0_Port;
	static uint_fast16_t CREAM_Port;
	static uint_fast16_t STRAW_PORT;
	static uint_fast32_t MyIP;


	int threadNum_;
	bool running_;
	static uint NUMBER_OF_EBS;

	/**
	 * @return <true> In case of success, false in case of a serious error (we should stop the thread in this case)
	 */
	void thread();
	bool PacketHandler::checkFrame(UDP_HDR* hdr, uint_fast16_t length);
	void PacketHandler::processARPRequest(ARP_HDR* arp);
};

} /* namespace na62 */
#endif /* PACKETHANDLER_H_ */
