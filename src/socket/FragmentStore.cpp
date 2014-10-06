/*
 * FragmentStore.cpp
 *
 *  Created on: Sep 29, 2014
 *      Author: root
 */

#include "FragmentStore.h"

namespace na62 {

std::map<ushort, std::vector<DataContainer>> FragmentStore::fragmentsById_;
tbb::spin_mutex FragmentStore::newFragmentMutex_;

} /* namespace na62 */