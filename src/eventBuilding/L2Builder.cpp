/*
 * HandleFrameTask.cpp
 *
 *  Created on: Jun 27, 2014
 *      Author: Jonas Kunze (kunze.jonas@gmail.com)
 */

#include "L2Builder.h"

#include <eventBuilding/Event.h>
#include <eventBuilding/EventPool.h>
#include <LKr/LkrFragment.h>

#include <l2/L2TriggerProcessor.h>
#include "StorageHandler.h"

namespace na62 {

std::atomic<uint64_t>* L2Builder::L2Triggers_ = new std::atomic<uint64_t>[0xFF
		+ 1];

std::atomic<uint64_t> L2Builder::BytesSentToStorage_(0);
std::atomic<uint64_t> L2Builder::EventsSentToStorage_(0);

void L2Builder::buildEvent(cream::LkrFragment* LkrFragment) {
	Event *event = EventPool::GetEvent(LkrFragment->getEventNumber());

	/*
	 * If the event number is too large event is null and we have to drop the data
	 */
	if (event == nullptr) {
		delete LkrFragment;
		return;
	}

	/*
	 * Add new packet to EventCollector
	 */
	if (event->addLkrFragment(LkrFragment)) {
		/*
		 * This event is complete -> process it
		 */
		processL2(event);
	}
}

void L2Builder::processL2(Event *event) {
	if (!event->isWaitingForNonZSuppressedLKrData()) {
		/*
		 * L1 already passed but non zero suppressed LKr data not yet requested -> Process Level 2 trigger
		 */
		uint8_t L2Trigger = L2TriggerProcessor::compute(event);

		event->setL2Processed(L2Trigger);

		/*
		 * Event has been processed and saved or rejected -> destroy, don't delete so that it can be reused if
		 * during L2 no non zero suppressed LKr data has been requested
		 */
		if (!event->isWaitingForNonZSuppressedLKrData()) {
			if (event->isL2Accepted()) {
				BytesSentToStorage_ += StorageHandler::SendEvent(event);
				EventsSentToStorage_++;
			}
			L2Triggers_[L2Trigger]++;
			EventPool::FreeEvent(event);
		}
	} else {
		uint8_t L2Trigger = L2TriggerProcessor::onNonZSuppressedLKrDataReceived(
				event);

		event->setL2Processed(L2Trigger);
		if (event->isL2Accepted()) {
			BytesSentToStorage_ += StorageHandler::SendEvent(event);
			EventsSentToStorage_++;
		}
		L2Triggers_[L2Trigger]++;
		EventPool::FreeEvent(event);
	}
}
}
/* namespace na62 */
